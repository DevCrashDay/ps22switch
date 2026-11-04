#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2::gs::gpu::deko {

// The programs the deko3d renderer draws with, made from hw.wgsl and blit.wgsl by scripts/wgsl_dksh.py: naga writes GLSL 450 and uam
// compiles that into a deko3d shader module for the Tegra X1. dksh_table.cpp holds the bytes.
enum DkshStage { DkshStage_Vert, DkshStage_Frag, DkshStage_Comp };

struct DkshProgram {
    const char *name;  // the WGSL entry point it came from
    DkshStage stage;
    const std::uint32_t *words;
    std::size_t count;  // in 32-bit words (a deko3d shader module is a multiple of 4 bytes)
};

// Looked up by the entry point's name: nullptr when there is no such program.
const DkshProgram *dksh_program(const char *name);

extern const DkshProgram kDkshPrograms[];
extern const std::size_t kDkshProgramCount;

}
