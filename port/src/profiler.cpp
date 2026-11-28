#include "profiler.h"

#if defined(_WIN32)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <fmt/format.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>

#include "common/logging/log.h"

namespace Port::Profiler {

namespace {

using Clock = std::chrono::steady_clock;

// A sample: where the emulation thread was, and the return addresses into mh4u.exe found on its
// stack above that (the nearest first): who called into Windows, the driver or the C library.
constexpr int CALLERS = 3;
struct Sample {
    std::uint64_t pc = 0;
    std::array<std::uint64_t, CALLERS> callers{};
};

// The samples: the sampler fills a fixed buffer (no allocation while the emulation thread is
// stopped, which may hold the heap's lock); the emulation thread swaps it out at each report.
constexpr std::size_t CAPACITY = 1 << 15;
struct Buffer {
    std::array<Sample, CAPACITY> samples{};
    std::size_t n = 0;
};
std::mutex lock;
Buffer* filling = nullptr;
Buffer buffers[2];
HANDLE target = nullptr;
std::atomic<bool> running{false};
Clock::time_point window_start{};
// mh4u.exe's code, for the stack's return addresses (set before the sampler starts)
std::uintptr_t exe_text_begin = 0, exe_text_end = 0;

// A return address: in mh4u.exe's code, right after a call (E8 rel32; FF /2 through a register
// or memory, 2 to 7 bytes long).
bool IsReturnAddress(std::uint64_t a) {
    if (a < exe_text_begin + 8 || a >= exe_text_end) {
        return false;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(a);
    if (p[-5] == 0xE8) {
        return true;
    }
    for (int len = 2; len <= 7; ++len) {
        const std::uint8_t* c = p - len;
        const std::uint8_t* op = (c[0] >= 0x40 && c[0] <= 0x4F) ? c + 1 : c;  // a REX prefix
        if (op[0] == 0xFF && ((op[1] >> 3) & 7) == 2) {
            return true;
        }
    }
    return false;
}

constexpr std::size_t STACK_SCAN = 64 * 1024;  // bytes of stack looked at above RSP

void Sampler() {
    timeBeginPeriod(1);
    std::uint64_t stack[STACK_SCAN / 8];
    while (running.load(std::memory_order_relaxed)) {
        Sleep(1);
        if (SuspendThread(target) == static_cast<DWORD>(-1)) {
            continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        const bool ok = GetThreadContext(target, &c) != 0;
        std::size_t words = 0;
        if (ok) {
            // the stack as it is now, copied while the thread stands still; read past its end
            // only as far as the memory is there
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(c.Rsp), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
                const auto region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                const std::size_t bytes = (std::min)(STACK_SCAN, static_cast<std::size_t>(region_end - c.Rsp));
                std::memcpy(stack, reinterpret_cast<const void*>(c.Rsp), bytes & ~std::size_t{7});
                words = bytes / 8;
            }
        }
        ResumeThread(target);
        if (!ok) {
            continue;
        }
        Sample s;
        s.pc = c.Rip;
        int found = 0;
        for (std::size_t i = 0; i < words && found < CALLERS; ++i) {
            if (IsReturnAddress(stack[i])) {
                s.callers[found++] = stack[i];
            }
        }
        std::lock_guard g(lock);
        if (filling && filling->n < CAPACITY) {
            filling->samples[filling->n++] = s;
        }
    }
    timeEndPeriod(1);
}

struct Module {
    std::uintptr_t base = 0, end = 0;
    std::string name;
    std::vector<std::pair<std::uintptr_t, std::string>> exports;  // by address
};

// A DLL's exported functions, from its export directory as loaded: Windows' own DLLs come
// without symbols, but their exports name the functions that matter (NtWaitForSingleObject,
// RtlAllocateHeap, ...).
void ReadExports(Module& m) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(m.base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) {
        return;
    }
    const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + ex->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + ex->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + ex->AddressOfFunctions);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        const DWORD rva = functions[ordinals[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
            continue;  // a forwarder, not code
        }
        m.exports.emplace_back(m.base + rva, reinterpret_cast<const char*>(base + names[i]));
    }
    std::sort(m.exports.begin(), m.exports.end());
}

std::vector<Module> modules;

void ReadModules() {
    modules.clear();
    HMODULE mods[1024];
    DWORD needed = 0;
    const HANDLE self = GetCurrentProcess();
    if (!EnumProcessModules(self, mods, sizeof(mods), &needed)) {
        return;
    }
    for (DWORD i = 0; i < (std::min)(static_cast<DWORD>(needed / sizeof(HMODULE)), static_cast<DWORD>(1024)); ++i) {
        MODULEINFO info{};
        char name[MAX_PATH] = {};
        if (!GetModuleInformation(self, mods[i], &info, sizeof(info))) {
            continue;
        }
        GetModuleBaseNameA(self, mods[i], name, MAX_PATH);
        const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        Module m{base, base + info.SizeOfImage, name, {}};
        ReadExports(m);
        modules.push_back(std::move(m));
    }
    std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
}

const Module* ModuleOf(std::uint64_t pc) {
    auto m = std::upper_bound(modules.begin(), modules.end(), pc,
                              [](std::uint64_t v, const Module& mod) { return v < mod.base; });
    return (m != modules.begin() && pc < std::prev(m)->end) ? &*std::prev(m) : nullptr;
}

// mh4u.exe's functions, from mh4u.syms ("<address> <type> <name>", nm's lines), by the address
// the linker gave them: the image base in the exe's file (the loaded header's is where Windows put
// it), so a sample's address is moved by the difference.
struct Symbols {
    bool loaded = false;
    std::vector<std::pair<std::uint64_t, std::string>> by_address;
    std::uint64_t linked_base = 0;
};
Symbols symbols;

std::string ExePath() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return path;
}

