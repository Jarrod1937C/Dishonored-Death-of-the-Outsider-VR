#include "pch.h"
#include "StereoFeedback.h"
#include "GpuTrace.h"
#include "PassProfile.h"
#include "ViewMatrixHook.h"
#include "StereoSync.h"
#include "PlayerState.h"
#include <atomic>
#include <mutex>

namespace
{
    constexpr uint64_t kPsDistortApply = 0x6b05662764a19199ull;   // POSTPROC: t0 = the distortion pass's half-size RGBA16F image
    std::atomic<bool> g_On{ true };
    std::mutex g_M;                                  // g_Tex, g_Slot*, g_Valid
    ID3D11Texture2D* g_Tex = nullptr;                // the game's texture (a reference held)
    ID3D11Texture2D* g_Slot[2] = {};                 // its content as each eye's last frame left it
    unsigned long long g_Valid[2] = {};              // the Present that filled each slot (0 = empty)
    std::atomic<unsigned long long> g_Presents{ 0 }, g_SeenAt{ 0 };
    std::atomic<unsigned long long> g_Reads[2], g_Saved{ 0 }, g_Restored{ 0 }, g_NoEye{ 0 }, g_Found{ 0 };
    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    void DropSlots() { Rel(g_Slot[0]); Rel(g_Slot[1]); g_Valid[0] = g_Valid[1] = 0; }

    //! The eye of the image this Present shows (as VRRuntime picks it: the view the executed commands drew with)
    int ImageEye()
    {
        unsigned long long s = 0, age = 0;
        if (GpuTrace::ExecutedViewSerial(s, age) && age <= 1) { const ViewMatrixHook::XrPose rp = ViewMatrixHook::RenderPoseXr(s); if (rp.valid) return rp.flat ? -1 : rp.eye; }
        if (GpuTrace::DrawViewSerial(s, age) && age <= 1) { const ViewMatrixHook::XrPose rp = ViewMatrixHook::RenderPoseXr(s); if (rp.valid) return rp.flat ? -1 : rp.eye; }
        return -1;
    }

    // ---- flight trace (2026-11-08: "the double image disappears in a screenshot ... every other frame pair it flips
    // between two positions") ----
    // For the first 150 Presents of each of the first four Foresight flights (`Flying`), one line per Present: the image's
    // eye and view serial, where that view was rendered from (its origin and forward, and the headset pose it was built
    // with), the distortion image's reads by eye in the frame, and what this Present did with the per-eye copies. A camera
    // or an image that alternates frame pair by frame pair shows here as two interleaved series in one eye.
    int g_TraceLeft = 0, g_Flights = 0; bool g_WasFlying = false;
    unsigned long long g_ReadsAtLast[2] = {};
    void TraceFlight(unsigned long long n, int eye, const char* did)
    {
        const bool flying = PlayerState::Running("Flying");
        if (flying && !g_WasFlying && g_Flights < 4) { g_Flights++; g_TraceLeft = 150; LOGI("StereoFeedback: flight #{}: tracing the next {} Presents", g_Flights, g_TraceLeft); }
        g_WasFlying = flying;
        if (g_TraceLeft <= 0 || !flying) return;
        g_TraceLeft--;
        unsigned long long s = 0, age = 0;
        const bool have = GpuTrace::ExecutedViewSerial(s, age);
        float m[16], org[3] = {}, ax[9] = {};
        const bool v = have && ViewMatrixHook::ViewBySerial(s, m, org, ax);
        const ViewMatrixHook::XrPose rp = have ? ViewMatrixHook::RenderPoseXr(s) : ViewMatrixHook::XrPose();
        const unsigned long long r0 = g_Reads[0].load(), r1 = g_Reads[1].load();
        LOGI("StereoFeedback: flight #{} present {} | image eye {} view {} (age {}, pair {}) org ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) | hmd ({:.3f},{:.3f},{:.3f}) | reads this frame L {} R {} | {}",
             g_Flights, n, eye, have ? s : 0ull, age, rp.valid ? rp.pair : 0ull, v ? org[0] : 0.f, v ? org[1] : 0.f, v ? org[2] : 0.f, v ? ax[0] : 0.f, v ? ax[1] : 0.f, v ? ax[2] : 0.f,
             rp.pos[0], rp.pos[1], rp.pos[2], r0 - g_ReadsAtLast[0], r1 - g_ReadsAtLast[1], did);
        g_ReadsAtLast[0] = r0; g_ReadsAtLast[1] = r1;
    }
}

