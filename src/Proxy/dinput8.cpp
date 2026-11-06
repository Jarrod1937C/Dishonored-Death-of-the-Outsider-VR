// dinput8.dll proxy: the game loads this from its own folder instead of the
// system one. We forward the single export and load DOTOVR.dll.
#include <windows.h>
#include <objbase.h>

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static HMODULE g_Real = nullptr;
static DirectInput8Create_t g_RealCreate = nullptr;

static void LoadReal()
{
    if (g_Real) return;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\dinput8.dll");
    g_Real = LoadLibraryW(path);
    if (g_Real) g_RealCreate = (DirectInput8Create_t)GetProcAddress(g_Real, "DirectInput8Create");
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    LoadReal();
    if (!g_RealCreate) return E_FAIL;
    return g_RealCreate(hinst, ver, riid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        // Load the mod from the same folder as this proxy.
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(self, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) { slash[1] = 0; lstrcatW(path, L"DOTOVR.dll"); LoadLibraryW(path); }
    }
    return TRUE;
}
