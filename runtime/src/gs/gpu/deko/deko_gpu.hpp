#pragma once

// The deko3d layer under the WebGPU shim (wgpu_deko.cpp): a device, a queue, memory the CPU writes and the GPU reads, images, and the
// swap chain on the console's own window. Nothing of the WebGPU object model here — that file has enough of its own.
//
// Every buffer and image is a deko3d memory block, page-aligned, with the CPU-uncached/Gpu-cached flags the console wants for memory both
// sides touch. The GPU's L2 is not coherent with the CPU, so anything written by the CPU must be followed by flush() before the GPU reads
// it, and anything written by the GPU must be followed by invalidate() before the CPU reads it (both are the barriers NETHerSX2 uses:
// DkBarrier_Full with DkInvalidateFlags_L2Cache).

#include <cstdint>
#include <deko3d.hpp>
#include <memory>

namespace ps2::gs::gpu::deko {

// The image formats gpu.cpp asks for. (Colour and depth targets, R32F for the depth upload, RGBA8Uint for the frame the compute
// renderer keeps, RGBA8Unorm for the display.)
enum class Format { Rgba8Unorm, Rgba8Uint, R32Float, Depth32Float };

// The deko3d format of one of those, or DkImageFormat_None for a format this layer does not know.
DkImageFormat image_format(Format format);

// A page-aligned memory block: the CPU address, the GPU address and the size, for the same bytes.
struct Memory {
    dk::UniqueMemBlock block;
    std::uint8_t *cpu = nullptr;
    DkGpuAddr gpu = 0;
    std::uint32_t size = 0;
};

// A memory block that is a texture: one image, one view of it (what a render pass or a descriptor wants).
struct Image {
    std::shared_ptr<Memory> memory;
    dk::Image image;
    DkImageView view{};
    Format format = Format::Rgba8Unorm;
    std::uint32_t width = 0, height = 0;
    // Its two entries in the device's descriptor tables, made on first use: one for reading it as a texture (a handle and a sampler),
    // one for writing it as a storage image. They belong to the image and not to the WebGPU texture object, because gpu.cpp makes a new
    // texture object for the same screen image every frame and the table has a thousand entries.
    std::uint32_t image_slot = 0xFFFFFFFFu, storage_slot = 0xFFFFFFFFu;
};

// The device, the queue and the command list a frame is recorded into. One Gpu per program.
class Gpu {
public:
    ~Gpu();
    bool open(std::uint32_t command_bytes = 1 << 20);
    void close();

    dk::Device device() const { return m_device; }
    dk::Queue queue() const { return m_queue; }
    bool is_open() const { return static_cast<bool>(m_device); }

    // flags: DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached for what both sides touch, plus DkMemBlockFlags_Code for shaders.
    std::shared_ptr<Memory> allocate(std::uint64_t bytes, std::uint32_t flags);
    // A texture: DkImageFlags_UsageRender | UsageStorage | UsageSample | UsageTransfer as needed by the caller.
    std::shared_ptr<Image> image(std::uint32_t width, std::uint32_t height, Format format, std::uint32_t flags);

    // Recording. begin() waits for the previous frame to finish, so the command memory is free again; if the CPU has written memory since
    // the last frame, it starts with the L2 invalidate that makes those writes visible to the GPU.
    DkCmdBuf begin();
    // Submit what begin() recorded, and optionally wait for the GPU to finish it.
    void submit(bool wait);
    void wait();
    bool in_error() const;

    // Call after the CPU has written a buffer the GPU will read (a queue write): the next begin() starts with the invalidate.
    void note_cpu_write();
    // Make what the GPU wrote readable by the CPU: a barrier and a wait. Reading without this can see the L2's stale lines instead.
    void sync_for_cpu_read();

private:
    dk::UniqueDevice m_device;
    dk::UniqueQueue m_queue;
    dk::UniqueMemBlock m_command_memory;
    dk::UniqueCmdBuf m_commands;
    DkFence m_fence{};
    bool m_pending = false;
    bool m_cpu_wrote = false;
    std::uint32_t m_command_bytes = 0;
};

// The console's window and the three images the compositor presents from: acquired (one at a time), cleared or drawn into, presented.
class Swapchain {
public:
    // `window` is the console's NWindow (nwindowGetDefault()).
    bool create(Gpu &gpu, void *window, std::uint32_t width, std::uint32_t height);
    void destroy();
    bool ok() const { return static_cast<bool>(m_swapchain); }

    // Blocks until a back buffer is free. nullptr when the swap chain is not usable (the caller then draws nowhere).
    std::shared_ptr<Image> acquire();
    void present();
    std::uint32_t width() const { return m_width; }
    std::uint32_t height() const { return m_height; }

private:
    dk::UniqueSwapchain m_swapchain;
    Gpu *m_gpu = nullptr;
    std::shared_ptr<Image> m_images[3];
    int m_slot = -1;
    std::uint32_t m_width = 0, m_height = 0;
};

}
