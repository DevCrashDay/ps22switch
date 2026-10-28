#pragma once

#include "analysis.hpp"
#include "elf.hpp"
#include "snr2.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace recomp {

// An instruction the game's patch file (PCSX2 .pnach) switches at run time: the translation holds both versions, and the one
// that runs depends on a 16-bit game variable (see scripts/pnach.py, "toggle"). `word` is the version used while the
// variable at `cond_addr` equals `value`; the other version is the instruction in the ELF.
struct Toggle {
    std::uint32_t word{};
    std::uint32_t cond_addr{};
    std::uint16_t value{};
};

struct CodegenOptions {
    std::string output;
    std::uint32_t unit_bits = 14;
    std::set<std::uint32_t> probes;
    std::map<std::uint32_t, Toggle> toggles;
    // Not 0: the translation becomes a loadable library of the runner (runtime/src/ee_live.cpp) and table.cpp exports its
    // ps2_ee_live_stamp() and ps2_ee_live_init() instead of register_generated_code().
    std::uint64_t live_stamp = 0;
    bool quiet = false;  // no line on stdout
};

struct Translation {
    const Image *image{};
    Program *program{};
    const Links *links{};
    std::string prefix;
    bool relocatable{};
    std::string name;
    std::uint32_t size{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> checks;
    // A translation of a code window the game itself rewrites. It is emitted
    // into the module table like a relocatable module's, but its addresses are
    // absolute: every image of that window is precompiled for the one base it
    // occupies.
    bool fixed_module{};
    std::uint32_t fixed_base{};
};

void generate(const std::vector<Translation> &translations, const CodegenOptions &options);

}
