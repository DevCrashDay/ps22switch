#pragma once

// Vector unit programs the build does not know, compiled while the game runs (docs/vu-live.md).
//
// A game build compiles the vector unit programs that were recorded before it (vurecomp); every other program runs in the
// interpreter, several times slower. With live compiling the runner keeps interpreting such a program and, in the background,
// makes the C++ for it with the same generator (recomp/vurecomp_gen.cpp), compiles that into a library with a C++ compiler,
// loads the library and from then on uses the compiled program like a built-in one. The libraries stay in a cache folder next
// to the game, so the next start loads them without compiling anything.

#include "ps2/vu_ops.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2::vu::live {

// Called once before the game starts. cache is the folder for the libraries; it is made when the first one is built. Does
// nothing (with one line in the log) when no C++ compiler is found, and then only loads what an earlier run left in the cache.
void start(const std::string &cache);

bool active();

// The vector unit met a program the build does not know (unit 1; called on the thread that runs the vector unit). code is its
// whole 16 KiB of micro memory. Cheap: a program is queued once.
void unknown(std::uint64_t key, std::uint32_t entry, const std::uint8_t *code);

// An interpreted call of a program the build does not know, by key: the busiest programs are built first.
void note_call(std::uint64_t key);

extern std::atomic<bool> g_ready;

// True when the background thread has loaded programs the vector unit does not use yet.
inline bool ready() { return g_ready.load(std::memory_order_relaxed); }

// Hands over the programs loaded since the last call (the caller adds them to its table).
std::vector<Program> take();

}  // namespace ps2::vu::live
