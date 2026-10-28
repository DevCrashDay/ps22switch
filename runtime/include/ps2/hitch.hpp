#pragma once

// PS2_HITCH_LOG=<ms> (off unless set; 1 means 30 ms): a timing aid for stutters (docs/hitches.md). The threads that can stall the game
// time what they do and print one "[hitch] ..." line whenever one step took longer than that many milliseconds: the window thread
// (each picture: the drawing messages, the hand-over to the screen, waiting for the next swap chain picture, the window's own event
// polling), the disc reads, and the sound hand-over. PS2_HITCH_PICTURES=<file> (needs PS2_HITCH_LOG too; a large value such as 100000 keeps
// the console quiet) writes one line per picture: what the window thread did for it. Nothing is timed and nothing is printed when the variable is not set.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace ps2::hitch {

inline double threshold_ms() {
    static const double value = [] {
        const char *text = std::getenv("PS2_HITCH_LOG");
        if (!text || !*text) return 0.0;
        const double ms = std::atof(text);
        return ms <= 1.0 ? 30.0 : ms;
    }();
    return value;
}

inline bool on() {
    static const bool value = threshold_ms() > 0.0;
    return value;
}

using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

// How long the steps of the last hand-over of a picture to the screen took (milliseconds), set by the graphics code, read by the window
// thread's per-picture table (PS2_HITCH_PICTURES=<file>, see gs/gs.cpp).
struct PictureTimes {
    double flush = 0, acquire = 0, submit = 0, present = 0;
};
inline PictureTimes &picture_times() {
    static PictureTimes times;
    return times;
}

// Where the window thread is while it hands a picture to the screen ("waiting for the next swap chain picture", "presenting") and since
// when (steady clock, nanoseconds); a null name means it is somewhere else. Always on: two stores per step. The Linux watchdog
// (host/window.cpp) prints it when no new frame comes, so a freeze in the driver leaves the step in the log.
struct PresentStage {
    std::atomic<const char *> name{nullptr};
    std::atomic<std::int64_t> since_ns{0};
};
inline PresentStage &present_stage() {
    static PresentStage stage;
    return stage;
}
inline std::int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
inline void enter_present_stage(const char *name) {
    PresentStage &s = present_stage();
    s.since_ns.store(name ? now_ns() : 0, std::memory_order_relaxed);
    s.name.store(name, std::memory_order_release);
}

// CPU time (user + kernel) this thread has used, in milliseconds (defined in gs/queue.cpp, which has the Windows header). Windows counts
// it in clock ticks of about 15.6 ms, so it only says "the thread was running the whole time" or "it was not" for steps of 50 ms and more.
double thread_cpu_ms();

}  // namespace ps2::hitch
