// Sampling profiler for the emulation thread on Windows: the same PS2_PROFILE=<file> and PS2_PROFILE_FROM=<frame>
// switches as host/profile.cpp on Linux, but the file is a readable list of the busiest functions (DbgHelp reads the
// symbol table the linker left in ps2run.exe), so no other tool is needed.
//
// A helper thread stops the emulation thread about every millisecond, reads where it is (the instruction pointer and
// the word on top of the stack, which is the caller when the thread sits at the start of a library function) and lets
// it go on. Only the thread that called start_profile is sampled; the GPU and VU threads are not.

#include "profile.hpp"

#include "ps2/system.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct Sample {
    std::uintptr_t pc, caller;
};

constexpr std::size_t kMaxSamples = 1'000'000;  // 16 MB; about 15 minutes of sampling, later samples are dropped
HANDLE g_target = nullptr;
std::atomic<bool> g_running{false};
std::thread g_thread;
std::vector<Sample> g_samples;
std::atomic<std::size_t> g_count{0};
std::uint64_t g_from = 0;
bool g_armed = false;

void sampler() {
    timeBeginPeriod(1);
    while (g_running.load(std::memory_order_relaxed)) {
        Sample s{};
        bool got = false;
        // nothing that can take a lock (malloc, stdio) may run between Suspend and Resume: the target may hold it
        if (SuspendThread(g_target) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_target, &c)) {
                s.pc = static_cast<std::uintptr_t>(c.Rip);
                SIZE_T read = 0;
                std::uintptr_t top = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c.Rsp), &top, sizeof top, &read) && read == sizeof top) s.caller = top;
                got = true;
            }
            ResumeThread(g_target);
        }
        const std::size_t n = g_count.load(std::memory_order_relaxed);
        if (got && n < g_samples.size()) {
            g_samples[n] = s;
            g_count.store(n + 1, std::memory_order_relaxed);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
}

void arm() {
    if (g_armed) return;
    g_armed = true;
    // a real handle of the calling (emulation) thread: GetCurrentThread() is only a placeholder
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        std::fprintf(stderr, "[profile] cannot open the emulation thread (error %lu)\n", GetLastError());
        return;
    }
    g_samples.assign(kMaxSamples, Sample{});
    g_running.store(true);
    g_thread = std::thread(sampler);
}

std::string demangle(const char *name) {
    int status = 0;
    char *text = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string out = status == 0 && text ? text : name;
    std::free(text);
    return out;
}

struct Symbol {
    std::uint32_t rva;
    std::string name;
};

// The function symbols the linker left in ps2run.exe (COFF symbol table), sorted by address. DbgHelp does not read them.
std::vector<Symbol> load_symbols() {
    std::vector<Symbol> symbols;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::FILE *f = _wfopen(path, L"rb");
    if (!f) return symbols;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> file(static_cast<std::size_t>(size));
    const bool ok = size > 0 && std::fread(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!ok) return symbols;
    const auto u32 = [&](std::size_t at) { std::uint32_t v = 0; if (at + 4 <= file.size()) std::memcpy(&v, &file[at], 4); return v; };
    const auto u16 = [&](std::size_t at) { std::uint16_t v = 0; if (at + 2 <= file.size()) std::memcpy(&v, &file[at], 2); return v; };
    const std::size_t pe = u32(0x3C);
    const std::size_t sections = u16(pe + 6), optional = u16(pe + 20);
    const std::size_t table = u32(pe + 12), count = u32(pe + 16);
    if (!table || !count || table + count * 18 > file.size()) return symbols;
    const std::size_t first_section = pe + 24 + optional, strings = table + count * 18;
    for (std::size_t i = 0; i < count;) {
        const std::size_t at = table + i * 18;
        const std::size_t aux = file[at + 17];
        const int section = static_cast<std::int16_t>(u16(at + 12));
        const unsigned storage = file[at + 16];
        i += 1 + aux;
        if (section <= 0 || static_cast<std::size_t>(section) > sections || (storage != 2 && storage != 3)) continue;
        const std::size_t header = first_section + static_cast<std::size_t>(section - 1) * 40;
        if (!(u32(header + 36) & 0x20000000u)) continue;  // not in a section with code
        std::string name;
        if (u32(at) == 0) {
            const std::size_t offset = strings + u32(at + 4);
            if (offset < file.size()) name = reinterpret_cast<const char *>(&file[offset]);
        } else {
            name.assign(reinterpret_cast<const char *>(&file[at]), strnlen(reinterpret_cast<const char *>(&file[at]), 8));
        }
        symbols.push_back({u32(header + 12) + u32(at + 8), std::move(name)});
    }
    std::sort(symbols.begin(), symbols.end(), [](const Symbol &a, const Symbol &b) { return a.rva < b.rva; });
    return symbols;
}

