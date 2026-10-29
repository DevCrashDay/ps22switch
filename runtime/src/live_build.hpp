#pragma once

// What both kinds of live compiling share (runtime/src/vu_live.cpp: vector programs, runtime/src/ee_live.cpp: main processor code):
// finding a C++ compiler, starting it as a process (job object on Windows, priority handling), compiling a few source files side by
// side and linking them into a library, loading a library, and the small helpers around that. Moved out of vu_live.cpp unchanged in
// behaviour when ee_live.cpp needed the same machinery (docs/ee-live.md).

#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace ps2::live {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t);
std::uint64_t fnv(std::uint64_t h, const void *data, std::size_t size);
std::string hex(std::uint64_t v, int digits);
fs::path exe_directory();
bool is_file(const fs::path &p);

// The compiler: PS2_VU_CXX, then a compiler\ folder next to the runner or one folder up (a player's folder with several games keeps
// one compiler\ in program\, next to program\<game>\), then the compiler this runner was built with, then $CXX, then clang++ / g++
// from the PATH. Empty when there is none.
fs::path find_compiler();

// The shared library kind of this system: ".dll", ".dylib" or ".so".
extern const char *const kLibraryExtension;
void *open_library(const fs::path &path);
void *find_symbol(void *library, const char *name);

struct Process {
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    pid_t pid = -1;
#endif
};

// The compiler processes of the runner, whoever started them. stop_all() (registered with atexit by the first user) ends them.
struct Toolchain {
    std::mutex mutex;                         // guards alive and stopping
    std::vector<Process> alive;
    bool stopping = false;
    std::atomic<bool> normal_priority{false}; // true once a compile step starved at below normal priority (watch_priority)
    double starve_s = 5.0;                    // a step this long at below normal priority switches to normal for good (0: never)
};
Toolchain &toolchain();
void stop_all();   // ends every running compiler process and refuses new ones
void use_environment();  // PS2_VU_LIVE_PRIORITY / PS2_VU_LIVE_STARVE_S, once

// One compile-and-link job: the sources are compiled side by side (jobs at a time), then linked into library.
struct Job {
    std::vector<fs::path> sources;
    fs::path include_dir;
    fs::path work;       // object files and log.txt go here
    fs::path library;    // the output
    fs::path compiler;
    std::vector<std::string> flags;  // compile flags; the link step takes the ones that are neither -O nor -std
    std::size_t jobs = 2;
    std::size_t jobs_urgent = 0;     // more at a time once `urgent` is set (the game waits: use the whole machine)
    bool below_normal = true;        // the game comes first; a paused game (ee_live.cpp) compiles at normal priority
    bool starve = true;              // false: a compile that gets no processor time does not switch the toolchain to normal priority by itself
    const std::atomic<bool> *cancel = nullptr;  // set: stop at once (the compilers are ended, nothing more is said); the caller no longer wants the result
    const std::atomic<int> *hold = nullptr;     // above 0: start no more compilers for now (the game waits for another build, which should have the processors); running ones go on
    const std::atomic<bool> *urgent = nullptr;  // set while the game waits for this job (ee_live.cpp, background translation): normal priority from then on
    std::function<void(const std::string &)> say;  // one line of the log
};
struct Timing {
    double generate = 0, compile = 0, link = 0;
    double cpu = 0;       // processor seconds the compilers used (Windows only; 0 elsewhere)
    double longest = 0;   // the longest life of one compiler process (Windows only)
};
bool compile_and_link(const Job &job, Timing &timing);

// The flags of a live library: C++20, the given optimisation level, and the instruction set options the runner itself was built with
// (the generated code must compute what the build's does, bit for bit).
std::vector<std::string> compile_flags(const std::string &optimisation);

struct HeaderFile {
    const char *name;
    const char *text;
};
// Writes the embedded headers to <include_dir>/ps2 when they are not there yet (or differ).
bool write_headers(const fs::path &include_dir, const std::vector<HeaderFile> &files);

}  // namespace ps2::live

#endif
