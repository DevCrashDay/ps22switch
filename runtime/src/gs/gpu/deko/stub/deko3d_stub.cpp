// A stand-in for deko3d on the PC, so the WebGPU shim (wgpu_deko.cpp) and the layer under it (deko_gpu.cpp) can be *run* against the
// real gpu.cpp usage instead of only compiled: the console round trip is minutes, this is a second. Nothing here draws; it checks that
// every call is made correctly - handles alive, sizes and alignments sane, entry points present, images inside their memory - and reports
// through harness_failures() when the shim asks for something deko3d would reject.
//
// Built only by scripts/deko_harness.py; never part of a .nro.

#include <deko3d.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// deko3d's handles are opaque pointers to types the real library owns: the stub makes its own and casts between them.
namespace stub {
struct Device { int unused = 0; };
struct Queue { int unused = 0; };
struct MemBlock {
    std::uint8_t *bytes = nullptr;
    std::uint32_t size = 0;
    std::uint32_t flags = 0;
};
struct CmdBuf {
    std::uint32_t binds = 0, draws = 0, dispatches = 0, barriers = 0, copies = 0, calls = 0;
    bool memory_added = false;
};
struct Swapchain {
    int images = 0;
};
// the opaque structs are byte arrays; their state lives here, keyed by the address the shim was given
struct ShaderState {
    DkStage stage = DkStage_Vertex;
    bool valid = false;
};
struct ImageState {
    std::uint32_t width = 0, height = 0;
    DkImageFormat format = DkImageFormat_None;
    std::uint64_t bytes = 0;
};
}

static int g_failures = 0;
static std::map<const void *, stub::ShaderState> g_shaders;
static std::map<const void *, stub::ImageState> g_images;
static std::uint32_t g_shader_loads = 0;

static void fail(const char *what) {
    std::fprintf(stderr, "[harness] FAILED: %s\n", what);
    ++g_failures;
}

extern "C" {

int harness_failures() { return g_failures; }
std::uint32_t harness_shader_loads() { return g_shader_loads; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The window: the one thing the shim wants from libnx
// ---------------------------------------------------------------------------------------------------------------------------------

struct NWindow {
    int unused = 0;
};

static NWindow g_window;

NWindow *nwindowGetDefault() { return &g_window; }

// ---------------------------------------------------------------------------------------------------------------------------------
// Device, queue, fences
// ---------------------------------------------------------------------------------------------------------------------------------

DkDevice dkDeviceCreate(DkDeviceMaker const *maker) {
    if (!maker) fail("a device maker that is null");
    return reinterpret_cast<DkDevice>(new stub::Device);
}

void dkDeviceDestroy(DkDevice device) { delete reinterpret_cast<stub::Device *>(device); }

DkQueue dkQueueCreate(DkQueueMaker const *maker) {
    if (!maker) fail("a queue maker that is null");
    else if (!maker->perWarpScratchMemorySize) fail("a queue with no per-warp scratch: compute shaders have nowhere to run");
    return reinterpret_cast<DkQueue>(new stub::Queue);
}

void dkQueueDestroy(DkQueue queue) { delete reinterpret_cast<stub::Queue *>(queue); }

bool dkQueueIsInErrorState(DkQueue) { return false; }

void dkQueueWaitFence(DkQueue, DkFence *fence) { if (fence) std::memset(fence, 0, sizeof *fence); }
void dkQueueSignalFence(DkQueue, DkFence *fence, bool) { if (fence) std::memset(fence, 0, sizeof *fence); }
void dkQueueWaitIdle(DkQueue) {}

void dkQueueSubmitCommands(DkQueue, DkCmdList cmds) {
    if (!cmds) fail("submitting an empty command list");
}

void dkQueueFlush(DkQueue) {}

int dkQueueAcquireImage(DkQueue, DkSwapchain) { return 0; }

void dkQueuePresentImage(DkQueue, DkSwapchain, int slot) {
    if (slot < 0 || slot > 2) fail("presenting a slot that is not one of the three back buffers");
}

DkResult dkFenceWait(DkFence *obj, int64_t) {
    if (obj) std::memset(obj, 0, sizeof *obj);
    return DkResult_Success;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------------------------------------------------------

DkMemBlock dkMemBlockCreate(DkMemBlockMaker const *maker) {
    if (!maker || !maker->size) {
        fail("a memory block of zero bytes");
        return nullptr;
    }
    if (maker->size % DK_MEMBLOCK_ALIGNMENT) {
        fail("a memory block that is not a multiple of the page size");
        return nullptr;
    }
    auto *block = new stub::MemBlock;
    block->size = maker->size;
    block->flags = maker->flags;
    // deko3d hands out page-aligned memory, and the shim is checked for the alignments that follow from it
    block->bytes = static_cast<std::uint8_t *>(std::aligned_alloc(DK_MEMBLOCK_ALIGNMENT, (maker->size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1)));
    if (block->bytes) std::memset(block->bytes, 0, maker->size);
    return reinterpret_cast<DkMemBlock>(block);
}

void dkMemBlockDestroy(DkMemBlock mem) {
    auto *block = reinterpret_cast<stub::MemBlock *>(mem);
    if (!block) return;
    std::free(block->bytes);
    delete block;
}

void *dkMemBlockGetCpuAddr(DkMemBlock mem) { return mem ? reinterpret_cast<stub::MemBlock *>(mem)->bytes : nullptr; }
DkGpuAddr dkMemBlockGetGpuAddr(DkMemBlock mem) {
    return mem ? reinterpret_cast<DkGpuAddr>(reinterpret_cast<stub::MemBlock *>(mem)->bytes) : 0;
}
std::uint32_t dkMemBlockGetSize(DkMemBlock mem) { return mem ? reinterpret_cast<stub::MemBlock *>(mem)->size : 0; }
DkResult dkMemBlockFlushCpuCache(DkMemBlock, std::uint32_t, std::uint32_t) { return DkResult_Success; }

// ---------------------------------------------------------------------------------------------------------------------------------
// Command buffers
// ---------------------------------------------------------------------------------------------------------------------------------

DkCmdBuf dkCmdBufCreate(DkCmdBufMaker const *maker) {
    if (!maker) fail("a command buffer maker that is null");
    return reinterpret_cast<DkCmdBuf>(new stub::CmdBuf);
}

void dkCmdBufDestroy(DkCmdBuf buf) { delete reinterpret_cast<stub::CmdBuf *>(buf); }

void dkCmdBufAddMemory(DkCmdBuf buf, DkMemBlock, std::uint32_t, std::uint32_t size) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (!b) return;
    b->memory_added = true;
    if (!size) fail("command memory of zero bytes");
}

DkCmdList dkCmdBufFinishList(DkCmdBuf buf) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b && !b->memory_added) fail("finishing a command list that has no memory");
    return reinterpret_cast<DkCmdList>(buf);
}

