// Stage 0 of the deko3d renderer: the smallest program that puts a frame on the console's screen with deko3d and nothing else.
//
// It opens a device, makes a swap chain on the console's own default window (nwindowGetDefault), clears each back buffer to a colour
// and presents it, then leaves. SDL and NVK are not touched, so what it proves is deko3d itself: the device opens, a swap chain exists
// on this console's window, a command list is submitted, the GPU clears the image, and the compositor shows it. That is the ground the
// next stage (a wgpu_deko.cpp underneath gpu.cpp's WebGPU calls) stands on; on its own it draws no game.
//
// Started from host/main.cpp: `PS2_DEKO3D=<frames>` in launch.txt takes this path instead of the game, before SDL opens the window
// (SDL takes the window for itself, which is why the Vulkan present path had nothing to present to).
#include "ps2/deko_stage0.h"

#ifdef __SWITCH__

#include <deko3d.hpp>
#include <switch.h>

#include "deko_gpu.hpp"
#include "dksh.hpp"

#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2 {
namespace {

using gs::gpu::deko::DkshProgram;
using gs::gpu::deko::DkshStage_Vert;
using gs::gpu::deko::dksh_program;
using gs::gpu::deko::Gpu;
using gs::gpu::deko::Memory;
using gs::gpu::deko::Image;
using gs::gpu::deko::Swapchain;

// A deko3d shader module starts with this: DkshHeader { magic, header_sz, control_sz, code_sz, programs_off, num_programs }.
constexpr std::uint32_t kDkshMagic = 0x48534B44;

// PS2_DEKO3D=1 means the default number of frames; PS2_DEKO3D=<n> with n > 1 means n frames, so a run can be made as short or as long
// as the person watching it wants.
std::uint32_t frames_wanted(std::uint32_t asked) {
    if (asked) return asked;
    const char *setting = std::getenv("PS2_DEKO3D");
    const long frames = setting ? std::strtol(setting, nullptr, 10) : 0;
    return frames > 1 ? static_cast<std::uint32_t>(frames) : 300;
}

// PS2_DEKO3D_DRAW=1 draws a triangle over the colour of every frame: the same device, swap chain and frame loop, with a shader module
// deko3d loaded, a pipeline, a viewport and a draw. The programs are stage_draw.wgsl's, so a failure here is the shader path's.
bool draw_wanted() {
    const char *setting = std::getenv("PS2_DEKO3D_DRAW");
    return setting && *setting && std::strcmp(setting, "0") != 0;
}

// PS2_DEKO3D_DRAW=2 loads one of the *compute* programs (the smallest, `snapshot`) and nothing else, then leaves. It answers a question
// the game run cannot: whether deko3d on this console takes a compute shader module from uam at all, with no buffers, no render pass and
// no game memory in the way.
const char *compute_test_wanted() {
    const char *setting = std::getenv("PS2_DEKO3D_DRAW");
    return setting && std::strcmp(setting, "2") == 0 ? "snapshot" : nullptr;
}

// A DKSH module is two sections: the control block, which stays in main memory (dkShaderInitialize keeps the pointer), and the code,
// which the GPU reads out of a memblock with the Code flag. This is the size deko3d wants for one module in that memblock.
std::uint32_t module_size(const DkshProgram *program) {
    if (!program || !program->words || program->count < 6 || program->words[0] != kDkshMagic) return 0;
    return (program->words[3] + DK_SHADER_CODE_UNUSABLE_SIZE + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
}

// Copy one program's code into the memblock and hand deko3d its address and its control block.
bool load_module(dk::MemBlock code, std::uint32_t offset, const DkshProgram *program, DkShader &shader) {
    if (!program) return false;
    const std::uint32_t control_size = program->words[2], code_size = program->words[3];
    if (control_size + code_size != program->count * 4) {
        std::fprintf(stderr, "[deko] %s: the module says %u + %u bytes but is %zu\n", program->name, control_size, code_size,
                     program->count * 4);
        return false;
    }
    std::memcpy(static_cast<std::uint8_t *>(code.getCpuAddr()) + offset, reinterpret_cast<const std::uint8_t *>(program->words) + control_size,
                code_size);
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, code, offset);
    maker.control = program->words;
    dkShaderInitialize(&shader, &maker);
    if (!dkShaderIsValid(&shader) || dkShaderGetStage(&shader) != (program->stage == DkshStage_Vert ? DkStage_Vertex : DkStage_Fragment)) {
        std::fprintf(stderr, "[deko] %s: deko3d rejected the shader module\n", program->name);
        return false;
    }
    return true;
}

// The triangle's two programs, the memory they run from, and whether they are ready.
struct TestDraw {
    dk::UniqueMemBlock code;
    DkShader vertex{};
    DkShader fragment{};
    bool ready = false;
};

bool prepare_draw(dk::Device device, TestDraw &draw) {
    const DkshProgram *vertex = dksh_program("triangle_vs");
    const DkshProgram *fragment = dksh_program("triangle_fs");
    const std::uint32_t vertex_size = module_size(vertex), fragment_size = module_size(fragment);
    if (!vertex_size || !fragment_size) {
        std::fprintf(stderr, "[deko] the triangle's programs are not in this build (scripts/wgsl_dksh.py)\n");
        return false;
    }
    draw.code = dk::MemBlockMaker{device, (vertex_size + fragment_size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1)}
                    .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code).create();
    if (!draw.code) {
        std::fprintf(stderr, "[deko] no memory for the shader code (%u bytes)\n", vertex_size + fragment_size);
        return false;
    }
    if (!load_module(draw.code, 0, vertex, draw.vertex) || !load_module(draw.code, vertex_size, fragment, draw.fragment)) return false;
    draw.ready = true;
    std::fprintf(stderr, "[deko] shader modules loaded: %u + %u bytes of code\n", vertex_size, fragment_size);
    return true;
}

