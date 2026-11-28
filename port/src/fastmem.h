// Fastmem: the game's address space as one flat 4 GiB block of host memory, so recompiled code
// built for it (rc_build_mode 3, RC_FASTMEM) reaches guest address A at base + A with no check.
//
// The emulated RAM lives in one shared memory section (Memory::SetRamAllocator); each 4 KiB page
// of the game's page table that points into it is a view of that section at its place in the
// block, kept up to date as Azahar changes the table (PageTable::Pointers::on_change). Every
// other page (unmapped, watched by the GPU's cache, or backed by memory outside the RAM, such as
// the shared page) is left out of the block: an access to it faults, and the fault handler does
// what Azahar's slow path would have done (flush or invalidate the GPU's cache, read or write the
// right memory), through a scratch page and one single-stepped instruction.
//
// Windows only (10 1803 and newer, for its placeholder mappings); elsewhere Available() is false
// and code libraries stay on the page table (RC_PAGED).
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "common/common_types.h"

namespace Memory {
class MemorySystem;
struct PageTable;
} // namespace Memory

namespace Port::Fastmem {

/// Whether this system can: Windows 10 1803+ (VirtualAlloc2, MapViewOfFile3).
bool Available();

/// Gives the emulated RAM its shared section from the next emulated system on (call before the
/// system is created); false with the reason if this system can't.
bool Enable(std::string& error);

/// Back to ordinary RAM for the next system.
void Disable();

/// Whether Enable() is in effect.
bool Enabled();

/// The flat block mirroring `table` (Azahar's page table of the process the recompiled code runs),
/// or nullptr when fastmem isn't enabled or the RAM isn't in the section. The block follows one
/// table at a time: binding another one remaps it.
u8* Bind(const std::shared_ptr<Memory::PageTable>& table, Memory::MemorySystem& memory);

/// Counts since the last call, for trace_perf.
struct Stats {
    u64 faults = 0;          ///< accesses the fault handler did (by kind below)
    u64 cached = 0;          ///< to pages watched by the GPU's cache
    u64 other_memory = 0;    ///< to memory outside the RAM (shared page, config memory, ...)
    u64 unmapped = 0;        ///< to unmapped addresses (as Azahar, reads 0 and writes nothing)
    u64 maps = 0, unmaps = 0;  ///< pages put in or taken out of the block
};
Stats TakeStats();

} // namespace Port::Fastmem
