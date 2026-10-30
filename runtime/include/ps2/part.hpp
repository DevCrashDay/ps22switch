#pragma once
// PS2_PARTS=1: where the emulation thread's time goes, for machines without a sampling profiler (the Switch). The thread marks which part
// of the console it is in (one byte store on the way in and out), a helper thread looks at the mark every 250 microseconds, and the
// shares are printed every 2 seconds.
#include <atomic>
#include <cstdint>

namespace ps2::part {

enum Part : std::uint8_t { kEE, kEvents, kIop, kHw, kVif, kGif, kDma, kIpu, kIdle, kWaitWindow, kWaitSound, kParts };

inline std::atomic<std::uint8_t> g_now{kEE};
inline bool g_on = false;  // PS2_PARTS=1 (start() sets it): when off, a Scope costs one load of this bool

// Only the emulation thread marks: a part entered on another thread leaves the mark alone.
inline thread_local bool g_marking = false;

class Scope {
public:
    explicit Scope(Part p) : on_(g_on && g_marking) {
        if (on_) {
            before_ = g_now.load(std::memory_order_relaxed);
            g_now.store(p, std::memory_order_relaxed);
        }
    }
    ~Scope() {
        if (on_) g_now.store(before_, std::memory_order_relaxed);
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    bool on_;
    std::uint8_t before_ = kEE;
};

// called by the emulation thread when it starts; starts the sampler when PS2_PARTS=1
void start();

}  // namespace ps2::part
