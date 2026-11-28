// Fastmem (fastmem.h): the game's address space as a flat block of host memory.
#include "fastmem.h"

#include <atomic>

#include "common/logging/log.h"
#include "core/memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#endif

namespace Port::Fastmem {

namespace {

std::atomic<u64> s_faults{0}, s_cached{0}, s_other{0}, s_unmapped{0}, s_maps{0}, s_unmaps{0};

} // namespace

Stats TakeStats() {
    Stats s;
    s.faults = s_faults.exchange(0);
    s.cached = s_cached.exchange(0);
    s.other_memory = s_other.exchange(0);
    s.unmapped = s_unmapped.exchange(0);
    s.maps = s_maps.exchange(0);
    s.unmaps = s_unmaps.exchange(0);
    return s;
}

#ifdef _WIN32

namespace {

// ── Windows 10 1803's placeholder API, looked up at run time ──────────────────────────────
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

VirtualAlloc2Fn pVirtualAlloc2 = nullptr;
MapViewOfFile3Fn pMapViewOfFile3 = nullptr;
UnmapViewOfFile2Fn pUnmapViewOfFile2 = nullptr;

bool LoadApi() {
    static const bool loaded = [] {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) {
            kb = LoadLibraryW(L"kernelbase.dll");
        }
        if (!kb) {
            return false;
        }
        pVirtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2"));
        pMapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3"));
        pUnmapViewOfFile2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kb, "UnmapViewOfFile2"));
        return pVirtualAlloc2 && pMapViewOfFile3 && pUnmapViewOfFile2;
    }();
    return loaded;
}

constexpr std::size_t PAGE = 4096;
constexpr u32 GUEST_PAGES = 1u << 20;              // 4 GiB of 4 KiB pages
constexpr u32 ARENA_PAGES = GUEST_PAGES + 16;      // and 64 KiB past the end, for an access
                                                   // that runs over 0xFFFFFFFF
constexpr u32 NO_PAGE = 0xFFFFFFFFu;
constexpr u32 SCRATCH_PAGES = 4;                   // pages one instruction can fault on at once
constexpr std::size_t SECTION_SIZE = Memory::RAM_BLOCK_SIZE + SCRATCH_PAGES * PAGE;
constexpr std::size_t WINDOW = 128;                // bytes an instruction can reach from where it
                                                   // faulted (the widest copy is 32 words)

enum : u8 { NONE = 0, MAPPED = 1, TEMPORARY = 2 };

bool g_enabled = false;
HANDLE g_section = nullptr;
u8* g_view = nullptr;   // the section where Azahar reaches the RAM
u8* g_arena = nullptr;  // the flat block
std::vector<u8> g_state;      // per arena page
std::vector<u32> g_offset;    // per guest page: its section page while MAPPED
std::map<u32, u32> g_holes;   // the arena's placeholders: first page -> end page
std::shared_ptr<Memory::PageTable> g_table;
Memory::MemorySystem* g_memory = nullptr;
std::recursive_mutex g_mutex;
PVOID g_handler = nullptr;

// ── the arena's pages ──────────────────────────────────────────────────────────────────

/// The section page `pointer` is the start of, or NO_PAGE when it isn't one of the RAM's.
u32 SectionPage(const u8* pointer) {
    if (!g_view || pointer < g_view || pointer >= g_view + Memory::RAM_BLOCK_SIZE) {
        return NO_PAGE;
    }
    const std::size_t offset = static_cast<std::size_t>(pointer - g_view);
    return offset % PAGE == 0 ? static_cast<u32>(offset / PAGE) : NO_PAGE;
}

