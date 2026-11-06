#include "pch.h"
#include "XInputHook.h"
#include "VRRuntime.h"
#include "XRSession.h"
#include "Watchpoint.h"
#include <Xinput.h>
#include <detours/detours.h>
#include <cmath>

namespace
{
    using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    GetState_t g_RealGetState = nullptr;
    std::atomic<bool> g_Enabled{ true };
    std::atomic<float> g_SnapDeg{ 45.f };
    std::mutex g_M; XInputHook::Stats g_Stats;
    DWORD g_Packet = 1;
    // snap-turn state machine
    bool g_SnapArmed = true; LARGE_INTEGER g_SnapT{}; int g_SnapDir = 0;

    // The engine (arkXInputControllerState::Poll, exe+0xE3760) posts axis events only when
    // the raw value CHANGED since the previous poll (a real stick always jitters). A perfectly
    // constant synthetic value yields a single event and no motion, so we dither the LSB
    // of every non-zero axis/trigger on alternate polls.
    inline SHORT Axis(float v) { v = v < -1 ? -1 : (v > 1 ? 1 : v); int s = (int)(v * 32000.f); if (s != 0 && (g_Packet & 1)) s += (s > 0 ? -1 : 1); return (SHORT)s; }
    inline BYTE Trig(float v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); int b = (int)(v * 254.f); if (b != 0 && (g_Packet & 1)) b += 1; return (BYTE)b; }

    DWORD WINAPI HkGetState(DWORD user, XINPUT_STATE* st)
    {
        { std::lock_guard lk(g_M); g_Stats.calls++; }
        if (user == 0) Watchpoint::SetGameThreadId(GetCurrentThreadId());   // the pad is polled by the game thread
        if (!g_Enabled.load() || user != 0)
            return g_RealGetState(user, st);

        // Always report a connected pad on slot 0 while VR input is enabled: the game stops
        // polling slots that answer ERROR_DEVICE_NOT_CONNECTED (observed: 4 calls, then silence).
        ZeroMemory(st, sizeof *st);
        st->dwPacketNumber = ++g_Packet;
        XRInputState in;
        if (!VRRuntime::GetInput(in)) return ERROR_SUCCESS;   // connected, idle
        XINPUT_GAMEPAD& g = st->Gamepad;
        g.sThumbLX = Axis(in.moveX); g.sThumbLY = Axis(in.moveY);
        g.bLeftTrigger = Trig(in.triggerL); g.bRightTrigger = Trig(in.triggerR);

        // Right stick: smooth look (pass-through) or snap turn (a flick produces a short, strong pulse).
        float snap = g_SnapDeg.load();
        if (snap <= 0.f) { g.sThumbRX = Axis(in.turnX); }
        else
        {
            LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
            const float dead = 0.6f;
            if (g_SnapDir == 0)
            {
                if (g_SnapArmed && fabsf(in.turnX) > dead) { g_SnapDir = in.turnX > 0 ? 1 : -1; g_SnapT = now; g_SnapArmed = false; }
                else if (fabsf(in.turnX) < 0.3f) g_SnapArmed = true;
            }
            if (g_SnapDir != 0)
            {
                // Pulse length tuned to ~snap degrees at the game's default pad sensitivity; refined per user.
                double dt = double(now.QuadPart - g_SnapT.QuadPart) / f.QuadPart;
                if (dt < 0.0028 * snap) g.sThumbRX = Axis((float)g_SnapDir);
                else g_SnapDir = 0;
            }
        }
        g.sThumbRY = Axis(in.turnY);

        WORD b = 0;
        if (in.buttons & kXRBtnA) b |= XINPUT_GAMEPAD_A;
        if (in.buttons & kXRBtnB) b |= XINPUT_GAMEPAD_B;
        if (in.buttons & kXRBtnX) b |= XINPUT_GAMEPAD_X;
        if (in.buttons & kXRBtnY) b |= XINPUT_GAMEPAD_Y;
        if (in.buttons & kXRBtnThumbL) b |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (in.buttons & kXRBtnThumbR) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (in.buttons & kXRBtnMenu) b |= XINPUT_GAMEPAD_START;
        if (in.gripL > 0.5f) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (in.gripR > 0.5f) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        g.wButtons = b;

        { static int logged = 0; if (logged < 3 && (fabsf(in.moveY) > 0.5f || in.triggerR > 0.5f)) { logged++; LOGI("XInputHook: synthesized LY={:.2f} RT={} tid={}", in.moveY, g.bRightTrigger, GetCurrentThreadId()); } }
        { std::lock_guard lk(g_M); g_Stats.synthesized++; g_Stats.lx = in.moveX; g_Stats.ly = in.moveY; g_Stats.rx = in.turnX; g_Stats.ry = in.turnY;
          g_Stats.lt = in.triggerL; g_Stats.rt = in.triggerR; g_Stats.buttons = b; }
        return ERROR_SUCCESS;
    }
}

