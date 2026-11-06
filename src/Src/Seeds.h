#pragma once

//! Learned hand data shipped with the mod (tools/make_seeds.py): how each hand sits on each weapon (DVR_grips.txt) and
//! the hand socket (DVR_hands.txt), learned on the developer's machine. Keyed by skeleton and mesh, i.e. by the game's
//! own data, so they are valid on any machine with the same game build. Written to the game folder only when the file
//! is not there: what the player's game learns afterwards replaces them as usual, and an update never overwrites it.
namespace Seeds
{
    void Install();   //!< InitThread, before anything reads either file
}
