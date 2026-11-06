#pragma once
#include <string>
//! One-shot OpenXR runtime probe (no graphics binding): logs runtime/system
//! names so we know the headset is reachable before stage 1 wires D3D11.
namespace XRProbe
{
    void Run();
    const std::string& Summary();
    //! True once Run has returned. The session must not create its instance before: Meta's runtime allows one
    //! instance per process (the second fails with XR_ERROR_LIMIT_REACHED, measured).
    bool Done();
}
