#include "ps2/part.hpp"

#include "ps2/os.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef __SWITCH__
#include <pthread.h>
#endif

namespace ps2::part {

namespace {

void sample() {
    static const char *const kNames[kParts] = {"game code", "events", "IOP and sound", "hardware registers", "VIF", "GIF", "DMA", "IPU", "idle",
                                               "waiting for the window", "waiting for the sound"};
    std::uint64_t seen[kParts] = {}, total = 0;
    auto since = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(250));
        const std::uint8_t now = g_now.load(std::memory_order_relaxed);
        if (now < kParts) ++seen[now];
        ++total;
        if (std::chrono::steady_clock::now() - since < std::chrono::seconds(2)) continue;
        char line[512];
        int n = std::snprintf(line, sizeof line, "[parts] emulation thread:");
        for (int i = 0; i < kParts && n > 0 && n < static_cast<int>(sizeof line); ++i)
            if (seen[i]) n += std::snprintf(line + n, sizeof line - n, " %s %.0f%%", kNames[i], 100.0 * static_cast<double>(seen[i]) / static_cast<double>(total));
        std::fprintf(stderr, "%s\n", line);
        for (std::uint64_t &s : seen) s = 0;
        total = 0;
        since = std::chrono::steady_clock::now();
    }
}

}  // namespace

void start() {
    g_marking = true;
    if (os::env("PS2_PARTS") != "1" || g_on) return;
    std::fprintf(stderr, "[parts] sampling the emulation thread every 250 us\n");
#ifdef __SWITCH__
    // a thread of our own making with a stack of known size (libnx's default is 128 KiB)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 10);
    pthread_t thread;
    const bool started = pthread_create(&thread, &attr, [](void *) -> void * {
        sample();
        return nullptr;
    }, nullptr) == 0;
    pthread_attr_destroy(&attr);
    if (!started) {
        std::fprintf(stderr, "[parts] cannot start the sampling thread\n");
        return;
    }
    pthread_detach(thread);
#else
    std::thread(sample).detach();
#endif
    g_on = true;
}

}  // namespace ps2::part
