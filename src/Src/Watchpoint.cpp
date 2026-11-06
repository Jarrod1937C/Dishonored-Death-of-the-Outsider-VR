#include "pch.h"
#include "Watchpoint.h"
#include "GameInfo.h"
#include <cstdio>
#include <tlhelp32.h>

// NOTE: never allocate or log while the target thread is suspended — if it was suspended
// inside the heap lock, the suspending thread deadlocks (seen 2026-08-29: game froze).
namespace
{
    std::atomic<uint32_t> g_Tid{ 0 };
    std::atomic<uintptr_t> g_Addr{ 0 };
    std::atomic<int> g_HitsLeft{ 0 }; int g_MaxHits = 0;
    PVOID g_Veh = nullptr;

    // Reader histogram: one line per DISTINCT instruction, not per access. A read-watch on the camera axis fires
    // dozens of times a frame from the same handful of places; what the research needs is the SET of readers
    // (and how often each), and that has to be built without allocating inside the exception handler.
    constexpr int kMaxRips = 128;
    uintptr_t g_Rips[kMaxRips]; unsigned g_Counts[kMaxRips]; std::atomic<int> g_NumRips{ 0 };
    unsigned g_Overflow = 0;

    LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
    {
        if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* c = ep->ContextRecord;
        if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;   // not our DR0
        uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        int left = --g_HitsLeft;
        int n = g_NumRips.load(); int k = 0;
        for (; k < n; k++) if (g_Rips[k] == rip) break;
        if (k < n) g_Counts[k]++;
        else if (n < kMaxRips)
        {
            g_Rips[n] = rip; g_Counts[n] = 1; g_NumRips = n + 1;
            char buf[256];
            _snprintf_s(buf, sizeof buf, _TRUNCATE, "Watchpoint: access to 0x%llX from exe+0x%llX tid=%lu rax=0x%llX rcx=0x%llX rdx=0x%llX (reader #%d, hit %d)",
                        (unsigned long long)g_Addr.load(), (unsigned long long)(rip - GameInfo::Base()), GetCurrentThreadId(),
                        (unsigned long long)c->Rax, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, n + 1, (int)(g_MaxHits - left));
            Log::Write(buf);
            {   // who called it: the stack's return addresses into the exe (a scan, like CrashLog's -- no unwinding here)
                const uintptr_t base = GameInfo::Base(); const uint64_t* sp = (const uint64_t*)c->Rsp;
                int len = _snprintf_s(buf, sizeof buf, _TRUNCATE, "Watchpoint:   stack of reader #%d:", n + 1); int got = 0;
                for (int i = 0; i < 96 && got < 10 && len > 0 && len < (int)sizeof buf - 16; i++)
                {
                    const uint64_t v = sp[i];
                    if (v > base + 0x1000 && v < base + 0x1B80000 /* DOTO's .text ends at exe+0x1B8006C */) { len += _snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " 0x%llX", (unsigned long long)(v - base)); got++; }
                }
                Log::Write(buf);
            }
        }
        else g_Overflow++;
        c->Dr6 = 0;
        if (left <= 0) { c->Dr0 = 0; c->Dr7 &= ~(3ull | (0xFull << 16)); Log::Write("Watchpoint: hit budget spent, disarmed"); }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

void Watchpoint::SetGameThreadId(uint32_t tid) { g_Tid = tid; }
uint32_t Watchpoint::GameThreadId() { return g_Tid; }

namespace
{
    struct ArmJob { uint32_t tid; uintptr_t address; int lenBits; bool arm; bool rw; };

    // Program DR0/DR7 on ONE thread. Returns false (and the error) when it could not.
    bool Program(HANDLE th, const ArmJob& job, DWORD& err)
    {
        bool ok = false;
        // --- critical section: no heap, no logging while the target is suspended ---
        if (SuspendThread(th) != (DWORD)-1)
        {
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c))
            {
                c.Dr7 &= ~(3ull | (0xFull << 16));
                if (job.arm) { c.Dr0 = job.address; c.Dr7 |= 1ull /*L0*/ | ((job.rw ? 3ull : 1ull) << 16) /*RW0: 01 write, 11 read/write*/ | ((uint64_t)job.lenBits << 18); }
                else c.Dr0 = 0;
                ok = SetThreadContext(th, &c) != 0;
            }
            if (!ok) err = GetLastError();
            ResumeThread(th);
        }
        else err = GetLastError();
        // --- end critical section ---
        return ok;
    }

    // Runs on a helper thread: the target may be the very thread that issued the command
    // (in this game the pad poll, Present and our command channel all run on the main thread,
    // and SuspendThread on yourself never returns).
    // Debug registers are PER THREAD, and the engine runs game logic on threads other than the one that polls
    // the pad: a read-watch on the camera axis armed on the main thread alone saw two readers, neither of them a
    // player sensor. tid 0 = every thread of the process except this helper.
    DWORD WINAPI ArmWorker(LPVOID p)
    {
        ArmJob job = *(ArmJob*)p; delete (ArmJob*)p;
        int done = 0, failed = 0; DWORD err = 0;
        if (job.tid)
        {
            HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, job.tid);
            if (!th) { LOGI("Watchpoint: OpenThread failed {}", GetLastError()); return 0; }
            if (Program(th, job, err)) done++; else failed++;
            CloseHandle(th);
        }
        else
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{}; te.dwSize = sizeof te;
            const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                    if (!th) { failed++; continue; }
                    if (Program(th, job, err)) done++; else failed++;
                    CloseHandle(th);
                } while (Thread32Next(snap, &te));
                CloseHandle(snap);
            }
        }
        LOGI("Watchpoint: {} DR0=0x{:X} on {} thread(s) ({} failed, last err={}){}", job.arm ? "armed" : "disarmed", job.address, done, failed, err, job.tid ? "" : " [all threads]");
        return 0;
    }
    void Spawn(ArmJob* job) { HANDLE h = CreateThread(nullptr, 0, ArmWorker, job, 0, nullptr); if (h) CloseHandle(h); else delete job; }
}

bool Watchpoint::Arm(uintptr_t address, int len, int maxHits, bool readWrite, bool allThreads)
{
    uint32_t tid = allThreads ? 0 : g_Tid.load();
    if (!tid && !allThreads) { LOGI("Watchpoint: game thread id unknown"); return false; }
    if (!g_Veh) g_Veh = AddVectoredExceptionHandler(1, Handler);
    int lenBits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;   // DR7 LEN0: 00=1,01=2,10=8,11=4
    g_Addr = address; g_HitsLeft = maxHits; g_MaxHits = maxHits; g_NumRips = 0; g_Overflow = 0;
    LOGI("Watchpoint: arming 0x{:X} len={} on tid {} (max {} hits), this tid={}", address, len, tid, maxHits, GetCurrentThreadId());
    Spawn(new ArmJob{ tid, address, lenBits, true, readWrite });
    return true;
}

void Watchpoint::Disarm()
{
    Spawn(new ArmJob{ 0, 0, 0, false, false });   // every thread: harmless on those that were never armed
}

void Watchpoint::Dump()
{
    int n = g_NumRips.load();
    LOGI("Watchpoint: 0x{:X} readers={} overflow={} hitsLeft={}", g_Addr.load(), n, g_Overflow, g_HitsLeft.load());
    for (int k = 0; k < n; k++) LOGI("Watchpoint:   exe+0x{:X} x{}", g_Rips[k] - GameInfo::Base(), g_Counts[k]);
}
