#include "pch.h"
#include "SimStep.h"
#include "GameInfo.h"
#include "VRRuntime.h"
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <detours/detours.h>

namespace
{
    // DOTO (Dishonored_DO.exe, mapped from Dishonored 2 and checked byte for byte 2026-11-05): the clock is exe+0x5A790
    // (Dishonored 2 exe+0x59990), the same code; only its addresses moved.
    constexpr uintptr_t kRateInsn = 0x5AC0D;       // movss xmm10, [rip+disp] -> the 120.0 at kRateConst (D2 exe+0x59E0D)
    constexpr uint8_t kRateOp[5] = { 0xF3, 0x44, 0x0F, 0x10, 0x15 };
    constexpr uintptr_t kRateConst = 0x1BCFAF8;     // 120.0f (read-only, also read elsewhere) (D2 exe+0x1AC9C28)
    constexpr uintptr_t kStepVar = 0x2341A94;       // 1/120 s (writable, read only by the clock, exe+0x5AD55) (D2 exe+0x2280A54)
    constexpr float kGameHz = 120.f;

    std::atomic<bool> g_On{ true };
    bool g_Tried = false, g_Ok = false;
    float* g_Rate = nullptr;                        // the mod's own rate, within reach of the instruction
    std::atomic<float> g_Cur{ kGameHz };
    std::atomic<unsigned> g_Changes{ 0 };