void dkCmdBufClear(DkCmdBuf buf) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (!b) return;
    b->binds = b->draws = b->dispatches = b->barriers = b->copies = b->calls = 0;
}

void dkCmdBufCallList(DkCmdBuf buf, DkCmdList list) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (!b || !list) fail("calling an empty command list");
    if (b) ++b->calls;
}

void dkCmdBufBarrier(DkCmdBuf buf, DkBarrier, uint32_t) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->barriers;
}

void dkCmdBufSignalFence(DkCmdBuf buf, DkFence *fence, bool) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b && !fence) fail("a fence signal with no fence");
}

void dkCmdBufBindShaders(DkCmdBuf buf, std::uint32_t stageMask, DkShader const *const shaders[], std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (!b) return;
    ++b->binds;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!shaders[i]) {
            fail("binding a null shader");
            continue;
        }
        auto it = g_shaders.find(shaders[i]);
        if (it == g_shaders.end() || !it->second.valid) {
            fail("binding a shader deko3d would not have accepted");
            continue;
        }
        if (!(stageMask & (1u << it->second.stage))) fail("binding a shader for a stage the mask does not name");
    }
}

void dkCmdBufBindUniformBuffers(DkCmdBuf buf, DkStage, std::uint32_t, DkBufExtents const buffers[], std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (!b) return;
    ++b->binds;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (buffers[i].size > DK_UNIFORM_BUF_MAX_SIZE) fail("a uniform buffer larger than 64 KiB");
        if (buffers[i].addr % DK_UNIFORM_BUF_ALIGNMENT) fail("a uniform buffer address that is not 256-byte aligned");
        if (!buffers[i].addr) fail("a uniform buffer at address zero");
    }
}

void dkCmdBufBindStorageBuffers(DkCmdBuf buf, DkStage, std::uint32_t, DkBufExtents const buffers[], std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (!b) return;
    ++b->binds;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!buffers[i].addr) fail("a storage buffer at address zero");
        if (!buffers[i].size) fail("a storage buffer of zero bytes");
    }
}

void dkCmdBufBindImages(DkCmdBuf buf, DkStage, std::uint32_t, DkResHandle const handles[], std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    // handle 0 is a real handle: it is the descriptor table's first entry (dkMakeImageHandle(0))
    (void)handles;
}

void dkCmdBufBindTextures(DkCmdBuf buf, DkStage, std::uint32_t, DkResHandle const handles[], std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    (void)handles;
}

