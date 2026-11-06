#include "pch.h"
#include "PadProbe.h"
#include "GameInfo.h"
#include <detours/detours.h>
#include <cmath>
#include <intrin.h>

namespace
{
    using Fn = void (*)(uint8_t* dev);
    Fn g_Real = nullptr;
    unsigned long long g_Calls = 0;

    //! MSVC RTTI: vtable[-1] -> RTTICompleteObjectLocator -> TypeDescriptor (image-relative in x64).
    const char* RttiNameRaw(void* obj)
    {
        __try
        {
            uintptr_t* vt = *(uintptr_t**)obj;
            uintptr_t col = vt[-1];
            uint32_t tdRva = *(uint32_t*)(col + 12);          // pTypeDescriptor (RVA)
            return (const char*)(GameInfo::Base() + tdRva + 16);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
    }
    std::string RttiName(void* obj) { const char* n = RttiNameRaw(obj); return std::string(n, strnlen(n, 120)); }

    void Hook(uint8_t* dev)
    {
        g_Real(dev);
        g_Calls++;
        float lx = *(float*)(dev + 0x10), ly = *(float*)(dev + 0x14);
        static int logged = 0;
        if ((g_Calls <= 2 || (fabsf(ly) > 1000.f && logged < 4)) )
        {
            if (fabsf(ly) > 1000.f) logged++;
            LOGI("PadProbe: dev={} class={} connected={} flags=0x{:X} L=({:.0f},{:.0f}) R=({:.0f},{:.0f}) mag={:.0f}/{:.0f} pkt={} btn=0x{:X} trig={},{} tid={}",
                 (void*)dev, RttiName(dev), *(int*)(dev + 0xC), *(uint32_t*)(dev + 0x2C), lx, ly, *(float*)(dev + 0x18), *(float*)(dev + 0x1C),
                 *(float*)(dev + 0x20), *(float*)(dev + 0x24), *(uint32_t*)(dev + 0x34), *(uint16_t*)(dev + 0x38), *(uint8_t*)(dev + 0x3A), *(uint8_t*)(dev + 0x3B), GetCurrentThreadId());
        }
    }
}

namespace
{
    // PostAxisEvent(dev, ctx, queue, table, float x, float y) — only appends when the change bit is set.
    using PostFn = void (*)(uint8_t* dev, void* ctx, uint8_t* queue, const int* table, float x, float y);
    PostFn g_RealPost = nullptr;
    unsigned long long g_PostCalls = 0, g_PostLogged = 0;
    void HkPost(uint8_t* dev, void* ctx, uint8_t* queue, const int* table, float x, float y)
    {
        g_PostCalls++;
        uint32_t before = *(uint32_t*)(queue + 8) & 0xffffff;
        g_RealPost(dev, ctx, queue, table, x, y);
        uint32_t after = *(uint32_t*)(queue + 8) & 0xffffff;
        if ((fabsf(x) > 0.1f || fabsf(y) > 0.1f) && g_PostLogged < 6)
        {
            g_PostLogged++;
            LOGI("PadProbe: PostAxis table={} id={} x={:.2f} y={:.2f} flags=0x{:X} queue count {} -> {} ctx={} queue={}",
                 (void*)table, table[0], x, y, *(uint32_t*)(dev + 0x2C), before, after, ctx, (void*)queue);
        }
    }
    // GenerateEvents(dev, ctx, queue): log the queue after generation (first entries).
    using GenFn = void (*)(uint8_t* dev, void* ctx, uint8_t* queue);
    GenFn g_RealGen = nullptr; unsigned long long g_GenLogged = 0;
    void HkGen(uint8_t* dev, void* ctx, uint8_t* queue)
    {
        g_RealGen(dev, ctx, queue);
        uint32_t count = *(uint32_t*)(queue + 8) & 0xffffff;
        if (count > 0 && g_GenLogged < 6)
        {
            g_GenLogged++;
            std::string s;
            uint8_t* items = *(uint8_t**)queue;
            for (uint32_t i = 0; i < count && i < 6; i++)
            {
                uint32_t* e = (uint32_t*)(items + i * 16);
                s += fmt::format(" [{:08X} {:08X} {:08X} {:08X}]", e[0], e[1], e[2], e[3]);
            }
            LOGI("PadProbe: GenerateEvents queue count={} items:{} caller=exe+0x{:X} dev=exe+0x{:X} ctx=exe+0x{:X}", count, s,
                 (uintptr_t)_ReturnAddress() - GameInfo::Base(), (uintptr_t)dev - GameInfo::Base(), (uintptr_t)ctx - GameInfo::Base());
        }
    }
}

bool PadProbe::Install()
{
    if (!GameInfo::IsSupportedBuild()) return false;
    g_Real = (Fn)(GameInfo::Base() + 0xE3760);
    g_RealPost = (PostFn)(GameInfo::Base() + 0xE3910);
    g_RealGen = (GenFn)(GameInfo::Base() + 0xE3FC0);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    DetourAttach(&(PVOID&)g_RealPost, HkPost);
    DetourAttach(&(PVOID&)g_RealGen, HkGen);
    LONG r = DetourTransactionCommit();
    LOGI("PadProbe: Poll/PostAxis/GenerateEvents detour={}", r);
    return r == NO_ERROR;
}
void PadProbe::Uninstall()
{
    if (!g_Real) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_Real, Hook);
    DetourDetach(&(PVOID&)g_RealPost, HkPost);
    DetourDetach(&(PVOID&)g_RealGen, HkGen);
    DetourTransactionCommit();
}