    // ---- the frame lock: exactly one step per headset frame ----
    // The clock (exe+0x5A790, this = the clock; called once per SIMULATED frame, from exe+0x91E1ED inside RunFrame's normal
    // path, so not on synchronised stereo's held frames) takes n = floor(lag * rate + 0.5) steps, lag = the game time not yet
    // stepped. With the step at the refresh and a frame every refresh, lag * rate hovers at 1 + r, r the leftover, which keeps
    // whatever value it had (a random walk of the frames' timing). The clock is read where RunFrame happens to start, and that
    // moves by a few ms from frame to frame (PaceLog 2026-11-08: the left image 1.8 - 6.4 ms after the submit, submits 9 - 12 ms
    // apart): with r near +-0.5 the count flips 0, 2, 0, 2 -- frames 11.1 ms apart showing 0 and 22.2 ms of motion, the judder
    // of a sideways move "despite 90 fps" (reported 2026-11-08), coming and going as r drifts. The lock feeds the clock its
    // time quantised: a frame whose real lag is 0.15 - 1.85 steps gets exactly one step's time (so the leftover never moves),
    // anything else the nearest whole number (a dropped frame catches up at once; a long run of frames that are early or late
    // corrects once, by one step, when the difference reaches 0.85 of a step). The clock's "now" is QPC seconds + its offset
    // (+0x1A8; it rewrites that itself when it resyncs after a pause, branch +0x1A3): the lock writes the offset each tick
    // and keeps the game's own to put back. It runs only while the ticks come one per refresh (their average within -20% /
    // +25%): throttled to half rate, or the game's own pacing, the clock is left alone.
    constexpr uintptr_t kClock = 0x5A790, kQpcNow = 0x15B390, kQpcFreq = 0x15B380;   // the game's QPC (since its start), its frequency
    constexpr size_t kNow = 0x18, kSteps = 0x198, kSkipOnce = 0x1A1, kResync = 0x1A3, kOffset = 0x1A8;
    using ClockFn = bool (*)(void*);
    using I64Fn = long long (*)();
    ClockFn g_ClockReal = nullptr;
    void* g_LockClk = nullptr; double g_LockOff = 0, g_LockWrote = 0; bool g_LockHave = false;   // the game thread only
    double g_CallT = 0; std::atomic<double> g_CallAvg{ 0 };
    std::atomic<unsigned long long> g_LockTicks{ 0 }, g_LockFix{ 0 }, g_LockPass{ 0 }, g_StepHist[4]{};
    // The lock's safety net (2026-11-08: a load into the level and the game never started -- black, controls dead: the
    // 30 s line read "steps 0:2700 1:0 | locked 2700 (of them not one step: 2700)", the lock feeding the clock NO time
    // every frame, so the world, the player and the camera stood still). However that comes about, no run of frames gets
    // zero steps from the lock: after 6 in a row it lets go for 3 s WITHOUT putting its offset back (the time then flows
    // on from where it stands) and logs the numbers.
    int g_ZeroRun = 0; double g_SuspendUntil = 0;
    std::atomic<unsigned long long> g_LockSuspends{ 0 };
    bool HkClock(void* clk)
    {
        uint8_t* c = (uint8_t*)clk;
        const uintptr_t b = GameInfo::Base();
        const long long fq = ((I64Fn)(b + kQpcFreq))();
        const double q = fq > 0 ? (double)((I64Fn)(b + kQpcNow))() / (double)fq : 0.0;
        if (g_CallT > 0 && q > g_CallT && q - g_CallT < 0.1)
        {
            const double a = g_CallAvg.load(), d = q - g_CallT;
            g_CallAvg = a > 0 ? a * 0.95 + d * 0.05 : d;
        }
        g_CallT = q;
        const float hz = g_Cur.load();
        const double step = 1.0 / hz, avg = g_CallAvg.load();
        double& off = *(double*)(c + kOffset);
        if (clk != g_LockClk) { g_LockClk = clk; g_LockHave = false; }
        if (!g_LockHave || off != g_LockWrote) g_LockOff = off;     // the game's own offset (it set a new one: a resync)
        const bool lock = fq > 0 && g_On.load() && hz != kGameHz && avg > step * 0.8 && avg < step * 1.25 && c[kSkipOnce] != 1 && c[kResync] == 0
                          && q >= g_SuspendUntil;
        bool suspended = false;
        if (lock)
        {
            const double prev = *(const double*)(c + kNow);
            const double f = (q + g_LockOff - prev) / step;
            const int n = f > 0.15 && f < 1.85 ? 1 : f <= 0.15 ? 0 : (int)floor(f + 0.5);
            if (n == 0 && ++g_ZeroRun >= 6)
            {   // (see g_ZeroRun) the offset is left as it is: this frame and the next 3 s run on the game's clock
                const unsigned long long k = ++g_LockSuspends;
                if (k <= 20) LOGI("SimStep: the frame lock gave 6 frames in a row no step: let go for 3 s (#{}; lag {:.3f} steps, now {:.4f}, qpc {:.4f}, "
                                  "the game's offset {:.4f}, the one written {:.4f}, now {:.4f}; skip-once {} resync {})",
                                  k, f, prev, q, g_LockOff, g_LockWrote, off, (int)c[kSkipOnce], (int)c[kResync]);
                g_ZeroRun = 0; g_SuspendUntil = q + 3.0; g_LockHave = false; suspended = true;
            }
            else
            {
                if (n != 0) g_ZeroRun = 0;
                if (n != 1) g_LockFix++;
                off = prev + n * step - q;
                g_LockWrote = off; g_LockHave = true; g_LockTicks++;
            }
        }
        if (suspended) g_LockPass++;
        else if (!lock)
        {
            if (g_LockHave) { off = g_LockOff; g_LockHave = false; }   // the game's own time back
            g_LockPass++;
        }
        const bool r = g_ClockReal(clk);
        const int s = *(const int*)(c + kSteps);
        g_StepHist[s < 0 ? 0 : s > 3 ? 3 : s]++;
        return r;
    }

