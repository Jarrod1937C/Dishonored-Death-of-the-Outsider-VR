#pragma once
#include <cstdint>
#include <string>

//! The keys the game listens to, as the game names them. DOTO reads the keyboard through DirectInput, i.e.
//! by the key's PHYSICAL position (DIK codes = set-1 scan codes): its "W" is the key left of E on any layout. So a key
//! is sent by that scan code, never through the Windows layout -- on a French AZERTY layout the layout's 'W' is the key
//! the game calls "Z" (zoom). And the player may have rebound an action: the bindings are read from the game's own
//! config (Saved Games\Arkane Studios\Dishonored_DO\base\dishonored_DOConfig.cfg, the IN_GAME set) so the controllers press
//! whatever key the game expects for that action.
namespace KeyBinds
{
    struct Bind
    {
        uint16_t scan = 0;       //!< keyboard scan code (0 = not a key)
        bool ext = false;        //!< E0-prefixed (arrows, right ctrl/alt, ...)
        uint8_t mouse = 0;       //!< 1..5 mouse button, 6 wheel up, 7 wheel down (0 = not the mouse)
        std::string name;        //!< the game's name for it ("W", "LCTRL", "MOUSE1")
        bool valid() const { return scan || mouse; }
    };
    bool FromName(const std::string& gameKeyName, Bind& out);   //!< "W", "LCTRL", "MOUSE1"... (case-insensitive)
    //! The key the game has bound to an IN_GAME action ("JUMP", "WEAP_1"...): primary, else secondary, among those that
    //! can be sent. False when the config could not be read or the action has no sendable key (e.g. only a pad button).
    bool ForAction(const char* action, Bind& out);
    void Reload();                 //!< (re)read the config (cheap; also called when its time stamp changes)
    void Poll();                   //!< re-read if the file changed since the last read (call at a few Hz at most)
    unsigned Generation();         //!< bumps every time the bindings were (re)read
    std::string Info();            //!< for the log / heartbeat
}
