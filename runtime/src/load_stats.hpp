#pragma once
// PS2_IOP_LOAD=1 (porting work): how much real time the emulation thread spends in each part of the sound side of the console
// (the IOP processor and its modules, the sound chip, the disc), printed every 100 frames by report() in system.cpp. Nothing is
// timed and nothing is printed when the variable is not set; the checks cost one load of a bool.
#include "ps2/part.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace ps2::cost {

enum Part { kSync, kIopRun, kIopExec, kIopJobs, kSpuTick, kSifPump, kDisc, kParts };

inline const bool g_on = std::getenv("PS2_IOP_LOAD") != nullptr;
inline std::uint64_t g_ns[kParts] = {}, g_count[kParts] = {}, g_exec_cycles = 0, g_disc_bytes = 0;

class Timer {
public:
    explicit Timer(Part part) : mark_(ps2::part::kIop), part_(part) {
        if (g_on) start_ = std::chrono::steady_clock::now();
    }
    ~Timer() {
        if (!g_on) return;
        g_ns[part_] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
        ++g_count[part_];
    }

private:
    ps2::part::Scope mark_;
    Part part_;
    std::chrono::steady_clock::time_point start_;
};

}