void dkCmdBufBindImageDescriptorSet(DkCmdBuf buf, DkGpuAddr addr, std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    if (!addr || !count) fail("an image descriptor set with no address or no descriptors");
}

void dkCmdBufBindSamplerDescriptorSet(DkCmdBuf buf, DkGpuAddr addr, std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    if (!addr || !count) fail("a sampler descriptor set with no address or no descriptors");
}

void dkCmdBufBindRenderTargets(DkCmdBuf buf, DkImageView const *const colors[], std::uint32_t count, DkImageView const *depth) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    for (std::uint32_t i = 0; i < count; ++i)
        if (!colors[i] || !colors[i]->pImage) fail("a render target that is not an image view");
    if (depth && !depth->pImage) fail("a depth target that is not an image view");
}

void dkCmdBufSetViewports(DkCmdBuf buf, std::uint32_t, DkViewport const *ports, std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    for (std::uint32_t i = 0; i < count; ++i)
        if (ports[i].width <= 0.0f || ports[i].height <= 0.0f) fail("a viewport with no area");
}

void dkCmdBufSetScissors(DkCmdBuf buf, std::uint32_t, DkScissor const *scissors, std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    for (std::uint32_t i = 0; i < count; ++i)
        if (!scissors[i].width || !scissors[i].height) fail("a scissor with no area");
}

void dkCmdBufDraw(DkCmdBuf buf, DkPrimitive, std::uint32_t vertices, std::uint32_t, std::uint32_t, std::uint32_t) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->draws;
    if (!vertices) fail("a draw with no vertices");
}

void dkCmdBufDispatchCompute(DkCmdBuf buf, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->dispatches;
    if (!x || !y || !z) fail("a dispatch of zero workgroups");
}

void dkCmdBufCopyBuffer(DkCmdBuf buf, DkGpuAddr src, DkGpuAddr dst, std::uint32_t size) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->copies;
    if (!src || !dst || !size) fail("a buffer copy with no address or no size");
}

void dkCmdBufCopyImage(DkCmdBuf, DkImageView const *, DkImageRect const *, DkImageView const *, DkImageRect const *, std::uint32_t) {}
void dkCmdBufBlitImage(DkCmdBuf, DkImageView const *, DkImageRect const *, DkImageView const *, DkImageRect const *, std::uint32_t, std::uint32_t) {}
void dkCmdBufCopyBufferToImage(DkCmdBuf, DkCopyBuf const *, DkImageView const *, DkImageRect const *, std::uint32_t) {}
void dkCmdBufCopyImageToBuffer(DkCmdBuf, DkImageView const *, DkImageRect const *, DkCopyBuf const *, std::uint32_t) {}

void dkCmdBufBindRasterizerState(DkCmdBuf buf, DkRasterizerState const *state) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    if (state && !state->rasterizerEnable) fail("rasterization switched off: nothing would be drawn");
}
void dkCmdBufBindMultisampleState(DkCmdBuf buf, DkMultisampleState const *) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufBindColorState(DkCmdBuf buf, DkColorState const *) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufBindColorWriteState(DkCmdBuf buf, DkColorWriteState const *state) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    if (state && !state->masks) fail("every colour channel masked off: nothing would be drawn");
}
void dkCmdBufBindBlendStates(DkCmdBuf buf, std::uint32_t, DkBlendState const *states, std::uint32_t count) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    for (std::uint32_t i = 0; i < count; ++i)
        if (!states[i].colorBlendOp) fail("a blend state with no operation");
}
void dkCmdBufBindDepthStencilState(DkCmdBuf buf, DkDepthStencilState const *) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufBindVtxAttribState(DkCmdBuf buf, DkVtxAttribState const *, std::uint32_t) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufBindVtxBufferState(DkCmdBuf buf, DkVtxBufferState const *, std::uint32_t) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufSetSampleMask(DkCmdBuf buf, std::uint32_t mask) {
    auto *b = reinterpret_cast<stub::CmdBuf *>(buf);
    if (b) ++b->binds;
    if (!mask) fail("a sample mask of zero: nothing would be drawn");
}
void dkCmdBufSetBlendConst(DkCmdBuf buf, float, float, float, float) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufClearColor(DkCmdBuf buf, std::uint32_t, std::uint32_t, const void *) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufClearDepthStencil(DkCmdBuf buf, bool, float, std::uint8_t, std::uint8_t) { if (auto *b = reinterpret_cast<stub::CmdBuf *>(buf)) ++b->binds; }
void dkCmdBufSetPointSize(DkCmdBuf, float) {}
void dkCmdBufSetLineWidth(DkCmdBuf, float) {}

// ---------------------------------------------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------------------------------------------