/// Makes arena page `page` a placeholder of its own (splitting the one it is in), so a view can
/// replace it exactly.
bool IsolateHole(u32 page) {
    auto it = g_holes.upper_bound(page);
    if (it == g_holes.begin()) {
        return false;
    }
    --it;
    const u32 lo = it->first, hi = it->second;
    if (page < lo || page >= hi) {
        return false;
    }
    if (lo < page) {
        if (!VirtualFree(g_arena + std::size_t{lo} * PAGE, std::size_t{page - lo} * PAGE,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            return false;
        }
        it->second = page;
        it = g_holes.emplace(page, hi).first;
    }
    if (page + 1 < hi) {
        if (!VirtualFree(g_arena + std::size_t{page} * PAGE, PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            return false;
        }
        it->second = page + 1;
        g_holes.emplace(page + 1, hi);
    }
    g_holes.erase(page);
    return true;
}

bool MapView(u32 page, u64 section_offset) {
    if (!IsolateHole(page)) {
        LOG_CRITICAL(Core_ARM11, "fastmem: no placeholder at page {:05X}", page);
        return false;
    }
    void* at = g_arena + std::size_t{page} * PAGE;
    void* view = pMapViewOfFile3(g_section, GetCurrentProcess(), at, section_offset, PAGE,
                                 MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (view != at) {
        LOG_CRITICAL(Core_ARM11, "fastmem: can't map page {:05X} (error {})", page, GetLastError());
        g_holes.emplace(page, page + 1);
        return false;
    }
    s_maps.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void UnmapView(u32 page) {
    if (!pUnmapViewOfFile2(GetCurrentProcess(), g_arena + std::size_t{page} * PAGE, MEM_PRESERVE_PLACEHOLDER)) {
        LOG_CRITICAL(Core_ARM11, "fastmem: can't unmap page {:05X} (error {})", page, GetLastError());
        return;
    }
    g_holes.emplace(page, page + 1);
    g_state[page] = NONE;
    s_unmaps.fetch_add(1, std::memory_order_relaxed);
}

/// Puts guest page `page` in the arena as the page table has it: the view of its RAM page, or
/// nothing. A page the fault handler is using is left to it (it applies this when done).
void Apply(u32 page, const u8* pointer) {
    if (page >= GUEST_PAGES || g_state[page] == TEMPORARY) {
        return;
    }
    const u32 want = SectionPage(pointer);
    if (g_state[page] == MAPPED) {
        if (g_offset[page] == want) {
            return;
        }
        UnmapView(page);
    }
    if (want != NO_PAGE && MapView(page, u64{want} * PAGE)) {
        g_state[page] = MAPPED;
        g_offset[page] = want;
    }
}

void UnmapAll() {
    for (u32 page = 0; page < GUEST_PAGES; ++page) {
        if (g_state[page] != NONE) {
            UnmapView(page);
        }
    }
}

void Resync() {
    auto& raw = g_table->GetPointerArray();
    for (u32 page = 0; page < GUEST_PAGES; ++page) {
        Apply(page, raw[page]);
    }
}

void OnPageChange(const Memory::PageTable::Pointers* pointers, std::size_t idx, u8* pointer) {
    if (!g_table || pointers != &g_table->pointers) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (idx >= GUEST_PAGES) {
        Resync();
    } else {
        Apply(static_cast<u32>(idx), pointer);
    }
}

// ── the fault handler ─────────────────────────────────────────────────────────────────

enum class Kind : u8 { Cached, OtherMemory, Api, Unmapped };

struct Pending {
    u32 page;
    u32 vaddr;      // the window's start
    u32 length;
    Kind kind;
    u8* source;     // OtherMemory: where the bytes live
    u8 before[WINDOW];
};

// one instruction's worth, on the thread that faulted (one emulation thread)
thread_local std::array<Pending, SCRATCH_PAGES> t_pending;
thread_local u32 t_pending_count = 0;

u8* ScratchHost(u32 slot) {
    return g_view + Memory::RAM_BLOCK_SIZE + std::size_t{slot} * PAGE;
}

/// An access to arena offset `offset` faulted: stands the page in with a scratch page holding
/// what the access should see, for one instruction. False if this isn't ours to handle.
bool BeginFault(u64 offset) {
    std::scoped_lock lock{g_mutex};
    if (!g_table || !g_memory || t_pending_count >= SCRATCH_PAGES) {
        return false;
    }
    const u32 page = static_cast<u32>(offset / PAGE);
    if (page < GUEST_PAGES && g_state[page] == MAPPED) {
        // mapped since (the GPU's cache let go of it): the instruction can just run again, but
        // the single step still has to be taken
        Pending& p = t_pending[t_pending_count++];
        p = {};
        p.page = NO_PAGE;
        return true;
    }
    if (page < ARENA_PAGES && g_state[page] == TEMPORARY) {
        return false; // can't fault on a page that is there
    }
    const u32 slot = t_pending_count;
    Pending& p = t_pending[slot];
    p.page = page;
    p.source = nullptr;
    u8* scratch = ScratchHost(slot);
    const u64 page_start = u64{page} * PAGE;
    const u64 start = std::max<u64>(page_start, offset & ~u64{3});
    p.vaddr = static_cast<u32>(start);
    p.length = static_cast<u32>(std::min<u64>(WINDOW, page_start + PAGE - start));
    u8* window = scratch + (start - page_start);

    if (page >= GUEST_PAGES) {
        p.kind = Kind::Unmapped;
    } else {
        const auto type = g_table->attributes[page];
        u8* raw = g_table->GetPointerArray()[page];
        if (type == Memory::PageType::RasterizerCachedMemory ||
            type == Memory::PageType::RasterizerCachedMemoryWatchpoint) {
            p.kind = Kind::Cached;
        } else if (raw) {
            p.kind = Kind::OtherMemory;
            p.source = raw + (start - page_start);
        } else if (type == Memory::PageType::Unmapped && !(p.vaddr & 0x80000000u)) {
            p.kind = Kind::Unmapped;
        } else {
            p.kind = Kind::Api; // watchpoints, Luma's alias of physical memory: Azahar's own Read
        }
    }
    switch (p.kind) {
    case Kind::Cached: {
        g_memory->RasterizerFlushVirtualRegion(p.vaddr, p.length, Memory::FlushMode::Flush);
        auto ref = g_memory->GetCachedPagePointer(p.vaddr);
        std::memcpy(window, ref.GetPtr(), p.length);
        s_cached.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case Kind::OtherMemory:
        std::memcpy(window, p.source, p.length);
        s_other.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::Api:
        for (u32 i = 0; i < p.length; i += 4) {
            const u32 v = g_memory->Read32(p.vaddr + i);
            std::memcpy(window + i, &v, 4);
        }
        s_other.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::Unmapped:
        std::memset(window, 0, p.length);
        if (s_unmapped.fetch_add(1, std::memory_order_relaxed) < 20) {
            LOG_ERROR(HW_Memory, "fastmem: access to unmapped memory @ 0x{:08X}", p.vaddr);
        }
        break;
    }
    if (page < GUEST_PAGES && g_state[page] == MAPPED) {
        // flushing the GPU's cache let go of the page, which is now in the arena: run again
        p.page = NO_PAGE;
        ++t_pending_count;
        return true;
    }
    std::memcpy(p.before, window, p.length);
    if (!MapView(page, Memory::RAM_BLOCK_SIZE + u64{slot} * PAGE)) {
        return false;
    }
    g_state[page] = TEMPORARY;
    ++t_pending_count;
    s_faults.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/// The single-stepped instruction ran: what it wrote goes where Azahar would have put it, and the
/// pages go back to what the page table says.
void EndFaults() {
    std::scoped_lock lock{g_mutex};
    for (u32 slot = 0; slot < t_pending_count; ++slot) {
        Pending& p = t_pending[slot];
        if (p.page == NO_PAGE) {
            continue;
        }
        const u8* window = ScratchHost(slot) + (p.vaddr - u64{p.page} * PAGE);
        u32 first = p.length, last = 0;
        for (u32 i = 0; i < p.length; ++i) {
            if (window[i] != p.before[i]) {
                first = std::min(first, i);
                last = i + 1;
            }
        }
        if (first < last) {
            switch (p.kind) {
            case Kind::Cached: {
                auto ref = g_memory->GetCachedPagePointer(p.vaddr + first);
                std::memcpy(ref.GetPtr(), window + first, last - first);
                g_memory->RasterizerFlushVirtualRegion(p.vaddr + first, last - first,
                                                       Memory::FlushMode::Invalidate);
                break;
            }
            case Kind::OtherMemory:
                std::memcpy(p.source + first, window + first, last - first);
                break;
            case Kind::Api:
                for (u32 i = first & ~3u; i < last; i += 4) {
                    u32 v;
                    std::memcpy(&v, window + i, 4);
                    g_memory->Write32(p.vaddr + i, v);
                }
                break;
            case Kind::Unmapped:
                if (s_unmapped.load(std::memory_order_relaxed) < 20) {
                    LOG_ERROR(HW_Memory, "fastmem: write to unmapped memory @ 0x{:08X}", p.vaddr + first);
                }
                break;
            }
        }
        UnmapView(p.page);
        if (p.page < GUEST_PAGES) {
            Apply(p.page, g_table->GetPointerArray()[p.page]);
        }
    }
    t_pending_count = 0;
}

constexpr DWORD TRAP_FLAG = 0x100;

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_arena && record->NumberParameters >= 2) {
        const auto address = static_cast<uintptr_t>(record->ExceptionInformation[1]);
        const auto base = reinterpret_cast<uintptr_t>(g_arena);
        if (address < base || address >= base + std::size_t{ARENA_PAGES} * PAGE) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // Azahar's code expects the host's default rounding, not the guest's
        const unsigned csr = _mm_getcsr();
        _mm_setcsr(0x1F80);
        const bool handled = BeginFault(address - base);
        _mm_setcsr(csr);
        if (!handled) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        info->ContextRecord->EFlags |= TRAP_FLAG;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pending_count > 0) {
        const unsigned csr = _mm_getcsr();
        _mm_setcsr(0x1F80);
        EndFaults();
        _mm_setcsr(csr);
        info->ContextRecord->EFlags &= ~TRAP_FLAG;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── the RAM's section ────────────────────────────────────────────────────────────────

u8* AllocateRam(std::size_t size) {
    if (size != Memory::RAM_BLOCK_SIZE || g_view) {
        return nullptr;
    }
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(u64{SECTION_SIZE} >> 32),
                                        static_cast<DWORD>(SECTION_SIZE & 0xFFFFFFFFu), nullptr);
    if (!section) {
        LOG_ERROR(Core_ARM11, "fastmem: no memory section for the RAM (error {})", GetLastError());
        return nullptr;
    }
    auto* view = static_cast<u8*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, SECTION_SIZE));
    if (!view) {
        LOG_ERROR(Core_ARM11, "fastmem: can't map the RAM's section (error {})", GetLastError());
        CloseHandle(section);
        return nullptr;
    }
    std::scoped_lock lock{g_mutex};
    g_section = section;
    g_view = view;
    LOG_INFO(Core_ARM11, "fastmem: the RAM is in a shared section ({} MiB)", SECTION_SIZE >> 20);
    return view;
}

void ReleaseRam(u8* block, std::size_t) {
    std::scoped_lock lock{g_mutex};
    if (block != g_view) {
        return;
    }
    if (g_arena) {
        UnmapAll();
    }
    g_table.reset();
    g_memory = nullptr;
    UnmapViewOfFile(g_view);
    CloseHandle(g_section);
    g_view = nullptr;
    g_section = nullptr;
}

} // namespace

bool Available() {
    return LoadApi();
}

bool Enable(std::string& error) {
    if (!LoadApi()) {
        error = "this version of Windows can't map memory the way the code library needs (Windows 10 "
                "version 1803 or newer)";
        return false;
    }
    if (!g_handler) {
        g_handler = AddVectoredExceptionHandler(1, Handler);
        if (!g_handler) {
            error = "can't install the memory fault handler";
            return false;
        }
    }
    Memory::PageTable::Pointers::on_change = OnPageChange;
    Memory::SetRamAllocator({AllocateRam, ReleaseRam});
    g_enabled = true;
    return true;
}

void Disable() {
    Memory::SetRamAllocator({});
    g_enabled = false;
}

bool Enabled() {
    return g_enabled;
}

u8* Bind(const std::shared_ptr<Memory::PageTable>& table, Memory::MemorySystem& memory) {
    std::scoped_lock lock{g_mutex};
    if (!g_enabled || !g_view || !table) {
        return nullptr;
    }
    if (!g_arena) {
        g_arena = static_cast<u8*>(pVirtualAlloc2(GetCurrentProcess(), nullptr, std::size_t{ARENA_PAGES} * PAGE,
                                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                                  nullptr, 0));
        if (!g_arena) {
            LOG_ERROR(Core_ARM11, "fastmem: can't reserve 4 GiB of address space (error {})", GetLastError());
            return nullptr;
        }
        g_state.assign(ARENA_PAGES, NONE);
        g_offset.assign(GUEST_PAGES, NO_PAGE);
        g_holes.clear();
        g_holes.emplace(0, ARENA_PAGES);
        LOG_INFO(Core_ARM11, "fastmem: the game's address space at {}", static_cast<void*>(g_arena));
    }
    g_memory = &memory;
    if (g_table != table) {
        // The block follows the game's process: the recompiled code is its code. Another process
        // (only ever interpreted) taking the CPU for a while doesn't remap 4 GiB; the block moves
        // on only once the old process is gone (nothing but this holds its table any more).
        if (g_table && g_table.use_count() > 1) {
            static bool told = false;
            if (!told) {
                told = true;
                LOG_WARNING(Core_ARM11, "fastmem: another process's page table; the block stays the game's");
            }
            return g_arena;
        }
        if (g_table) {
            UnmapAll();
            LOG_INFO(Core_ARM11, "fastmem: following a new process's page table");
        }
        g_table = table;
        Resync();
    }
    return g_arena;
}

#else // !_WIN32

bool Available() {
    return false;
}

bool Enable(std::string& error) {
    error = "fastmem code libraries are for Windows only";
    return false;
}

void Disable() {}

bool Enabled() {
    return false;
}

u8* Bind(const std::shared_ptr<Memory::PageTable>&, Memory::MemorySystem&) {
    return nullptr;
}

#endif

} // namespace Port::Fastmem
