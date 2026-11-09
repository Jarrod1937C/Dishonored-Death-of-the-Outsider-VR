#include "pch.h"
#include "CrashLog.h"
#include <detours/detours.h>
#include <psapi.h>
#include <dbghelp.h>   // the types only: MiniDumpWriteDump is looked up at run time (no dbghelp.lib)
#include <shlobj.h>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <eh.h>

// See CrashLog.h. The report is written without the heap (it may be what broke): fixed buffers, snprintf, WriteFile.
namespace
{
    // ---- state ----
    std::atomic<int> g_Logged{ 0 };            // first-chance faults noted in DOTOVR.log (a hint, not a crash)
    std::atomic<bool> g_InHandler{ false };
    constexpr int kMaxLogged = 6;
    const char* volatile g_Phase = "init";
    ULONGLONG g_Start = 0;
    std::atomic<bool> g_Claimed{ false };      // one report per process: the first crash wins
    LPTOP_LEVEL_EXCEPTION_FILTER g_Next = nullptr;   // the filter someone else set (called after ours)
    wchar_t g_Dir[MAX_PATH] = {}, g_AltDir[MAX_PATH] = {};
    char g_Info[CrashLog::kInfoCount][4096] = {};

    // the report's thread: asleep until a crash
    HANDLE g_Go = nullptr, g_Done = nullptr, g_Writer = nullptr;
    DWORD g_WriterId = 0;
    std::atomic<bool> g_WriterReady{ false };   // the thread runs (it cannot start before DllMain returns)
    struct Request { EXCEPTION_POINTERS* ep; DWORD tid; const char* what; } g_Req{};

    // minidump (dbghelp, loaded by Prepare)
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    DumpFn g_Dump = nullptr;

