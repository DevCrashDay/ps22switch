#include "ps2/part.hpp"

#include "ps2/os.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

namespace ps2::part {

void start() {
    g_marking = true;
    if (os::env("PS2_PARTS") != "1" || g_on) return;
    g_on = true;
    std::thread([] {
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
            std::fprintf(stderr, "[parts] emulation thread:");
            for (int i = 0; i < kParts; ++i)
                if (seen[i]) std::fprintf(stderr, " %s %.0f%%", kNames[i], 100.0 * static_cast<double>(seen[i]) / static_cast<double>(total));
            std::fprintf(stderr, "\n");
            for (std::uint64_t &s : seen) s = 0;
            total = 0;
            since = std::chrono::steady_clock::now();
        }
    }).detach();
}

}  // namespace ps2::part
