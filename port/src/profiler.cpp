#include "profiler.h"

#if defined(_WIN32)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
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

// The samples: the sampler fills a fixed buffer (no allocation while the emulation thread is
// stopped, which may hold the heap's lock); the emulation thread swaps it out at each report.
constexpr std::size_t CAPACITY = 1 << 16;
struct Buffer {
    std::array<std::uint64_t, CAPACITY> pcs{};
    std::size_t n = 0;
};
std::mutex lock;
Buffer* filling = nullptr;
Buffer buffers[2];
HANDLE target = nullptr;
std::atomic<bool> running{false};
Clock::time_point window_start{};

void Sampler() {
    timeBeginPeriod(1);
    while (running.load(std::memory_order_relaxed)) {
        Sleep(1);
        if (SuspendThread(target) == static_cast<DWORD>(-1)) {
            continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        const bool ok = GetThreadContext(target, &c) != 0;
        ResumeThread(target);
        if (!ok) {
            continue;
        }
        std::lock_guard g(lock);
        if (filling && filling->n < CAPACITY) {
            filling->pcs[filling->n++] = c.Rip;
        }
    }
    timeEndPeriod(1);
}

struct Module {
    std::uintptr_t base = 0, end = 0;
    std::string name;
};

std::vector<Module> Modules() {
    std::vector<Module> out;
    HMODULE mods[1024];
    DWORD needed = 0;
    const HANDLE self = GetCurrentProcess();
    if (!EnumProcessModules(self, mods, sizeof(mods), &needed)) {
        return out;
    }
    for (DWORD i = 0; i < (std::min)(static_cast<DWORD>(needed / sizeof(HMODULE)), static_cast<DWORD>(1024)); ++i) {
        MODULEINFO info{};
        char name[MAX_PATH] = {};
        if (!GetModuleInformation(self, mods[i], &info, sizeof(info))) {
            continue;
        }
        GetModuleBaseNameA(self, mods[i], name, MAX_PATH);
        const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        out.push_back({base, base + info.SizeOfImage, name});
    }
    std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    return out;
}

// mh4u.exe's functions, from mh4u.syms ("<address> <type> <name>", nm's lines), by the
// address the linker gave them (the exe's preferred base): a sample's is moved by where Windows
// loaded it.
struct Symbols {
    bool loaded = false;
    std::vector<std::pair<std::uint64_t, std::string>> by_address;
    std::uint64_t preferred_base = 0;
};
Symbols symbols;

void LoadSymbols() {
    symbols.loaded = true;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string file(path);
    const auto slash = file.find_last_of("\\/");
    file = (slash == std::string::npos ? std::string() : file.substr(0, slash + 1)) + "mh4u.syms";
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
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    symbols.preferred_base = nt->OptionalHeader.ImageBase;
    LOG_INFO(Core, "profile: {} names of mh4u.exe's functions from {}", symbols.by_address.size(),
             symbols.by_address.empty() ? std::string("(none: no mh4u.syms beside it)") : file);
}

std::string SymbolOf(std::uint64_t pc, std::uintptr_t exe_base) {
    if (!symbols.loaded) {
        LoadSymbols();
    }
    if (symbols.by_address.empty()) {
        return fmt::format("mh4u.exe+{:X}", pc - exe_base);
    }
    const std::uint64_t at = pc - exe_base + symbols.preferred_base;
    auto it = std::upper_bound(symbols.by_address.begin(), symbols.by_address.end(), at,
                               [](std::uint64_t v, const auto& e) { return v < e.first; });
    if (it == symbols.by_address.begin()) {
        return fmt::format("mh4u.exe+{:X}", pc - exe_base);
    }
    std::string name = std::prev(it)->second;
    if (name.size() > 110) {
        name = name.substr(0, 107) + "...";
    }
    return name;
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
    const auto modules = Modules();
    const auto exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    std::unordered_map<std::string, std::size_t> by_module, by_exe, by_game;
    for (std::size_t i = 0; i < b.n; ++i) {
        const std::uint64_t pc = b.pcs[i];
        auto m = std::upper_bound(modules.begin(), modules.end(), pc,
                                  [](std::uint64_t v, const Module& mod) { return v < mod.base; });
        const Module* mod = (m != modules.begin() && pc < std::prev(m)->end) ? &*std::prev(m) : nullptr;
        const std::string module = mod ? mod->name : std::string("(outside a module)");
        ++by_module[module];
        if (mod && mod->base == exe_base) {
            ++by_exe[SymbolOf(pc, exe_base)];
        }
        if (!guest.empty() && pc >= guest.front().first) {
            auto g = std::upper_bound(guest.begin(), guest.end(), static_cast<std::uintptr_t>(pc),
                                      [](std::uintptr_t v, const auto& e) { return v < e.first; });
            if (g != guest.begin() && mod && module.find("0004000000126") != std::string::npos) {
                ++by_game[fmt::format("{:08X}", std::prev(g)->second)];
            }
        }
    }
    LOG_INFO(Core, "profile: {:.1f} s, {} samples of the emulation thread. By module:{}", seconds, b.n,
             Top(by_module, b.n, 12));
    LOG_INFO(Core, "profile: mh4u.exe's functions (the port and Azahar):{}", Top(by_exe, b.n, 30));
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
    Report(*done, elapsed);
}

} // namespace Port::Profiler

#else

namespace Port::Profiler {

void Tick(float, std::vector<std::pair<std::uintptr_t, std::uint32_t>> (*)()) {}

} // namespace Port::Profiler

#endif
