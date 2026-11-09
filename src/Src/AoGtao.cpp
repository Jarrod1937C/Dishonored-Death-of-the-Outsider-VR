#include "pch.h"
#include "AoGtao.h"
#include "Settings.h"
#include "Foveation.h"
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
cbuffer Gtao : register(b4) { float4 ga; float4 gb; }
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
#if !GTAO_FAST
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
#else
#define VPOS(fpx, z) (float3((r0 + ((fpx).x * invSize.x - 1.0) * rx + (1.0 - (fpx).y * invSize.y) * ry) * sg * (z), (z)))
// FAST (AmbientOcclusionQuality = high, 2026-11-08): the same GTAO, about a third of the work.
//  - The 2x2 quad shares its slices: each pixel integrates its own slice direction(s) (the 4x4 pattern gives the four
//    pixels of a quad four different angles), then takes its three quad neighbours' results too (ddx_fine / ddy_fine:
//    plain SM 5, every GPU), each weighted by how close that neighbour lies to this pixel's surface plane -- so four
//    directions per pixel for the price of one, never across a depth edge. The game's 4x4 blur and the TAA follow.
//  - Foveated in the shader (any GPU; FoveatedEffects): gb.x = tan^2 of the angle within which a pixel takes gb.z
//    slices, beyond it gb.w (with FoveatedEffects off gb.x is huge: gb.z everywhere).
// No early return before the exchange (its derivatives need all four pixels of the quad running).
float SliceVis(float3 P, float3 N, float3 V, float2 c, float screenR, float W, float H, float2 r0, float2 rx, float2 ry,
               float2 sg, float2 invSize, float fMul, float fAdd, float n1, float n2, int SL)
{
    float vis = 0;
    [loop] for (int s = 0; s < SL; s++)
    {
        const float phi = (s + n1) * PI / SL;
        const float cp = cos(phi), sp = sin(phi);
        const float2 omega = float2(cp, -sp);
        const float3 dirV = float3(cp, sp, 0);
        const float3 orthoDir = dirV - dot(dirV, V) * V;
        const float3 axis = normalize(cross(orthoDir, V));
        const float3 projN = N - axis * dot(N, axis);
        const float projNLen = max(length(projN), 1e-5);
        const float signN = sign(dot(orthoDir, projN));
        const float cosN = saturate(dot(projN, V) / projNLen);
        const float n = signN * FastAcos(cosN);
        const float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
        float hc0 = low0, hc1 = low1;
        [unroll] for (int k = 0; k < 4; k++)
        {
            float t = (k + n2) / 4.0; t *= t;
            const float2 off = omega * max(t * screenR, 2.0 + 2.0 * k);
            [unroll] for (int side = 0; side < 2; side++)
            {
                const float2 sp2 = clamp(c + (side == 0 ? off : -off), 0.5, float2(W, H) - 0.5);
                const int2 spx = int2(sp2);
                float Ls = LinZ(zfull.Load(int3(spx, 0)));
                if (!(Ls > 0.001)) Ls = 1e6;
                const float3 Ps = VPOS(float2(spx) + 0.5, Ls);
                const float3 d = Ps - P;
                const float len = max(length(d), 1e-5);
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
    return vis;
}
// the same value in this pixel's horizontal / vertical quad neighbour (fine derivatives: within the 2x2 quad)
)"
// (MSVC: one string literal at most ~16 KB -- the shader is in pieces, joined by the compiler)
R"(float QuadX(float v, int2 px) { const float d = ddx_fine(v); return (px.x & 1) ? v - d : v + d; }
float QuadY(float v, int2 px) { const float d = ddy_fine(v); return (px.y & 1) ? v - d : v + d; }
#if GTAO_LITE
// LITE (AmbientOcclusionQuality = low, 2026-11-08: "as cheap as the game's own, without its faults"). The game's DWAO
// costs ~0.11 ms an eye (two taps a pixel) but its radius is a fixed size ON SCREEN -- the occlusion grows as you walk
// away and shrinks as you come close, and differs between the eyes -- and its coarse dither leaves a halo. This is the
// same GTAO with the radius in the WORLD (the same in both eyes, at any distance), one slice of two steps each way
// (one each way beyond FoveatedInner), the normal from the quad's own positions (derivatives: no extra taps; at a
// depth edge the pixel faces the eye instead), and the quad's four slice directions shared (as FAST). ~5 depth taps a
// pixel instead of ~20.
float SliceLite(float3 P, float3 N, float3 V, float2 c, float screenR, float W, float H, float2 r0, float2 rx, float2 ry,
                float2 sg, float2 invSize, float fMul, float fAdd, float n1, float n2, int ST)
{
    const float phi = n1 * PI;
    const float cp = cos(phi), sp = sin(phi);
    const float2 omega = float2(cp, -sp);
    const float3 dirV = float3(cp, sp, 0);
    const float3 orthoDir = dirV - dot(dirV, V) * V;
    const float3 axis = normalize(cross(orthoDir, V));
    const float3 projN = N - axis * dot(N, axis);
    const float projNLen = max(length(projN), 1e-5);
    const float signN = sign(dot(orthoDir, projN));
    const float cosN = saturate(dot(projN, V) / projNLen);
    const float n = signN * FastAcos(cosN);
    const float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
    float hc0 = low0, hc1 = low1;
    [loop] for (int k = 0; k < ST; k++)
    {
        float t = (k + n2) / ST; t *= t;
        const float2 off = omega * max(t * screenR, 2.0 + 2.0 * k);
        [unroll] for (int side = 0; side < 2; side++)
        {
            const float2 sp2 = clamp(c + (side == 0 ? off : -off), 0.5, float2(W, H) - 0.5);
            const int2 spx = int2(sp2);
            float Ls = LinZ(zfull.Load(int3(spx, 0)));
            if (!(Ls > 0.001)) Ls = 1e6;
            const float3 Ps = VPOS(float2(spx) + 0.5, Ls);
            const float3 d = Ps - P;
            const float len = max(length(d), 1e-5);
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
    return projNLen * (iarc0 + iarc1);
}
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint W, H; zfull.GetDimensions(W, H);
    const int2 px = int2(pos.xy);
    const float Lraw = LinZ(zfull.Load(int3(px, 0)));
    const float hi = trunc(Lraw * 1.00390625);
    const float2 packed = float2(hi, Lraw * 257.0 - hi * 256.0) * (1.0 / 255.0);
    const bool valid = Lraw > 0.001 && Lraw <= 5000.0;
    const float L = valid ? Lraw : 1.0;
    const float2 r0 = RayRaw(float2(0, 0));
    float2 rx = 0.5 * (RayRaw(float2(1, 0)) - RayRaw(float2(-1, 0)));
    float2 ry = 0.5 * (RayRaw(float2(0, 1)) - RayRaw(float2(0, -1)));
    const float2 sg = float2(rx.x < 0 ? -1 : 1, ry.y < 0 ? -1 : 1);
    const float2 invSize = float2(2.0 / W, 2.0 / H);
    const float2 c = float2(px) + 0.5;
    const float3 P = VPOS(c, L);
    // the quad's other positions, exchanged (no taps): the normal from them, and the sharing's weights below
    const bool ex = (px.x & 1) == 0, ey = (px.y & 1) == 0;
    const float3 Ph = float3(QuadX(P.x, px), QuadX(P.y, px), QuadX(P.z, px));
    const float3 Pv = float3(QuadY(P.x, px), QuadY(P.y, px), QuadY(P.z, px));
    const float3 Pg = float3(QuadY(Ph.x, px), QuadY(Ph.y, px), QuadY(Ph.z, px));
    const float3 V = normalize(-P);
    const float3 dx = ex ? Ph - P : P - Ph;
    const float3 dy = ey ? Pv - P : P - Pv;
    float3 N = cross(dx, dy);
    const bool edge = abs(Ph.z - P.z) > 0.05 * L || abs(Pv.z - P.z) > 0.05 * L;
    N = (!edge && dot(N, N) > 1e-20) ? normalize(N) : V;
    if (dot(N, V) < 0) N = -N;
    const float pxPerUnit = (W * 0.5) / max(abs(rx.x), 1e-6);
    const float screenR = min(ga.x * pxPerUnit / L, ga.w);
    const bool doAo = valid && screenR >= 2.0;
    const float falloffRange = ga.y * ga.x, falloffFrom = ga.x - falloffRange;
    const float fMul = -1.0 / falloffRange, fAdd = falloffFrom / falloffRange + 1.0;
    const float n1 = (bayer[(px.x & 3) * 4 + (px.y & 3)] + 0.5) / 16.0;
    const float n2 = (bayer[((px.x + 2) & 3) * 4 + ((px.y + 1) & 3)] + 0.5) / 16.0;
    const float2 ndc = float2(c.x * invSize.x - 1.0, 1.0 - c.y * invSize.y);
    const float2 tanP = r0 + ndc.x * rx + ndc.y * ry;
    const int ST = dot(tanP, tanP) < gb.x ? (int)gb.z : (int)gb.w;
    float myV = 1.0, myW = 0.0;
    [branch] if (doAo)
    {
        myV = SliceLite(P, N, V, c, screenR, W, H, r0, rx, ry, sg, invSize, fMul, fAdd, n1, n2, ST);
        myW = 1.0;
    }
    const float tol = 0.02 * L + 0.005;
    const float wh = saturate(2.0 - abs(dot(N, Ph - P)) / tol);
    const float wv = saturate(2.0 - abs(dot(N, Pv - P)) / tol);
    const float wg = saturate(2.0 - abs(dot(N, Pg - P)) / tol);
    const float vw = myV * myW;
    const float vwX = QuadX(vw, px), wX = QuadX(myW, px);
    const float vwY = QuadY(vw, px), wY = QuadY(myW, px);
    const float vwG = QuadY(vwX, px), wG = QuadY(wX, px);
    const float sumW = myW + wh * wX + wv * wY + wg * wG;
    const float sumV = vw + wh * vwX + wv * vwY + wg * vwG;
    float vis = (doAo && sumW > 0) ? sumV / sumW : 1.0;
    vis = pow(saturate(vis), ga.z);
    return float4(doAo ? vis : 1.0, 1, packed);
}
#else
)"
// (MSVC: one string literal at most ~16 KB -- the shader is in pieces, joined by the compiler)
R"(float4 main(float4 pos : SV_Position) : SV_Target
{
    uint W, H; zfull.GetDimensions(W, H);
    const int2 px = int2(pos.xy);
    const float Lraw = LinZ(zfull.Load(int3(px, 0)));
    const float hi = trunc(Lraw * 1.00390625);
    const float2 packed = float2(hi, Lraw * 257.0 - hi * 256.0) * (1.0 / 255.0);
    bool valid = Lraw > 0.001 && Lraw <= 5000.0;
    const float L = valid ? Lraw : 1.0;
    const float2 r0 = RayRaw(float2(0, 0));
    float2 rx = 0.5 * (RayRaw(float2(1, 0)) - RayRaw(float2(-1, 0)));
    float2 ry = 0.5 * (RayRaw(float2(0, 1)) - RayRaw(float2(0, -1)));
    const float2 sg = float2(rx.x < 0 ? -1 : 1, ry.y < 0 ? -1 : 1);
    const float2 invSize = float2(2.0 / W, 2.0 / H);
    const float2 c = float2(px) + 0.5;
    const float3 P = VPOS(c, L);
    float Ll = LinZ(zfull.Load(int3(px + int2(-1, 0), 0))), Lr = LinZ(zfull.Load(int3(px + int2(1, 0), 0)));
    float Lu = LinZ(zfull.Load(int3(px + int2(0, -1), 0))), Ld = LinZ(zfull.Load(int3(px + int2(0, 1), 0)));
    Ll = Ll > 0.001 ? Ll : L; Lr = Lr > 0.001 ? Lr : L; Lu = Lu > 0.001 ? Lu : L; Ld = Ld > 0.001 ? Ld : L;
    const float3 Pl = VPOS(c + float2(-1, 0), Ll), Pr = VPOS(c + float2(1, 0), Lr);
    const float3 Pu = VPOS(c + float2(0, -1), Lu), Pd = VPOS(c + float2(0, 1), Ld);
    const float3 dx = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
    const float3 dy = abs(Pd.z - P.z) < abs(P.z - Pu.z) ? Pd - P : P - Pu;
    float3 N = cross(dx, dy);
    N = dot(N, N) > 1e-20 ? normalize(N) : float3(0, 0, -1);
    const float3 V = normalize(-P);
    if (dot(N, V) < 0) N = -N;
    const float pxPerUnit = (W * 0.5) / max(abs(rx.x), 1e-6);
    const float screenR = min(ga.x * pxPerUnit / L, ga.w);
    const bool doAo = valid && screenR >= 2.0;
    const float falloffRange = ga.y * ga.x, falloffFrom = ga.x - falloffRange;
    const float fMul = -1.0 / falloffRange, fAdd = falloffFrom / falloffRange + 1.0;
    const float n1 = (bayer[(px.x & 3) * 4 + (px.y & 3)] + 0.5) / 16.0;
    const float n2 = (bayer[((px.x + 2) & 3) * 4 + ((px.y + 1) & 3)] + 0.5) / 16.0;
    // the foveation: this pixel's angle from the view's axis (as a tan^2)
    const float2 ndc = float2(c.x * invSize.x - 1.0, 1.0 - c.y * invSize.y);
    const float2 tanP = r0 + ndc.x * rx + ndc.y * ry;
    const int SL = dot(tanP, tanP) < gb.x ? (int)gb.z : (int)gb.w;
    float myV = 1.0, myW = 0.0;
    [branch] if (doAo)
    {
        myV = SliceVis(P, N, V, c, screenR, W, H, r0, rx, ry, sg, invSize, fMul, fAdd, n1, n2, SL) / SL;
        myW = SL;
    }
    // the quad's other three pixels: their results, weighted by their distance from this pixel's plane (2 % of the
    // depth or less: all of it; twice that: nothing) -- so an edge, a silhouette, never mixes
    const bool ex = (px.x & 1) == 0, ey = (px.y & 1) == 0;
    const float3 Ph = ex ? Pr : Pl, Pv = ey ? Pd : Pu;
    const int2 pdg = px + int2(ex ? 1 : -1, ey ? 1 : -1);
    float Lg = LinZ(zfull.Load(int3(pdg, 0))); Lg = Lg > 0.001 ? Lg : 1e6;
    const float3 Pg = VPOS(float2(pdg) + 0.5, Lg);
    const float tol = 0.02 * L + 0.005;
    const float wh = saturate(2.0 - abs(dot(N, Ph - P)) / tol);
    const float wv = saturate(2.0 - abs(dot(N, Pv - P)) / tol);
    const float wg = saturate(2.0 - abs(dot(N, Pg - P)) / tol);
    const float vw = myV * myW;
    const float vwX = QuadX(vw, px), wX = QuadX(myW, px);
    const float vwY = QuadY(vw, px), wY = QuadY(myW, px);
    const float vwG = QuadY(vwX, px), wG = QuadY(wX, px);
    const float sumW = myW + wh * wX + wv * wY + wg * wG;
    const float sumV = vw + wh * vwX + wv * vwY + wg * vwG;
    float vis = (doAo && sumW > 0) ? sumV / sumW : 1.0;
    vis = pow(saturate(vis), ga.z);
    return float4(doAo ? vis : 1.0, 1, packed);
}
#endif
#endif
)";

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    std::mutex g_M;
    // [0] the full GTAO (AmbientOcclusionQuality = ultra: three slices per pixel), [1] the fast one (high: quad-shared
    // slices, foveated in the shader)
    // [2] the lite one (low: one slice, two steps each way, the normal from the quad: about the game's own AO's cost)
    ID3D11PixelShader* g_PsV[3] = {}; ID3D11Buffer* g_Cb = nullptr;
    std::atomic<int> g_Variant{ 1 };
    bool g_Failed = false;
    float g_Cur[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    std::atomic<unsigned long long> g_DrawsV[3] = {};
    const char* kVariantName[3] = { "full (ultra)", "fast (high)", "lite (low)" };
    std::atomic<bool> g_Active{ false };
    std::atomic<unsigned long long> g_Draws{ 0 };
    thread_local bool t_InDraw = false;

    bool Compile(ID3D11Device* dev)
    {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { LOGI("AoGtao: d3dcompiler_47 not available: the game's own ambient occlusion"); return false; }
        for (int v = 0; v < 3; v++)
        {
            const D3D_SHADER_MACRO defs[] = { { "GTAO_FAST", v ? "1" : "0" }, { "GTAO_LITE", v == 2 ? "1" : "0" }, { nullptr, nullptr } };
            ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
            if (FAILED(compile(kHlsl, strlen(kHlsl), "aogtao", defs, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)) || !b)
            {
                LOGI("AoGtao: the {} shader failed to compile: {}", kVariantName[v], err ? (const char*)err->GetBufferPointer() : "?");
                if (err) err->Release();
                continue;
            }
            if (err) err->Release();
            if (FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_PsV[v]))) g_PsV[v] = nullptr;
            b->Release();
        }
        if (!g_PsV[0] && !g_PsV[1] && !g_PsV[2]) { LOGI("AoGtao: CreatePixelShader failed: the game's own ambient occlusion"); return false; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb)) || !g_Cb) { Rel(g_PsV[0]); Rel(g_PsV[1]); Rel(g_PsV[2]); LOGI("AoGtao: no constant buffer: the game's own ambient occlusion"); return false; }
        LOGI("AoGtao: GTAO ready: full (ultra: 3 slices x 4 steps each way per pixel) {}, fast (high: slices shared in the 2x2 quad, foveated in the shader) {}, "
             "lite (low: 1 slice x 2 steps each way, shared, the normal from the quad) {}", g_PsV[0] ? "ok" : "FAILED", g_PsV[1] ? "ok" : "FAILED", g_PsV[2] ? "ok" : "FAILED");
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
    if (!g_PsV[0] && !g_PsV[1] && !g_PsV[2] && !g_Failed && !Compile(dev)) g_Failed = true;
    int variant = Settings::AmbientOcclusionUltra() ? 0 : Settings::AmbientOcclusionLow() ? 2 : 1;
    if (!g_PsV[variant]) variant = g_PsV[1] ? 1 : g_PsV[2] ? 2 : 0;
    if (!g_PsV[variant] || !g_Cb) { g_Active = false; return; }
    if (g_Variant.exchange(variant) != variant) LOGI("AoGtao: {} GTAO", kVariantName[variant]);
    // the fast one's foveation (FoveatedEffects, any GPU): 2 slices within FoveatedInner degrees of the view's axis, 1
    // beyond (each shared with the quad: 8 / 4 directions a pixel); off: 2 everywhere. The lite one: 2 / 1 steps each way
    const Foveation::Params fp = Settings::Foveated();
    const float tIn = tanf(std::clamp(fp.innerDeg, 5.f, 80.f) / 57.29578f);
    const bool fov = Settings::FoveatedEffects();
    const float v[8] = { std::clamp(Settings::AmbientOcclusionRadius(), 0.1f, 3.f), 0.6f, std::clamp(Settings::AmbientOcclusionStrength(), 0.5f, 6.f), 220.f,
                         fov ? tIn * tIn : 1e30f, 0.f, 2.f, fov ? 1.f : 2.f };
    if (memcmp(v, g_Cur, sizeof v))
    {
        ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm);
        if (imm)
        {
            imm->UpdateSubresource(g_Cb, 0, nullptr, v, 0, 0); imm->Release();
            memcpy(g_Cur, v, sizeof v);
            LOGI("AoGtao: radius {:.2f} m, strength {:.2f}; the fast one's foveation {}", v[0], v[2],
                 fov ? fmt::format("on: 2 slices within {:.0f} deg of the view's axis, 1 beyond (FoveatedEffects)", fp.innerDeg) : std::string("off: 2 slices everywhere"));
        }
    }
    g_Active = true;
}

bool AoGtao::Replace(ID3D11DeviceContext* c)
{
    if (!g_Active.load(std::memory_order_relaxed) || t_InDraw || !c) return false;
    ID3D11PixelShader* ps; ID3D11Buffer* cb;
    const int variant = g_Variant.load(std::memory_order_relaxed);
    { std::lock_guard lk(g_M); ps = g_PsV[variant]; cb = g_Cb; if (!ps || !cb) return false; ps->AddRef(); cb->AddRef(); }
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
    g_DrawsV[variant]++;
    if (g_Draws.fetch_add(1) == 0) LOGI("AoGtao: the game's AO draw replaced by GTAO (first time)");
    return true;
}

std::string AoGtao::Stats()
{
    return fmt::format("gtao active {} draws {} (lite {} fast {} full {}) radius {:.2f} strength {:.2f} foveated {}", g_Active.load() ? 1 : 0, g_Draws.load(),
                       g_DrawsV[2].load(), g_DrawsV[1].load(), g_DrawsV[0].load(), g_Cur[0], g_Cur[2], g_Cur[4] < 1e29f ? 1 : 0);
}
