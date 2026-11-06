#pragma once
#include <cstdint>

//! A short, ordered trace of the frame pipeline (`vr pipetrace <presents>`): when each main view is built, when the
//! draws pick a view up, when command lists are finished and executed, when the frame is presented -- with the thread
//! and a timestamp. It answers "which view is the image being presented drawn from", which a fixed guess got wrong
//! (measured 2026-10-04: alternating-eye stereo came out with the eyes swapped in one session and right in another).
namespace PipeTrace
{
    enum Kind : char { View = 'V', Xform = 'R', Finish = 'F', Execute = 'X', Present = 'P', Submit = 'S' };
    void Start(int presents);
    bool On();
    void Event(Kind k, uint64_t a = 0, uint64_t b = 0);
    //! Called at every Present: counts down and writes the trace to the log when done.
    void OnPresent();
}
