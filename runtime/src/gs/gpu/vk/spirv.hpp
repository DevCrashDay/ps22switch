#pragma once

// The graphics chip's drawing program (gs.wgsl) as SPIR-V for the Vulkan renderer (wgpu_vk.cpp): made by scripts/wgsl_spirv.py with
// naga, one variant for each set of override constants, and built into the program from runtime/src/gs/gpu/vk/spirv/*.spv.

#include <cstddef>
#include <cstdint>

namespace ps2::gs::gpu::vk {

struct SpirvVariant {
    std::uint64_t wgsl_hash;  // FNV-1a of the WGSL text without carriage returns: the text it was made from
    std::uint32_t scale, tile;
    bool explicit_fma;
    const std::uint32_t *words;
    std::size_t word_count;
};

extern const SpirvVariant kSpirvVariants[];
extern const std::size_t kSpirvVariantCount;

}  // namespace ps2::gs::gpu::vk
