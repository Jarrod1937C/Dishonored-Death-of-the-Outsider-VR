#pragma once
#include <string>

//! The hand's POINTER: one ray per hand per frame, in world space, that everything aimed with a hand at the WORLD
//! agrees on -- the reticle draws it and the interaction sensor picks along it (GrabHand).
//!
//! Two things make it different from the controller pose the weapons are placed with:
//! - It is anchored to the world, not to the game camera. With head aim the camera's yaw is driven to the head's
//!   by a mouse servo, so it trails the head by the servo's error; a hand expressed relative to that camera turns
//!   with the error. Measured in the mock (head swaying +-6 deg, hand perfectly still): the old ray moved 2.7 deg,
//!   up to 1.5 deg in one frame. The pointer uses where the servo is DRIVING the camera (KbmInject::WorldYawOffset),
//!   which only changes when the player turns with the stick.
//! - It is filtered (One Euro: a low cut-off while the hand is still, opening up with its speed), so hand tremor
//!   does not shake a ray that reaches metres out, and a deliberate move is followed without lag.
//! Its origin is the controller where the HAND is drawn (the rendered view, head lean included).
namespace PointerRay
{
    //! Per frame (Present, the game's main thread): samples both controllers and updates the filters.
    void OnPresent();
    //! This frame's pointer for `hand` (0 left, 1 right): origin and unit forward, world space. Any thread.
    bool Get(int hand, float org[3], float fwd[3]);
    //! 0 = the controller pose as before (camera-anchored, unfiltered; for comparison), 1 = world-anchored,
    //! 2 = world-anchored and filtered (default).
    void SetMode(int m);
    int Mode();
    bool Set(const std::string& key, float v);   //!< mincutoff, beta, dcutoff, poscutoff, posbeta
    std::string Config();
    std::string Stats();
    //! Research: logs the next `frames` frames of the right hand's ray as each consumer sees it, with the head and
    //! the game camera next to it (`vr raylog n`).
    void Log(int frames);
    //! Research: sweeps the right hand's pointer over a grid of directions -- yaw `yawFrom..yawTo` relative to the
    //! camera, pitch `pitchFrom..pitchTo` (world), `step` degrees -- from where the hand is now, holding each for
    //! `settle` frames, and logs what the interaction sensor picked at each (`vr pickscan`). The pick map.
    void Scan(float yawFrom, float yawTo, float pitchFrom, float pitchTo, float step, int settle);
}
