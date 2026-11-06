#include "pch.h"
#include "PlayerViewHook.h"
#include "GameInfo.h"
#include "Game.h"
#include "VRRuntime.h"
#include "CameraHook.h"
#include <detours/detours.h>
#include <cmath>

namespace
{
    using Fn = void* (*)(void*, void*, void*, void*);
    Fn g_Real = nullptr;
    std::atomic<bool> g_Enabled{ true };
    std::mutex g_M;
    PlayerViewHook::Stats g_Stats;
    float g_RefPos[3] = {}; bool g_RefValid = false;

    inline void XrToId(const float v[3], float out[3]) { out[0] = -v[2]; out[1] = -v[0]; out[2] = v[1]; }
    void QuatBasis(const float q[4], float fwd[3], float right[3], float up[3])
    {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - z * w), m02 = 2 * (x * z + y * w);
        float m10 = 2 * (x * y + z * w), m11 = 1 - 2 * (x * x + z * z), m12 = 2 * (y * z - x * w);
        float m20 = 2 * (x * z - y * w), m21 = 2 * (y * z + x * w), m22 = 1 - 2 * (x * x + y * y);
        right[0] = m00; right[1] = m10; right[2] = m20;
        up[0] = m01;    up[1] = m11;    up[2] = m21;
        fwd[0] = -m02;  fwd[1] = -m12;  fwd[2] = -m22;
    }

    //! Composes game (body) yaw with the HMD pose. In/out: org (3), axis (9).
    void Compose(const VRRuntime::HeadPose& hp, float org[3], float axis[9])
    {
        if (!g_RefValid) { memcpy(g_RefPos, hp.pos, 12); g_RefValid = true; }
        float yaw = atan2f(axis[1], axis[0]);
        float cy = cosf(yaw), sy = sinf(yaw);
        auto toWorld = [&](const float l[3], float out[3]) {
            out[0] = l[0] * cy - l[1] * sy; out[1] = l[0] * sy + l[1] * cy; out[2] = l[2]; };
        float fx[3], rx[3], ux[3]; QuatBasis(hp.quat, fx, rx, ux);
        float fl[3], rl[3], ul[3]; XrToId(fx, fl); XrToId(rx, rl); XrToId(ux, ul);
        float fw[3], rw[3], uw[3]; toWorld(fl, fw); toWorld(rl, rw); toWorld(ul, uw);
        axis[0] = fw[0]; axis[1] = fw[1]; axis[2] = fw[2];
        axis[3] = -rw[0]; axis[4] = -rw[1]; axis[5] = -rw[2];
        axis[6] = uw[0]; axis[7] = uw[1]; axis[8] = uw[2];
        float dxr[3] = { hp.pos[0] - g_RefPos[0], hp.pos[1] - g_RefPos[1], hp.pos[2] - g_RefPos[2] };
        float dl[3]; XrToId(dxr, dl); float dw[3]; toWorld(dl, dw);
        org[0] += dw[0]; org[1] += dw[1]; org[2] += dw[2];
    }

    void DumpPlayer(uint8_t* p)
    {
        std::string s;
        for (uint32_t off = 0x300; off < 0x440; off += 4)
        {
            float f = *(float*)(p + off);
            if (off % 0x40 == 0) s += fmt::format("\n    +{:03X}:", off);
            s += fmt::format(" {:8.3f}", f);
        }
        LOGI("PlayerView dump (floats +0x300..+0x43C):{}", s);
    }

    void* Hook(void* player, void* b, void* c, void* d)
    {
        uint8_t* p = (uint8_t*)player;
        unsigned long long n;
        { std::lock_guard lk(g_M); n = ++g_Stats.calls; }
        // This builder runs for many entities (NPC/camera views); only touch the local player.
        const bool isLocal = (player == Game::LocalPlayer());
        if (isLocal && (n <= 2 || n % 3000 == 0)) DumpPlayer(p);

        float saved[12]; memcpy(saved, p + Game::kPlayerViewOrigin, 48);
        bool applied = false;
        VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
        if (isLocal && g_Enabled.load() && hp.valid)
        {
            float org[3], axis[9];
            memcpy(org, saved, 12); memcpy(axis, saved + 3, 36);
            Compose(hp, org, axis);
            memcpy(p + Game::kPlayerViewOrigin, org, 12);
            memcpy(p + Game::kPlayerViewAxis, axis, 36);
            applied = true;
            std::lock_guard lk(g_M); g_Stats.applied++; memcpy(g_Stats.lastOrg, org, 12); memcpy(g_Stats.lastFwd, axis, 12);
        }
        void* r = g_Real(player, b, c, d);
        if (applied) memcpy(p + Game::kPlayerViewOrigin, saved, 48);   // give the game its own view back
        return r;
    }
}

bool PlayerViewHook::Install()
{
    if (!GameInfo::IsSupportedBuild()) return false;
    g_Real = (Fn)(GameInfo::Base() + kBuildRenderView);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    LONG r = DetourTransactionCommit();
    LOGI("PlayerViewHook: BuildRenderView@exe+0x{:X} detour={}", kBuildRenderView, r);
    return r == NO_ERROR;
}
void PlayerViewHook::Uninstall()
{
    if (!g_Real) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_Real, Hook); DetourTransactionCommit();
}
void PlayerViewHook::SetEnabled(bool on) { g_Enabled = on; }
bool PlayerViewHook::Enabled() { return g_Enabled; }
PlayerViewHook::Stats PlayerViewHook::GetStats() { std::lock_guard lk(g_M); return g_Stats; }
