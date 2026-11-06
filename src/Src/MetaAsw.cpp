#include "pch.h"
#include "MetaAsw.h"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <vector>

namespace
{
    std::atomic<int> g_Mode{ 1 };
    std::atomic<bool> g_Busy{ false };
    std::atomic<int> g_Runs{ 0 }, g_Ok{ 0 }, g_Fail{ 0 };
    std::mutex g_M;
    std::string g_Last = "not run";

    bool IsMeta(std::string n)
    {
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
        return n.find("oculus") != std::string::npos || n.find("meta") != std::string::npos;
    }

    bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

    std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* value)
    {
        wchar_t buf[1024]; DWORD sz = sizeof(buf);
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
        return buf;
    }

    //! <dir of a path>\..\oculus-diagnostics\OculusDebugToolCLI.exe: the runtime manifest sits in
    //! <install>\Support\oculus-runtime\, the tool in <install>\Support\oculus-diagnostics\.
    std::wstring FromManifest(std::wstring m)
    {
        size_t s = m.find_last_of(L"\\/"); if (s == std::wstring::npos) return {};
        m.resize(s); s = m.find_last_of(L"\\/"); if (s == std::wstring::npos) return {};
        m.resize(s);
        return m + L"\\oculus-diagnostics\\OculusDebugToolCLI.exe";
    }

    std::wstring FindTool()
    {
        std::vector<std::wstring> c;
        wchar_t env[1024];
        if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 1024)) c.push_back(FromManifest(env));
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
            if (std::wstring r = RegStr(root, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime"); !r.empty()) c.push_back(FromManifest(r));
        if (std::wstring b = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Oculus VR, LLC\\Oculus", L"Base"); !b.empty())
        {
            if (b.back() != L'\\') b += L'\\';
            c.push_back(b + L"Support\\oculus-diagnostics\\OculusDebugToolCLI.exe");
        }
        if (GetEnvironmentVariableW(L"ProgramFiles", env, 1024))
        {
            c.push_back(std::wstring(env) + L"\\Meta Horizon\\Support\\oculus-diagnostics\\OculusDebugToolCLI.exe");
            c.push_back(std::wstring(env) + L"\\Oculus\\Support\\oculus-diagnostics\\OculusDebugToolCLI.exe");
        }
        for (const std::wstring& p : c) if (!p.empty() && FileExists(p)) return p;
        return {};
    }

    //! Runs the tool on a script file; its output (it prints what each command did) comes back in `out`.
    bool RunTool(const std::wstring& exe, const std::wstring& script, std::string& out, DWORD waitMs)
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe + L"\" -f \"" + script + L"\"";
        BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        if (!ok) { CloseHandle(rd); return false; }
        const ULONGLONG until = GetTickCount64() + waitMs;
        char buf[512];
        for (;;)
        {
            DWORD avail = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail)
            {
                DWORD n = 0; if (!ReadFile(rd, buf, (DWORD)std::min<size_t>(sizeof(buf), avail), &n, nullptr) || !n) break;
                if (out.size() < 4096) out.append(buf, n);
            }
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
            {
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail)
                {
                    DWORD n = 0; if (!ReadFile(rd, buf, (DWORD)std::min<size_t>(sizeof(buf), avail), &n, nullptr) || !n) break;
                    if (out.size() < 4096) out.append(buf, n);
                }
                break;
            }
            if (GetTickCount64() > until) { TerminateProcess(pi.hProcess, 1); out += " [timed out]"; break; }
        }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
        return true;
    }

    std::string OneLine(std::string s)
    {
        std::replace(s.begin(), s.end(), '\r', ' '); std::replace(s.begin(), s.end(), '\n', ' '); std::replace(s.begin(), s.end(), '\t', ' ');
        return s;
    }

    DWORD WINAPI Worker(LPVOID)
    {
        const std::wstring exe = FindTool();
        if (exe.empty())
        {
            LOGI("MetaAsw: Meta's OculusDebugToolCLI.exe was not found: ASW stays as Meta has it");
            { std::lock_guard<std::mutex> lk(g_M); g_Last = "no tool"; }
            g_Busy = false; return 0;
        }
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const std::wstring script = std::wstring(tmp) + L"DOTOVR_asw.txt";
        const bool off = g_Mode.load() != 0;
        // the override is for the app in VR focus: right after FOCUSED the runtime may not have it yet ("pid 0"), so retry
        for (int attempt = 0; attempt < 4; attempt++)
        {
            if (attempt) Sleep(2000);
            if (HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr); f != INVALID_HANDLE_VALUE)
            {
                const char* text = off ? "server: asw.OverrideFocusedApp off\nexit\n" : "server: asw.OverrideFocusedApp default\nexit\n";
                DWORD n = 0; WriteFile(f, text, (DWORD)strlen(text), &n, nullptr); CloseHandle(f);
            }
            else break;
            std::string out;
            g_Runs++;
            const bool ran = RunTool(exe, script, out, 8000);
            const bool failed = !ran || out.find("ailed") != std::string::npos || out.find("timed out") != std::string::npos;
            { std::lock_guard<std::mutex> lk(g_M); g_Last = failed ? "failed" : (off ? "off for this game" : "Meta's default"); }
            LOGI("MetaAsw: ASW {} for the app in VR focus (attempt {}): {}", off ? "off" : "back to default", attempt + 1, ran ? OneLine(out) : "the tool did not start");
            if (!failed) { g_Ok++; break; }
            g_Fail++;
        }
        DeleteFileW(script.c_str());
        g_Busy = false;
        return 0;
    }

    std::atomic<int> g_Applied{ -1 };   //!< the mode last applied in this session (-1 = none)
}

void MetaAsw::SetMode(int mode)
{
    g_Mode = mode ? 1 : 0;
    LOGI("MetaAsw: {}", mode ? "Meta's ASW off for this game (applied when the headset session is in focus)" : "Meta's ASW left to Meta");
    if (g_Applied.load() >= 0 && g_Applied.load() != g_Mode.load()) g_Applied = -2;   // re-apply at the next FOCUSED
}

int MetaAsw::Mode() { return g_Mode.load(); }

void MetaAsw::OnFocused(const std::string& runtimeName)
{
    if (!IsMeta(runtimeName)) return;
    // mode 0 at startup: nothing to undo, Meta's default is in force; only a switch back from 1 needs the tool
    if (g_Mode.load() == 0 && g_Applied.load() == -1) return;
    if (g_Busy.exchange(true)) return;
    g_Applied = g_Mode.load();
    if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(t);
    else g_Busy = false;
}

std::string MetaAsw::Status()
{
    std::lock_guard<std::mutex> lk(g_M);
    return fmt::format("metaasw mode {} last '{}' runs {} ok {} fail {}", g_Mode.load(), g_Last, g_Runs.load(), g_Ok.load(), g_Fail.load());
}
