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

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ps2 {
namespace {

// The console's compositor is given three back buffers, as the swap chains that work on it have.
constexpr std::uint32_t kBuffers = 3;
// One frame of this test is three commands: this is room for thousands of them, and the GPU reads them from here.
constexpr std::uint32_t kCmdBytes = 0x10000;

// Where deko3d reports an error it hit (a bad command, an out of memory, a rejected object): on the console the only place to put it is
// the log, which is ps2run.log in the .nro's folder.
void log_device_error(void *, const char *context, DkResult result, const char *message) {
    std::fprintf(stderr, "[deko] device says: %s: %s (%d)\n", context && *context ? context : "error", message && *message ? message : "", int(result));
}

// PS2_DEKO3D=1 means the default number of frames; PS2_DEKO3D=<n> with n > 1 means n frames, so a run can be made as short or as long
// as the person watching it wants.
std::uint32_t frames_wanted(std::uint32_t asked) {
    if (asked) return asked;
    const char *setting = std::getenv("PS2_DEKO3D");
    const long frames = setting ? std::strtol(setting, nullptr, 10) : 0;
    return frames > 1 ? static_cast<std::uint32_t>(frames) : 300;
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

    // The device: deko3d's handle on the GPU. Everything else is created from it.
    dk::UniqueDevice device = dk::DeviceMaker{}.setCbDebug(log_device_error).create();
    if (!device) {
        std::fprintf(stderr, "[deko] the device did not open\n");
        return 1;
    }
    std::fprintf(stderr, "[deko] device open\n");

    // One queue, graphics only: the commands of every frame go through it.
    dk::UniqueQueue queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics)
                                .setCommandMemorySize(512 * 1024).setFlushThreshold(64 * 1024).create();
    if (!queue) {
        std::fprintf(stderr, "[deko] the queue did not come up\n");
        return 1;
    }

    // Where the command lists are built (the CPU writes them, the GPU reads them afterwards).
    dk::UniqueMemBlock command_memory = dk::MemBlockMaker{device, kCmdBytes}
                                            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached).create();
    dk::UniqueCmdBuf commands = dk::CmdBufMaker{device}.create();
    if (!command_memory || !commands) {
        std::fprintf(stderr, "[deko] no command memory\n");
        return 1;
    }
    dkCmdBufAddMemory(commands, command_memory, 0, kCmdBytes);

    // The back buffers the compositor presents from: they have to be render targets and presentable images at once.
    std::array<dk::Image, kBuffers> images;
    std::array<dk::UniqueMemBlock, kBuffers> image_memory;
    std::array<DkImageView, kBuffers> views;
    for (std::uint32_t i = 0; i < kBuffers; ++i) {
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}.setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent)
            .setFormat(DkImageFormat_RGBA8_Unorm).setDimensions(width, height).setMipLevels(1).initialize(layout);
        // a memory block is a multiple of the GPU's page size
        const std::uint64_t bytes = (std::uint64_t(layout.getSize()) + DK_MEMBLOCK_ALIGNMENT - 1) & ~std::uint64_t(DK_MEMBLOCK_ALIGNMENT - 1);
        image_memory[i] = dk::MemBlockMaker{device, static_cast<std::uint32_t>(bytes)}
                              .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image).create();
        if (!image_memory[i]) {
            std::fprintf(stderr, "[deko] back buffer %u has no memory (%llu bytes)\n", i, static_cast<unsigned long long>(bytes));
            return 1;
        }
        images[i].initialize(layout, image_memory[i], 0);
        views[i] = dk::ImageView{images[i]};
    }
    std::fprintf(stderr, "[deko] %u back buffers of %ux%u\n", kBuffers, unsigned(width), unsigned(height));

    // The swap chain: the handle the compositor presents through, made of those back buffers on that window.
    std::array<const DkImage *, kBuffers> presentable{};
    for (std::uint32_t i = 0; i < kBuffers; ++i) presentable[i] = &images[i];
    dk::UniqueSwapchain swapchain = dk::SwapchainMaker{device, window, presentable}.create();
    if (!swapchain) {
        std::fprintf(stderr, "[deko] no swap chain on the default window (the window is taken by something else, or %ux%u is not a size"
                             " it accepts)\n", unsigned(width), unsigned(height));
        return 1;
    }
    std::fprintf(stderr, "[deko] swap chain made\n");

    const auto started = std::chrono::steady_clock::now();
    std::uint32_t presented = 0;
    DkFence finished{};  // the GPU signals it at the end of each frame, so the next frame may write the commands again

    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        if (frame) {
            const DkResult waited = dkFenceWait(&finished, 1000000000LL);  // a second
            if (waited != DkResult_Success) {
                std::fprintf(stderr, "[deko] frame %u: the GPU did not finish the frame before it (%d)\n", frame, int(waited));
                return 1;
            }
        }
        // Blocks until a back buffer is free: this is where the console's refresh rate shows up.
        const int slot = dkQueueAcquireImage(queue, swapchain);
        if (slot < 0 || slot >= static_cast<int>(kBuffers)) {
            std::fprintf(stderr, "[deko] frame %u: no back buffer came back (%d)\n", frame, slot);
            return 1;
        }
        float red = 0.0f, green = 0.0f, blue = 0.0f;
        frame_colour(frame, red, green, blue);
        dkCmdBufClear(commands);  // the last frame's commands are free to be overwritten (the fence above said so)
        dkCmdBufBindRenderTarget(commands, &views[slot], nullptr);
        dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, red, green, blue, 1.0f);
        // the clear has to be in memory before the display engine reads the image: the strongest ordering there is, one per frame
        dkCmdBufBarrier(commands, DkBarrier_Full, 0);
        dkCmdBufSignalFence(commands, &finished, true);
        dkQueueSubmitCommands(queue, dkCmdBufFinishList(commands));
        dkQueueFlush(queue);
        dkQueuePresentImage(queue, swapchain, slot);
        ++presented;
        if (frame % 60 == 59) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr, "[deko] %u frames in %.1f s (%.1f frames a second)\n", presented, seconds, presented / seconds);
        }
    }

    dkQueueWaitIdle(queue);
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
