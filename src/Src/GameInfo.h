#pragma once
#include <cstdint>
#include <string>

//! Facts about the host executable. Every hook in the mod sits at a fixed offset of ONE build of Dishonored_DO.exe;
//! on any other build those offsets point into unrelated code, so the mod must not touch the game at all there.
namespace GameInfo
{
    // DOTO: the Steam build of Dishonored_DO.exe, file version 1.145.0.0, 42,192,424 bytes, PE link time stamp 0x67AB7505
    // (2025-02-11), SHA256 BE73D979FDB5A6EACBCD385944C6060B97C997D06AB9D72FB39F275A866B955D (measured 2026-11-05). Every
    // address in the mod was ported to this build from Dishonored 2 1.77.9.0 (Dishonored2.exe, 41,134,632 bytes, stamp
    // 0x67A50CDE) by matching the code of the two executables. The version string alone does not identify a build
    // (Steam rebuilt Dishonored 2's exe once without changing it): the link time stamp and the size do.
    constexpr const char* kSupportedVersion = "1.145.0.0";
    constexpr uint64_t kSupportedExeSize = 42192424;
    constexpr uint32_t kSupportedTimeStamp = 0x67AB7505;

    void Init();
    uintptr_t Base();                //!< Dishonored_DO.exe image base
    const std::string& Version();    //!< FileVersion string
    bool IsSupportedBuild();         //!< version + size + link time stamp match
    const std::string& Mismatch();   //!< why not (empty when supported), for the log and the message box
    const std::string& ExeName();    //!< file name of the host executable
    const std::wstring& GameDir();   //!< folder containing the exe
}