std::uint64_t LinkedImageBase(const std::string& exe) {
    std::ifstream f(exe, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    if (!f.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    IMAGE_NT_HEADERS64 nt{};
    f.seekg(dos.e_lfanew);
    if (!f.read(reinterpret_cast<char*>(&nt), sizeof(nt))) {
        return 0;
    }
    return nt.OptionalHeader.ImageBase;
}

void LoadSymbols() {
    symbols.loaded = true;
    const std::string exe = ExePath();
    const auto slash = exe.find_last_of("\\/");
    const std::string file = (slash == std::string::npos ? std::string() : exe.substr(0, slash + 1)) + "mh4u.syms";
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto a = line.find(' ');
        if (a == std::string::npos || a + 3 > line.size()) {
            continue;
        }
        const std::uint64_t address = std::strtoull(line.substr(0, a).c_str(), nullptr, 16);
        if (address) {
            symbols.by_address.emplace_back(address, line.substr(a + 3));
        }
    }
    std::sort(symbols.by_address.begin(), symbols.by_address.end());
    symbols.linked_base = LinkedImageBase(exe);
    LOG_INFO(Core, "profile: {} names of mh4u.exe's functions from {} (linked at {:X})", symbols.by_address.size(),
             symbols.by_address.empty() ? std::string("(none: no mh4u.syms beside it)") : file, symbols.linked_base);
}

std::string Shorten(std::string name) {
    return name.size() > 120 ? name.substr(0, 117) + "..." : name;
}

std::string ExeSymbol(std::uint64_t pc, std::uintptr_t exe_base) {
    if (!symbols.loaded) {
        LoadSymbols();
    }
    if (symbols.by_address.empty() || !symbols.linked_base) {
        return fmt::format("mh4u.exe+{:X}", pc - exe_base);
    }
    const std::uint64_t at = pc - exe_base + symbols.linked_base;
    auto it = std::upper_bound(symbols.by_address.begin(), symbols.by_address.end(), at,
                               [](std::uint64_t v, const auto& e) { return v < e.first; });
    if (it == symbols.by_address.begin()) {
        return fmt::format("mh4u.exe+{:X}", pc - exe_base);
    }
    return Shorten(std::prev(it)->second);
}

std::string ExportSymbol(const Module& m, std::uint64_t pc) {
    auto it = std::upper_bound(m.exports.begin(), m.exports.end(), pc,
                               [](std::uint64_t v, const auto& e) { return v < e.first; });
    if (it == m.exports.begin()) {
        return fmt::format("{}+{:X}", m.name, pc - m.base);
    }
    return m.name + "!" + std::prev(it)->second;
}

std::vector<std::pair<std::uintptr_t, std::uint32_t>> guest;  // sorted by host address

std::string Top(const std::unordered_map<std::string, std::size_t>& counts, std::size_t total, std::size_t n) {
    std::vector<std::pair<std::size_t, std::string>> v;
    for (const auto& [k, c] : counts) {
        v.emplace_back(c, k);
    }
    std::sort(v.begin(), v.end(), std::greater<>());
    std::string out;
    for (std::size_t i = 0; i < v.size() && i < n; ++i) {
        out += fmt::format("\n    {:5.1f}%  {}", 100.0 * static_cast<double>(v[i].first) / static_cast<double>(total),
                           v[i].second);
    }
    return out;
}

void Report(Buffer& b, double seconds) {
    if (b.n == 0) {
        return;
    }
    const auto exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    std::unordered_map<std::string, std::size_t> by_module, by_function, by_game, by_caller, by_chain;
    for (std::size_t i = 0; i < b.n; ++i) {
        const Sample& s = b.samples[i];
        const Module* mod = ModuleOf(s.pc);
        const std::string module = mod ? mod->name : std::string("(outside a module)");
        ++by_module[module];
        const bool is_exe = mod && mod->base == exe_base;
        const bool is_game = mod && module.find("0004000000126") != std::string::npos;
        if (is_exe) {
            ++by_function[ExeSymbol(s.pc, exe_base)];
        } else if (is_game) {
            if (!guest.empty() && s.pc >= guest.front().first) {
                auto g = std::upper_bound(guest.begin(), guest.end(), static_cast<std::uintptr_t>(s.pc),
                                          [](std::uintptr_t v, const auto& e) { return v < e.first; });
                if (g != guest.begin()) {
                    ++by_game[fmt::format("{:08X}", std::prev(g)->second)];
                }
            }
        } else if (mod) {
            ++by_function[ExportSymbol(*mod, s.pc)];
        }
        if (!is_exe && !is_game && s.callers[0]) {
            // time outside mh4u.exe (Windows, the driver, the C library): the exe function that
            // called it, and the chain above
            const std::string caller = ExeSymbol(s.callers[0], exe_base);
            ++by_caller[caller];
            std::string chain = caller;
            for (int k = 1; k < CALLERS && s.callers[k]; ++k) {
                chain += " <- " + ExeSymbol(s.callers[k], exe_base);
            }
            ++by_chain[chain];
        }
    }
    LOG_INFO(Core, "profile: {:.1f} s, {} samples of the emulation thread. By module:{}", seconds, b.n,
             Top(by_module, b.n, 12));
    LOG_INFO(Core, "profile: functions (mh4u.exe's by name, Windows' by export):{}", Top(by_function, b.n, 40));
    LOG_INFO(Core, "profile: outside mh4u.exe, the exe function that called:{}", Top(by_caller, b.n, 25));
    LOG_INFO(Core, "profile: outside mh4u.exe, the calls that led there:{}", Top(by_chain, b.n, 25));
    LOG_INFO(Core, "profile: the game's functions (3DS addresses):{}", Top(by_game, b.n, 15));
}

} // namespace

