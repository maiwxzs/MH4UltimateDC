// trace_profile (mh4u.ini): a sampling profiler for the emulation thread, to find what holds the
// frame rate. A thread of its own looks at where the emulation thread is about a thousand times a
// second (Windows: SuspendThread, GetThreadContext, ResumeThread; nothing is allocated while it is
// stopped) and every trace_profile seconds the log says where the time went: by module (the port
// and Azahar's mh4u.exe, the code library, the graphics driver, Windows), by function of mh4u.exe
// (its names from mh4u.syms beside it, which the Windows package writes before it strips the exe)
// and by function of the game (the code library's, by their 3DS address). Elsewhere: nothing.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace Port::Profiler {

/// From the emulation thread, often (each CPU slice): starts sampling it the first time
/// `seconds` > 0, and every `seconds` logs and starts a new window. `guest_functions` gives the
/// code library's functions as (host address, 3DS address), asked once.
void Tick(float seconds, std::vector<std::pair<std::uintptr_t, std::uint32_t>> (*guest_functions)());

} // namespace Port::Profiler