// Everything the triangle needs bound: the shaders, the state, and the viewport and scissor, which say which part of the image it
// covers. The two state values that are not deko3d's defaults are the ones that silently draw nothing: deko3d culls back faces and has
// the depth test on by default, and this triangle is one winding order with no depth target bound.
void draw_triangle(DkCmdBuf cmd, const DkImageView &target, std::uint32_t width, std::uint32_t height, const TestDraw &draw) {
    dk::RasterizerState rasterizer;  // fill, no culling (the default culls back faces, and a device whose window origin is the top
    rasterizer.setCullMode(DkFace_None);  // left sees this triangle's winding the other way round)
    dk::MultisampleState multisample;
    dk::ColorState color;
    dk::ColorWriteState color_write;
    dk::BlendState blend;
    dk::DepthStencilState depth;  // no depth test, no depth write (the default has both on, and there is no depth target here)
    depth.setDepthTestEnable(false);
    depth.setDepthWriteEnable(false);
    DkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    DkScissor scissor{0, 0, width, height};
    const DkShader *shaders[] = {&draw.vertex, &draw.fragment};
    dkCmdBufBindRenderTarget(cmd, &target, nullptr);
    dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindRasterizerState(cmd, &rasterizer);
    dkCmdBufBindMultisampleState(cmd, &multisample);
    dkCmdBufBindColorState(cmd, &color);
    dkCmdBufBindColorWriteState(cmd, &color_write);
    dkCmdBufBindBlendStates(cmd, 0, &blend, 1);
    dkCmdBufBindDepthStencilState(cmd, &depth);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);  // the triangle comes from the vertex index: no attributes, no buffers
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    dkCmdBufSetSampleMask(cmd, 0x1);  // one sample, the first: without it a cleared command buffer draws into nothing
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
}

// The colour of a frame: one turn of the hue wheel a second at 60 Hz, so that a screen that is being presented over and over looks
// different from a screen that is stuck.
void frame_colour(std::uint32_t frame, float &red, float &green, float &blue) {
    const float hue = std::fmod(static_cast<float>(frame) / 60.0f, 1.0f) * 6.0f;
    const int sector = static_cast<int>(hue) % 6;
    const float rising = hue - std::floor(hue), falling = 1.0f - rising;
    const float full = 0.95f, empty = 0.05f;
    switch (sector) {
        case 0: red = full; green = full * rising; blue = empty; break;
        case 1: red = full * falling; green = full; blue = empty; break;
        case 2: red = empty; green = full; blue = full * rising; break;
        case 3: red = empty; green = full * falling; blue = full; break;
        case 4: red = full * rising; green = empty; blue = full; break;
        default: red = full; green = empty; blue = full * falling; break;
    }
}

}
}  // namespace ps2