void Tick(float seconds, std::vector<std::pair<std::uintptr_t, std::uint32_t>> (*guest_functions)()) {
    if (seconds <= 0.0f) {
        return;
    }
    const auto now = Clock::now();
    if (!running.load(std::memory_order_relaxed)) {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
            return;
        }
        guest = guest_functions();
        std::sort(guest.begin(), guest.end());
        ReadModules();
        // mh4u.exe's code section, for the return addresses on the stack
        const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        exe_text_begin = reinterpret_cast<std::uintptr_t>(base) + nt->OptionalHeader.BaseOfCode;
        exe_text_end = exe_text_begin + nt->OptionalHeader.SizeOfCode;
        {
            std::lock_guard g(lock);
            filling = &buffers[0];
            filling->n = 0;
        }
        window_start = now;
        running = true;
        std::thread(Sampler).detach();
        LOG_INFO(Core, "profile: sampling the emulation thread, a report every {:.0f} s", seconds);
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - window_start).count();
    if (elapsed < seconds) {
        return;
    }
    Buffer* done = nullptr;
    {
        std::lock_guard g(lock);
        done = filling;
        filling = filling == &buffers[0] ? &buffers[1] : &buffers[0];
        filling->n = 0;
    }
    window_start = now;
    if (modules.empty()) {
        ReadModules();
    }
    Report(*done, elapsed);
}

} // namespace Port::Profiler

#else

namespace Port::Profiler {

void Tick(float, std::vector<std::pair<std::uintptr_t, std::uint32_t>> (*)()) {}

} // namespace Port::Profiler

#endif