void StereoFeedback::SetEnabled(bool on) { g_On = on; PassProfile::SetPostWatch(on); LOGI("StereoFeedback: {}", on ? "on (each eye its own distortion image)" : "OFF (the game's: the distortion image of the frame before, the other eye's)"); }

void StereoFeedback::OnDraw(ID3D11DeviceContext* c)
{
    if (!g_On.load(std::memory_order_relaxed) || !PassProfile::PostTaskNow()) return;
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return;
    thread_local const void* lastPs = nullptr; thread_local uint64_t lastHash = 0;
    if (ps != lastPs) { lastPs = ps; lastHash = GpuTrace::ShaderHashOf(ps); }
    ps->Release();
    if (lastHash != kPsDistortApply) return;
    ID3D11ShaderResourceView* srv = nullptr; c->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* r = nullptr; srv->GetResource(&r); srv->Release();
    if (!r) return;
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t)
    {
        const int eye = PassProfile::CharEye();
        if (eye == 0 || eye == 1) g_Reads[eye]++;
        g_SeenAt = g_Presents.load() + 1;
        {
            std::lock_guard lk(g_M);
            if (t != g_Tex)
            {
                D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
                LOGI("StereoFeedback: the distortion image POSTPROC reads ({}x{} fmt {}, {} mip(s)) {}: kept per eye from now on",
                     d.Width, d.Height, (int)d.Format, d.MipLevels, g_Tex ? "changed (a resize)" : "found");
                Rel(g_Tex); DropSlots(); g_Tex = t; g_Tex->AddRef(); g_Found++;
            }
        }
        t->Release();
    }
    r->Release();
}

void StereoFeedback::OnPresent(IDXGISwapChain* sc)
{
    const unsigned long long n = ++g_Presents;
    std::lock_guard lk(g_M);
    if (!g_Tex) { TraceFlight(n, ImageEye(), "no texture yet"); return; }
    if (!g_On.load() || n > g_SeenAt.load() + 90)
    {   // not in use (the effect gone for a second, or switched off): the copies go, the game's texture stays as it is
        if (g_Slot[0] || g_Slot[1]) DropSlots();
        TraceFlight(n, ImageEye(), g_On.load() ? "not read for 90 Presents" : "OFF (vr stereofeedback 0)");
        return;
    }
    const int eye = ImageEye();
    if ((eye != 0 && eye != 1) || !StereoSync::Active()) { g_NoEye++; g_Valid[0] = g_Valid[1] = 0; TraceFlight(n, eye, "no eye: copies dropped"); return; }   // mono: nothing to separate
    ID3D11Device* dev = nullptr; g_Tex->GetDevice(&dev);
    if (!dev) return;
    ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm);
    if (!g_Slot[0])
    {
        D3D11_TEXTURE2D_DESC d{}; g_Tex->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
        d.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        for (auto& s : g_Slot) if (FAILED(dev->CreateTexture2D(&d, nullptr, &s))) s = nullptr;
        if (!g_Slot[0] || !g_Slot[1]) { LOGI("StereoFeedback: the per-eye copies could not be made: off"); DropSlots(); g_On = false; }
    }
    const char* did = "no copies";
    if (imm && g_Slot[0] && g_Slot[1])
    {
        imm->CopyResource(g_Slot[eye], g_Tex); g_Valid[eye] = n; g_Saved++;
        const int other = eye ^ 1;
        if (g_Valid[other] && n - g_Valid[other] <= 3) { imm->CopyResource(g_Tex, g_Slot[other]); g_Restored++; did = eye ? "saved R, restored L" : "saved L, restored R"; }
        else did = eye ? "saved R, nothing to restore" : "saved L, nothing to restore";
    }
    TraceFlight(n, eye, did);
    if (imm) imm->Release();
    dev->Release();
    (void)sc;
}

std::string StereoFeedback::Status()
{
    return fmt::format("stereofeedback {} tex {} found {} reads L {} R {} saved {} restored {} noeye {}", g_On.load() ? 1 : 0, g_Tex ? 1 : 0, g_Found.load(),
                       g_Reads[0].load(), g_Reads[1].load(), g_Saved.load(), g_Restored.load(), g_NoEye.load());
}
