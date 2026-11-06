#pragma once
#include <string>

struct IDXGISwapChain;

//! What a tester's log needs to say about their machine, so one log is enough to see why something differs there:
//! Windows, CPU/RAM, every GPU, the displays, the keyboard layout, the OpenXR runtimes, other DLLs in the game folder
//! (other mods, ReShade...) and, once the game renders, which GPU it renders on and which overlays hooked it.
namespace SysInfo
{
    void LogStartup();                    //!< InitThread
    void LogFirstPresent(IDXGISwapChain* sc);   //!< first Present (the game's device exists, overlays are loaded)
    std::string ActiveOpenXRRuntime();    //!< the manifest Windows points OpenXR applications to (registry)
}