    // ---- names ----
    const char* CodeName(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case 0xC0000409:                      return "STACK_BUFFER_OVERRUN/fastfail";
        case 0xC0000374:                      return "HEAP_CORRUPTION";
        case 0xC0000420:                      return "ASSERTION_FAILURE";
        case 0xE06D7363:                      return "C++ exception";
        case 0xE0DE0001:                      return "abort() in the mod";
        case 0xE0DE0002:                      return "std::terminate in the mod";
        case 0xE0DE0003:                      return "pure virtual call in the mod";
        case 0xE0DE0004:                      return "invalid CRT parameter in the mod";
        case 0xE0DE0005:                      return "the process terminated itself (TerminateProcess, no exception)";
        default:                              return "?";
        }
    }
    //! Faults worth a hint in the log at first chance (the engine raises C++ exceptions and the thread-name exception
    //! in normal operation: those are not).
    bool Interesting(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_IN_PAGE_ERROR: case 0xC0000409: case 0xC0000374:
            return true;
        default: return false;
        }
    }
    //! The codes that end the process whoever catches them (seen first chance: the fast fail and the heap's report come
    //! after, and do not reach the unhandled filter).
    bool Fatal(const EXCEPTION_RECORD* er)
    {
        if (!er) return false;
        switch (er->ExceptionCode)
        {
        case 0xC0000374: case 0xC0000409: case EXCEPTION_STACK_OVERFLOW: case 0xC0000420: return true;
        default: return (er->ExceptionFlags & EXCEPTION_NONCONTINUABLE) && er->ExceptionCode != 0xE06D7363;
        }
    }

    // ---- reading memory that may not be there (POD helpers: __try) ----
    bool SafeCopy(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadPtr(const uintptr_t* p, uintptr_t& out) { return SafeCopy(&out, p, sizeof out); }

    // ---- the faults seen first chance, kept (2026-11-08) ----
    // The load-a-save crash: an int 3 (a breakpoint, 0x80000003) on the render thread that nothing handled. Our
    // unhandled-exception filter was never called for it; the thread's root handler (ntdll's RtlUserThreadStart)
    // ended the process with NtTerminateProcess(exit code = the exception's code), its stack already unwound -- so the
    // report written there showed only the exit, not the fault. Each fault worth it (Interesting, breakpoints, single
    // steps) is kept here with its context, at first chance; an exit whose code is a fault this thread saw moments ago
    // is reported from that fault's own context (the stack below is still there: the unwind only moved rsp up).
    struct LastFault { CONTEXT ctx; EXCEPTION_RECORD rec; DWORD tid; ULONGLONG at; };   // (CONTEXT: 16-byte aligned)
    constexpr unsigned kKept = 32;
    LastFault g_Kept[kKept];
    std::atomic<unsigned> g_KeptN{ 0 };
    std::atomic_flag g_KeptLock = ATOMIC_FLAG_INIT;
    void KeepFault(const EXCEPTION_POINTERS* ep)
    {
        if (!ep || !ep->ContextRecord || !ep->ExceptionRecord) return;
        if (g_KeptLock.test_and_set(std::memory_order_acquire)) return;   // another thread is storing one: this one is skipped
        LastFault& f = g_Kept[g_KeptN.fetch_add(1) % kKept];
        memcpy(&f.ctx, ep->ContextRecord, sizeof f.ctx);
        memcpy(&f.rec, ep->ExceptionRecord, sizeof f.rec);
        f.rec.ExceptionRecord = nullptr;
        f.tid = GetCurrentThreadId(); f.at = GetTickCount64();
        g_KeptLock.clear(std::memory_order_release);
    }
    //! The newest fault with this code that this thread saw in the last 10 s, or null.
    LastFault* FindFault(DWORD tid, DWORD code)
    {
        for (int spin = 0; g_KeptLock.test_and_set(std::memory_order_acquire); spin++) if (spin > 100000) return nullptr;
        LastFault* best = nullptr;
        const unsigned n = g_KeptN.load();
        const ULONGLONG t = GetTickCount64();
        for (unsigned i = 0; i < kKept && i < n; i++)
        {
            LastFault& f = g_Kept[(n - 1 - i) % kKept];
            if (f.tid == tid && f.rec.ExceptionCode == code && t - f.at < 10000) { best = &f; break; }
        }
        g_KeptLock.clear(std::memory_order_release);
        return best;
    }
    std::atomic<int> g_BpLogged{ 0 };
    //! One frame of the unwind: the caller's context from this one's, by the module's unwind data (x64 .pdata). False at
    //! the end (no return address) or when the stack is unreadable.
    bool UnwindStep(CONTEXT* c, DWORD64* fnStart)
    {
        __try
        {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c->Rip, &base, nullptr);
            *fnStart = f ? base + f->BeginAddress : 0;
            if (!f)
            {   // a leaf function: its return address is at the top of the stack
                c->Rip = *(DWORD64*)c->Rsp; c->Rsp += 8;
            }
            else
            {
                PVOID hd = nullptr; DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &hd, &est, nullptr);
            }
            return c->Rip != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- the report's writer (no heap) ----
    HANDLE g_Out = INVALID_HANDLE_VALUE;
    char g_Line[8192];
    void Put(const char* s, size_t n) { DWORD w = 0; if (g_Out != INVALID_HANDLE_VALUE && n) WriteFile(g_Out, s, (DWORD)n, &w, nullptr); }
    void P(const char* f, ...)
    {
        va_list a; va_start(a, f);
        int n = _vsnprintf_s(g_Line, sizeof g_Line, _TRUNCATE, f, a);
        va_end(a);
        if (n < 0) n = (int)strlen(g_Line);
        Put(g_Line, (size_t)n); Put("\r\n", 2);
    }
    //! "<module>+0x<rva>" into a buffer
    void WhereBuf(const void* a, char* out, size_t n)
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m)
        {
            wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(m, p, MAX_PATH);
            const wchar_t* leaf = wcsrchr(p, L'\\'); leaf = leaf ? leaf + 1 : p;
            _snprintf_s(out, n, _TRUNCATE, "%ls+0x%llX", leaf, (unsigned long long)((uintptr_t)a - (uintptr_t)m));
        }
        else _snprintf_s(out, n, _TRUNCATE, "0x%016llX", (unsigned long long)(uintptr_t)a);
    }
    bool InModule(uintptr_t v)
    {
        HMODULE m = nullptr;
        return v >= 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m) && m;
    }
    void Hex(const char* title, const void* at, size_t n)
    {
        uint8_t b[16];
        for (size_t o = 0; o < n; o += 16)
        {
            const uint8_t* p = (const uint8_t*)at + o;
            if (!SafeCopy(b, p, 16)) { P("  %s %016llX: (unreadable)", title, (unsigned long long)(uintptr_t)p); return; }
            P("  %s %016llX: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X", title,
              (unsigned long long)(uintptr_t)p, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
    }
    //! the file: crash_log-<date>_<hh>-<mm>[-n].log in the exe's folder, else the fallback; `base` gets the name without
    //! its extension (for the minidump)
    HANDLE OpenReport(wchar_t* base, size_t baseLen)
    {
        SYSTEMTIME t; GetLocalTime(&t);
        for (const wchar_t* dir : { (const wchar_t*)g_Dir, (const wchar_t*)g_AltDir })
        {
            if (!dir[0]) continue;
            if (dir == g_AltDir) CreateDirectoryW(g_AltDir, nullptr);
            for (int k = 1; k < 10; k++)
            {
                if (k == 1) _snwprintf_s(base, baseLen, _TRUNCATE, L"%ls\\crash_log-%04u-%02u-%02u_%02u-%02u", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
                else _snwprintf_s(base, baseLen, _TRUNCATE, L"%ls\\crash_log-%04u-%02u-%02u_%02u-%02u-%d", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, k);
                wchar_t path[MAX_PATH + 16]; _snwprintf_s(path, MAX_PATH + 16, _TRUNCATE, L"%ls.log", base);
                HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) return h;
                if (GetLastError() != ERROR_FILE_EXISTS) break;   // not writable here: the next folder
            }
        }
        return INVALID_HANDLE_VALUE;
    }
    void TailLine(const char* s, size_t n, void*) { Put("  ", 2); Put(s, n); Put("\r\n", 2); }

    //! The whole report. On the report's thread (or, without it, on the crashing one).
    //! The minidump's extra memory: the faulting thread's stack from the fault's rsp up. The dump takes a thread's
    //! stack from where the thread is NOW; for a fault kept at first chance (ReportExit) the thread has unwound since,
    //! and the frames of the fault lie below that -- without this a debugger cannot walk them.
    struct DumpExtra { ULONG64 base = 0; ULONG size = 0; bool given = false; };
    BOOL CALLBACK DumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out)
    {
        DumpExtra* x = (DumpExtra*)param;
        if (!in || !out) return FALSE;
        switch (in->CallbackType)
        {
        case MemoryCallback:       // one extra range, then done
            if (x->given || !x->size) return FALSE;
            x->given = true; out->MemoryBase = x->base; out->MemorySize = x->size;
            return TRUE;
        case IncludeThreadCallback: case IncludeModuleCallback: case ThreadCallback: case ThreadExCallback: case ModuleCallback:
            return TRUE;           // everything, as without a callback
        case CancelCallback:
            out->Cancel = FALSE; out->CheckCancel = FALSE; return TRUE;
        case ReadMemoryFailureCallback:
            out->Status = S_OK; return TRUE;   // a page gone since: left out, the dump goes on
        default:
            return FALSE;
        }
    }
    void WriteReport(const char* what, EXCEPTION_POINTERS* ep, DWORD tid)
    {
        static wchar_t base[MAX_PATH + 16];
        g_Out = OpenReport(base, MAX_PATH + 16);
        if (g_Out == INVALID_HANDLE_VALUE) return;
        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        const CONTEXT* cx = ep ? ep->ContextRecord : nullptr;
        SYSTEMTIME t; GetLocalTime(&t);
        char w[256], w2[256];
        P("DOTO VR crash report");
        P("==============================================================================================");
        P("time        %04u-%02u-%02u %02u:%02u:%02u (local), %.1f s after the mod loaded", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
          (GetTickCount64() - g_Start) / 1000.0);
        P("what        %s", what);
        P("process     %lu, faulting thread %lu, mod phase '%s'", GetCurrentProcessId(), tid, g_Phase);
        static const char* const kInfoName[CrashLog::kInfoCount] = { "mod", "system", "gpu", "runtime", "headset", "map", "render", "settings" };
        for (int i = 0; i < CrashLog::kInfoCount; i++) if (g_Info[i][0]) P("%-11s %s", kInfoName[i], g_Info[i]);
        P("");
        P("EXCEPTION");
        for (const EXCEPTION_RECORD* r = er; r; )
        {
            WhereBuf(r->ExceptionAddress, w, sizeof w);
            P("  code 0x%08lX %s, flags 0x%lX%s, at %s", r->ExceptionCode, CodeName(r->ExceptionCode), r->ExceptionFlags,
              (r->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? " (non-continuable)" : "", w);
            if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
                P("  %s address 0x%016llX", r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing",
                  (unsigned long long)r->ExceptionInformation[1]);
            for (DWORD i = 0; i < r->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
                P("  parameter %lu: 0x%016llX", i, (unsigned long long)r->ExceptionInformation[i]);
            EXCEPTION_RECORD* nx = nullptr;
            if (!SafeCopy(&nx, &r->ExceptionRecord, sizeof nx) || !nx || nx == r) break;
            P("  (nested in:)"); r = nx;
        }
        if (cx)
        {
            P("");
            P("REGISTERS");
            WhereBuf((void*)cx->Rip, w, sizeof w);
            P("  rip %016llX  %s", cx->Rip, w);
            P("  rax %016llX  rbx %016llX  rcx %016llX  rdx %016llX", cx->Rax, cx->Rbx, cx->Rcx, cx->Rdx);
            P("  rsi %016llX  rdi %016llX  rbp %016llX  rsp %016llX", cx->Rsi, cx->Rdi, cx->Rbp, cx->Rsp);
            P("  r8  %016llX  r9  %016llX  r10 %016llX  r11 %016llX", cx->R8, cx->R9, cx->R10, cx->R11);
            P("  r12 %016llX  r13 %016llX  r14 %016llX  r15 %016llX", cx->R12, cx->R13, cx->R14, cx->R15);
            P("  eflags %08lX  mxcsr %08lX", cx->EFlags, cx->MxCsr);
            for (int i = 0; i < 6; i++)
            {
                const float* x = (const float*)&cx->Xmm0 + i * 4;
                P("  xmm%d %g %g %g %g", i, x[0], x[1], x[2], x[3]);
            }
            P("");
            P("CODE AT RIP (32 bytes before, 32 after)");
            Hex("code", (const uint8_t*)cx->Rip - 32, 64);
            P("");
            P("CALL STACK (unwound)");
            CONTEXT c = *cx;
            for (int i = 0; i < 64; i++)
            {
                WhereBuf((void*)c.Rip, w, sizeof w);
                DWORD64 fn = 0; const DWORD64 rip = c.Rip, rsp = c.Rsp;
                const bool more = UnwindStep(&c, &fn);
                if (fn) { WhereBuf((void*)fn, w2, sizeof w2); P("  #%-2d %-48s function %s +0x%llX  (rsp %016llX)", i, w, w2, (unsigned long long)(rip - fn), rsp); }
                else P("  #%-2d %-48s (no unwind data)  (rsp %016llX)", i, w, rsp);
                if (!more || c.Rsp <= rsp) break;
            }
            P("");
            P("STACK SCAN (values on the stack that point into a module: callers the unwind may have missed)");
            {
                char line[1024]; size_t len = 0; int found = 0;
                for (int i = 0; i < 1024 && found < 48; i++)
                {
                    uintptr_t v = 0;
                    if (!ReadPtr((const uintptr_t*)cx->Rsp + i, v)) break;
                    if (!InModule(v)) continue;
                    WhereBuf((void*)v, w, sizeof w);
                    len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " [rsp+%X] %s", i * 8, w);
                    if (++found % 4 == 0) { P("  %s", line); len = 0; line[0] = 0; }
                }
                if (len) P("  %s", line);
            }
            P("");
            P("STACK (512 bytes at rsp)");
            Hex("stack", (const void*)cx->Rsp, 512);
            P("");
            P("MEMORY AT THE REGISTERS (64 bytes where they point to readable memory)");
            const struct { const char* n; DWORD64 v; } regs[] = { { "rax", cx->Rax }, { "rbx", cx->Rbx }, { "rcx", cx->Rcx }, { "rdx", cx->Rdx },
                { "rsi", cx->Rsi }, { "rdi", cx->Rdi }, { "rbp", cx->Rbp }, { "r8", cx->R8 }, { "r9", cx->R9 }, { "r10", cx->R10 },
                { "r11", cx->R11 }, { "r12", cx->R12 }, { "r13", cx->R13 }, { "r14", cx->R14 }, { "r15", cx->R15 } };
            for (const auto& r : regs)
            {
                uint8_t probe;
                if (r.v < 0x10000 || !SafeCopy(&probe, (const void*)r.v, 1)) continue;
                WhereBuf((void*)r.v, w, sizeof w);
                P("  %s -> %s", r.n, w);
                Hex(r.n, (const void*)r.v, 64);
            }
        }
        P("");
        P("MODULES (base, size, link timestamp, path)");
        {
            static HMODULE mods[1024]; DWORD need = 0;
            if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
                for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++)
                {
                    MODULEINFO mi{}; K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
                    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(mods[i], p, MAX_PATH);
                    DWORD stamp = 0; LONG lfa = 0;
                    if (SafeCopy(&lfa, (const uint8_t*)mods[i] + 0x3C, 4)) SafeCopy(&stamp, (const uint8_t*)mods[i] + lfa + 8, 4);
                    P("  %016llX %08lX %08lX %ls", (unsigned long long)(uintptr_t)mods[i], mi.SizeOfImage, stamp, p);
                }
        }
        P("");
        P("THE LOG'S LAST LINES (DOTOVR.log; none with WriteLog = 0)");
        Log::CrashTail(&TailLine, nullptr);
        FlushFileBuffers(g_Out);
        // the minidump last: dbghelp may allocate, and the text above is already on disk
        if (g_Dump)
        {
            wchar_t dp[MAX_PATH + 16]; _snwprintf_s(dp, MAX_PATH + 16, _TRUNCATE, L"%ls.dmp", base);
            HANDLE d = CreateFileW(dp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (d != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION mei{ tid, ep, FALSE };   // (a 4-byte packed struct: dbghelp.h's own type)
                // every thread's stack, what the stacks point at, the unloaded modules and the threads' times; not the
                // data segments (the game's are large): a few MB
                const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo);
                DumpExtra extra;
                if (ep && ep->ContextRecord && ep->ContextRecord->Rsp)
                {
                    MEMORY_BASIC_INFORMATION mbi{};
                    const ULONG64 lo = (ep->ContextRecord->Rsp - 0x200) & ~0xFull;
                    if (VirtualQuery((const void*)lo, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
                    {
                        const ULONG64 end = (ULONG64)mbi.BaseAddress + mbi.RegionSize;
                        extra.base = lo; extra.size = (ULONG)std::min<ULONG64>(end - lo, 1024 * 1024);
                    }
                }
                MINIDUMP_CALLBACK_INFORMATION cb{ DumpCallback, &extra };
                const BOOL ok = g_Dump(GetCurrentProcess(), GetCurrentProcessId(), d, type, ep ? &mei : nullptr, nullptr, extra.size ? &cb : nullptr);
                const DWORD err = ok ? 0 : GetLastError();
                CloseHandle(d);
                P("");
                P("minidump: %ls.dmp %s (error %lu)", wcsrchr(base, L'\\') ? wcsrchr(base, L'\\') + 1 : base, ok ? "written" : "FAILED", err);
                if (!ok) DeleteFileW(dp);
            }
        }
        else { P(""); P("minidump: none (dbghelp.dll not loaded)"); }
        FlushFileBuffers(g_Out);
        CloseHandle(g_Out);
        g_Out = INVALID_HANDLE_VALUE;
    }

    DWORD WINAPI WriterThread(LPVOID)
    {
        g_WriterReady = true;
        WaitForSingleObject(g_Go, INFINITE);
        WriteReport(g_Req.what, g_Req.ep, g_Req.tid);
        SetEvent(g_Done);
        return 0;
    }
    //! A crash: the report, once. Hands it to the report's thread (its own stack) and waits for it.
    void Crash(const char* what, EXCEPTION_POINTERS* ep)
    {
        if (g_Claimed.exchange(true)) return;
        g_Req = { ep, GetCurrentThreadId(), what };
        if (g_WriterReady.load() && GetCurrentThreadId() != g_WriterId)
        {
            SetEvent(g_Go);
            WaitForSingleObject(g_Done, 60000);
        }
        else WriteReport(what, ep, GetCurrentThreadId());
    }

    // ---- the DOTOVR.log hint (heap: not for the fatal codes) ----
    void LogHint(const char* what, EXCEPTION_POINTERS* ep)
    {
        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        if (!er) return;
        std::string detail;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            detail = fmt::format(" {} 0x{:016X}", er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                                 (uintptr_t)er->ExceptionInformation[1]);
        LOGI("fault [{}] {} (0x{:08X}){} at {} - phase '{}' thread {}", what, CodeName(er->ExceptionCode), (unsigned)er->ExceptionCode, detail,
             CrashLog::Where(er->ExceptionAddress), g_Phase, GetCurrentThreadId());
    }

    LONG CALLBACK Vectored(EXCEPTION_POINTERS* ep)
    {
        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        if (!er) return EXCEPTION_CONTINUE_SEARCH;
        if (Fatal(er)) { Crash("fatal exception (seen first chance: it ends the process whoever handles it)", ep); return EXCEPTION_CONTINUE_SEARCH; }
        const DWORD code = er->ExceptionCode;
        const bool bp = code == EXCEPTION_BREAKPOINT || code == 0x4000001F;   // (and the WOW64 one)
        if (Interesting(code) || bp || code == EXCEPTION_SINGLE_STEP) KeepFault(ep);
        // A breakpoint in a release game: a debug break left in, or code run that is padding (0xCC between functions,
        // the rest of a hook's patch) -- nothing handles one, Windows ends the process with 0x80000003. Logged with its
        // callers now, while the stack is there (the report comes at the exit: see FindFault).
        if (bp && !IsDebuggerPresent() && Log::Enabled() && g_BpLogged.fetch_add(1) < 4 && !g_InHandler.exchange(true))
        {
            Log::ForceFile();
            LogHint("first chance: a breakpoint (int 3); unless something handles it Windows ends the game with exit code 0x80000003", ep);
            unsigned char bytes[32] = {};
            const bool got = SafeCopy(bytes, (const unsigned char*)er->ExceptionAddress - 16, sizeof bytes);
            std::string hex;
            if (got) for (int i = 0; i < 32; i++) hex += i == 16 ? fmt::format("[{:02X}]", bytes[i]) : fmt::format(" {:02X}", bytes[i]);
            CONTEXT c; memcpy(&c, ep->ContextRecord, sizeof c);
            std::string s; DWORD64 fn = 0;
            for (int i = 0; i < 20 && UnwindStep(&c, &fn); i++) s += " " + CrashLog::Where((const void*)c.Rip);
            LOGI("CrashLog: the breakpoint's code (16 bytes before it, [it], after): {}; its callers:{}", got ? hex : std::string("(unreadable)"), s);
            g_InHandler = false;
        }
        // a fault the game (or the mod's own guarded reads) may well handle: a hint in the log, the first few only
        if (Interesting(er->ExceptionCode) && Log::Enabled() && g_Logged.load() < kMaxLogged && !g_InHandler.exchange(true))
        {
            g_Logged++;
            LogHint("first chance, may be handled", ep);
            g_InHandler = false;
        }
        return EXCEPTION_CONTINUE_SEARCH;      // never swallow: the game's own __except may be the right handler
    }

    LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep)
    {
        Crash("unhandled exception", ep);
        if (!g_InHandler.exchange(true))
        {   // and the old one-line summary in DOTOVR.log
            const DWORD c = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
            if (c != 0xC0000374 && c != EXCEPTION_STACK_OVERFLOW) { Log::ForceFile(); LogHint("fatal: see the crash_log file", ep); }
            g_InHandler = false;
        }
        if (g_Next && g_Next != Unhandled) return g_Next(ep);   // the game's own filter, after ours
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The game (or a library) setting its own filter would replace ours: it is kept as the one ours calls next.
    LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* g_RealSetFilter)(LPTOP_LEVEL_EXCEPTION_FILTER) = SetUnhandledExceptionFilter;
    LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
    {
        if (f == Unhandled) return g_RealSetFilter(f);
        LPTOP_LEVEL_EXCEPTION_FILTER prev = g_Next;
        g_Next = f;
        return prev;
    }

    // ---- the mod's own CRT (it links the CRT statically: these are the mod's, not the game's) ----
    //! a report from here (no exception): the context of this call
    void SelfCrash(DWORD code)
    {
        static CONTEXT ctx; static EXCEPTION_RECORD rec;
        RtlCaptureContext(&ctx);
        memset(&rec, 0, sizeof rec);
        rec.ExceptionCode = code; rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE; rec.ExceptionAddress = (void*)ctx.Rip;
        EXCEPTION_POINTERS ep{ &rec, &ctx };
        Crash(CodeName(code), &ep);
    }
    void OnAbort(int) { SelfCrash(0xE0DE0001); }                       // then abort goes on
    //! The process ends itself with this exit code: the report. From the fault's own context when the code is a fault this
    //! thread saw first chance moments ago (an unhandled exception ended by the thread's root handler, see KeepFault);
    //! otherwise from here.
    void ReportExit(DWORD code)
    {
        if (LastFault* f = FindFault(GetCurrentThreadId(), code))
        {
            static EXCEPTION_POINTERS ep;
            ep.ExceptionRecord = &f->rec; ep.ContextRecord = &f->ctx;
            Crash("unhandled exception: the process was ended with the exception's code by Windows' default handler (the thread's root __except), without our filter -- the context below is the fault's own, kept at first chance", &ep);
            return;
        }
        SelfCrash(0xE0DE0005);
    }
    // 2026-11-08: two crashes while loading a save (from the pause menu, in a level), no crash_log written and nothing in
    // DOTOVR.log -- no exception reached the handlers above. Either the process ended itself (the engine's or a
    // library's TerminateProcess: no exception, no DLL detach) or a fail-fast (__fastfail / a /GS cookie: straight to the
    // kernel, which nothing in the process can catch -- Windows' Event Viewer has those). The first is caught here:
    // TerminateProcess on this process writes a full report (stack and minidump) first; ExitProcess is logged with its
    // callers (a normal quit goes there too, so no report).
    BOOL(WINAPI* g_RealTerminate)(HANDLE, UINT) = TerminateProcess;
    VOID(WINAPI* g_RealExit)(UINT) = ExitProcess;
    BOOL WINAPI HookTerminate(HANDLE h, UINT code)
    {
        if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId())
        {
            Log::ForceFile();
            LOGI("CrashLog: the process is terminating itself (TerminateProcess, exit code {:#x}, thread {}): writing a crash report", code, GetCurrentThreadId());
            if (code != 0xE0DE0003 && code != 0xE0DE0004) ReportExit(code);
        }
        return g_RealTerminate(h, code);
    }
    // 2026-11-08, second pass: with the two above hooked (log b6e13fa6) a load-a-save crash still left nothing -- neither
    // was called, no exception was seen on any thread (the vectored handler is first in line), and Windows' Event
    // Viewer had no Application Error either. So the process left through ntdll directly: NtTerminateProcess (what
    // every exit path ends in, kernel32's included; a library calling it, or the game) or RtlExitUserProcess. Both
    // hooked: a report with the callers (the kernel32 hooks above stay: they say which API it was).
    using NtTerminateFn = LONG(NTAPI*)(HANDLE, LONG);
    using RtlExitFn = VOID(NTAPI*)(ULONG);
    NtTerminateFn g_RealNtTerminate = nullptr;
    RtlExitFn g_RealRtlExit = nullptr;
    std::atomic<bool> g_ExitSeen{ false };
    LONG NTAPI HookNtTerminate(HANDLE h, LONG code)
    {
        // (ExitProcess calls it twice: once with a null handle for the other threads, then on itself)
        const bool self = h == nullptr || h == GetCurrentProcess() || (h && GetProcessId(h) == GetCurrentProcessId());
        if (self && !g_ExitSeen.exchange(true))
        {
            Log::ForceFile();
            void* frames[14] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 14, frames, nullptr);
            std::string s; for (USHORT i = 0; i < n; i++) s += " " + CrashLog::Where(frames[i]);
            LOGI("CrashLog: NtTerminateProcess on this process (exit code {:#x}, thread {}, handle {}); from{}", (unsigned)code, GetCurrentThreadId(), h ? "self" : "null (the other threads)", s);
            if (code != 0 && code != 0xE0DE0003 && code != 0xE0DE0004) ReportExit((DWORD)code);
        }
        return g_RealNtTerminate(h, code);
    }
    VOID NTAPI HookRtlExit(ULONG code)
    {
        if (!g_ExitSeen.exchange(true))
        {
            Log::ForceFile();
            void* frames[14] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 14, frames, nullptr);
            std::string s; for (USHORT i = 0; i < n; i++) s += " " + CrashLog::Where(frames[i]);
            LOGI("CrashLog: RtlExitUserProcess (exit code {:#x}, thread {}); from{}", (unsigned)code, GetCurrentThreadId(), s);
            if (code != 0) ReportExit(code);
        }
        g_RealRtlExit(code);
    }
    // A quit that is no crash at all (2026-11-08, "is the game mistakenly just exiting?"): the ways a game leaves cleanly,
    // each logged with its callers -- PostQuitMessage (the message loop ends), DestroyWindow on a top-level window (the
    // main window closed: Alt-F4, a WM_CLOSE from outside, the engine's own shutdown), a message box (the engine's fatal
    // error / "out of memory" boxes go through MessageBoxA/W; in a headset one is easy to miss), and the DLL's own
    // detach (Main.cpp: a clean process exit unloads the mod; a kill from outside does not).
    std::string Narrow(const std::wstring& s);
    std::string Callers(int skip = 1)
    {
        void* frames[14] = {}; const USHORT n = RtlCaptureStackBackTrace(skip, 14, frames, nullptr);
        std::string s; for (USHORT i = 0; i < n; i++) s += " " + CrashLog::Where(frames[i]);
        return s;
    }
    VOID(WINAPI* g_RealPostQuit)(int) = PostQuitMessage;
    BOOL(WINAPI* g_RealDestroyWindow)(HWND) = DestroyWindow;
    int(WINAPI* g_RealMsgBoxA)(HWND, LPCSTR, LPCSTR, UINT) = MessageBoxA;
    int(WINAPI* g_RealMsgBoxW)(HWND, LPCWSTR, LPCWSTR, UINT) = MessageBoxW;
    VOID WINAPI HookPostQuit(int code)
    {
        Log::ForceFile();
        LOGI("CrashLog: PostQuitMessage({}) on thread {}: the game's message loop is told to end; from{}", code, GetCurrentThreadId(), Callers());
        g_RealPostQuit(code);
    }
    BOOL WINAPI HookDestroyWindow(HWND h)
    {
        if (h && IsWindow(h) && !GetParent(h))
        {
            char title[128] = {}; GetWindowTextA(h, title, sizeof title);
            char cls[64] = {}; GetClassNameA(h, cls, sizeof cls);
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId() && (title[0] || GetWindow(h, GW_OWNER) == nullptr))
                LOGI("CrashLog: DestroyWindow on top-level window '{}' (class {}) on thread {}; from{}", title, cls, GetCurrentThreadId(), Callers());
        }
        return g_RealDestroyWindow(h);
    }
    int WINAPI HookMsgBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type)
    {
        Log::ForceFile();
        LOGI("CrashLog: MessageBoxA '{}': {} (thread {}); from{}", cap ? cap : "", text ? text : "", GetCurrentThreadId(), Callers());
        return g_RealMsgBoxA(h, text, cap, type);
    }
    int WINAPI HookMsgBoxW(HWND h, LPCWSTR text, LPCWSTR cap, UINT type)
    {
        Log::ForceFile();
        LOGI("CrashLog: MessageBoxW '{}': {} (thread {}); from{}", cap ? Narrow(cap) : std::string(), text ? Narrow(text) : std::string(), GetCurrentThreadId(), Callers());
        return g_RealMsgBoxW(h, text, cap, type);
    }
    VOID WINAPI HookExit(UINT code)
    {
        void* frames[12] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
        std::string s; for (USHORT i = 0; i < n; i++) s += " " + CrashLog::Where(frames[i]);
        LOGI("CrashLog: the process is exiting (ExitProcess, exit code {:#x}, thread {}); from{}", code, GetCurrentThreadId(), s);
        g_RealExit(code);
    }
    void OnTerminate() { SelfCrash(0xE0DE0002); abort(); }
    void OnPurecall() { SelfCrash(0xE0DE0003); TerminateProcess(GetCurrentProcess(), 0xE0DE0003); }
    void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { SelfCrash(0xE0DE0004); TerminateProcess(GetCurrentProcess(), 0xE0DE0004); }

    //! the newest crash_log-*.log in the two folders: its full path
    std::wstring Latest()
    {
        std::wstring best; FILETIME bestT{};
        for (const wchar_t* dir : { (const wchar_t*)g_Dir, (const wchar_t*)g_AltDir })
        {
            if (!dir[0]) continue;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((std::wstring(dir) + L"\\crash_log-*.log").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
                if (CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) { bestT = fd.ftLastWriteTime; best = std::wstring(dir) + L"\\" + fd.cFileName; }
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return best;
    }
    std::string Narrow(const std::wstring& s)
    {
        std::string o; for (wchar_t c : s) o += (c < 128 ? (char)c : '?'); return o;
    }
}

void CrashLog::Install()
{
    g_Start = GetTickCount64();
    GetModuleFileNameW(nullptr, g_Dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_Dir, L'\\')) *s = 0;
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) _snwprintf_s(g_AltDir, MAX_PATH, _TRUNCATE, L"%ls\\DOTOVR", la);
    g_Go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_Done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // its own stack, large enough for the unwinder and dbghelp; asleep until a crash
    if (g_Go && g_Done) g_Writer = CreateThread(nullptr, 256 * 1024, WriterThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_WriterId);
    AddVectoredExceptionHandler(1, Vectored);
    g_Next = SetUnhandledExceptionFilter(Unhandled);   // one set before ours (if any) is called after it
    signal(SIGABRT, OnAbort);
    set_terminate(OnTerminate);
    _set_purecall_handler(OnPurecall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    LOGI("CrashLog: installed (a crash writes crash_log-<date>_<time>.log and a minidump next to the game's exe)");
}

