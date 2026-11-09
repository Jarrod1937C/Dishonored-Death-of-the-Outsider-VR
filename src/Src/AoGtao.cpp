#include "pch.h"
#include "AoGtao.h"
#include "Settings.h"
#include <d3dcompiler.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace
{
    // GTAO in the game's AO draw's place. Bound by the game at that draw (and left so): t0 ro_ssao_zbuffer (the eye's
    // full-resolution depth, hardware values), t1 ro_downsampling_downsampleddepthbuffer (half resolution, the LINEAR
    // depth in z: what the game's DWAO taps), cb1 PerViewCB (cb_inverseprojectionmatrix at c38..c41, used as a row
    // vector times the matrix: the game's own linear depth is -c41.z / (c40.w * d + c41.w)). The output is the game's
    // layout: x the visibility, y 1, z / w the linear depth packed in 16 bits exactly as DWAO and the "no AO" fill pack
    // it (the game's blur reads it). cb4 is the mod's: x radius (world units, metres), y the falloff's share of the
    // radius, z the power applied to the result, w the largest screen radius (full-resolution pixels).
    const char* kHlsl = R"(
cbuffer PerViewCB : register(b1) { float4 pv[50]; }
cbuffer Gtao : register(b4) { float4 ga; }
Texture2D<float> zfull : register(t0);
Texture2D<float4> zhalf : register(t1);
static const float PI = 3.14159265, HALF_PI = 1.57079633, THIN = 3.0;
static const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
float LinZ(float d) { return -pv[41].z / (pv[40].w * d + pv[41].w); }
float2 RayRaw(float2 ndc)
{
    const float4 p = ndc.x * pv[38] + ndc.y * pv[39] + 0.5 * pv[40] + pv[41];
    const float3 v = p.xyz / p.w;
    return v.xy / -v.z;
}
float FastAcos(float x)
{
    const float ax = abs(x);
    float r = (-0.156583 * ax + HALF_PI) * sqrt(saturate(1.0 - ax));
    return x >= 0 ? r : PI - r;
}
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint W, H; zfull.GetDimensions(W, H);
    const int2 px = int2(pos.xy);
    const float L = LinZ(zfull.Load(int3(px, 0)));
    const float hi = trunc(L * 1.00390625);
    const float2 packed = float2(hi, L * 257.0 - hi * 256.0) * (1.0 / 255.0);
    if (!(L > 0.001) || L > 5000.0) return float4(1, 1, packed);
    // view space as XeGTAO has it: x right, y up (the matrix's own signs taken out), z forward = the linear depth
    const float2 r0 = RayRaw(float2(0, 0));
    float2 rx = 0.5 * (RayRaw(float2(1, 0)) - RayRaw(float2(-1, 0)));
    float2 ry = 0.5 * (RayRaw(float2(0, 1)) - RayRaw(float2(0, -1)));
    const float2 sg = float2(rx.x < 0 ? -1 : 1, ry.y < 0 ? -1 : 1);
    const float2 invSize = float2(2.0 / W, 2.0 / H);
    #define VPOS(fpx, z) (float3((r0 + ((fpx).x * invSize.x - 1.0) * rx + (1.0 - (fpx).y * invSize.y) * ry) * sg * (z), (z)))
    const float2 c = float2(px) + 0.5;
    const float3 P = VPOS(c, L);
    // the normal from the nearer neighbour on each axis (no smearing across a depth edge)
    const float3 Pl = VPOS(c + float2(-1, 0), LinZ(zfull.Load(int3(px + int2(-1, 0), 0))));
    const float3 Pr = VPOS(c + float2(1, 0), LinZ(zfull.Load(int3(px + int2(1, 0), 0))));
    const float3 Pu = VPOS(c + float2(0, -1), LinZ(zfull.Load(int3(px + int2(0, -1), 0))));
    const float3 Pd = VPOS(c + float2(0, 1), LinZ(zfull.Load(int3(px + int2(0, 1), 0))));
    const float3 dx = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
    const float3 dy = abs(Pd.z - P.z) < abs(P.z - Pu.z) ? Pd - P : P - Pu;
    float3 N = normalize(cross(dx, dy));
    const float3 V = normalize(-P);
    if (dot(N, V) < 0) N = -N;
    // the radius on screen, in full-resolution pixels
    const float pxPerUnit = (W * 0.5) / max(abs(rx.x), 1e-6);
    const float screenR = min(ga.x * pxPerUnit / L, ga.w);
    if (screenR < 2.0) return float4(1, 1, packed);
    const float falloffRange = ga.y * ga.x, falloffFrom = ga.x - falloffRange;
    const float fMul = -1.0 / falloffRange, fAdd = falloffFrom / falloffRange + 1.0;
    const float n1 = (bayer[(px.x & 3) * 4 + (px.y & 3)] + 0.5) / 16.0;
    const float n2 = (bayer[((px.x + 2) & 3) * 4 + ((px.y + 1) & 3)] + 0.5) / 16.0;
    // v2 (2026-11-08: "the more I turn up the strength the more a moire pattern shows", and the arms / weapons
    // darkening the scenery behind them): the horizon taps read the FULL-resolution depth at the exact pixel they land
    // on, not the half-resolution buffer (a 2x2 reduction whose value is not the block centre's depth: on every slanted
    // plane the taps came out a little in front of or behind it with a 2-pixel period -- a grid of false occlusion the
    // power then multiplied into moire -- and whose content need not match this eye's depth where the moved arms
    // are); three slices instead of two (less of the 4x4 pattern left for the blur at high strength).
    const int SLICES = 3, STEPS = 4;
    float vis = 0;
    [unroll] for (int s = 0; s < SLICES; s++)
    {
        const float phi = (s + n1) * PI / SLICES;
        const float cp = cos(phi), sp = sin(phi);
        const float2 omega = float2(cp, -sp);                 // on screen (y down)
        const float3 dirV = float3(cp, sp, 0);                 // in view space (y up)
        const float3 orthoDir = dirV - dot(dirV, V) * V;
        const float3 axis = normalize(cross(orthoDir, V));
        const float3 projN = N - axis * dot(N, axis);
        const float projNLen = max(length(projN), 1e-5);
        const float signN = sign(dot(orthoDir, projN));
        const float cosN = saturate(dot(projN, V) / projNLen);
        const float n = signN * FastAcos(cosN);
        const float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
        float hc0 = low0, hc1 = low1;
        [unroll] for (int k = 0; k < STEPS; k++)
        {
            float t = (k + n2) / STEPS; t *= t;
            const float2 off = omega * max(t * screenR, 2.0 + 2.0 * k);
            [unroll] for (int side = 0; side < 2; side++)
            {
                const float2 sp2 = clamp(c + (side == 0 ? off : -off), 0.5, float2(W, H) - 0.5);
                const int2 sp = int2(sp2);
                float Ls = LinZ(zfull.Load(int3(sp, 0)));
                if (!(Ls > 0.001)) Ls = 1e6;   // (no depth there: far away, no occlusion)
                const float3 Ps = VPOS(float2(sp) + 0.5, Ls);
                const float3 d = Ps - P;
                const float len = max(length(d), 1e-5);
                // v3 (2026-11-08: "the hands still darken the environment"): something well IN FRONT of this pixel --
                // the arms and what they hold over the floor, any silhouette over what is behind it -- is taken as a
                // thin shell, not a solid reaching back to the surface (XeGTAO's thin-occluder compensation): its
                // depth gap counts THIN+1 times in the falloff, so 0.15 m in front at the default 0.6 m radius no
                // longer occludes, while a crease (a gap of a few cm) still does.
                const float3 dT = float3(d.xy, d.z < 0 ? d.z * (1.0 + THIN) : d.z);
                const float w = saturate(length(dT) * fMul + fAdd);
                float shc = dot(d / len, V);
                if (side == 0) { shc = lerp(low0, shc, w); hc0 = max(hc0, shc); }
                else           { shc = lerp(low1, shc, w); hc1 = max(hc1, shc); }
            }
        }
        float h0 = -FastAcos(hc1), h1 = FastAcos(hc0);
        h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
        h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);
        const float sn = sin(n);
        const float iarc0 = (cosN + 2.0 * h0 * sn - cos(2.0 * h0 - n)) * 0.25;
        const float iarc1 = (cosN + 2.0 * h1 * sn - cos(2.0 * h1 - n)) * 0.25;
        vis += projNLen * (iarc0 + iarc1);
    }
    vis = pow(saturate(vis / SLICES), ga.z);
    return float4(vis, 1, packed);
}
)";

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    std::mutex g_M;
    ID3D11PixelShader* g_Ps = nullptr; ID3D11Buffer* g_Cb = nullptr;
    bool g_Failed = false;
    float g_Cur[4] = { -1, -1, -1, -1 };
    std::atomic<bool> g_Active{ false };
    std::atomic<unsigned long long> g_Draws{ 0 };
    thread_local bool t_InDraw = false;

    bool Compile(ID3D11Device* dev)
    {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { LOGI("AoGtao: d3dcompiler_47 not available: the game's own ambient occlusion"); return false; }
        ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(compile(kHlsl, strlen(kHlsl), "aogtao", nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)) || !b)
        {
            LOGI("AoGtao: the shader failed to compile: {}", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); return false;
        }
        if (err) err->Release();
        const bool ok = SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_Ps)) && g_Ps;
        b->Release();
        if (!ok) { LOGI("AoGtao: CreatePixelShader failed: the game's own ambient occlusion"); return false; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb)) || !g_Cb) { Rel(g_Ps); LOGI("AoGtao: no constant buffer: the game's own ambient occlusion"); return false; }
        LOGI("AoGtao: GTAO ready (2 slices x 4 steps each way per pixel, in the game's AO draw's place)");
        return true;
    }
}