void dkShaderInitialize(DkShader *shader, DkShaderMaker const *maker) {
    if (!shader || !maker || !maker->control || !maker->codeMem) {
        fail("a shader with no control block or no code memory");
        return;
    }
    const auto *words = static_cast<const std::uint32_t *>(maker->control);
    if (words[0] != 0x48534B44u) {
        fail("a shader whose control block is not a deko3d module");
        return;
    }
    stub::ShaderState state;
    const std::uint32_t type = words[6];  // the program header follows the 24-byte file header; its first word is the stage
    state.stage = type == 5 ? DkStage_Compute : type == 1 ? DkStage_Fragment : DkStage_Vertex;
    state.valid = true;
    g_shaders[shader] = state;
    ++g_shader_loads;
    if (maker->codeOffset % DK_SHADER_CODE_ALIGNMENT) fail("a shader code offset that is not 256-byte aligned");
    if (std::uint64_t(maker->codeOffset) + words[3] > dkMemBlockGetSize(maker->codeMem))
        fail("a shader whose code does not fit in the block it was given");
}

bool dkShaderIsValid(DkShader const *shader) {
    auto it = g_shaders.find(shader);
    return it != g_shaders.end() && it->second.valid;
}

DkStage dkShaderGetStage(DkShader const *shader) {
    auto it = g_shaders.find(shader);
    return it == g_shaders.end() ? DkStage_Vertex : it->second.stage;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Images, descriptors, swap chains
// ---------------------------------------------------------------------------------------------------------------------------------

void dkImageLayoutInitialize(DkImageLayout *layout, DkImageLayoutMaker const *maker) {
    if (!layout || !maker) return;
    if (!maker->dimensions[0] || !maker->dimensions[1]) fail("an image with no width or height");
    stub::ImageState state;
    state.width = maker->dimensions[0];
    state.height = maker->dimensions[1];
    state.format = maker->format;
    state.bytes = std::uint64_t(state.width) * state.height * 4;  // every format the shim asks for is 4 bytes per texel
    g_images[layout] = state;
}

void dkImageInitialize(DkImage *image, DkImageLayout const *layout, DkMemBlock memBlock, std::uint32_t offset) {
    if (!image || !layout || !memBlock) {
        fail("an image with no layout or no memory");
        return;
    }
    auto it = g_images.find(layout);
    if (it == g_images.end()) {
        fail("an image from a layout that was never made");
        return;
    }
    if (std::uint64_t(offset) + it->second.bytes > dkMemBlockGetSize(memBlock)) fail("an image whose memory block is too small for it");
    g_images[image] = it->second;
}

void dkImageDescriptorInitialize(DkImageDescriptor *descriptor, DkImageView const *view, bool, bool) {
    if (!descriptor || !view || !view->pImage) {
        fail("an image descriptor for no image");
        return;
    }
    std::memset(descriptor, 0, sizeof *descriptor);
}

std::uint64_t dkImageLayoutGetSize(DkImageLayout const *layout) {
    auto it = g_images.find(layout);
    return it == g_images.end() ? 0 : it->second.bytes;
}

std::uint32_t dkImageLayoutGetAlignment(DkImageLayout const *) { return DK_MEMBLOCK_ALIGNMENT; }

DkGpuAddr dkImageGetGpuAddr(DkImage const *image) {
    (void)image;
    return 1;  // the harness only needs it to be non-zero
}

void dkSamplerDescriptorInitialize(DkSamplerDescriptor *descriptor, DkSampler const *) {
    if (descriptor) std::memset(descriptor, 0, sizeof *descriptor);
}

DkSwapchain dkSwapchainCreate(DkSwapchainMaker const *maker) {
    if (!maker || !maker->device || !maker->nativeWindow || !maker->pImages) {
        fail("a swap chain that is not images on a window");
        return nullptr;
    }
    if (maker->numImages != 3) fail("a swap chain that is not three images");
    for (std::uint32_t i = 0; i < maker->numImages; ++i)
        if (!maker->pImages[i]) fail("a swap chain image that is null");
    auto *chain = new stub::Swapchain;
    chain->images = int(maker->numImages);
    return reinterpret_cast<DkSwapchain>(chain);
}

void dkSwapchainDestroy(DkSwapchain chain) { delete reinterpret_cast<stub::Swapchain *>(chain); }

void dkSwapchainAcquireImage(DkSwapchain, int *slot, DkFence *fence) {
    if (slot) *slot = 0;
    if (fence) std::memset(fence, 0, sizeof *fence);
}

void dkSwapchainSetSwapInterval(DkSwapchain, std::uint32_t) {}
void dkSwapchainSetCrop(DkSwapchain, int32_t, int32_t, int32_t, int32_t) {}

}
