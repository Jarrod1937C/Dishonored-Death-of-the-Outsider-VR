#include "pch.h"
#include "Undetected.h"
#include "BodyHide.h"
#include "GameInfo.h"
#include <detours/detours.h>
#include <atomic>
#include <cstring>

namespace
{
    // DOTO (re-measured 2026-11-07): the relationship is a faction matrix -- two bits per pair of factions (0 ally,
    // 1 neutral, 2 enemy, 3 undefined), each entity's faction a word at +0x2C0 (+0x2C2 used for the regarded one when
    // the regarding one has a flag set: a disguise) -- and there are no per-entity entries, so no setter to hold off
    // (Dishonored 2's exe+0x605990 has no counterpart). Every question "how does A regard B" goes through
    //   exe+0x3A4AD0 (manager, entity A, entity B)  -- Dishonored 2's exe+0x604C60 took ids; the id version here,
    //                 exe+0x3A4BA0 (manager, id A, id B), resolves them and calls it; the game's wrapper exe+0x917370
    //                 (gameLocal, A, B; manager gameLocal+0x1F8340) is called from 77 places (the AI's)
    //   exe+0x3A4C10 (manager, entity A, entity B)  -- the same on both entities' +0x2C2 (wrapper exe+0x9173A0, 9 callers)
    // Both are hooked the same way. The entity id is at +0xB0 (D2 +0xA8: it read 0x80000000 for the player).
    constexpr uintptr_t kGetRelationship = 0x3A4AD0;    // int (void* manager, void* entity A, void* entity B): A regarding B (D2 0x604C60, by id)
    constexpr uintptr_t kGetRelationship2 = 0x3A4C10;   // the same on the +0x2C2 factions (no D2 counterpart)
    constexpr int kNeutral = 1, kEnemy = 2;   // (0 ally, 3 undefined)
    constexpr uint32_t kNoId = 0x3FFF0010u;
    constexpr size_t kEntityId = 0xB0;
    using FnRel = int (*)(void*, void*, void*);
    FnRel g_Real = nullptr, g_Real2 = nullptr;
    std::atomic<bool> g_On{ false };
    std::atomic<uint32_t> g_PlayerId{ 0 };
    std::atomic<unsigned long long> g_Masked{ 0 }, g_Blocked{ 0 }, g_PlayerRegards{ 0 };
    unsigned long long g_LoggedMasked = 0; double g_LogAt = 0;

    uint32_t ReadId(void* e)
    {
        if (!e) return 0;
        __try { return *(const uint32_t*)((const uint8_t*)e + kEntityId); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    int Mask(int r, void* a, void* b)
    {
        if (r != kEnemy || !g_On.load(std::memory_order_relaxed) || !a || !b || a == b) return r;
        const uint32_t pid = g_PlayerId.load(std::memory_order_relaxed);
        if (!pid || pid == kNoId) return r;
        if (ReadId(b) == pid) { g_Masked.fetch_add(1, std::memory_order_relaxed); return kNeutral; }
        if (ReadId(a) == pid) g_PlayerRegards.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    int Hook(void* mgr, void* a, void* b) { return Mask(g_Real(mgr, a, b), a, b); }
    int Hook2(void* mgr, void* a, void* b) { return Mask(g_Real2(mgr, a, b), a, b); }
}

namespace
{
    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }

}

bool Undetected::Install()
{
    if (g_Real || !GameInfo::IsSupportedBuild()) return g_Real != nullptr;
    const uint8_t* p = (const uint8_t*)(GameInfo::Base() + kGetRelationship);
    static const uint8_t kExpect[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
    if (p[0] != 0xE9 && memcmp(p, kExpect, sizeof kExpect) != 0) { LOGI("Undetected: exe+0x{:X} is not the relationship lookup (another game build?): the cheat does nothing", kGetRelationship); return false; }
    g_Real = (FnRel)(GameInfo::Base() + kGetRelationship);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) g_Real = nullptr;
    LOGI("Undetected: the relationship lookup exe+0x{:X} hooked (detour={}): while the cheat is on, nobody regards you as an enemy", kGetRelationship, r);
    const uint8_t* q = (const uint8_t*)(GameInfo::Base() + kGetRelationship2);
    static const uint8_t kExpect2[] = { 0x0F, 0xB7, 0x92, 0xC2, 0x02, 0x00, 0x00, 0x41, 0x0F, 0xB7, 0x80, 0xC2, 0x02, 0x00, 0x00 };
    if (g_Real && (q[0] == 0xE9 || memcmp(q, kExpect2, sizeof kExpect2) == 0))
    {
        g_Real2 = (FnRel)(GameInfo::Base() + kGetRelationship2);
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Real2, Hook2);
        const LONG r2 = DetourTransactionCommit();
        if (r2 != NO_ERROR) g_Real2 = nullptr;
        LOGI("Undetected: the second relationship lookup exe+0x{:X} hooked (detour={})", kGetRelationship2, r2);
    }
    else if (g_Real) LOGI("Undetected: exe+0x{:X} is not the second relationship lookup: only the first is hooked", kGetRelationship2);
    return g_Real != nullptr;
}
void Undetected::SetEnabled(bool on)
{
    g_On = on;
    LOGI("Undetected: {}", on ? "on: guards and hostile people regard you as neutral (they see you, and do not attack or hunt you)" : "off");
}
bool Undetected::Enabled() { return g_On.load(); }
void Undetected::Tick()
{
    BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
    if (an[BodyHide::aPlayer].valid && an[BodyHide::aPlayer].ent)
    {
        const uint32_t id = ReadId(an[BodyHide::aPlayer].ent);
        if (id && id != g_PlayerId.load())
        {
            g_PlayerId = id;
            LOGI("Undetected: the player's entity id {:#x}", id);
        }
    }
    if (!g_On.load() || !Log::Enabled()) return;
    const double now = Now();
    if (now - g_LogAt < 30.0) return;
    g_LogAt = now;
    const unsigned long long m = g_Masked.load();
    if (m != g_LoggedMasked)
    {
        LOGI("Undetected: in the last 30 s, {} reads of someone regarding you as an ENEMY turned to neutral ({} in all; the player regarding others as enemies: {}, left alone)",
             m - g_LoggedMasked, m, g_PlayerRegards.load());
        g_LoggedMasked = m;
    }
}
std::string Undetected::Status()
{
    return fmt::format("undetected {} (enemy reads of you turned neutral {}, the player regarding others as enemies {})", g_On.load() ? "on" : "off", g_Masked.load(), g_PlayerRegards.load());
}
