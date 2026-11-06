#pragma once
#include <cstdint>

//! Game-side accessors resolved from RE of Dishonored_DO.exe (ported from Dishonored 2 1.77.9.0 by matching the code,
//! 2026-11-05; see docs/ENGINE.md).
//! Everything is null-safe: returns false when not in gameplay.
namespace Game
{
    // Static addresses (RVAs) — build-specific.
    constexpr uint32_t kGameLocalPtr   = 0x3A72280;   //!< idGameLocal* (global pointer)
    constexpr uint32_t kGetLocalPlayer = 0x917230;    //!< idPlayer* GetLocalPlayer(idGameLocal*)
    constexpr uint32_t kCmdSystemPtr   = 0x234CA60;   //!< idCmdSystem** ; vtable +0x10 = AddCommand
    constexpr uint32_t kCmdGetViewpos  = 0x8F4140;
    constexpr uint32_t kCmdSetViewpos  = 0x8F4CA0;
    // idPlayer fields (from Cmd_GetViewpos_f)
    constexpr uint32_t kPlayerViewOrigin = 0x310;     //!< idVec3 firstPersonViewOrigin (D2 0x300)
    constexpr uint32_t kPlayerViewAxis   = 0x31C;     //!< idMat3 firstPersonViewAxis (row-major, 9 floats) (D2 0x30C)

    void*  GameLocal();
    void*  LocalPlayer();
    //! Reads the first-person view origin (3) and axis (9, rows = forward/left/up in id Tech).
    bool   PlayerView(float origin[3], float axis[9]);

    // idCmdSystem vtable (measured 1.77.9.0, the same in DOTO): 2 AddCommand, 8 ExecuteCommandText(text) [immediate,
    // calling thread], 9 BufferCommandText(text) [queued, main thread], 10 ExecuteCommandBuffer.
    constexpr int kCmdSlotExecuteText = 8;
    constexpr int kCmdSlotBufferText  = 9;
    bool   BufferCommandText(const char* text);   //!< queue a console command line (thread-safe enough)
}
