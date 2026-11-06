#include "pch.h"
#include "KeyBinds.h"
#include <shlobj.h>
#include <map>
#include <algorithm>

namespace
{
    //! The game's key names in DirectInput order (the table sits in the exe next to "UPARROW"), with their DIK codes.
    //! A DIK code is the set-1 scan code, with 0x80 set for the E0-prefixed keys.
    struct Name { const char* n; uint8_t dik; };
    const Name kNames[] = {
        { "ESCAPE", 0x01 }, { "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 }, { "5", 0x06 }, { "6", 0x07 },
        { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0A }, { "0", 0x0B }, { "MINUS", 0x0C }, { "EQUALS", 0x0D },
        { "BACKSPACE", 0x0E }, { "TAB", 0x0F }, { "Q", 0x10 }, { "W", 0x11 }, { "E", 0x12 }, { "R", 0x13 }, { "T", 0x14 },
        { "Y", 0x15 }, { "U", 0x16 }, { "I", 0x17 }, { "O", 0x18 }, { "P", 0x19 }, { "LBRACKET", 0x1A }, { "RBRACKET", 0x1B },
        { "ENTER", 0x1C }, { "LCTRL", 0x1D }, { "A", 0x1E }, { "S", 0x1F }, { "D", 0x20 }, { "F", 0x21 }, { "G", 0x22 },
        { "H", 0x23 }, { "J", 0x24 }, { "K", 0x25 }, { "L", 0x26 }, { "SEMICOLON", 0x27 }, { "APOSTROPHE", 0x28 },
        { "GRAVE", 0x29 }, { "LSHIFT", 0x2A }, { "BACKSLASH", 0x2B }, { "Z", 0x2C }, { "X", 0x2D }, { "C", 0x2E },
        { "V", 0x2F }, { "B", 0x30 }, { "N", 0x31 }, { "M", 0x32 }, { "COMMA", 0x33 }, { "PERIOD", 0x34 }, { "SLASH", 0x35 },
        { "RSHIFT", 0x36 }, { "KP_STAR", 0x37 }, { "LALT", 0x38 }, { "SPACE", 0x39 }, { "CAPSLOCK", 0x3A },
        { "F1", 0x3B }, { "F2", 0x3C }, { "F3", 0x3D }, { "F4", 0x3E }, { "F5", 0x3F }, { "F6", 0x40 }, { "F7", 0x41 },
        { "F8", 0x42 }, { "F9", 0x43 }, { "F10", 0x44 }, { "NUMLOCK", 0x45 }, { "SCROLL", 0x46 }, { "KP_7", 0x47 },
        { "KP_8", 0x48 }, { "KP_9", 0x49 }, { "KP_MINUS", 0x4A }, { "KP_4", 0x4B }, { "KP_5", 0x4C }, { "KP_6", 0x4D },
        { "KP_PLUS", 0x4E }, { "KP_1", 0x4F }, { "KP_2", 0x50 }, { "KP_3", 0x51 }, { "KP_0", 0x52 }, { "KP_DOT", 0x53 },
        { "OEM_102", 0x56 }, { "F11", 0x57 }, { "F12", 0x58 }, { "KP_ENTER", 0x9C }, { "RCTRL", 0x9D },
        { "KP_SLASH", 0xB5 }, { "RALT", 0xB8 }, { "HOME", 0xC7 }, { "UPARROW", 0xC8 }, { "PGUP", 0xC9 },
        { "LEFTARROW", 0xCB }, { "RIGHTARROW", 0xCD }, { "END", 0xCF }, { "DOWNARROW", 0xD0 }, { "PGDN", 0xD1 },
        { "INS", 0xD2 }, { "DEL", 0xD3 },
        // the generic modifier names the game also accepts
        { "CTRL", 0x1D }, { "SHIFT", 0x2A }, { "ALT", 0x38 },
    };

    std::mutex g_M;
    //! action -> its keys in the config's order, primaries first. An action is bound several times in one set: the
    //! keyboard key, then the pad button ("LSHIFT" and "PAD_LTHUMB" both SPRINT), plus a secondary key.
    struct Keys { std::vector<std::string> primary, secondary; };
    std::map<std::string, Keys> g_Act;
    unsigned g_Gen = 0;
    bool g_Read = false;
    std::wstring g_Path;
    FILETIME g_Stamp{};
    std::string g_Info = "not read";

    std::wstring ConfigPath()
    {
        if (!g_Path.empty()) return g_Path;
        PWSTR sg = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &sg)) && sg)
        {
            g_Path = std::wstring(sg) + L"\\Arkane Studios\\Dishonored_DO\\base\\dishonored_DOConfig.cfg";   // the name Dishonored_DO.exe writes (its string table)
            CoTaskMemFree(sg);
        }
        return g_Path;
    }
    std::string Upper(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)toupper(c); }); return s; }
    //! The quoted words of a config line: arkBind "W" "MOVE_FORWARD" -> {W, MOVE_FORWARD}.
    std::vector<std::string> Quoted(const std::string& line)
    {
        std::vector<std::string> out; size_t p = 0;
        while ((p = line.find('"', p)) != std::string::npos)
        {
            const size_t q = line.find('"', p + 1); if (q == std::string::npos) break;
            out.push_back(line.substr(p + 1, q - p - 1)); p = q + 1;
        }
        return out;
    }
}

