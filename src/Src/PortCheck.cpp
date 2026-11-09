#include "pch.h"
#include "PortCheck.h"
#include "GameInfo.h"
#include "Game.h"
#include "CVars.h"
#include "CameraHook.h"
#include "HandAimHook.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <detours/detours.h>

namespace
{
    std::atomic<bool> g_On{ false };

    // ---- the hook table (tools/port/gen_portcheck.py) ----
    struct Entry { const char* module; const char* name; uint32_t rva; uint8_t bytes[16]; };
    const Entry kTable[] = {
#include "PortCheckTable.inc"
    };
    constexpr size_t kTableN = sizeof(kTable) / sizeof(kTable[0]);
    uint8_t g_AtStart[kTableN][16] = {};
    bool g_StartRead[kTableN] = {};

    bool ReadMem(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::string Hex(const uint8_t* b, size_t n)
    {
        std::string s; char t[4];
        for (size_t i = 0; i < n; i++) { snprintf(t, sizeof t, i ? " %02X" : "%02X", b[i]); s += t; }
        return s;
    }

    // ---- the shader census ----
    struct Want { PortCheck::Site site; const char* what; uint64_t hash; };
    const Want kWant[] = {
        { PortCheck::kSiteDepthOnly,    "GpuTrace depth-only pass PS",           0x48f2037fd43d86c8ull },
        { PortCheck::kSiteDepthCopy,    "GpuTrace depth copy PS",                0x904e7f2d94d6f085ull },
        { PortCheck::kSiteDepthCopy,    "GpuTrace DOF depth downsample PS",      0xb152af85c03cbbafull },
        { PortCheck::kSiteSneakOverlay, "GpuTrace sneak overlay PS",             0x2ee2b9822f61d7d1ull },
        { PortCheck::kSiteAo,           "GpuTrace DWAO PS",                      0x7012fd32e80f486eull },
        { PortCheck::kSiteAo,           "GpuTrace AO fill PS",                   0xea85def507442b03ull },
        { PortCheck::kSiteTrail,        "GpuTrace sword trail VS 1",             0x364e76313eb849ecull },
        { PortCheck::kSiteTrail,        "GpuTrace sword trail VS 2",             0x3fea51d57d8fff32ull },
        { PortCheck::kSiteTrail,        "GpuTrace sword trail VS 3",             0x412fe65bec01fc06ull },
        { PortCheck::kSitePostDraw,     "PostShare outline mask PS",             0x075df32c84f09477ull },
        { PortCheck::kSitePostDraw,     "PostShare outline blur PS",             0x2d11c3a17106ea42ull },
        { PortCheck::kSitePostDraw,     "PostShare outline compose PS",          0xd18f70002e2ca13aull },
        { PortCheck::kSitePostDispatch, "PostShare exposure histogram CS",       0xdc6d8bbce02a1266ull },
        { PortCheck::kSitePostDispatch, "PostShare exposure histogram merge CS", 0x9ba51c0d902365b7ull },
        { PortCheck::kSitePostDispatch, "PostShare exposure adapt CS",           0x577946c9f0ee49c9ull },
        { PortCheck::kSiteHudVs,        "WristHud interface fill VS",            0x7acc1a9a27c4d9e2ull },
    };
    constexpr size_t kWantN = sizeof(kWant) / sizeof(kWant[0]);
    const char* const kSiteName[PortCheck::kSiteCount] = { "depth-only pass PS", "depth copy PS", "POSTPROC while sneaking PS",
        "SSAO task PS", "draws during a Slash VS", "outline draws PS", "exposure dispatches CS", "interface fills VS" };
    std::atomic<unsigned> g_WantSeen[kWantN];
    std::atomic<unsigned long long> g_ShadersCreated{ 0 };
    constexpr int kSlots = 48;
    struct Slot { std::atomic<uint64_t> h{ 0 }; std::atomic<unsigned> n{ 0 }; };
    Slot g_Cand[PortCheck::kSiteCount][kSlots];
    std::atomic<unsigned> g_CandFull[PortCheck::kSiteCount];

    // ---- module fields ----
    struct FieldVal { std::string value; unsigned long long count = 0; };
    std::mutex g_FieldM;
    std::map<std::string, FieldVal> g_Fields;

    // ---- the throw probe: entity velocity setter (entity+0x34C = *v), DOTO exe+0x8E9C20 ----
    constexpr uint32_t kSetVelocity = 0x8E9C20;
    const uint8_t kSetVelocityBytes[] = { 0x8B, 0x02, 0x89, 0x81, 0x4C, 0x03, 0x00, 0x00 };   // mov eax,[rdx]; mov [rcx+0x34C],eax
    using SetVelFn = void(*)(void*, const float*);
    SetVelFn g_RealSetVel = nullptr;
    struct VelCaller { unsigned long long calls = 0; std::string cls; float speed = 0.f; };
    std::mutex g_VelM;
    std::map<uint32_t, VelCaller> g_VelCallers;
    std::atomic<unsigned> g_VelNewLogged{ 0 };

    double Now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    bool RttiName(const void* obj, char* out, size_t n)
    {
        out[0] = 0;
        __try
        {
            const uintptr_t base = GameInfo::Base();
            const auto* dos = (const IMAGE_DOS_HEADER*)base;
            const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
            const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
            const uintptr_t vt = *(const uintptr_t*)obj;
            if (vt < base || vt >= end) return false;
            const uintptr_t col = *(const uintptr_t*)(vt - 8);
            if (col < base || col >= end || *(const uint32_t*)col != 1) return false;   // x64 complete-object locator
            const uintptr_t td = base + *(const uint32_t*)(col + 12);
            if (td < base || td >= end) return false;
            const char* name = (const char*)(td + 16);
            size_t i = 0;
            for (; i + 1 < n && name[i]; i++) out[i] = name[i];
            out[i] = 0;
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void HkSetVelocity(void* ent, const float* v)
    {
        g_RealSetVel(ent, v);
        if (!g_On.load(std::memory_order_relaxed) || !v) return;
        const uint32_t ret = (uint32_t)((uintptr_t)_ReturnAddress() - GameInfo::Base());
        float s = 0.f;
        float w[3] = {};
        if (ReadMem(w, v, 12)) s = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        bool fresh = false; std::string cls;
        {
            std::unique_lock lk(g_VelM, std::try_to_lock);
            if (!lk.owns_lock()) return;
            VelCaller& c = g_VelCallers[ret];
            fresh = c.calls++ == 0;
            if (fresh || s > c.speed) { c.speed = s; }
            if (fresh) { c.cls = PortCheck::ClassName(ent); cls = c.cls; }
        }
        if (fresh && g_VelNewLogged.fetch_add(1) < 200)
            LOGI("PortCheck: throw probe: entity velocity set from exe+0x{:X} for a {} ({:.2f} m/s){}", ret, cls.empty() ? "?" : cls, s,
                 s > 3.f ? " <- fast: a thrown object?" : "");
    }

    void InstallThrowProbe()
    {
        uint8_t* p = (uint8_t*)(GameInfo::Base() + kSetVelocity);
        uint8_t b[8] = {};
        if (!ReadMem(b, p, 8) || (b[0] != 0xE9 && memcmp(b, kSetVelocityBytes, 8) != 0))
        { LOGI("PortCheck: throw probe: exe+0x{:X} is not the entity velocity setter ({}): not installed", kSetVelocity, Hex(b, 8)); return; }
        g_RealSetVel = (SetVelFn)p;
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_RealSetVel, HkSetVelocity);
        const LONG r = DetourTransactionCommit();
        if (r != NO_ERROR) g_RealSetVel = nullptr;
        LOGI("PortCheck: throw probe on the entity velocity setter exe+0x{:X} {} (each new caller is logged once: throw a grenade "
             "and look for the fast one)", kSetVelocity, r == NO_ERROR ? "installed" : "FAILED");
    }

    // ---- reports ----
    void ShaderReport(const char* when)
    {
        unsigned seen = 0;
        for (size_t i = 0; i < kWantN; i++) if (g_WantSeen[i].load()) seen++;
        LOGI("PortCheck: shader census ({}): {} shaders created so far; {} of the {} Dishonored 2 hashes the mod uses are in DOTO",
             when, g_ShadersCreated.load(), seen, kWantN);
        for (int s = 0; s < PortCheck::kSiteCount; s++)
        {
            std::string want;
            for (size_t i = 0; i < kWantN; i++)
                if (kWant[i].site == s)
                    want += fmt::format("{}{} {:016x} {}", want.empty() ? "" : ", ", kWant[i].what, kWant[i].hash,
                                        g_WantSeen[i].load() ? "SEEN" : "NOT SEEN");
            struct C { uint64_t h; unsigned n; };
            std::vector<C> met;
            for (auto& sl : g_Cand[s]) { const uint64_t h = sl.h.load(); if (h) met.push_back({ h, sl.n.load() }); }
            std::sort(met.begin(), met.end(), [](const C& a, const C& b) { return a.n > b.n; });
            std::string m;
            for (size_t i = 0; i < met.size() && i < 24; i++)
            {
                bool known = false;
                for (size_t k = 0; k < kWantN; k++) if (kWant[k].hash == met[i].h) known = true;
                m += fmt::format(" {:016x}x{}{}", met[i].h, met[i].n, known ? "(D2)" : "");
            }
            LOGI("PortCheck:   {}: wants {}; met{}{}", kSiteName[s], want, met.empty() ? " nothing yet" : ":" + m,
                 g_CandFull[s].load() ? " (table full)" : "");
        }
    }

    float Len3(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

    void FieldReport(const char* when)
    {
        void* gl = Game::GameLocal();
        void* pl = Game::LocalPlayer();
        std::string s = fmt::format("PortCheck: fields ({}): gameLocal {} [{}], player {} [{}]", when, gl, PortCheck::ClassName(gl), pl,
                                    PortCheck::ClassName(pl));
        float o[3] = {}, a[9] = {};
        if (Game::PlayerView(o, a))
        {
            const float r0 = Len3(a), r1 = Len3(a + 3), r2 = Len3(a + 6);
            const bool axisOk = fabsf(r0 - 1.f) < 0.05f && fabsf(r1 - 1.f) < 0.05f && fabsf(r2 - 1.f) < 0.05f;
            s += fmt::format("; player view +0x{:X} ({:.2f},{:.2f},{:.2f}) axis +0x{:X} rows {:.3f}/{:.3f}/{:.3f} {}", Game::kPlayerViewOrigin,
                             o[0], o[1], o[2], Game::kPlayerViewAxis, r0, r1, r2, axisOk ? "OK" : "BAD (not a rotation: wrong offset?)");
            float co[3] = {}, ca[9] = {};
            if (HandAimHook::RealCamera(co, ca))
            {
                const float d[3] = { co[0] - o[0], co[1] - o[1], co[2] - o[2] };
                const float dot = ca[0] * a[0] + ca[1] * a[1] + ca[2] * a[2];
                s += fmt::format("; camera eye +0x{:X} is {:.2f} m from it, forwards agree {:.3f} {}", HandAimHook::kCamOrigin, Len3(d), dot,
                                 Len3(d) < 1.f && dot > 0.8f ? "OK" : "CHECK (expected: within 1 m, about the same direction)");
            }
            else s += "; camera not known yet";
        }
        else s += "; no player view (menu / loading)";
        float fx = 0.f, fy = 0.f; CameraHook::GameFov(fx, fy);
        s += fmt::format("; game fov {:.1f} x {:.1f} deg (renderView +0x{:X}) {}", fx, fy, CameraHook::kRV_FovX,
                         fx == 0.f ? "not built yet" : fx > 20.f && fx < 170.f && fy > 10.f && fy < 170.f ? "OK" : "BAD (wrong offset?)");
        s += fmt::format("; com_disableFrameSpinning {}", CVars::GetInt("com_disableFrameSpinning", -1));
        LOGI("{}", s);
        std::lock_guard lk(g_FieldM);
        for (auto& [k, v] : g_Fields) LOGI("PortCheck:   field {}: {} (reported {} times)", k, v.value, v.count);
    }

    void VelocityReport()
    {
        std::lock_guard lk(g_VelM);
        if (g_VelCallers.empty()) return;
        std::vector<std::pair<uint32_t, VelCaller>> v(g_VelCallers.begin(), g_VelCallers.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.speed > b.second.speed; });
        std::string s;
        for (size_t i = 0; i < v.size() && i < 16; i++)
            s += fmt::format(" exe+0x{:X} {}x {} max {:.1f} m/s;", v[i].first, v[i].second.calls, v[i].second.cls.empty() ? "?" : v[i].second.cls, v[i].second.speed);
        LOGI("PortCheck: throw probe: velocity setters by top speed:{}", s);
    }
}

void PortCheck::Configure(bool on)
{
    g_On = on;
}

bool PortCheck::On() { return g_On.load(std::memory_order_relaxed); }

void PortCheck::Startup()
{
    if (!On()) return;
    const uintptr_t base = GameInfo::Base();
    size_t bad = 0, unreadable = 0;
    for (size_t i = 0; i < kTableN; i++)
    {
        const Entry& e = kTable[i];
        g_StartRead[i] = ReadMem(g_AtStart[i], (const void*)(base + e.rva), 16);
        if (!g_StartRead[i]) { unreadable++; LOGI("PortCheck: hook table: {}::{} exe+0x{:X} UNREADABLE", e.module, e.name, e.rva); continue; }
        if (memcmp(g_AtStart[i], e.bytes, 16) != 0)
        {
            bad++;
            LOGI("PortCheck: hook table: {}::{} exe+0x{:X} MISMATCH: running {} | ported for {}", e.module, e.name, e.rva,
                 Hex(g_AtStart[i], 16), Hex(e.bytes, 16));
        }
    }
    LOGI("PortCheck: on (game {} {}, supported build: {}). Hook table: {} code addresses of the mod checked against the running exe "
         "before any hook: {} match, {} MISMATCH, {} unreadable{}", GameInfo::ExeName(), GameInfo::Version(), GameInfo::IsSupportedBuild() ? "yes" : "NO",
         kTableN, kTableN - bad - unreadable, bad, unreadable, bad ? " (a mismatch = another game build, or a wrong port)" : "");
}

void PortCheck::AfterHooks(const char* when)
{
    if (!On()) return;
    if (!g_RealSetVel && GameInfo::IsSupportedBuild()) InstallThrowProbe();
    const uintptr_t base = GameInfo::Base();
    std::map<std::string, std::pair<std::string, std::string>> byModule;   // module -> (patched, untouched)
    for (size_t i = 0; i < kTableN; i++)
    {
        const Entry& e = kTable[i];
        uint8_t now[16] = {};
        if (!g_StartRead[i] || !ReadMem(now, (const void*)(base + e.rva), 16)) continue;
        auto& m = byModule[e.module];
        std::string& dst = memcmp(now, g_AtStart[i], 16) != 0 ? m.first : m.second;
        dst += fmt::format("{}{}", dst.empty() ? "" : " ", e.name);
    }
    LOGI("PortCheck: hook table ({}): per module, the addresses the mod has patched (a hook or a code change) / left as they are "
         "(called or read, or a hook that is off or not installed yet)", when);
    for (auto& [mod, pu] : byModule)
        LOGI("PortCheck:   {}: patched [{}] untouched [{}]", mod, pu.first, pu.second);
}

void PortCheck::OnShaderCreated(uint64_t hash)
{
    if (!On() || !hash) return;
    g_ShadersCreated.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < kWantN; i++)
        if (kWant[i].hash == hash && g_WantSeen[i].fetch_add(1) == 0)
            LOGI("PortCheck: shader census: {} {:016x} created (the Dishonored 2 hash holds in DOTO)", kWant[i].what, hash);
}

void PortCheck::Candidate(Site s, uint64_t hash)
{
    if (!g_On.load(std::memory_order_relaxed) || !hash || s < 0 || s >= kSiteCount) return;
    for (auto& sl : g_Cand[s])
    {
        uint64_t h = sl.h.load(std::memory_order_relaxed);
        if (h == hash) { sl.n.fetch_add(1, std::memory_order_relaxed); return; }
        if (!h && sl.h.compare_exchange_strong(h, hash)) { sl.n.fetch_add(1, std::memory_order_relaxed); return; }
        if (h == hash) { sl.n.fetch_add(1, std::memory_order_relaxed); return; }
    }
    g_CandFull[s].store(1, std::memory_order_relaxed);
}

void PortCheck::Field(const char* what, const std::string& value)
{
    if (!On() || !what) return;
    bool first = false;
    {
        std::lock_guard lk(g_FieldM);
        FieldVal& f = g_Fields[what];
        first = f.count++ == 0;
        f.value = value;
    }
    if (first) LOGI("PortCheck: field {}: {}", what, value);
}

std::string PortCheck::ClassName(const void* obj)
{
    if (!obj || (uintptr_t)obj < 0x10000) return {};
    char buf[160];
    if (!RttiName(obj, buf, sizeof buf)) return {};
    std::string n = buf;                                  // ".?AVarkPlayer@@" -> "arkPlayer"
    if (n.rfind(".?AV", 0) == 0 || n.rfind(".?AU", 0) == 0) n = n.substr(4);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "@@") == 0) n.resize(n.size() - 2);
    return n;
}

