#pragma once
#include <string>

//! Meta's ASW (Asynchronous SpaceWarp) for this game. When the game misses the headset's refresh (synchronised stereo
//! runs ~half the mono rate), Meta's runtime invents the missing frames from the last two pictures, and the edges of
//! whatever moves (the hands, the world's silhouettes on a fast head turn) come out warped. Without it the runtime
//! re-shows the last picture turned with the head: clean, the hands at the game's own rate (the user's Quest 3 test,
//! 2026-10-24: "ahora va perfecto"). OpenXR has no switch for it, so this asks Meta's own diagnostics tool
//! (OculusDebugToolCLI, installed with the Meta Horizon PC app) to turn it off for the app in VR focus only
//! (`asw.OverrideFocusedApp off`): other apps keep their setting. Only on Meta's runtime; anywhere else, or without
//! the tool, nothing happens. The tool runs on its own thread with a time limit: it never holds the game.
namespace MetaAsw
{
    void SetMode(int mode);                        //!< `vr metaasw`: 1 = off for this game (default), 0 = leave it to Meta
    int Mode();
    void OnFocused(const std::string& runtimeName); //!< the OpenXR session reached FOCUSED (frame thread)
    std::string Status();                           //!< one line for the heartbeat
}
