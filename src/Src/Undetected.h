#pragma once
#include <string>

//! [Cheats.Player] Undetected: guards and other hostile people do not regard you as an enemy -- walk around the world
//! in the open, seen, without being attacked or hunted. People who are not your enemies (civilians, shopkeepers) see
//! and react to you as usual.
//!
//! How (DOTO): every question "how does A regard B" goes through exe+0x3A4AD0(relationship manager, entity A,
//! entity B) -> 0 ALLY, 1 NEUTRAL, 2 ENEMY, 3 UNDEFINED, read from a matrix of factions (each entity's faction a word at
//! +0x2C0; exe+0x3A4C10 does the same on +0x2C2). Both are hooked: while the cheat is on, ENEMY from anyone regarding
//! the player reads NEUTRAL, so a guard turned against you is turned back the moment you switch it on. What the
//! player regards others as (A = the player) is left alone. (Dishonored 2 had per-entity entries and a setter,
//! exe+0x605990, also held off; DOTO has neither.)
namespace Undetected
{
    bool Install();
    void SetEnabled(bool on);
    bool Enabled();
    void Tick();   //!< the game thread, once a frame: the player's entity id
    std::string Status();
}
