// Switch only: the CPU, GPU and memory clocks while the game runs (PS2_CPU_MHZ, PS2_GPU_MHZ, PS2_MEM_MHZ in launch.txt).
//
// The same way OpenTrickyNX and flatout2-sw do it: Horizon's clock services (clkrst from 8.0.0 on, pcv before), the nearest rate the
// console offers at or below the one asked for, a thread that puts the clock back when the system resets it (docking, the home menu),
// and the old clocks back when the game ends. When a clock manager (sys-clk, Horizon OC) runs, its profile decides and nothing is set
// here, unless PS2_CLOCK_FORCE=1.
#ifdef __SWITCH__

#include "switch_clock.hpp"

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {

struct Clock {
    const char *setting, *name;
    PcvModule module;
    PcvModuleId id;
    std::uint32_t cap_hz;  // the most this code asks for, whatever the setting says
    double default_mhz;
    ClkrstSession session{};
    bool open = false;
    std::uint32_t old_hz = 0, set_hz = 0;
};

Clock g_clocks[3] = {
    {"PS2_CPU_MHZ", "CPU", PcvModule_CpuBus, PcvModuleId_CpuBus, 1785000000u, 1785},
    {"PS2_GPU_MHZ", "GPU", PcvModule_GPU, PcvModuleId_GPU, 921600000u, 921.6},
    {"PS2_MEM_MHZ", "memory", PcvModule_EMC, PcvModuleId_EMC, 1600000000u, 1600},
};
int g_service = -1;  // 1 clkrst, 0 pcv, -1 none
std::atomic<bool> g_stop{false};
Thread g_keeper;
bool g_keeper_up = false;

Result get_rate(Clock &c, std::uint32_t *hz) { return g_service ? clkrstGetClockRate(&c.session, hz) : pcvGetClockRate(c.module, hz); }
Result set_rate(Clock &c, std::uint32_t hz) { return g_service ? clkrstSetClockRate(&c.session, hz) : pcvSetClockRate(c.module, hz); }

// the highest rate the console lists that is not above the one wanted
std::uint32_t nearest(Clock &c, std::uint32_t want) {
    std::uint32_t rates[64], best = 0;
    s32 count = 0;
    PcvClockRatesListType type;
    const Result rc = g_service ? clkrstGetPossibleClockRates(&c.session, rates, 64, &type, &count) : pcvGetPossibleClockRates(c.module, rates, 64, &type, &count);
    if (R_FAILED(rc) || count <= 0 || type != PcvClockRatesListType_Discrete) return want;
    for (s32 i = 0; i < count; ++i)
        if (rates[i] <= want && rates[i] > best) best = rates[i];
    return best ? best : want;
}

// another process serves this name: registering it fails with "already registered"
bool service_running(const char *name) {
    Handle h;
    const Result rc = smRegisterService(&h, smEncodeName(name), false, 1);
    if (R_SUCCEEDED(rc)) {
        smUnregisterService(smEncodeName(name));
        svcCloseHandle(h);
        return false;
    }
    return rc == MAKERESULT(21, 4);
}

void keep(void *) {
    while (!g_stop.load()) {
        svcSleepThread(1000000000ll);
        if (g_stop.load() || appletGetFocusState() != AppletFocusState_InFocus) continue;
        for (Clock &c : g_clocks) {
            std::uint32_t hz = 0;
            if (!c.set_hz || R_FAILED(get_rate(c, &hz)) || hz == c.set_hz) continue;
            if (hz > c.set_hz) {  // someone else raised it: theirs
                c.set_hz = 0;
                continue;
            }
            set_rate(c, c.set_hz);
            std::fprintf(stderr, "[clock] %s was reset to %u MHz, back to %u MHz\n", c.name, hz / 1000000u, c.set_hz / 1000000u);
        }
    }
}

}  // namespace

void switch_clock_start() {
    std::uint32_t want[3];
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        const char *e = std::getenv(g_clocks[i].setting);
        const double mhz = e && *e ? std::atof(e) : g_clocks[i].default_mhz;
        want[i] = mhz > 0 ? static_cast<std::uint32_t>(mhz * 1e6) : 0;
        if (want[i] > g_clocks[i].cap_hz) want[i] = g_clocks[i].cap_hz;
        any |= want[i] != 0;
    }
    if (!any) {
        std::fprintf(stderr, "[clock] PS2_*_MHZ=0: the system's clocks\n");
        return;
    }
    const char *force = std::getenv("PS2_CLOCK_FORCE");
    const char *manager = service_running("hoc:clk") ? "Horizon OC" : service_running("sys:clk") ? "sys-clk" : nullptr;
    if (manager && !(force && force[0] == '1')) {
        std::fprintf(stderr, "[clock] %s is running: its profile sets the clocks (PS2_CLOCK_FORCE=1 overrides)\n", manager);
        return;
    }
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) g_service = 1;
    else if (R_SUCCEEDED(pcvInitialize())) g_service = 0;
    else {
        std::fprintf(stderr, "[clock] neither clkrst nor pcv is available: clocks unchanged\n");
        return;
    }
    for (int i = 0; i < 3; ++i) {
        Clock &c = g_clocks[i];
        if (!want[i]) continue;
        if (g_service) {
            if (const Result rc = clkrstOpenSession(&c.session, c.id, 3); R_FAILED(rc)) {
                std::fprintf(stderr, "[clock] %s: no clkrst session (0x%X)\n", c.name, rc);
                continue;
            }
            c.open = true;
        }
        get_rate(c, &c.old_hz);
        const std::uint32_t hz = nearest(c, want[i]);
        if (c.old_hz >= hz) {
            std::fprintf(stderr, "[clock] %s already at %u MHz (asked for %u): left alone\n", c.name, c.old_hz / 1000000u, want[i] / 1000000u);
            continue;
        }
        const Result rc = set_rate(c, hz);
        std::uint32_t now = 0;
        get_rate(c, &now);
        if (R_SUCCEEDED(rc)) c.set_hz = now ? now : hz;
        std::fprintf(stderr, "[clock] %s %u -> %u MHz (asked for %u)%s\n", c.name, c.old_hz / 1000000u, now / 1000000u, want[i] / 1000000u,
                     R_FAILED(rc) ? ": refused" : "");
    }
    std::atexit(switch_clock_restore);
    // a high priority: at the lowest, the busy emulation threads on its core would starve it
    if (R_SUCCEEDED(threadCreate(&g_keeper, keep, nullptr, nullptr, 0x4000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&g_keeper))) g_keeper_up = true;
}

void switch_clock_restore() {
    if (g_service < 0) return;
    g_stop.store(true);
    if (g_keeper_up) {
        threadWaitForExit(&g_keeper);
        threadClose(&g_keeper);
        g_keeper_up = false;
    }
    for (Clock &c : g_clocks) {
        if (c.set_hz && c.old_hz) set_rate(c, c.old_hz);
        if (c.open) clkrstCloseSession(&c.session);
        c.open = false;
        c.set_hz = 0;
    }
    if (g_service) clkrstExit();
    else pcvExit();
    g_service = -1;
}

#endif