void CrashLog::Prepare()
{
    // dbghelp from System32 (not the game folder's, if it has one)
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    if (HMODULE h = LoadLibraryW((std::wstring(sys) + L"\\dbghelp.dll").c_str())) g_Dump = (DumpFn)GetProcAddress(h, "MiniDumpWriteDump");
    // keep our filter first: whoever sets one later is called after it
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealSetFilter, HookSetFilter);
    DetourAttach(&(PVOID&)g_RealTerminate, HookTerminate);
    DetourAttach(&(PVOID&)g_RealExit, HookExit);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    {
        g_RealNtTerminate = (NtTerminateFn)GetProcAddress(nt, "NtTerminateProcess");
        g_RealRtlExit = (RtlExitFn)GetProcAddress(nt, "RtlExitUserProcess");
        if (g_RealNtTerminate) DetourAttach(&(PVOID&)g_RealNtTerminate, HookNtTerminate);
        if (g_RealRtlExit) DetourAttach(&(PVOID&)g_RealRtlExit, HookRtlExit);
    }
    DetourAttach(&(PVOID&)g_RealPostQuit, HookPostQuit);
    DetourAttach(&(PVOID&)g_RealDestroyWindow, HookDestroyWindow);
    DetourAttach(&(PVOID&)g_RealMsgBoxA, HookMsgBoxA);
    DetourAttach(&(PVOID&)g_RealMsgBoxW, HookMsgBoxW);
    const LONG r = DetourTransactionCommit();
    LOGI("CrashLog: minidumps {}, the unhandled-exception filter kept first, TerminateProcess / ExitProcess / NtTerminateProcess / RtlExitUserProcess / PostQuitMessage / DestroyWindow / MessageBox watched: detour={}", g_Dump ? "on" : "OFF (no dbghelp)", r);
}

