#pragma once
#include <cstdint>

//! Stage 3: controller input. The game natively supports XInput pads
//! (imports XINPUT9_1_0.dll). We detour XInputGetState and, while a VR
//! session delivers controller input, synthesize an Xbox pad from it:
//!   left stick -> move, right stick -> look/turn, triggers/grips/buttons -> pad buttons.
namespace XInputHook
{
    bool Install();
    void Uninstall();
    void SetEnabled(bool on);
    bool Enabled();
    //! Snap turn: right-stick X flick applies a discrete yaw impulse to the
    //! game's look instead of continuous turning. 0 = smooth (pass stick through).
    void SetSnapAngle(float deg);
    struct Stats { unsigned long long calls = 0, synthesized = 0; float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; unsigned buttons = 0; };
    Stats GetStats();
}