bool AoGtao::Active() { return g_Active.load(std::memory_order_relaxed); }
bool AoGtao::InDraw() { return t_InDraw; }

void AoGtao::OnPresent(ID3D11Device* dev)
{
    const bool want = Settings::AmbientOcclusion() && Settings::AmbientOcclusionHigh();
    if (!want || !dev) { g_Active = false; return; }
    std::lock_guard lk(g_M);
    if (!g_Ps && !g_Failed && !Compile(dev)) g_Failed = true;
    if (!g_Ps || !g_Cb) { g_Active = false; return; }
    const float v[4] = { std::clamp(Settings::AmbientOcclusionRadius(), 0.1f, 3.f), 0.6f, std::clamp(Settings::AmbientOcclusionStrength(), 0.5f, 6.f), 220.f };
    if (memcmp(v, g_Cur, sizeof v))
    {
        ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm);
        if (imm)
        {
            imm->UpdateSubresource(g_Cb, 0, nullptr, v, 0, 0); imm->Release();
            memcpy(g_Cur, v, sizeof v);
            LOGI("AoGtao: radius {:.2f} m, strength {:.2f}", v[0], v[2]);
        }
    }
    g_Active = true;
}

bool AoGtao::Replace(ID3D11DeviceContext* c)
{
    if (!g_Active.load(std::memory_order_relaxed) || t_InDraw || !c) return false;
    ID3D11PixelShader* ps; ID3D11Buffer* cb;
    { std::lock_guard lk(g_M); ps = g_Ps; cb = g_Cb; if (!ps || !cb) return false; ps->AddRef(); cb->AddRef(); }
    ID3D11PixelShader* old = nullptr; c->PSGetShader(&old, nullptr, nullptr);
    ID3D11Buffer* oldCb = nullptr; c->PSGetConstantBuffers(4, 1, &oldCb);
    c->PSSetShader(ps, nullptr, 0);
    c->PSSetConstantBuffers(4, 1, &cb);
    t_InDraw = true;
    c->Draw(3, 0);                                   // (through the mod's own draw hook, which lets it through)
    t_InDraw = false;
    c->PSSetShader(old, nullptr, 0);
    c->PSSetConstantBuffers(4, 1, &oldCb);
    if (old) old->Release();
    if (oldCb) oldCb->Release();
    ps->Release(); cb->Release();
    if (g_Draws.fetch_add(1) == 0) LOGI("AoGtao: the game's AO draw replaced by GTAO (first time)");
    return true;
}

std::string AoGtao::Stats()
{
    return fmt::format("gtao active {} draws {} radius {:.2f} strength {:.2f}", g_Active.load() ? 1 : 0, g_Draws.load(), g_Cur[0], g_Cur[2]);
}