void CrashLog::SetPhase(const char* phase) { g_Phase = phase ? phase : "?"; }

std::string CrashLog::Where(const void* addr)
{
    char b[256]; WhereBuf(addr, b, sizeof b); return b;
}

void CrashLog::SetInfo(Info what, const std::string& text)
{
    if (what < 0 || what >= kInfoCount) return;
    char* d = g_Info[what];
    const size_t n = std::min(text.size(), sizeof g_Info[0] - 1);
    d[0] = 0;   // a crash meanwhile reads an empty line rather than a torn one
    if (n > 1) memcpy(d + 1, text.data() + 1, n - 1);
    d[n] = 0;
    if (n) d[0] = text[0];
}

std::string CrashLog::LatestReport()
{
    const std::wstring p = Latest();
    const size_t s = p.find_last_of(L'\\');
    return Narrow(s == std::wstring::npos ? p : p.substr(s + 1));
}

bool CrashLog::SaveLatestToDesktop(std::string& result)
{
    const std::wstring src = Latest();
    if (src.empty()) { result = "no crash log to save"; return false; }
    PWSTR desk = nullptr;
    static const GUID kDesktop = { 0xB4BFCC3A, 0xDB2C, 0x424C, { 0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41 } };   // FOLDERID_Desktop
    if (FAILED(SHGetKnownFolderPath(kDesktop, 0, nullptr, &desk)) || !desk) { result = "the desktop folder was not found"; return false; }
    const std::wstring dir = desk; CoTaskMemFree(desk);
    const std::wstring name = src.substr(src.find_last_of(L'\\') + 1);
    bool ok = CopyFileW(src.c_str(), (dir + L"\\" + name).c_str(), FALSE) != 0;
    const DWORD err = ok ? 0 : GetLastError();
    bool dump = false;
    {   // its minidump, when there is one
        const std::wstring dsrc = src.substr(0, src.size() - 4) + L".dmp";
        if (GetFileAttributesW(dsrc.c_str()) != INVALID_FILE_ATTRIBUTES)
            dump = CopyFileW(dsrc.c_str(), (dir + L"\\" + name.substr(0, name.size() - 4) + L".dmp").c_str(), FALSE) != 0;
    }
    result = ok ? "saved " + Narrow(name) + (dump ? " and its minidump" : "") + " to the desktop" : fmt::format("could not copy {} to the desktop (error {})", Narrow(name), err);
    LOGI("CrashLog: {}", result);
    return ok;
}
