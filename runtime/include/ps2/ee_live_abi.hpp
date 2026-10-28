#pragma once

// How a library of main processor code that was compiled while the game ran (runtime/src/ee_live.cpp, docs/ee-live.md) reaches the
// runner. The library is not linked to the runner: the runner hands it a table of the functions the generated code calls, and the
// base of its memory, when the library is loaded. This header is used in two places:
//   - the runner includes it and fills an EeLiveHost with the real functions;
//   - the library (compiled against the copy embedded in the runner) defines each function below as a forwarder to the table, in the
//     one file that defines PS2_LIVE_EE_DEFINE before including generated.hpp (the table.cpp that recomp/codegen.cpp writes).
// Adding a function the generated code calls means adding it to the list: both sides follow.

#include "cpu.hpp"
#include "memory.hpp"

#include <cstdint>

namespace ps2 {

// X(return type, name, (parameters), (arguments))
#define PS2_EE_LIVE_FUNCTIONS(X)                                                                                                \
    X(void, call, (Cpu &c, std::uint32_t target, std::uint32_t depth), (c, target, depth))                                     \
    X(void, bad_entry, (Cpu &c, std::uint32_t unit_base), (c, unit_base))                                                      \
    X(bool, preempt, (Cpu &c), (c))                                                                                            \
    X(void, syscall, (Cpu &c, std::uint32_t pc), (c, pc))                                                                      \
    X(void, breakpoint, (Cpu &c, std::uint32_t pc), (c, pc))                                                                   \
    X(void, trap, (Cpu &c, std::uint32_t pc), (c, pc))                                                                         \
    X(void, stop, (Cpu &c, std::uint32_t pc, const char *what), (c, pc, what))                                                 \
    X(void, probe, (Cpu &c, std::uint32_t pc), (c, pc))                                                                        \
    X(std::uint32_t, mfc0, (Cpu &c, std::uint32_t reg, std::uint32_t sel), (c, reg, sel))                                      \
    X(void, mtc0, (Cpu &c, std::uint32_t reg, std::uint32_t value), (c, reg, value))                                           \
    X(void, set_interrupts, (Cpu &c, bool enabled), (c, enabled))                                                              \
    X(std::uint32_t, eret, (Cpu &c), (c))                                                                                      \
    X(bool, cpcond0, (Cpu &c), (c))                                                                                            \
    X(std::uint32_t, cfc1, (Cpu &c, std::uint32_t reg), (c, reg))                                                              \
    X(void, ctc1, (Cpu &c, std::uint32_t reg, std::uint32_t value), (c, reg, value))                                           \
    X(std::uint32_t, cfc2, (Cpu &c, std::uint32_t reg), (c, reg))                                                              \
    X(void, ctc2, (Cpu &c, std::uint32_t reg, std::uint32_t value), (c, reg, value))                                           \
    X(void, vcallms, (Cpu &c, std::uint32_t address), (c, address))                                                            \
    X(bool, vu0_busy, (Cpu &c), (c))                                                                                           \
    X(void, vu0_wait, (Cpu &c), (c))                                                                                           \
    X(std::uint8_t *, vu0_data, (), ())                                                                                        \
    X(std::uint32_t, mfperf, (Cpu &c, std::uint32_t spec), (c, spec))                                                          \
    X(void, mtperf, (Cpu &c, std::uint32_t spec, std::uint32_t value), (c, spec, value))                                       \
    X(std::uint8_t, slow_ld8, (std::uint32_t a), (a))                                                                          \
    X(std::uint16_t, slow_ld16, (std::uint32_t a), (a))                                                                        \
    X(std::uint32_t, slow_ld32, (std::uint32_t a), (a))                                                                        \
    X(std::uint64_t, slow_ld64, (std::uint32_t a), (a))                                                                        \
    X(void, slow_ld128, (std::uint32_t a, Reg128 &out), (a, out))                                                              \
    X(void, slow_st8, (std::uint32_t a, std::uint8_t v), (a, v))                                                               \
    X(void, slow_st16, (std::uint32_t a, std::uint16_t v), (a, v))                                                             \
    X(void, slow_st32, (std::uint32_t a, std::uint32_t v), (a, v))                                                             \
    X(void, slow_st64, (std::uint32_t a, std::uint64_t v), (a, v))                                                             \
    X(void, slow_st128, (std::uint32_t a, const Reg128 &v), (a, v))                                                            \
    X(void, watch_hit, (std::uint32_t a, std::uint32_t v, const char *via), (a, v, via))                                       \
    X(void, watch_span, (std::uint32_t a, std::uint32_t n, const char *via), (a, n, via))

struct EeLiveHost {
    std::uint64_t layout;            // sizeof(Cpu) and friends: a library built for another layout is refused
    std::uint8_t *ram;               // g_ram and g_spr, which do not move
    std::uint8_t *spr;
    bool watch_on;                   // the write watch (memory.hpp), copied when the library is loaded
    std::uint32_t watch_base, watch_end;
    void (*dispatch)(Cpu &c, std::uint32_t entry, std::uint32_t depth);
#define X(R, name, params, args) R(*name) params;
    PS2_EE_LIVE_FUNCTIONS(X)
#undef X
};

}  // namespace ps2

#ifdef PS2_LIVE_EE_DEFINE
// Inside the library only: what the generated code declared in generated.hpp / memory.hpp / cpu.hpp is defined here, once, and
// forwards to the runner. dispatch is a tail call so that the code units keep their stack as in a build of the game.
namespace ps2 {
const EeLiveHost *g_ee_host = nullptr;
std::uint8_t *g_live_ram = nullptr;
std::uint8_t *g_live_spr = nullptr;
bool g_watch_on = false;
std::uint32_t g_watch_base = 0, g_watch_end = 0;
void dispatch(Cpu &c, std::uint32_t entry, std::uint32_t depth) { [[clang::musttail]] return g_ee_host->dispatch(c, entry, depth); }
#define X(R, name, params, args) \
    R name params { return g_ee_host->name args; }
PS2_EE_LIVE_FUNCTIONS(X)
#undef X
}  // namespace ps2
#endif