    bool Rd(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    //! a page within a rel32 of `target` (below the image first, then above it)
    void* AllocNear(uintptr_t target)
    {
        SYSTEM_INFO si; GetSystemInfo(&si);
        const uintptr_t g = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
        const uintptr_t lim = 0x70000000ull;
        for (uintptr_t a = (target - 0x1000000) & ~(g - 1); a > g && target - a < lim; a -= g)
            if (void* p = VirtualAlloc((void*)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
        for (uintptr_t a = (target + 0x8000000) & ~(g - 1); a - target < lim; a += g)
            if (void* p = VirtualAlloc((void*)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
        return nullptr;
    }
    bool Patch()
    {
        if (g_Tried) return g_Ok;
        g_Tried = true;
        const uintptr_t b = GameInfo::Base();
        uint8_t* insn = (uint8_t*)(b + kRateInsn);
        uint8_t op[5] = {}; int32_t disp = 0; float rate = 0.f, step = 0.f;
        if (!Rd(op, insn, 5) || memcmp(op, kRateOp, 5) != 0 || !Rd(&disp, insn + 5, 4) || (uintptr_t)(insn + 9 + disp) != b + kRateConst ||
            !Rd(&rate, (const void*)(b + kRateConst), 4) || rate != kGameHz || !Rd(&step, (const void*)(b + kStepVar), 4) || fabsf(step - 1.f / kGameHz) > 1e-6f)
        {
            LOGI("SimStep: the game clock's step is not as expected (rate {:g}, step {:g}): left at the game's 1/120 s", rate, step);
            return false;
        }
        g_Rate = (float*)AllocNear((uintptr_t)insn);
        if (!g_Rate) { LOGI("SimStep: no memory within reach of the game's clock: left at 1/120 s"); return false; }
        *g_Rate = kGameHz;
        const long long nd = (long long)(uintptr_t)g_Rate - (long long)(uintptr_t)(insn + 9);
        if (nd < INT32_MIN || nd > INT32_MAX) { LOGI("SimStep: the mod's rate is out of the instruction's reach: left at 1/120 s"); return false; }
        DWORD old = 0;
        if (!VirtualProtect(insn + 5, 4, PAGE_EXECUTE_READWRITE, &old)) { LOGI("SimStep: could not patch the game's clock"); return false; }
        const int32_t d32 = (int32_t)nd;
        memcpy(insn + 5, &d32, 4);
        VirtualProtect(insn + 5, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), insn, 9);
        g_Ok = true;
        LOGI("SimStep: the game clock's step rate now read from the mod ({}), at 120 until the headset runs", (void*)g_Rate);
        {   // the frame lock (see HkClock): the clock's first bytes checked (push rbx; sub rsp,0xA0)
            uint8_t head[9] = {}; const uint8_t want[9] = { 0x40, 0x53, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 };
            if (!Rd(head, (const void*)(b + kClock), 9) || memcmp(head, want, 9) != 0)
                LOGI("SimStep: the clock's code at exe+0x{:X} is not as expected: no frame lock", kClock);
            else
            {
                g_ClockReal = (ClockFn)(b + kClock);
                DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
                DetourAttach(&(PVOID&)g_ClockReal, HkClock);
                const LONG e = DetourTransactionCommit();
                LOGI("SimStep: the frame lock (one step per headset frame, the clock fed whole steps of time) {}", e == NO_ERROR ? "installed" : "FAILED");
            }
        }
        return true;
    }
    void Apply(float hz)
    {
        if (!g_Ok) return;
        const uintptr_t b = GameInfo::Base();
        *(volatile float*)(b + kStepVar) = 1.f / hz;   // (the step first: a frame between the two takes the old count of new steps)
        *(volatile float*)g_Rate = hz;
        const float was = g_Cur.exchange(hz);
        g_Changes++;
        LOGI("SimStep: the game's simulation step {:.2f} ms ({:.1f} steps a second; was {:.1f}){}", 1000.f / hz, hz, was,
             hz == kGameHz ? ": the game's own" : ": one step per headset frame");
    }
}

void SimStep::SetEnabled(bool on)
{
    g_On = on;
    if (on) Patch();
    LOGI("SimStep: {}", on ? "the game's simulation steps follow the headset's refresh (SimStepAtRefresh)" : "the game's own 1/120 s steps");
    if (!on && g_Ok && g_Cur.load() != kGameHz) Apply(kGameHz);
}

void SimStep::OnPresent()
{
    if (!g_Ok) return;
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 250;
    // The headset's REFRESH, not the runtime's period of the moment. Reported 2026-11-07 (DOTO): lateral motion at
    // 90 fps juddered, and the log showed the step going 90 -> 120 -> 90 every second or two for a quarter to half a
    // second each time -- the runtime's predicted period left 11.1 ms now and then (SteamVR stretches it when it
    // throttles the app), fell outside 50-200 Hz, and the step went back to the game's 120: 4 steps one frame, 1 the
    // next, the stutter this fix exists to remove. The refresh is the SHORTEST period seen in the last 5 s; a longer
    // one (a throttled stretch) changes nothing: the game then makes whole multiples of steps per frame, which is
    // smooth. Leaving VR (no session for 5 s) goes back to the game's 120.
    static long long ring[20] = {}; static int head = 0;
    const bool vr = g_On.load() && VRRuntime::GetStatus().running;
    const long long per = vr ? VRRuntime::DisplayPeriodNs() : 0;
    ring[head] = per > 1000000 && per < 50000000 ? per : 0; head = (head + 1) % 20;
    long long mn = 0; for (long long p : ring) if (p > 0 && (mn == 0 || p < mn)) mn = p;
    float want = kGameHz;
    if (g_On.load() && mn > 0)
    {
        const float hz = roundf(1e10f / (float)mn) / 10.f;   // (to 0.1 Hz)
        if (hz >= 30.f && hz <= 200.f) want = hz;
    }
    {   // what the runtime reported, when it is not the refresh (to see the stretches; at most every 5 s)
        static ULONGLONG nextLog = 0;
        if (per > 0 && mn > 0 && per > mn + mn / 20 && now >= nextLog)
        {
            nextLog = now + 5000;
            LOGI("SimStep: the runtime's display period is {:.2f} ms now (the refresh {:.2f} ms): the step stays at the refresh", per * 1e-6, mn * 1e-6);
        }
    }
    if (fabsf(want - g_Cur.load()) > 0.25f) Apply(want);
    {   // the steps the clock took per simulated frame, every 30 s (the frame lock's work)
        static ULONGLONG nextLog = 0; static unsigned long long last[4] = {}, lastFix = 0, lastTicks = 0, lastPass = 0;
        if (now >= nextLog)
        {
            if (nextLog)
            {
                unsigned long long h[4]; for (int i = 0; i < 4; i++) { h[i] = g_StepHist[i].load() - last[i]; last[i] += h[i]; }
                const unsigned long long fx = g_LockFix.load() - lastFix, tk = g_LockTicks.load() - lastTicks, ps = g_LockPass.load() - lastPass;
                lastFix += fx; lastTicks += tk; lastPass += ps;
                if (h[0] + h[1] + h[2] + h[3])
                    LOGI("SimStep: 30 s: steps per simulated frame 0:{} 1:{} 2:{} 3+:{} | frame lock: locked {} (of them not one step: {}), left to the game {} | ticks {:.2f} ms apart on average",
                         h[0], h[1], h[2], h[3], tk, fx, ps, g_CallAvg.load() * 1000.0);
            }
            nextLog = now + 30000;
        }
    }
}

std::string SimStep::Status()
{
    return fmt::format("simulation step {:.2f} ms ({:.1f}/s; {}, changes {}); frame lock {} locked {} fixed {} passed {} let go {}; steps 0:{} 1:{} 2:{} 3+:{}", 1000.f / g_Cur.load(), g_Cur.load(),
                       g_Ok ? (g_On.load() ? "at the headset's refresh" : "off") : "not patched", g_Changes.load(), g_ClockReal ? "on" : "off",
                       g_LockTicks.load(), g_LockFix.load(), g_LockPass.load(), g_LockSuspends.load(), g_StepHist[0].load(), g_StepHist[1].load(), g_StepHist[2].load(), g_StepHist[3].load());
}