// "ps2::vu::run_fast(int, unsigned int)" for an address of ps2run.exe, "SomeLibrary.dll!?" for one of a DLL
std::string describe(std::uintptr_t address, std::uintptr_t image_begin, std::uintptr_t image_end, const std::vector<Symbol> &symbols,
                     std::unordered_map<std::uintptr_t, std::string> &cache) {
    const auto found = cache.find(address);
    if (found != cache.end()) return found->second;
    std::string text;
    if (address >= image_begin && address < image_end) {
        const auto rva = static_cast<std::uint32_t>(address - image_begin);
        const auto it = std::upper_bound(symbols.begin(), symbols.end(), rva, [](std::uint32_t v, const Symbol &s) { return v < s.rva; });
        if (it != symbols.begin()) {
            text = demangle(std::prev(it)->name.c_str());
        } else {
            char hex[32];
            std::snprintf(hex, sizeof hex, "ps2run+0x%x", rva);
            text = hex;
        }
    } else {
        HMODULE module = nullptr;
        char path[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) &&
            GetModuleFileNameA(module, path, MAX_PATH)) {
            const std::string full = path;
            text = full.substr(full.find_last_of(R"(\/)") + 1) + "!?";
        } else {
            text = "?";
        }
    }
    cache[address] = text;
    return text;
}

}

void start_profile(std::uint64_t from_frame) {
    g_from = from_frame;
    if (!g_from) {
        arm();
        return;
    }
    auto previous = ps2::g_options.on_vblank;
    ps2::g_options.on_vblank = [previous]() {
        if (ps2::frame_count() == g_from) arm();
        if (previous) previous();
    };
}

void write_profile(const char *path) {
    if (g_running.exchange(false) && g_thread.joinable()) g_thread.join();
    if (g_target) {
        CloseHandle(g_target);
        g_target = nullptr;
    }
    const std::size_t total = g_count.load();
    std::FILE *f = std::fopen(path, "w");
    if (!f) return;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(GetModuleHandleA(nullptr));
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const char *>(dos) + dos->e_lfanew);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(dos), end = begin + nt->OptionalHeader.SizeOfImage;
    const std::vector<Symbol> symbols = load_symbols();
    std::unordered_map<std::uintptr_t, std::string> names;
    std::map<std::string, std::uint64_t> self, outside, callers;
    std::uint64_t in_program = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const Sample &s = g_samples[i];
        if (s.pc >= begin && s.pc < end) {
            ++self[describe(s.pc, begin, end, symbols, names)];
            ++in_program;
        } else {
            ++outside[describe(s.pc, begin, end, symbols, names)];
            if (s.caller >= begin && s.caller < end) ++callers[describe(s.caller, begin, end, symbols, names)];
        }
    }
    const auto sorted = [](const std::map<std::string, std::uint64_t> &m) {
        std::vector<std::pair<std::string, std::uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        return v;
    };
    const auto percent = [&](std::uint64_t n) { return total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0; };
    std::fprintf(f, "# %zu samples of the emulation thread (about one per millisecond of wall time), %llu of them inside ps2run.exe\n", total,
                 static_cast<unsigned long long>(in_program));
    std::fprintf(f, "# percent of all samples, samples, function\n\n# where the thread was, in ps2run.exe\n");
    std::size_t shown = 0;
    for (const auto &[name, hits] : sorted(self))
        if (shown++ < 60) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fprintf(f, "\n# where the thread was, outside ps2run.exe (system libraries)\n");
    shown = 0;
    for (const auto &[name, hits] : sorted(outside))
        if (shown++ < 20) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fprintf(f, "\n# the function of ps2run.exe on top of the stack at those moments (usually the caller)\n");
    shown = 0;
    for (const auto &[name, hits] : sorted(callers))
        if (shown++ < 20) std::fprintf(f, "%5.1f%% %7llu  %s\n", percent(hits), static_cast<unsigned long long>(hits), name.c_str());
    std::fclose(f);
}
