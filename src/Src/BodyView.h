#pragma once
#include <string>
#include <sstream>

//! "Does what you see of yourself match where your body really is?" -- the instrument.
//!
//! `vr bodyview <tag>` writes <game>\DVR_bodyview_<tag>.txt: the render view, the projection the player's own draws
//! used, the eye and controller poses, and every joint of both arms twice -- where the IK put the DRAWN arm, and where
//! a REAL arm with the configured anatomy would be -- all in the render view's frame (x right, y up, z forward, m).
//! scripts\testing\body\seen.py lays them over the mock compositor's picture of what each eye sees
//! (DVR_seen_<tag>_eye<i>.bmp): a drawn joint whose marker does not sit on the drawn arm means the IMAGE is placed
//! wrong in the eye; a drawn marker far from the real one means the ARM is.
//! `vr bodycfg <name> <value>` sets the anatomy (no args: list).
namespace BodyView
{
    bool Command(const std::string& what, std::istringstream& ss);
    void Dump(const std::string& tag);
    std::string Config();
    //! The render fov that covers both eyes of the headset (CameraHook::SetFovOverride), kept on as a mode: applied
    //! by OnPresent as soon as the eyes are known (false = not yet) and again if they change. FovAutoOff ends it.
    bool FovAuto(float marginDeg = 0.f);
    void FovAutoOff();
    bool FovAutoPending();   //!< on, but the headset has not given its fov yet
    void OnPresent();
}