namespace
{
    //! Patches the exe's import-address-table slot for XINPUT9_1_0!XInputGetState so the
    //! game's own calls reach us even when Steam's overlay has redirected the import.
    void* PatchIAT(const char* dllName, const char* funcName, void* replacement)
    {
        uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
        for (; imp->Name; imp++)
        {
            const char* name = (const char*)(base + imp->Name);
            if (_stricmp(name, dllName) != 0) continue;
            auto* thunkOrig = (IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
            auto* thunk = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
            for (; thunkOrig->u1.AddressOfData; thunkOrig++, thunk++)
            {
                if (IMAGE_SNAP_BY_ORDINAL64(thunkOrig->u1.Ordinal)) continue;
                auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + thunkOrig->u1.AddressOfData);
                if (strcmp(ibn->Name, funcName) != 0) continue;
                void* old = (void*)thunk->u1.Function;
                DWORD prot; VirtualProtect(&thunk->u1.Function, 8, PAGE_READWRITE, &prot);
                thunk->u1.Function = (ULONGLONG)replacement;
                VirtualProtect(&thunk->u1.Function, 8, prot, &prot);
                HMODULE owner = nullptr; wchar_t mod[MAX_PATH] = L"?";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)old, &owner))
                    GetModuleFileNameW(owner, mod, MAX_PATH);
                std::wstring wm(mod); std::string sm(wm.begin(), wm.end());
                LOGI("XInputHook: IAT {}!{} was {} (in {}), now hooked", dllName, funcName, old, sm);
                return old;
            }
        }
        LOGI("XInputHook: IAT entry {}!{} not found", dllName, funcName);
        return nullptr;
    }
}

bool XInputHook::Install()
{
    HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll");
    if (!m) m = LoadLibraryW(L"XINPUT9_1_0.dll");
    if (!m) { LOGI("XInputHook: xinput9_1_0.dll not found"); return false; }
    g_RealGetState = (GetState_t)GetProcAddress(m, "XInputGetState");
    if (!g_RealGetState) { LOGI("XInputHook: XInputGetState not found"); return false; }
    // IAT patch (what the game itself calls) — the original slot value is what we forward to.
    void* iatOld = PatchIAT("XINPUT9_1_0.dll", "XInputGetState", (void*)HkGetState);
    if (iatOld && iatOld != (void*)g_RealGetState)
    {
        // Someone (Steam overlay) already redirected the import; forward to *their* function so
        // real pads keep working when VR input is off, and skip the body detour.
        g_RealGetState = (GetState_t)iatOld;
        LOGI("XInputHook: import was pre-hooked; forwarding to it");
        return true;
    }
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealGetState, HkGetState);
    LONG r = DetourTransactionCommit();
    LOGI("XInputHook: XInputGetState@{} detour={}", (void*)g_RealGetState, r);
    return r == NO_ERROR;
}
void XInputHook::Uninstall()
{
    if (!g_RealGetState) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_RealGetState, HkGetState); DetourTransactionCommit();
}
void XInputHook::SetEnabled(bool on) { g_Enabled = on; }
bool XInputHook::Enabled() { return g_Enabled; }
void XInputHook::SetSnapAngle(float d) { g_SnapDeg = d; }
XInputHook::Stats XInputHook::GetStats() { std::lock_guard lk(g_M); return g_Stats; }