bool KeyBinds::FromName(const std::string& gameName, Bind& out)
{
    const std::string n = Upper(gameName);
    out = Bind{}; out.name = n;
    if (n.size() == 6 && n.compare(0, 5, "MOUSE") == 0 && n[5] >= '1' && n[5] <= '5') { out.mouse = (uint8_t)(n[5] - '0'); return true; }
    if (n == "MOUSE_WHEEL_UP") { out.mouse = 6; return true; }
    if (n == "MOUSE_WHEEL_DOWN") { out.mouse = 7; return true; }
    for (const Name& k : kNames)
        if (n == k.n) { out.scan = k.dik & 0x7F; out.ext = (k.dik & 0x80) != 0; return true; }
    return false;
}

void KeyBinds::Reload()
{
    const std::wstring path = ConfigPath();
    std::map<std::string, Keys> act;
    FILE* f = path.empty() ? nullptr : _wfopen(path.c_str(), L"rb");
    std::lock_guard lk(g_M);
    g_Gen++;
    if (!f)
    {
        g_Act.clear(); g_Read = false;
        g_Info = "game config not found: the game's default keys are used";
        LOGI("KeyBinds: {} ({})", g_Info, std::string(path.begin(), path.end()));
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) g_Stamp = fad.ftLastWriteTime;
    std::string set, line; char buf[1024];
    while (fgets(buf, sizeof buf, f))
    {
        line = buf;
        const std::vector<std::string> q = Quoted(line);
        if (line.rfind("arkSwitchToBindingSet", 0) == 0 && !q.empty()) { set = Upper(q[0]); continue; }
        if (set != "IN_GAME" || q.size() < 2) continue;
        const bool secondary = line.rfind("arkBindSecondary", 0) == 0;
        if (!secondary && line.rfind("arkBind", 0) != 0) continue;
        Keys& k = act[Upper(q[1])];
        (secondary ? k.secondary : k.primary).push_back(Upper(q[0]));
    }
    fclose(f);
    g_Act = std::move(act); g_Read = true;
    g_Info = fmt::format("{} IN_GAME actions read from the game's config", g_Act.size());
    LOGI("KeyBinds: {}", g_Info);
}

void KeyBinds::Poll()
{
    const std::wstring path = ConfigPath(); if (path.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
    bool changed;
    { std::lock_guard lk(g_M); changed = CompareFileTime(&fad.ftLastWriteTime, &g_Stamp) != 0; }
    if (changed) { LOGI("KeyBinds: the game's config changed: reading the bindings again"); Reload(); }
}

bool KeyBinds::ForAction(const char* action, Bind& out)
{
    std::lock_guard lk(g_M);
    if (!g_Read) return false;
    auto it = g_Act.find(action);
    if (it == g_Act.end()) return false;
    // A pad button is not something we can press: the first key or mouse button, primaries before secondaries.
    for (const auto* list : { &it->second.primary, &it->second.secondary })
        for (const std::string& k : *list)
            if (FromName(k, out)) return true;
    return false;
}

unsigned KeyBinds::Generation() { std::lock_guard lk(g_M); return g_Gen; }
std::string KeyBinds::Info() { std::lock_guard lk(g_M); return g_Info; }
