#include "deko_gpu.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace ps2::gs::gpu::deko {
namespace {

std::uint32_t align_up(std::uint64_t value, std::uint32_t alignment) {
    return static_cast<std::uint32_t>((value + alignment - 1) & ~std::uint64_t(alignment - 1));
}

void log_device_error(void *, const char *context, DkResult result, const char *message) {
    std::fprintf(stderr, "[deko] device says: %s: %s (%d)\n", context && *context ? context : "error",
                 message && *message ? message : "", int(result));
}

}

DkImageFormat image_format(Format format) {
    switch (format) {
        case Format::Rgba8Unorm: return DkImageFormat_RGBA8_Unorm;
        case Format::Rgba8Uint: return DkImageFormat_RGBA8_Uint;
        case Format::R32Float: return DkImageFormat_R32_Float;
        case Format::Depth32Float: return DkImageFormat_ZF32;
    }
    return DkImageFormat_None;
}

Gpu::~Gpu() { close(); }

bool Gpu::open(std::uint32_t command_bytes) {
    if (m_device) return true;
    m_device = dk::DeviceMaker{}.setCbDebug(log_device_error).create();
    if (!m_device) {
        std::fprintf(stderr, "[deko] the device did not open\n");
        return false;
    }
    // Graphics and compute: the renderer draws with both (the memory moves are compute programs).
    // 64 KiB of per-warp scratch: deko3d's default is 2 KiB, and the compute programs say how much they need (uam's `upload` asks for
    // exactly 2 KiB, which left no room at all).
    m_queue = dk::QueueMaker{m_device}.setFlags(DkQueueFlags_Graphics | DkQueueFlags_Compute)
                  .setCommandMemorySize(512 * 1024).setFlushThreshold(64 * 1024).setPerWarpScratchMemorySize(64 * 1024).create();
    if (!m_queue) {
        std::fprintf(stderr, "[deko] the queue did not come up\n");
        close();
        return false;
    }
    m_command_bytes = align_up(command_bytes, DK_MEMBLOCK_ALIGNMENT);
    m_command_memory = dk::MemBlockMaker{m_device, m_command_bytes}
                           .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached).create();
    m_commands = dk::CmdBufMaker{m_device}.create();
    if (!m_command_memory || !m_commands) {
        std::fprintf(stderr, "[deko] no command memory\n");
        close();
        return false;
    }
    dkCmdBufAddMemory(m_commands, m_command_memory, 0, m_command_bytes);
    return true;
}

void Gpu::close() {
    if (m_queue) wait();
    m_commands = nullptr;
    m_command_memory = nullptr;
    m_queue = nullptr;
    m_device = nullptr;
    m_pending = false;
    m_cpu_wrote = false;
}

std::shared_ptr<Memory> Gpu::allocate(std::uint64_t bytes, std::uint32_t flags) {
    if (!m_device || !bytes) return nullptr;
    // deko3d aborts rather than returning null when it cannot make a block, so a line before each of the first ones says how far the
    // renderer's memory setup got, and with which size, if one of them is the end of the log.
    static std::uint32_t traced = 0;
    if (traced < 40) std::fprintf(stderr, "[deko] memory block %u: %llu bytes, flags 0x%x\n", ++traced,
                                   static_cast<unsigned long long>(bytes), flags);
    auto memory = std::make_shared<Memory>();
    memory->size = align_up(bytes, DK_MEMBLOCK_ALIGNMENT);
    memory->block = dk::MemBlockMaker{m_device, memory->size}.setFlags(flags).create();
    if (!memory->block) {
        std::fprintf(stderr, "[deko] no memory block of %u bytes\n", memory->size);
        return nullptr;
    }
    memory->cpu = static_cast<std::uint8_t *>(memory->block.getCpuAddr());
    memory->gpu = memory->block.getGpuAddr();
    return memory;
}

std::shared_ptr<Image> Gpu::image(std::uint32_t width, std::uint32_t height, Format format, std::uint32_t flags) {
    if (!m_device || !width || !height) return nullptr;
    const DkImageFormat deko_format = image_format(format);
    if (deko_format == DkImageFormat_None) {
        std::fprintf(stderr, "[deko] image format %d is not one this layer knows\n", int(format));
        return nullptr;
    }
    auto result = std::make_shared<Image>();
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{m_device}.setFlags(flags).setFormat(deko_format).setDimensions(width, height).setMipLevels(1).initialize(layout);
    result->memory = allocate(layout.getSize(), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!result->memory) return nullptr;
    result->image.initialize(layout, result->memory->block, 0);
    result->view = dk::ImageView{result->image};
    result->format = format;
    result->width = width;
    result->height = height;
    return result;
}