namespace ps2 {

int deko_stage0(std::uint32_t asked) {
    const std::uint32_t frames = frames_wanted(asked);
    std::fprintf(stderr, "[deko] stage 0: deko3d on the default window, %u frames\n", frames);

    NWindow *window = nwindowGetDefault();
    if (!window) {
        std::fprintf(stderr, "[deko] the console has no default window\n");
        return 1;
    }
    u32 width = 0, height = 0;
    if (R_FAILED(nwindowGetDimensions(window, &width, &height)) || !width || !height) {
        width = 1280;
        height = 720;
        std::fprintf(stderr, "[deko] the window does not say its size: 1280x720 assumed\n");
    }
    std::fprintf(stderr, "[deko] window %ux%u\n", unsigned(width), unsigned(height));

    // The device, the queue and the command list: deko_gpu.cpp, the layer the WebGPU shim (wgpu_deko.cpp) will stand on.
    Gpu gpu;
    if (!gpu.open()) return 1;
    std::fprintf(stderr, "[deko] device open\n");

    // Three back buffers on the console's own window: render targets and presentable images, acquired one frame at a time.
    Swapchain swap_chain;
    if (!swap_chain.create(gpu, window, width, height)) {
        std::fprintf(stderr, "[deko] no swap chain on the default window (the window is taken by something else, or %ux%u is not a size"
                             " it accepts)\n", unsigned(width), unsigned(height));
        return 1;
    }
    std::fprintf(stderr, "[deko] 3 back buffers of %ux%u, swap chain made\n", unsigned(width), unsigned(height));

    if (const char *entry = compute_test_wanted()) {
        // One compute module through the same loader the renderer uses, then out: no swap chain, no passes, nothing else.
        const DkshProgram *program = dksh_program(entry);
        if (!program) {
            std::fprintf(stderr, "[deko] compute test: there is no module called %s\n", entry);
            return 1;
        }
        const std::uint32_t size = module_size(program);
        std::shared_ptr<Memory> code = gpu.allocate(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
        if (!code) {
            std::fprintf(stderr, "[deko] compute test: no memory for the code\n");
            return 1;
        }
        DkShader shader{};
        const std::uint32_t control_size = program->words[2], code_bytes = program->words[3];
        std::memcpy(code->cpu, reinterpret_cast<const std::uint8_t *>(program->words) + control_size, code_bytes);
        std::fprintf(stderr, "[deko] compute test: %s, %u bytes of code, control %u\n", entry, code_bytes, control_size);
        DkShaderMaker maker;
        dkShaderMakerDefaults(&maker, code->block, 0);
        maker.control = program->words;
        dkShaderInitialize(&shader, &maker);
        std::fprintf(stderr, "[deko] compute test: deko3d took it, valid %d, stage %d (compute is %d)\n", int(dkShaderIsValid(&shader)),
                     int(dkShaderGetStage(&shader)), int(DkStage_Compute));
        return 0;
    }

    // PS2_DEKO3D_DRAW=1: the triangle, which needs the shader path (module, pipeline, viewport, draw) and not the game
    TestDraw draw;
    if (draw_wanted() && !prepare_draw(gpu.device(), draw)) return 1;

    const auto started = std::chrono::steady_clock::now();
    std::uint32_t presented = 0;

    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        // Blocks until the compositor frees a back buffer: this is where the console's refresh rate shows itself.
        const std::shared_ptr<Image> target = swap_chain.acquire();
        if (!target) {
            std::fprintf(stderr, "[deko] frame %u: no back buffer\n", frame);
            return 1;
        }
        float red = 0.0f, green = 0.0f, blue = 0.0f;
        frame_colour(frame, red, green, blue);
        DkCmdBuf commands = gpu.begin();  // waits for the frame before, which frees the command memory
        dkCmdBufBindRenderTarget(commands, &target->view, nullptr);
        dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, red, green, blue, 1.0f);
        if (draw.ready) draw_triangle(commands, target->view, width, height, draw);
        // the frame has to be in memory before the display engine reads the image: the strongest ordering there is, once a frame
        dkCmdBufBarrier(commands, DkBarrier_Full, 0);
        gpu.submit(false);
        swap_chain.present();
        ++presented;
        if (frame % 60 == 59) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr, "[deko] %u frames in %.1f s (%.1f frames a second)\n", presented, seconds, presented / seconds);
        }
    }

    gpu.wait();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "[deko] stage 0 done: %u frames presented in %.0f ms (%.1f frames a second)\n", presented, seconds * 1000.0,
                 seconds > 0.0 ? presented / seconds : 0.0);
    return 0;
}

}  // namespace ps2

#else

#include <cstdio>

namespace ps2 {

int deko_stage0(std::uint32_t) {
    std::fprintf(stderr, "[deko] stage 0 is deko3d, which only the Switch build has (PS2_DEKO3D)\n");
    return 1;
}

}  // namespace ps2

#endif