void PortCheck::OnPresent()
{
    if (!On()) return;
    static double t0 = 0.0, nextFields = 0.0, nextShaders = 0.0;
    static bool hooksAgain = false;
    static void* lastPlayer = nullptr;
    const double t = Now();
    if (t0 == 0.0) { t0 = t; nextFields = t + 20.0; nextShaders = t + 30.0; }
    // GpuTrace and PlayerState install at the first Present: the patched list once more, a minute in
    if (!hooksAgain && t - t0 > 60.0) { hooksAgain = true; AfterHooks("a minute after the first Present"); }
    void* pl = Game::LocalPlayer();
    const bool newPlayer = pl && pl != lastPlayer;
    lastPlayer = pl;
    if (newPlayer) nextFields = std::min(nextFields, t + 5.0);   // a level just loaded: check its fields soon
    if (t >= nextFields) { nextFields = t + (pl ? 60.0 : 30.0); FieldReport(pl ? "in play" : "no player yet"); VelocityReport(); }
    if (t >= nextShaders) { nextShaders = t + 120.0; ShaderReport(t - t0 < 60.0 ? "30 s after the first Present" : "periodic"); }
}

std::string PortCheck::Status()
{
    if (!On()) return "off";
    unsigned seen = 0;
    for (size_t i = 0; i < kWantN; i++) if (g_WantSeen[i].load()) seen++;
    size_t callers = 0;
    { std::lock_guard lk(g_VelM); callers = g_VelCallers.size(); }
    return fmt::format("on: {} code addresses, {}/{} D2 shader hashes seen, {} velocity callers", kTableN, seen, kWantN, callers);
}