DkCmdBuf Gpu::begin() {
    if (m_pending) wait();
    m_commands.clear();
    if (m_cpu_wrote) {
        // What the CPU wrote is in memory but the GPU's L2 may still hold the lines from before: drop them before it reads.
        dkCmdBufBarrier(m_commands, DkBarrier_Full, DkInvalidateFlags_L2Cache);
        m_cpu_wrote = false;
    }
    return m_commands;
}

void Gpu::submit(bool wait_for_it) {
    if (!m_queue || !m_commands) return;
    if (m_pending) wait();  // one submission in flight: the command memory is reused, and the fence is the same one
    dkCmdBufSignalFence(m_commands, &m_fence, true);
    dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_commands));
    dkQueueFlush(m_queue);
    m_pending = true;
    if (dkQueueIsInErrorState(m_queue)) std::fprintf(stderr, "[deko] the queue is in an error state after submitting\n");
    if (wait_for_it) wait();
}

void Gpu::wait() {
    if (!m_pending) return;
    static std::uint32_t waits = 0;
    const bool loud = waits < 4;
    if (loud) std::fprintf(stderr, "[deko] waiting for submission #%u\n", ++waits);
    const DkResult result = dkFenceWait(&m_fence, 4000000000LL);  // four seconds
    if (result != DkResult_Success) std::fprintf(stderr, "[deko] the GPU did not finish a submission (%d)\n", int(result));
    if (loud) std::fprintf(stderr, "[deko] submission #%u finished, queue %s\n", waits,
                           dkQueueIsInErrorState(m_queue) ? "IN ERROR" : "healthy");
    m_pending = false;
}

bool Gpu::in_error() const { return m_queue && dkQueueIsInErrorState(m_queue); }

void Gpu::note_cpu_write() { m_cpu_wrote = true; }

void Gpu::sync_for_cpu_read() {
    if (!m_queue || !m_commands) return;
    if (m_pending) wait();
    m_commands.clear();
    // The GPU's writes have to be out of L2 before the CPU looks: the same barrier NetherSX2 uses before a readback.
    dkCmdBufBarrier(m_commands, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    dkCmdBufSignalFence(m_commands, &m_fence, true);
    dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_commands));
    dkQueueFlush(m_queue);
    m_pending = true;
    wait();
}

bool Swapchain::create(Gpu &gpu, void *window, std::uint32_t width, std::uint32_t height) {
    destroy();
    if (!gpu.is_open() || !window || !width || !height) return false;
    m_gpu = &gpu;
    m_width = width;
    m_height = height;
    for (auto &image : m_images) {
        // Render targets, shader image load/store (the compute renderer's frame) and presentable: the compositor reads them.
        image = gpu.image(width, height, Format::Rgba8Unorm, DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                                                               DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine);
        if (!image) {
            std::fprintf(stderr, "[deko] no memory for a back buffer of %ux%u\n", width, height);
            destroy();
            return false;
        }
    }
    std::array<const DkImage *, 3> presentable{&m_images[0]->image, &m_images[1]->image, &m_images[2]->image};
    m_swapchain = dk::SwapchainMaker{gpu.device(), window, presentable}.create();
    if (!m_swapchain) {
        std::fprintf(stderr, "[deko] no swap chain on the window (%ux%u)\n", width, height);
        destroy();
        return false;
    }
    m_slot = -1;
    return true;
}

void Swapchain::destroy() {
    if (m_gpu) m_gpu->wait();  // the GPU may still be drawing into these images
    m_swapchain = nullptr;
    for (auto &image : m_images) image = nullptr;
    m_slot = -1;
    m_gpu = nullptr;
}

std::shared_ptr<Image> Swapchain::acquire() {
    if (!m_swapchain || !m_gpu) return nullptr;
    // Blocks until the compositor frees a back buffer: this is where the console's refresh rate shows itself.
    const int slot = dkQueueAcquireImage(m_gpu->queue(), m_swapchain);
    if (slot < 0 || slot >= 3) {
        std::fprintf(stderr, "[deko] no back buffer came back (%d)\n", slot);
        return nullptr;
    }
    m_slot = slot;
    return m_images[slot];
}

void Swapchain::present() {
    if (!m_swapchain || !m_gpu || m_slot < 0) return;
    dkQueuePresentImage(m_gpu->queue(), m_swapchain, m_slot);
    m_slot = -1;
}

}
