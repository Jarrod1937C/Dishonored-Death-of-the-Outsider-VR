#include "pch.h"
#include "PipeTrace.h"

namespace
{
    struct Ev { int64_t t; uint64_t a, b; uint32_t tid; char k; };
    constexpr int kMax = 4096;
    Ev g_Ev[kMax];
    std::atomic<int> g_N{ 0 };
    std::atomic<int> g_Left{ 0 };
    std::atomic<bool> g_On{ false };
    int64_t g_T0 = 0;
    int64_t Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
}

void PipeTrace::Start(int presents)
{
    g_N = 0; g_T0 = Now(); g_Left = presents > 0 ? presents : 1; g_On = true;
    LOGI("pipetrace: tracing {} presents", g_Left.load());
}
bool PipeTrace::On() { return g_On.load(std::memory_order_relaxed); }
void PipeTrace::Event(Kind k, uint64_t a, uint64_t b)
{
    if (!g_On.load(std::memory_order_relaxed)) return;
    const int i = g_N.fetch_add(1);
    if (i >= kMax) return;
    g_Ev[i] = { Now(), a, b, GetCurrentThreadId(), (char)k };
}
void PipeTrace::OnPresent()
{
    if (!g_On.load()) return;
    if (g_Left.fetch_sub(1) > 1) return;
    g_On = false;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const int n = std::min(g_N.load(), kMax);
    std::string s = fmt::format("pipetrace: {} events (us, thread, kind, a, b)  V=view built (serial, eye)  R=draws took a view (serial)  "
                                "F=command list finished (ctx, list)  X=executed (list)  S=submitted (serial claimed, eye)  P=present (eye var)\n", n);
    for (int i = 0; i < n; i++)
    {
        const Ev& e = g_Ev[i];
        s += fmt::format("  {:9.1f} {:6} {} {} {}\n", (double)(e.t - g_T0) * 1e6 / (double)f.QuadPart, e.tid, e.k, e.a, e.b);
    }
    LOGI("{}", s);
}
