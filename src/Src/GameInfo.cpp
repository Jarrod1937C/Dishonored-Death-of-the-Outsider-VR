#include "pch.h"
#include "GameInfo.h"
#pragma comment(lib, "version.lib")

namespace
{
    uintptr_t g_Base = 0;
    std::string g_Version = "?";
    std::string g_Mismatch;
    std::string g_ExeName;
    std::wstring g_Dir;
    bool g_Supported = false;
}

void GameInfo::Init()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    g_Base = (uintptr_t)exe;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(exe, path, MAX_PATH);
    g_Dir = path;
    g_Dir = g_Dir.substr(0, g_Dir.find_last_of(L'\\'));
    {
        std::wstring n = path; n = n.substr(n.find_last_of(L'\\') + 1);
        g_ExeName.assign(n.begin(), n.end());
    }

    DWORD dummy, sz = GetFileVersionInfoSizeW(path, &dummy);
    if (sz)
    {
        std::vector<char> buf(sz);
        if (GetFileVersionInfoW(path, 0, sz, buf.data()))
        {
            VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
            if (VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) && ffi)
                g_Version = fmt::format("{}.{}.{}.{}", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                                        HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        }
    }
    uint64_t size = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    // The link time stamp, from the image already in memory (the loader mapped the headers as they are on disk).
    uint32_t stamp = 0;
    {
        const auto* dos = (const IMAGE_DOS_HEADER*)exe;
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const auto* nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)exe + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) stamp = nt->FileHeader.TimeDateStamp;
        }
    }
    const bool nameOk = _stricmp(g_ExeName.c_str(), "Dishonored_DO.exe") == 0;
    if (!nameOk)
        g_Mismatch = fmt::format("this is {}, not Dishonored_DO.exe (Dishonored 2 and other games are not supported; Dishonored 2 has its own VR mod)", g_ExeName);
    else if (g_Version != kSupportedVersion || size != kSupportedExeSize || stamp != kSupportedTimeStamp)
        g_Mismatch = fmt::format("Dishonored_DO.exe is version {} ({} bytes, built {:08X}); the mod supports only the current Steam "
                                 "build ({}, {} bytes, built {:08X}). GOG / Microsoft Store builds and older or newer patches "
                                 "are different executables.", g_Version, size, stamp, kSupportedVersion, kSupportedExeSize, kSupportedTimeStamp);
    g_Supported = g_Mismatch.empty();
    LOGI("{} base=0x{:X} version={} size={} timestamp={:08X} supported={}", g_ExeName, g_Base, g_Version, size, stamp, g_Supported);
    if (!g_Supported) LOGI("UNSUPPORTED GAME BUILD: {}", g_Mismatch);
}

uintptr_t GameInfo::Base() { return g_Base; }
const std::string& GameInfo::Version() { return g_Version; }
bool GameInfo::IsSupportedBuild() { return g_Supported; }
const std::string& GameInfo::Mismatch() { return g_Mismatch; }
const std::string& GameInfo::ExeName() { return g_ExeName; }
const std::wstring& GameInfo::GameDir() { return g_Dir; }
