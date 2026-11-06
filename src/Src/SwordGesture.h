#pragma once
#include <string>
struct XRInputState;

//! The sword by GESTURE: swing the right controller like a sword and it strikes; hold it by the right ear with the
//! blade forward and it charges the power attack, released by a thrust; bring the fist to the left shoulder and it
//! blocks. The trigger and the grip keep doing all of that too (the user's choice, 2026-09-30).
//!
//! Everything is measured in the XR LOCAL space, where the head and both controllers live, from the controller's aim
//! pose: the blade stands out of the fist along the controller's +Y (the sword's grip is -90 deg of pitch in GpuTrace,
//! validated on the Quest 3), so the tip is pose + up * bladeLen.
//!  - SLASH: the tip moving ACROSS the blade faster than `slashTip` while the hand itself moves faster than
//!    `slashHand`, for `slashFrames` samples. The hand term is what rejects a wrist flick (the tip is fast, the hand
//!    barely moves -- Skyrim VR's "waggle"); the across-the-blade term rejects a thrust or pulling the sword back.
//!    The tip must also be closing on where the head looks (`frontGate`): a wind-up takes it away, and a fast
//!    wind-up that struck used to eat the real swing in the cooldown. A new slash needs the hand to have slowed below
//!    `rearmHand` since the last one AND `cooldown` to have passed (the game drops a click made during its own
//!    Slash + Recoil, ~0.33 s: measured).
//!  - DIRECTION: the game asks for it ~0.2 s after the click, when the arc is over. So it is frozen at the swing's
//!    PEAK speed as head -> tip (where the blade physically was at its fastest: in a horizontal slash that is in
//!    front, not towards the side the motion goes) and handed to MeleeAim for `aimHold` seconds.
//!  - POWER ATTACK: the game's own is "hold the attack button, release". Holding the fist by the right ear (a box in
//!    the head's yaw frame) with the blade within `chargeCone` of forward, still, for `chargeDwell` seconds presses
//!    and HOLDS the attack; any fast motion (a thrust, above `chargeRelease`) releases it, aimed the same way. The
//!    game's charge is full after ~0.5 s (a 0.45 s hold still strikes as a plain slash, 0.55 s as the charged one).
//!    Lowering the blade out of the pose CANCELS the charge: a block pulse with the attack still held (the game drops
//!    a charge when block comes while it is held), then both let go -- a plain release would be the power attack.
//!  - BLOCK: the fist within `blockR` of the left shoulder (estimated from the head), instantly -- a parry has no time
//!    to spare. Released beyond `blockExit`, the moment a slash fires, and never within `blockAfterSlash` of one.
//!    Not while crouched (`blockCrouch`): the same key chokes a guard from behind. A strike asked for while block is
//!    down waits for it to be up 60 ms (KbmInject), because a click on block's last tick is lost (measured).
//! Haptics: a tick when a slash counts, a soft pulse when the charge starts and a firmer one when it is full, a
//! short one on block.
namespace SwordGesture
{
    struct Out { bool tap = false; bool hold = false; bool block = false; };
    //! One tick, from KbmInject::Tick in PLAY mode (render thread). triggerHeld / gripHeld: the right trigger and grip
    //! as the player is pressing them (they keep working; the gestures only add to them).
    Out Tick(const XRInputState& in, bool triggerHeld, bool gripHeld);
    //! Not playing (menu, wheel, takeover): forget the motion and drop a charge without striking.
    void Reset();
    bool StandsDown();               //!< a state that is not a sword fight (carrying, climbing, a takedown, the wheel...)
    bool Charging();                 //!< the power attack is being charged (or its thrust is in flight)
    //! SwordContact: the blade touched something during a charged thrust -- release the attack now.
    void ReleaseCharge();
    //! Bit 1 slash, 2 power attack, 4 block. Default 7.
    void SetMask(int mask);
    int  Mask();
    //! Tunables by name (vr swingcfg <name> <value>); false for an unknown name.
    bool Set(const std::string& key, float v);
    std::string Config();
    std::string Stats();             //!< heartbeat
    //! Research: one log line per pose sample for the next n samples (speeds, zones).
    void LogSamples(int n);
}
