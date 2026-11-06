#pragma once
#include <cstdint>

//! Research hook, installed switched OFF (Main.cpp): exe+0x8EEFE0 was first taken for the builder of the PLAYER's
//! renderView (via the wrapper at exe+0x8EF6D0) and this hook rewrote the first-person origin/axis around it. Measured
//! later, it builds other entities' views, not the player's; the head pose goes in through ViewMatrixHook instead.
namespace PlayerViewHook
{
    constexpr uint32_t kBuildRenderView = 0x8EEFE0;
    bool Install();
    void Uninstall();
    void SetEnabled(bool on);
    bool Enabled();
    struct Stats { unsigned long long calls = 0, applied = 0; float lastOrg[3] = {}, lastFwd[3] = {}; };
    Stats GetStats();
}
