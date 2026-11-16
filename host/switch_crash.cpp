// Switch only: what the CPU says when the program faults.
//
// A data abort prints nothing. The log simply stops, the system's error screen shows for a moment and the homebrew menu comes back, and
// the only evidence left is the last line of ps2run.log - which is why the renderer's first frames have been guessed at instead of read.
//
// libnx already catches the abort: its exception entry copies the registers into __nx_exceptiondump and then calls
// __libnx_exception_handler, a weak symbol nobody defines, so the dump is thrown away (init.o/exception.o in libnx.a). Defining it turns
// every fault into lines in ps2run.log: what kind of fault it was, the address it touched, the program counter, and the call chain.
//
// The addresses are printed as offsets from crash_marker below, so they can be read against the .elf the .nro was built from whatever
// address the console loaded the program at:
//
//   aarch64-none-elf-nm build/switch/.../ps2run.elf | grep crash_marker     # its address, A
//   aarch64-none-elf-addr2line -f -C -i -e build/switch/.../ps2run.elf $(printf '0x%x' $((A + OFFSET)))
//
// docs/deko3d.md has the same commands with a filled-in example.
#ifdef __SWITCH__

#include <switch.h>

#include "crash.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

// The address every printed one is an offset from: see the commands above.
void crash_marker() {}

const char *crash_name(std::uint32_t description) {
    switch (description) {
        case ThreadExceptionDesc_InstructionAbort: return "an instruction abort (the code jumped somewhere that is not code)";
        case ThreadExceptionDesc_MisalignedPC: return "a misaligned program counter";
        case ThreadExceptionDesc_MisalignedSP: return "a misaligned stack pointer";
        case ThreadExceptionDesc_SError: return "an SError";
        case ThreadExceptionDesc_BadSVC: return "a supervisor call the kernel refused";
        case ThreadExceptionDesc_Trap: return "a trap";
        default: return "a fault";
    }
}

// The frame pointer chain, as far as it looks like one. The fault has already happened, so a wrong guess here costs the rest of the dump:
// each link has to point forward and stay within a megabyte of the one before it, and the walk stops at the first that does not.
void crash_chain(std::uint64_t fp) {
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(&crash_marker);
    for (int frame = 0; frame < 16 && fp > 0x1000; ++frame) {
        const auto *words = reinterpret_cast<const std::uint64_t *>(fp);
        const std::uint64_t next = words[0], returned_to = words[1];
        if (returned_to > base) std::fprintf(stderr, "[crash]   #%d +0x%llx\n", frame, static_cast<unsigned long long>(returned_to - base));
        if (next <= fp || next - fp > (1u << 20)) return;
        fp = next;
    }
}

}  // namespace

extern "C" {

// libnx's own exception stack is a kilobyte (init.o). This handler prints, and printing on this console wants more room than that.
alignas(16) std::uint8_t __nx_exception_stack[0x4000];
std::uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);

// Filled in by libnx before it calls the handler below.
extern ThreadExceptionDump __nx_exceptiondump;

// libnx's return entry calls this with no arguments (the dump is the global above), and breaks the program when it returns.
void __libnx_exception_handler() {
    const ThreadExceptionDump &dump = __nx_exceptiondump;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(&crash_marker);
    std::fprintf(stderr, "[crash] the program hit %s\n", crash_name(dump.error_desc));
    std::fprintf(stderr, "[crash] the address it touched: 0x%llx, the instruction's exception syndrome: 0x%x\n",
                 static_cast<unsigned long long>(dump.far.x), dump.esr);
    std::fprintf(stderr, "[crash] pc 0x%llx (+0x%llx), lr 0x%llx (+0x%llx), sp 0x%llx, fp 0x%llx\n",
                 static_cast<unsigned long long>(dump.pc.x), static_cast<unsigned long long>(dump.pc.x - base),
                 static_cast<unsigned long long>(dump.lr.x), static_cast<unsigned long long>(dump.lr.x - base),
                 static_cast<unsigned long long>(dump.sp.x), static_cast<unsigned long long>(dump.fp.x));
    std::fflush(stderr);
    crash_chain(dump.fp.x);
    std::fprintf(stderr, "[crash] (the offsets are from crash_marker; aarch64-none-elf-addr2line reads them against ps2run.elf)\n");
    std::fflush(stderr);
}

}  // extern "C"

// One line at the start of a run, so that a log that has it says the fault dump is in place and a log that does not says the handler was
// not linked in: without this, "no crash lines" and "no crash" look the same.
void switch_crash_report() {
    std::fprintf(stderr, "[crash] a fault in the CPU prints its registers and call chain here (crash_marker 0x%llx)\n",
                 static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(&crash_marker)));
}

// host/crash.cpp (the PC's crash report) needs POSIX signals and ucontext, which the console does not have, so it is not built here.
// main() still installs it first thing: on the Switch the report is the exception handler above, and there is nothing else to install.
void crash::install(crash::After) {}

#endif
