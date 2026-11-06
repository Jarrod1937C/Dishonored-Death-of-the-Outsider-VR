#pragma once
#include <cstdint>
//! Research tool: hardware write-watchpoint (DR0) on a chosen thread + vectored exception
//! handler that logs the writing instruction (exe+RVA) for the first N hits, then disarms.
namespace Watchpoint
{
    void SetGameThreadId(uint32_t tid);   //!< thread whose debug registers we program
    uint32_t GameThreadId();
    bool Arm(uintptr_t address, int lengthBytes /*1,2,4,8*/, int maxHits, bool readWrite = false, bool allThreads = false);   //!< readWrite: also break on reads; allThreads: arm every thread (game logic runs off the main thread)
    void Disarm();
    void Dump();   //!< log the histogram of accessing instructions since the last Arm
}
