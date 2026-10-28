#include "gs/queue.hpp"
#include "dmac.hpp"
#include "hw.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"
#include "ps2/vu_ops.hpp"

#include <cstdio>

namespace ps2 {

// FCR31 of the console: bit 0 and bit 24 (FS) always read 1, which is also its value after a reset (0x01000001), so
// c.fcr31 (zero-initialised) only holds the bits that can change. These are the sticky flags (bits 3 to 6), the cause
// bits U, O, D and I (14 to 17) and the condition bit C (23); the rounding mode, the enables, the other cause bits,
// the flush bits and everything above the condition bit cannot be written.
// FCR0 (the revision, 0x2E30) cannot be written either. The registers 1 to 15 read as FCR0 and 16 to 31 as FCR31.
constexpr std::uint32_t kFcr31Fixed = 0x01000001u;
constexpr std::uint32_t kFcr31Writable = 0x0083C078u;

std::uint32_t cfc1(Cpu &c, std::uint32_t reg) {
    return reg < 16 ? 0x2E30u : (c.fcr31 | kFcr31Fixed);
}

void ctc1(Cpu &c, std::uint32_t reg, std::uint32_t value) {
    if (reg == 31) c.fcr31 = value & kFcr31Writable;
}

// PCCR (COP0 register 25) holds CTE (bit 31) and, for counter n, the modes EXL, K, S and U (bits 1 to 4, 11 to 14) and
// the event (bits 5 to 9, 15 to 19); everything else reads 0. The counters are not stepped: every access first adds the
// cycles that passed since the last one (now(), the clock of the Count register) to the counters that are running.
// A cycle is also an instruction here, so the events "processor cycle" (1) and "instruction completed" (12, 13) count
// them all; the others (cache and TLB misses, branches, bus events) never happen.
constexpr std::uint32_t kPccrWritable = 0x800FFBFEu;

namespace {

bool perf_running(const Cpu &c, std::uint32_t n) {
    const std::uint32_t pccr = c.cop0[25], status = c.cop0[12], modes = (pccr >> (n * 10)) & 0x1Eu, event = (pccr >> (5 + n * 10)) & 31u;
    // Exception level, else kernel (also at error level), supervisor or user mode.
    const std::uint32_t mode = status & 2u ? 2u : status & 4u ? 4u : 4u << ((status >> 3) & 3u);
    return (modes & mode) && (event == 1 || event == 12 || event == 13);
}

void perf_update(Cpu &c) {
    const std::uint64_t at = now();
    const std::uint32_t cycles = static_cast<std::uint32_t>(at - c.pcr_at);
    c.pcr_at = at;
    if (!(c.cop0[25] >> 31)) return;
    for (std::uint32_t n = 0; n < 2; ++n)
        if (perf_running(c, n)) c.pcr[n] += cycles;
}

}

// The low bits of the instruction word: bit 0 picks the counters (mfpc, mtpc) over PCCR (mfps, mtps), bits 5 to 1 are
// the register number, of which only the lowest bit matters for a counter and only 0 for writing PCCR.
std::uint32_t mfperf(Cpu &c, std::uint32_t spec) {
    if (!(spec & 1u)) return c.cop0[25];
    perf_update(c);
    return c.pcr[(spec >> 1) & 1u];
}

void mtperf(Cpu &c, std::uint32_t spec, std::uint32_t value) {
    perf_update(c);
    if (spec & 1u) c.pcr[(spec >> 1) & 1u] = value;
    else if (!(spec >> 1)) c.cop0[25] = value & kPccrWritable;
}

namespace {

// vi24 is not a documented register, but the console reads back 0x0C0C after -1 is written to it.
std::uint32_t g_vi24 = 0;

// ctc2 to CMSAR1: VU1 starts at once at this microprogram address (in 64-bit instructions), whatever
// the VIF1 is doing; the EE does not wait for it.
void start_vu1(std::uint32_t address) {
    if (gs::queue::vu_active()) {
        // VU1 runs on its own thread and takes its work from the VIF1 queue, so the start goes through
        // it as a MSCAL (which also latches TOP/ITOP, unlike the real start).
        alignas(16) const std::uint32_t mscal[4] = {(0x14u << 24) | address, 0, 0, 0};
        vif::transfer(1, reinterpret_cast<const std::uint8_t *>(mscal), 1);
        return;
    }
    // The EE writes the microprogram memory directly, which the VIF's MPG upload would have announced.
    vu::code_changed(1);
    // The console lets a program without an E bit (garbage, or an empty memory) run forever beside the EE;
    // here it would stop the EE for good, so it does not start.
    bool ends = false;
    for (const std::uint32_t pc : vu::reachable(g_vu1_code, 16383u, address * 8u))
        if (g_vu1_code[pc + 7] & 0x40u) {
            ends = true;
            break;
        }
    if (!ends) {
        std::fprintf(stderr, "[vu1] program at 0x%04X never ends, not started\n", (address * 8u) & 16383u);
        return;
    }
    vu::run(1, address * 8u);
}

}

std::uint32_t cfc2(Cpu &c, std::uint32_t reg) {
    Vu0Regs &v = c.vu0;
    switch (reg) {
    case 16: return v.status;
    case 17: return v.mac;
    case 18: return v.clip;
    case 19: return 0x2E30u;
    case 20: return v.r;
    case 21: return v.i;
    case 22: return v.q;
    case 24: return g_vi24;
    case 26: return v.tpc >> 3;
    case 27: return v.cmsar0;
    // Neither unit is ever busy: a microprogram runs to its end inside the instruction that starts it.
    case 29: return 0;
    case 31:  // VU1's own register, which the vector unit thread writes
        drain_vu_thread();
        return vu::regs(1).cmsar0;
    default: return 0;
    }
}

void ctc2(Cpu &c, std::uint32_t reg, std::uint32_t value) {
    Vu0Regs &v = c.vu0;
    switch (reg) {
    case 16: v.status = (v.status & 0x3Fu) | (value & 0xFC0u); break;
    case 18: v.clip = value & 0xFFFFFFu; break;
    case 20: v.r = value & 0x7FFFFFu; break;
    case 21: v.i = value; break;
    case 22: v.q = value; break;
    case 24: g_vi24 = value & 0x0C0Cu; break;
    case 27: v.cmsar0 = value & 0xFFFFu; break;
    case 31:
        vu::regs(1).cmsar0 = value & 0xFFFFu;
        start_vu1(value & 0xFFFFu);
        break;
    // MAC (17), the constant in 19, TPC (26) and VPU_STAT (29) are read only; the reserved registers
    // (23, 25, 30) and FBRST (28) take no value.
    default: break;
    }
}

bool vu0_busy(Cpu &) { return false; }
void vu0_wait(Cpu &) {}
std::uint8_t *vu0_data() { return g_vu0_mem; }

bool cpcond0(Cpu &) { return dmac::cpcond0(); }

}
