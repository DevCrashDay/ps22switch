// The part of the WebGPU C API (webgpu.h, wgpu.h) that runtime/src/gs/gpu/gpu.cpp uses, written on deko3d, so the graphics chip's
// drawing program runs without Vulkan, NVK, EGL or SDL's window in the way: deko3d is the console's own GPU driver API.
//
// gpu.cpp does not change; only the API under it. This is the deko3d counterpart of gpu/vk/wgpu_vk.cpp and stands on deko_gpu.cpp
// (device, queue, memory, images, the swap chain on the console's window) and dksh_table.cpp (the programs uam compiled).
//
// Where it differs from the Vulkan version, and why:
//  - A buffer is a deko3d memory block the CPU can see directly, so a queue write is a memcpy (followed by the L2 invalidate that makes
//    it visible to the GPU) and a readback is a barrier and a wait, not a staging copy.
//  - deko3d binds buffers by GPU address, so a bind group is a list of (slot, address, size) that a pass binds when it is used, dynamic
//    offset included: no descriptor sets for buffers at all. Textures and images need handles, which come from a descriptor pool.
//  - Shaders are deko3d shader modules from scripts/wgsl_dksh.py, found by entry point name. Nothing is compiled at run time.
//
// The slice this file is at: objects, device and queue, buffers, textures, surface and present, the command encoder and submission, and
// the render pass's clear, viewport and scissor. Pipelines, bind group bindings, dispatch and draw are declared and log that they are not
// done yet — the next slice puts them on deko3d commands.

#include <deko3d.hpp>
#include <switch.h>

#include "deko_gpu.hpp"
#include "dksh.hpp"

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

WGPUStringView text(const char *s) { return WGPUStringView{s, std::strlen(s)}; }

// A hard fault prints nothing, and on the console the only evidence is how far the log got: this writes one line before each object-making
// call, for the first hundred or so, so the last line of ps2run.log names the call that killed it. PS2_DEKO3D_TRACE=0 turns it off.
std::uint32_t g_traced = 0;

bool trace_on() {
    const char *off = std::getenv("PS2_DEKO3D_TRACE");
    return !off || std::strcmp(off, "0") != 0;
}

template <typename... Args>
void trace(const char *what, Args... args) {
    if (!trace_on() || g_traced >= 120) return;
    ++g_traced;
    std::fprintf(stderr, "[deko] #%u ", g_traced);
    std::fprintf(stderr, what, args...);
    std::fputc('\n', stderr);
}

// One line the first time each distinct message is seen, so a missing call says so once rather than thousands of times a second.
void log_once(const char *what) {
    static std::vector<std::string> said;
    for (const std::string &s : said)
        if (s == what) return;
    said.emplace_back(what);
    std::fprintf(stderr, "[deko] %s\n", what);
}

}

// ---------------------------------------------------------------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------------------------------------------------------------

struct WGPUInstanceImpl {
    int unused = 0;
};

struct WGPUAdapterImpl {
    int unused = 0;
};

struct WGPUQueueImpl;

struct WGPUDeviceImpl {
    ps2::gs::gpu::deko::Gpu gpu;
    std::unique_ptr<WGPUQueueImpl> queue;
    // The code of every shader module made on this device, one module after the other in one Code memory block, and what has been
    // loaded into it already: gpu.cpp makes a render pipeline for every combination of blend, depth and write mask, and they all share
    // the same few programs, so a module must be loaded once and not once per pipeline.
    std::shared_ptr<ps2::gs::gpu::deko::Memory> code_memory;
    std::uint32_t code_used = 0;
    std::vector<std::pair<std::string, DkShader>> loaded;
    // Command memory the GPU may still be reading: freed when it has finished with it.
    std::vector<std::shared_ptr<ps2::gs::gpu::deko::Memory>> retired;
    // Command memory, reused: one slot per command buffer that is being recorded, handed out again once the GPU has finished the
    // frame they were submitted in (the queue waits for the previous frame before recording another).
    struct CommandSlot {
        std::shared_ptr<ps2::gs::gpu::deko::Memory> memory;
        dk::CmdBuf commands;
        bool busy = false;
    };
    static constexpr std::uint32_t kCommandSlots = 32;
    CommandSlot slots[kCommandSlots];
    std::uint32_t slots_used = 0;
    CommandSlot *take_slot();
    void free_slots();
    // Image and sampler descriptors: deko3d binds images by handle, and a handle is an index into these two tables.
    std::shared_ptr<ps2::gs::gpu::deko::Memory> descriptors;
    std::uint32_t image_count = 0, sampler_count = 0;
    bool default_sampler = false;  // the sampler slot 0: what a binding that samples a texture uses
    // What the shim has actually done, for the log: a frame rate and a count of the work is the first thing a run should say.
    std::uint32_t submits = 0, passes = 0, draws = 0, dispatches = 0, skipped = 0;
    void error(std::string message);
};

struct WGPUQueueImpl {
    WGPUDeviceImpl *device = nullptr;
};

struct WGPUBufferImpl {
    WGPUDeviceImpl *device = nullptr;
    std::shared_ptr<ps2::gs::gpu::deko::Memory> memory;
    std::uint64_t size = 0;
    bool map_read = false;
    bool mapped = false;
};

struct WGPUTextureImpl {
    WGPUDeviceImpl *device = nullptr;
    std::shared_ptr<ps2::gs::gpu::deko::Image> image;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    std::uint32_t width = 0, height = 0;
    // Its descriptors in the device's table, made when it is first bound: an image used as a storage image needs a different
    // descriptor from the same image sampled as a texture, and the colour target is both.
    std::uint32_t image_slot = 0xFFFFFFFFu, storage_slot = 0xFFFFFFFFu;
    bool borrowed = false;                   // a swap chain back buffer: the swap chain owns it
};

struct WGPUTextureViewImpl {
    WGPUDeviceImpl *device = nullptr;
    WGPUTextureImpl *texture = nullptr;  // for its size and format; the image below is what keeps it valid
    std::shared_ptr<ps2::gs::gpu::deko::Image> image;
};

struct WGPUBindGroupLayoutImpl {
    struct Entry {
        std::uint32_t binding = 0;
        bool buffer = false;
        bool uniform = false;
        bool dynamic = false;  // has a dynamic offset: the pass's offset list supplies it
        bool storage_texture = false;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    };
    std::vector<Entry> entries;
};

struct WGPUPipelineLayoutImpl {
    std::vector<WGPUBindGroupLayoutImpl *> groups;
};

struct WGPUBindGroupImpl {
    WGPUBindGroupLayoutImpl *layout = nullptr;
    struct Entry {
        std::uint32_t binding = 0;
        WGPUBuffer buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
        WGPUTextureView texture = nullptr;
        bool is_buffer = false;
    };
    std::vector<Entry> entries;
};

struct WGPUShaderModuleImpl {
    std::string entry_point;  // the WGSL entry point this module is for
};

struct WGPUComputePipelineImpl {
    std::string entry_point;
    DkShader shader{};
    bool ready = false;  // false: no deko3d module for this entry point (it logs once)
};

struct WGPURenderPipelineImpl {
    std::string vertex_entry, fragment_entry;
    DkShader vertex{}, fragment{};
    DkPrimitive primitive = DkPrimitive_Triangles;
    dk::RasterizerState rasterizer;
    dk::MultisampleState multisample;
    dk::ColorState color;
    dk::ColorWriteState write;
    std::vector<dk::BlendState> blends;
    dk::DepthStencilState depth;
    bool ready = false;
};

struct WGPUCommandBufferImpl {
    std::shared_ptr<ps2::gs::gpu::deko::Memory> memory;  // the commands themselves
    DkCmdList list = 0;
};

struct WGPUCommandEncoderImpl {
    WGPUDeviceImpl *device = nullptr;
    std::shared_ptr<ps2::gs::gpu::deko::Memory> memory;
    dk::CmdBuf commands;
};

struct WGPUComputePassEncoderImpl {
    WGPUCommandEncoderImpl *encoder = nullptr;
    WGPUComputePipelineImpl *pipeline = nullptr;
};

struct WGPURenderPassEncoderImpl {
    WGPUCommandEncoderImpl *encoder = nullptr;
    WGPURenderPipelineImpl *pipeline = nullptr;
    std::uint32_t width = 0, height = 0;
};

struct WGPUSurfaceImpl {
    WGPUDeviceImpl *device = nullptr;
    void *window = nullptr;
    ps2::gs::gpu::deko::Swapchain swapchain;
    std::uint32_t width = 0, height = 0;
    bool acquired = false;
};

namespace {

void WGPUDeviceImpl_error(WGPUDeviceImpl *d, const std::string &message) {
    std::fprintf(stderr, "[deko] %s\n", message.c_str());
    (void)d;
}

// The deko3d layer's format for one of WebGPU's, or Format::Rgba8Unorm when this file does not know it.
bool to_deko_format(WGPUTextureFormat format, ps2::gs::gpu::deko::Format &out) {
    using ps2::gs::gpu::deko::Format;
    switch (format) {
        case WGPUTextureFormat_RGBA8Unorm: out = Format::Rgba8Unorm; return true;
        case WGPUTextureFormat_RGBA8Uint: out = Format::Rgba8Uint; return true;
        case WGPUTextureFormat_R32Float: out = Format::R32Float; return true;
        case WGPUTextureFormat_Depth32Float: out = Format::Depth32Float; return true;
        default: return false;
    }
}

// The image flags one of WebGPU's usage bits asks for.
std::uint32_t to_deko_flags(WGPUTextureUsage usage) {
    std::uint32_t flags = 0;
    if (usage & WGPUTextureUsage_RenderAttachment) flags |= DkImageFlags_UsageRender;
    if (usage & WGPUTextureUsage_StorageBinding) flags |= DkImageFlags_UsageLoadStore;
    if (usage & (WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst)) flags |= DkImageFlags_Usage2DEngine;
    return flags;
}

}

void WGPUDeviceImpl::error(std::string message) { WGPUDeviceImpl_error(this, message); }

WGPUDeviceImpl::CommandSlot *WGPUDeviceImpl::take_slot() {
    for (CommandSlot &slot : slots)
        if (!slot.busy) {
            slot.busy = true;
            ++slots_used;
            return &slot;
        }
    return nullptr;  // every slot is still recording or in flight: the caller cannot record another list
}

void WGPUDeviceImpl::free_slots() {
    for (CommandSlot &slot : slots) slot.busy = false;
    slots_used = 0;
}

extern "C" {

// ---------------------------------------------------------------------------------------------------------------------------------
// Instance, adapter, device
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUInstance wgpuCreateInstance(WGPUInstanceDescriptor const *) { return new WGPUInstanceImpl; }

void wgpuInstanceRelease(WGPUInstance instance) { delete instance; }

void wgpuInstanceProcessEvents(WGPUInstance) {}  // deko3d has no event loop: submissions either wait or they do not

WGPUFuture wgpuInstanceRequestAdapter(WGPUInstance, WGPURequestAdapterOptions const *, WGPURequestAdapterCallbackInfo info) {
    auto *adapter = new WGPUAdapterImpl;
    if (info.callback) info.callback(WGPURequestAdapterStatus_Success, adapter, text(""), info.userdata1, info.userdata2);
    return WGPUFuture{1};
}

void wgpuAdapterRelease(WGPUAdapter adapter) { delete adapter; }

WGPUStatus wgpuAdapterGetInfo(WGPUAdapter, WGPUAdapterInfo *info) {
    const auto copy = [](const char *s) {
        const std::size_t n = std::strlen(s);
        char *p = static_cast<char *>(std::malloc(n + 1));
        std::memcpy(p, s, n + 1);
        return WGPUStringView{p, n};
    };
    info->vendor = copy("NVIDIA");
    info->architecture = copy("Tegra X1");
    info->device = copy("deko3d");
    info->description = copy("the console's own GPU driver API (Nintendo Switch)");
    info->backendType = WGPUBackendType_Undefined;  // deko3d is none of WebGPU's backends; gpu.cpp prints "other"
    info->adapterType = WGPUAdapterType_IntegratedGPU;
    info->vendorID = 0x10de;  // NVIDIA
    info->deviceID = 0;
    return WGPUStatus_Success;
}

void wgpuAdapterInfoFreeMembers(WGPUAdapterInfo info) {
    for (WGPUStringView v : {info.vendor, info.architecture, info.device, info.description}) std::free(const_cast<char *>(v.data));
}

WGPUStatus wgpuAdapterGetLimits(WGPUAdapter, WGPULimits *limits) {
    // The console has 3 GiB of memory the program may use in title takeover; a single buffer is never anywhere near this.
    limits->maxStorageBufferBindingSize = 512ull << 20;
    limits->maxBufferSize = 512ull << 20;
    limits->maxUniformBufferBindingSize = 64ull << 10;
    limits->maxTextureDimension2D = 2048;
    limits->maxBindGroups = 4;
    limits->maxComputeWorkgroupStorageSize = 48u << 10;
    limits->maxComputeInvocationsPerWorkgroup = 1024;
    limits->maxComputeWorkgroupSizeX = 1024;
    limits->maxComputeWorkgroupSizeY = 1024;
    limits->maxComputeWorkgroupSizeZ = 64;
    return WGPUStatus_Success;
}

WGPUBool wgpuAdapterHasFeature(WGPUAdapter, WGPUFeatureName feature) {
    // Dual-source blending is on the hardware and in hw.wgsl. Timestamp queries: deko3d has no query sets, so gpu.cpp's own timing is off.
    return feature == WGPUFeatureName_DualSourceBlending;
}

WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter, WGPUDeviceDescriptor const *descriptor, WGPURequestDeviceCallbackInfo info) {
    auto *d = new WGPUDeviceImpl;
    if (!d->gpu.open()) {
        delete d;
        if (info.callback) info.callback(WGPURequestDeviceStatus_Error, nullptr, text("the deko3d device did not open"), info.userdata1, info.userdata2);
        return WGPUFuture{1};
    }
    d->queue = std::make_unique<WGPUQueueImpl>();
    d->queue->device = d;
    // Every shader module made on this device goes into this one block: 256 KiB of code is far more than the renderer's programs.
    d->code_memory = d->gpu.allocate(256 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    if (!d->code_memory) {
        delete d;
        if (info.callback) info.callback(WGPURequestDeviceStatus_Error, nullptr, text("no memory for shader code"), info.userdata1, info.userdata2);
        return WGPUFuture{1};
    }
    // The image and sampler descriptors a shader's bindings index into: deko3d binds images by handle, and a handle is an index here.
    constexpr std::uint32_t kImages = 1024, kSamplers = 256;
    d->descriptors = d->gpu.allocate(kImages * sizeof(DkImageDescriptor) + kSamplers * sizeof(DkSamplerDescriptor),
                                     DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!d->descriptors) {
        delete d;
        if (info.callback) info.callback(WGPURequestDeviceStatus_Error, nullptr, text("no memory for the image descriptors"), info.userdata1, info.userdata2);
        return WGPUFuture{1};
    }
    (void)descriptor;
    std::fprintf(stderr, "[deko] device open (deko3d)\n");
    if (info.callback) info.callback(WGPURequestDeviceStatus_Success, d, text(""), info.userdata1, info.userdata2);
    return WGPUFuture{1};
}

void wgpuDeviceRelease(WGPUDevice d) {
    if (!d) return;
    d->gpu.wait();
    delete d;
}

WGPUQueue wgpuDeviceGetQueue(WGPUDevice d) {
    trace("get queue");
    return d ? d->queue.get() : nullptr;
}

WGPUBool wgpuDevicePoll(WGPUDevice d, WGPUBool wait, WGPUSubmissionIndex const *) {
    if (d && wait) d->gpu.wait();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice d, WGPUBufferDescriptor const *descriptor) {
    trace("create buffer, %llu bytes", static_cast<unsigned long long>(descriptor->size));
    auto *b = new WGPUBufferImpl;
    b->device = d;
    b->size = descriptor->size;
    b->map_read = (descriptor->usage & WGPUBufferUsage_MapRead) != 0;
    b->memory = d->gpu.allocate(std::max<std::uint64_t>(descriptor->size, 4), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!b->memory) {
        delete b;
        d->error("cannot create a buffer of " + std::to_string(descriptor->size) + " bytes");
        return nullptr;
    }
    // WebGPU buffers start as zeros
    std::memset(b->memory->cpu, 0, b->memory->size);
    d->gpu.note_cpu_write();
    return b;
}

void wgpuBufferRelease(WGPUBuffer b) { delete b; }

void wgpuQueueWriteBuffer(WGPUQueue queue, WGPUBuffer buffer, std::uint64_t offset, void const *data, std::size_t size) {
    if (!queue || !buffer || !data || !size) return;
    if (offset + size > buffer->size) {
        queue->device->error("queue write past the end of a buffer");
        return;
    }
    // The CPU sees the buffer's bytes directly, so this is the write itself; the device's next command list starts with the barrier
    // that makes it visible to the GPU (Gpu::note_cpu_write).
    std::memcpy(buffer->memory->cpu + offset, data, size);
    queue->device->gpu.note_cpu_write();
}

WGPUFuture wgpuBufferMapAsync(WGPUBuffer b, WGPUMapMode, std::size_t, std::size_t, WGPUBufferMapCallbackInfo info) {
    if (!b || !b->map_read) {
        if (info.callback) info.callback(WGPUMapAsyncStatus_Error, text("the buffer is not MapRead"), info.userdata1, info.userdata2);
        return WGPUFuture{1};
    }
    // What the GPU wrote has to be out of its L2 before the CPU reads the same bytes.
    b->device->gpu.sync_for_cpu_read();
    b->mapped = true;
    if (info.callback) info.callback(WGPUMapAsyncStatus_Success, text(""), info.userdata1, info.userdata2);
    return WGPUFuture{1};
}

void const *wgpuBufferGetConstMappedRange(WGPUBuffer b, std::size_t offset, std::size_t) {
    if (!b || !b->mapped || !b->memory) return nullptr;
    return b->memory->cpu + offset;
}

void wgpuBufferUnmap(WGPUBuffer b) {
    if (b) b->mapped = false;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Textures and views
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUTexture wgpuDeviceCreateTexture(WGPUDevice d, WGPUTextureDescriptor const *descriptor) {
    trace("create texture %ux%u, format %d", unsigned(descriptor->size.width), unsigned(descriptor->size.height), int(descriptor->format));
    ps2::gs::gpu::deko::Format format{};
    if (!to_deko_format(descriptor->format, format) || descriptor->dimension != WGPUTextureDimension_2D) {
        d->error("a texture format or dimension this renderer does not use: " + std::to_string(int(descriptor->format)));
        return nullptr;
    }
    auto *t = new WGPUTextureImpl;
    t->device = d;
    t->format = descriptor->format;
    t->width = descriptor->size.width;
    t->height = descriptor->size.height;
    t->image = d->gpu.image(t->width, t->height, format, to_deko_flags(descriptor->usage));
    if (!t->image) {
        delete t;
        d->error("no memory for a texture of " + std::to_string(t->width) + "x" + std::to_string(t->height));
        return nullptr;
    }
    return t;
}

void wgpuTextureRelease(WGPUTexture t) { delete t; }  // the images are shared pointers: the last owner frees the memory

WGPUTextureView wgpuTextureCreateView(WGPUTexture t, WGPUTextureViewDescriptor const *) {
    trace("create view");
    if (!t) return nullptr;
    auto *v = new WGPUTextureViewImpl;
    v->device = t->device;
    v->texture = t;
    v->image = t->image;  // the view outlives the texture object in gpu.cpp, so it holds the image itself
    return v;
}

void wgpuTextureViewRelease(WGPUTextureView v) { delete v; }

// ---------------------------------------------------------------------------------------------------------------------------------
// Bind groups, layouts and pipelines
// ---------------------------------------------------------------------------------------------------------------------------------

namespace {

DkCompareOp to_compare(WGPUCompareFunction f) {
    switch (f) {
        case WGPUCompareFunction_Never: return DkCompareOp_Never;
        case WGPUCompareFunction_Less: return DkCompareOp_Less;
        case WGPUCompareFunction_Equal: return DkCompareOp_Equal;
        case WGPUCompareFunction_LessEqual: return DkCompareOp_Lequal;
        case WGPUCompareFunction_Greater: return DkCompareOp_Greater;
        case WGPUCompareFunction_NotEqual: return DkCompareOp_NotEqual;
        case WGPUCompareFunction_GreaterEqual: return DkCompareOp_Gequal;
        default: return DkCompareOp_Always;
    }
}

DkBlendFactor to_blend_factor(WGPUBlendFactor f) {
    switch (f) {
        case WGPUBlendFactor_One: return DkBlendFactor_One;
        case WGPUBlendFactor_Src: return DkBlendFactor_SrcColor;
        case WGPUBlendFactor_OneMinusSrc: return DkBlendFactor_InvSrcColor;
        case WGPUBlendFactor_SrcAlpha: return DkBlendFactor_SrcAlpha;
        case WGPUBlendFactor_OneMinusSrcAlpha: return DkBlendFactor_InvSrcAlpha;
        case WGPUBlendFactor_Dst: return DkBlendFactor_DstColor;
        case WGPUBlendFactor_OneMinusDst: return DkBlendFactor_InvDstColor;
        case WGPUBlendFactor_DstAlpha: return DkBlendFactor_DstAlpha;
        case WGPUBlendFactor_OneMinusDstAlpha: return DkBlendFactor_InvDstAlpha;
        case WGPUBlendFactor_SrcAlphaSaturated: return DkBlendFactor_SrcAlphaSaturate;
        case WGPUBlendFactor_Constant: return DkBlendFactor_ConstColor;
        case WGPUBlendFactor_OneMinusConstant: return DkBlendFactor_InvConstColor;
        // the second colour a dual-source fragment shader writes: what the PS2's alpha blending needs (hw_fs writes it)
        case WGPUBlendFactor_Src1: return DkBlendFactor_Src1Color;
        case WGPUBlendFactor_OneMinusSrc1: return DkBlendFactor_InvSrc1Color;
        case WGPUBlendFactor_Src1Alpha: return DkBlendFactor_Src1Alpha;
        case WGPUBlendFactor_OneMinusSrc1Alpha: return DkBlendFactor_InvSrc1Alpha;
        default: return DkBlendFactor_Zero;
    }
}

DkBlendOp to_blend_op(WGPUBlendOperation op) {
    switch (op) {
        case WGPUBlendOperation_Subtract: return DkBlendOp_Sub;
        case WGPUBlendOperation_ReverseSubtract: return DkBlendOp_RevSub;
        case WGPUBlendOperation_Min: return DkBlendOp_Min;
        case WGPUBlendOperation_Max: return DkBlendOp_Max;
        default: return DkBlendOp_Add;
    }
}

DkFace to_face(WGPUCullMode mode) {
    switch (mode) {
        case WGPUCullMode_Front: return DkFace_Front;
        case WGPUCullMode_Back: return DkFace_Back;
        default: return DkFace_None;
    }
}

DkFrontFace to_front_face(WGPUFrontFace face) { return face == WGPUFrontFace_CW ? DkFrontFace_CW : DkFrontFace_CCW; }

DkPrimitive to_primitive(WGPUPrimitiveTopology topology) {
    switch (topology) {
        case WGPUPrimitiveTopology_PointList: return DkPrimitive_Points;
        case WGPUPrimitiveTopology_LineList: return DkPrimitive_Lines;
        case WGPUPrimitiveTopology_LineStrip: return DkPrimitive_LineStrip;
        case WGPUPrimitiveTopology_TriangleStrip: return DkPrimitive_TriangleStrip;
        default: return DkPrimitive_Triangles;
    }
}

// One DKSH module into the device's code memory, ready for dkShaderInitialize. False when uam has no module for that entry point, which
// is a program this renderer cannot draw with (hw_writeback reads a depth texture, which naga cannot write as GLSL at all).
bool load_program(WGPUDeviceImpl *d, const std::string &entry, DkShader &shader) {
    for (const auto &pair : d->loaded)
        if (pair.first == entry) {  // already in the code block: a pipeline is a new use of the same module, not a new copy
            shader = pair.second;
            return dkShaderIsValid(&shader);
        }
    const ps2::gs::gpu::deko::DkshProgram *program = ps2::gs::gpu::deko::dksh_program(entry.c_str());
    if (!program || !program->words || program->count < 6) {
        std::fprintf(stderr, "[deko] no compiled module for %s: that program cannot run (scripts/wgsl_dksh.py)\n", entry.c_str());
        return false;
    }
    const std::uint32_t *words = program->words;
    const std::uint32_t control_size = words[2], code_size = words[3];
    if (words[0] != 0x48534B44 || control_size + code_size != program->count * 4) {
        std::fprintf(stderr, "[deko] %s: the module is not a deko3d shader module\n", entry.c_str());
        return false;
    }
    const std::uint32_t slot = (code_size + DK_SHADER_CODE_UNUSABLE_SIZE + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    if (d->code_used + slot > d->code_memory->size) {
        std::fprintf(stderr, "[deko] no room left for the shader %s\n", entry.c_str());
        return false;
    }
    std::memcpy(d->code_memory->cpu + d->code_used, reinterpret_cast<const std::uint8_t *>(words) + control_size, code_size);
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, d->code_memory->block, d->code_used);
    maker.control = words;
    trace("  %s: control %u bytes, code %u bytes into the block at %u", entry.c_str(), control_size, code_size, d->code_used);
    dkShaderInitialize(&shader, &maker);
    trace("  %s: deko3d took it", entry.c_str());
    d->code_used += slot;
    if (!dkShaderIsValid(&shader)) {
        std::fprintf(stderr, "[deko] deko3d rejected the shader %s\n", entry.c_str());
        return false;
    }
    d->loaded.emplace_back(entry, shader);
    trace("  %s: cached, %zu modules", entry.c_str(), d->loaded.size());
    return true;
}

// The handle of a texture's view in the device's descriptor tables: an image descriptor for a storage image (what a compute program
// writes), an image plus the default sampler for one that is sampled. deko3d binds textures and images by handle, and a handle is an
// index into those tables.
bool texture_handle(WGPUDeviceImpl *d, WGPUTextureImpl *texture, bool storage, DkResHandle &handle) {
    if (!texture || !texture->image || !d->descriptors) return false;
    auto *images = reinterpret_cast<DkImageDescriptor *>(d->descriptors->cpu);
    std::uint32_t *slot = storage ? &texture->storage_slot : &texture->image_slot;
    if (*slot == 0xFFFFFFFFu) {
        if (d->image_count >= 1024) {
            log_once("the image descriptor table is full");
            return false;
        }
        *slot = d->image_count++;
        dkImageDescriptorInitialize(&images[*slot], &texture->image->view, storage, false);
    }
    if (storage) {
        handle = dkMakeImageHandle(*slot);
        return true;
    }
    if (!d->default_sampler) {
        // One sampler for every sampling binding: the programs fetch whole texels, so its filtering never matters, only that it exists.
        auto *samplers = reinterpret_cast<DkSamplerDescriptor *>(d->descriptors->cpu + 1024 * sizeof(DkImageDescriptor));
        dk::Sampler sampler;
        dkSamplerDescriptorInitialize(&samplers[0], &sampler);
        d->default_sampler = true;
    }
    handle = dkMakeTextureHandle(*slot, 0);
    return true;
}

// The stage a bind group's buffers go to, as deko3d's per-stage binding.
constexpr DkStage kBufferStages[] = {DkStage_Vertex, DkStage_Fragment, DkStage_Compute};

// Binds a group's buffers (by address, dynamic offset included) and storage images at the binding numbers the shaders use. deko3d has no
// descriptor sets for buffers, so this is where the Vulkan version's per-draw set binding disappears.
void bind_group(DkCmdBuf cmd, const WGPUBindGroupImpl *group, bool compute, std::size_t offset_count, const std::uint32_t *offsets) {
    if (!group) return;
    std::size_t next_offset = 0;
    for (const WGPUBindGroupImpl::Entry &entry : group->entries) {
        // A dynamic offset comes from the pass's list, in the order the layout marks the entries dynamic.
        bool is_dynamic = false;
        if (group->layout)
            for (const WGPUBindGroupLayoutImpl::Entry &l : group->layout->entries)
                if (l.binding == entry.binding && l.dynamic) is_dynamic = true;
        std::uint32_t dynamic = 0;
        if (is_dynamic) {
            if (next_offset < offset_count) dynamic = offsets[next_offset];
            ++next_offset;
        }
        if (entry.is_buffer) {
            if (!entry.buffer || !entry.buffer->memory) continue;
            const std::uint64_t offset = entry.offset + dynamic;
            const std::uint64_t size = entry.size == WGPU_WHOLE_SIZE || entry.size == 0 ? entry.buffer->size - offset : entry.size;
            // deko3d's uniform and storage buffers are separate namespaces, and only the layout knows which one this binding is: a
            // uniform buffer is limited to 64 KiB, so binding video memory at a uniform slot is an error the device would report (or
            // worse, a list it rejects).
            bool uniform = false;
            if (group->layout)
                for (const WGPUBindGroupLayoutImpl::Entry &l : group->layout->entries)
                    if (l.binding == entry.binding) uniform = l.uniform;
            const DkBufExtents extents{entry.buffer->memory->gpu + offset, static_cast<std::uint32_t>(uniform ? std::min<std::uint64_t>(size, DK_UNIFORM_BUF_MAX_SIZE) : size)};
            for (DkStage stage : kBufferStages) {
                if (compute && stage != DkStage_Compute) continue;
                if (!compute && stage == DkStage_Compute) continue;
                if (uniform) dkCmdBufBindUniformBuffers(cmd, stage, entry.binding, &extents, 1);
                else dkCmdBufBindStorageBuffers(cmd, stage, entry.binding, &extents, 1);
            }
        } else if (entry.texture && entry.texture->texture) {
            // The layout says whether this binding is a storage image (a compute program writing it) or a sampled texture.
            bool storage = false;
            if (group->layout)
                for (const WGPUBindGroupLayoutImpl::Entry &l : group->layout->entries)
                    if (l.binding == entry.binding) storage = l.storage_texture;
            DkResHandle handle = 0;
            if (!texture_handle(entry.texture->texture->device, entry.texture->texture, storage, handle)) continue;
            for (DkStage stage : kBufferStages) {
                if (compute && stage != DkStage_Compute) continue;
                if (!compute && stage == DkStage_Compute) continue;
                if (storage) dkCmdBufBindImages(cmd, stage, entry.binding, &handle, 1);
                else dkCmdBufBindTextures(cmd, stage, entry.binding, &handle, 1);
            }
        }
    }
}

}

WGPUBindGroupLayout wgpuDeviceCreateBindGroupLayout(WGPUDevice, WGPUBindGroupLayoutDescriptor const *descriptor) {
    trace("create bind group layout, %zu entries", static_cast<std::size_t>(descriptor->entryCount));
    auto *l = new WGPUBindGroupLayoutImpl;
    for (std::size_t i = 0; i < descriptor->entryCount; ++i) {
        const WGPUBindGroupLayoutEntry &e = descriptor->entries[i];
        WGPUBindGroupLayoutImpl::Entry entry;
        entry.binding = e.binding;
        entry.buffer = e.buffer.type != WGPUBufferBindingType_BindingNotUsed;
        entry.uniform = e.buffer.type == WGPUBufferBindingType_Uniform;
        entry.dynamic = e.buffer.hasDynamicOffset;
        entry.storage_texture = e.storageTexture.access != WGPUStorageTextureAccess_Undefined;
        l->entries.push_back(entry);
    }
    return l;
}

void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout l) { delete l; }

WGPUPipelineLayout wgpuDeviceCreatePipelineLayout(WGPUDevice, WGPUPipelineLayoutDescriptor const *descriptor) {
    trace("create pipeline layout");
    auto *p = new WGPUPipelineLayoutImpl;
    for (std::size_t i = 0; i < descriptor->bindGroupLayoutCount; ++i) p->groups.push_back(descriptor->bindGroupLayouts[i]);
    return p;
}

void wgpuPipelineLayoutRelease(WGPUPipelineLayout p) { delete p; }

WGPUBindGroup wgpuDeviceCreateBindGroup(WGPUDevice, WGPUBindGroupDescriptor const *descriptor) {
    trace("create bind group, %zu entries", static_cast<std::size_t>(descriptor->entryCount));
    auto *g = new WGPUBindGroupImpl;
    g->layout = descriptor->layout;
    for (std::size_t i = 0; i < descriptor->entryCount; ++i) {
        const WGPUBindGroupEntry &e = descriptor->entries[i];
        WGPUBindGroupImpl::Entry entry;
        entry.binding = e.binding;
        if (e.buffer) {
            entry.is_buffer = true;
            entry.buffer = e.buffer;
            entry.offset = e.offset;
            entry.size = e.size;
        } else {
            entry.texture = e.textureView;
        }
        g->entries.push_back(entry);
    }
    return g;
}

void wgpuBindGroupRelease(WGPUBindGroup g) { delete g; }

WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice, WGPUShaderModuleDescriptor const *) {
    trace("create shader module");
    // The module itself is only a handle: a pipeline names the entry point, and that name finds the module uam compiled
    // (dksh_table.cpp). Nothing is compiled or even parsed here.
    return new WGPUShaderModuleImpl;
}

void wgpuShaderModuleRelease(WGPUShaderModule m) { delete m; }

WGPUComputePipeline wgpuDeviceCreateComputePipeline(WGPUDevice d, WGPUComputePipelineDescriptor const *descriptor) {
    trace("create compute pipeline, entry point %.*s", int(descriptor->compute.entryPoint.length),
          descriptor->compute.entryPoint.data ? descriptor->compute.entryPoint.data : "");
    auto *p = new WGPUComputePipelineImpl;
    if (descriptor->compute.entryPoint.data)
        p->entry_point.assign(descriptor->compute.entryPoint.data, descriptor->compute.entryPoint.length);
    p->ready = load_program(d, p->entry_point, p->shader);
    return p;
}

void wgpuComputePipelineRelease(WGPUComputePipeline p) { delete p; }

WGPURenderPipeline wgpuDeviceCreateRenderPipeline(WGPUDevice d, WGPURenderPipelineDescriptor const *descriptor) {
    trace("create render pipeline, vertex %.*s", int(descriptor->vertex.entryPoint.length),
          descriptor->vertex.entryPoint.data ? descriptor->vertex.entryPoint.data : "");
    auto *p = new WGPURenderPipelineImpl;
    if (descriptor->vertex.entryPoint.data)
        p->vertex_entry.assign(descriptor->vertex.entryPoint.data, descriptor->vertex.entryPoint.length);
    if (descriptor->fragment && descriptor->fragment->entryPoint.data)
        p->fragment_entry.assign(descriptor->fragment->entryPoint.data, descriptor->fragment->entryPoint.length);
    p->primitive = to_primitive(descriptor->primitive.topology);
    // The fixed state deko3d has to be told explicitly, from the pipeline's description.
    p->rasterizer.setCullMode(to_face(descriptor->primitive.cullMode)).setFrontFace(to_front_face(descriptor->primitive.frontFace));
    if (descriptor->depthStencil) {
        p->depth.setDepthCompareOp(to_compare(descriptor->depthStencil->depthCompare));
        p->depth.setDepthTestEnable(descriptor->depthStencil->depthCompare != WGPUCompareFunction_Always);
        p->depth.setDepthWriteEnable(descriptor->depthStencil->depthWriteEnabled == WGPUOptionalBool_True);
    }
    if (descriptor->fragment) {
        for (std::uint32_t i = 0; i < descriptor->fragment->targetCount; ++i) {
            const WGPUColorTargetState *target = &descriptor->fragment->targets[i];
            dk::BlendState blend;
            if (target->blend) {
                blend.setOps(to_blend_op(target->blend->color.operation), to_blend_op(target->blend->alpha.operation));
                blend.setFactors(to_blend_factor(target->blend->color.srcFactor), to_blend_factor(target->blend->color.dstFactor),
                                 to_blend_factor(target->blend->alpha.srcFactor), to_blend_factor(target->blend->alpha.dstFactor));
            }
            p->blends.push_back(blend);
            p->color.setBlendEnable(i, target->blend != nullptr);
            p->write.setMask(i, target->writeMask);
        }
    }
    p->ready = load_program(d, p->vertex_entry, p->vertex) && load_program(d, p->fragment_entry, p->fragment);
    return p;
}

void wgpuRenderPipelineRelease(WGPURenderPipeline p) { delete p; }

// ---------------------------------------------------------------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUCommandEncoder wgpuDeviceCreateCommandEncoder(WGPUDevice d, WGPUCommandEncoderDescriptor const *) {
    trace("create command encoder");
    auto *e = new WGPUCommandEncoderImpl;
    e->device = d;
    // Each encoder records into a slot of the device's command memory (made once, reused every frame): gpu.cpp keeps several encoders
    // alive and submits them in one go, so each needs its own memory, but allocating a memory block per encoder would churn hundreds of
    // kilobytes a frame.
    WGPUDeviceImpl::CommandSlot *slot = d->take_slot();
    if (!slot) {
        delete e;
        d->error("no free command memory for an encoder");
        return nullptr;
    }
    if (!slot->memory) {
        slot->memory = d->gpu.allocate(128 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        if (!slot->memory) {
            slot->busy = false;
            delete e;
            d->error("no command memory for an encoder");
            return nullptr;
        }
        slot->commands = dk::CmdBufMaker{d->gpu.device()}.create();
        dkCmdBufAddMemory(slot->commands, slot->memory->block, 0, slot->memory->size);
    }
    e->memory = slot->memory;
    e->commands = slot->commands;
    e->commands.clear();
    // The image and sampler tables, and the invalidate that makes newly written descriptors visible to the shaders.
    constexpr std::uint32_t kImages = 1024, kSamplers = 256;
    dkCmdBufBindImageDescriptorSet(e->commands, d->descriptors->gpu, kImages);
    dkCmdBufBindSamplerDescriptorSet(e->commands, d->descriptors->gpu + kImages * sizeof(DkImageDescriptor), kSamplers);
    dkCmdBufBarrier(e->commands, DkBarrier_None, DkInvalidateFlags_Descriptors);
    return e;
}

void wgpuCommandEncoderRelease(WGPUCommandEncoder e) { delete e; }

void wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer source, std::uint64_t source_offset, WGPUBuffer destination,
                                          std::uint64_t destination_offset, std::uint64_t size) {
    if (!e || !source || !destination || !size) return;
    dkCmdBufCopyBuffer(e->commands, source->memory->gpu + source_offset, destination->memory->gpu + destination_offset,
                       static_cast<std::uint32_t>(size));
}

void wgpuCommandEncoderResolveQuerySet(WGPUCommandEncoder, WGPUQuerySet, std::uint32_t, std::uint32_t, WGPUBuffer, std::uint64_t) {
    log_once("timestamp queries do not exist on deko3d: the renderer's own GPU timing is off");
}

WGPUQuerySet wgpuDeviceCreateQuerySet(WGPUDevice, WGPUQuerySetDescriptor const *) { return nullptr; }

WGPUComputePassEncoder wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder e, WGPUComputePassDescriptor const *) {
    auto *p = new WGPUComputePassEncoderImpl;
    p->encoder = e;
    return p;
}

void wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder p, WGPUComputePipeline pipeline) {
    p->pipeline = pipeline;
    if (!pipeline || !pipeline->ready) return;
    const DkShader *shaders[] = {&pipeline->shader};
    dkCmdBufBindShaders(p->encoder->commands, DkStageFlag_Compute, shaders, 1);
}

void wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder p, std::uint32_t, WGPUBindGroup group, std::size_t count,
                                        std::uint32_t const *offsets) {
    bind_group(p->encoder->commands, group, true, count, offsets);
}

void wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder p, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    // A pipeline without a module is a program uam could not compile (hw_writeback_depth reads a depth texture). Counting the dispatches
    // it costs says whether this game needs that program at all: a run where the count stays at zero does not.
    if (p && p->pipeline && !p->pipeline->ready) ++p->encoder->device->skipped;
    if (!p || !p->pipeline || !p->pipeline->ready || !x || !y || !z) return;
    ++p->encoder->device->dispatches;
    DkCmdBuf cmd = p->encoder->commands;
    dkCmdBufDispatchCompute(cmd, x, y, z);
    // WebGPU: each dispatch sees what the one before it wrote.
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
}

void wgpuComputePassEncoderEnd(WGPUComputePassEncoder) {}
void wgpuComputePassEncoderRelease(WGPUComputePassEncoder p) { delete p; }

WGPURenderPassEncoder wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e, WGPURenderPassDescriptor const *descriptor) {
    trace("begin render pass");
    auto *p = new WGPURenderPassEncoderImpl;
    p->encoder = e;
    // deko3d's attachments are the images themselves: there is no load or store, so a "load" pass simply finds the image as it was and a
    // "clear" is an explicit clear on it. That is also why the hardware path's separate depth-load pass works unchanged.
    DkCmdBuf cmd = e->commands;
    ++e->device->passes;
    DkImageView color_view{}, depth_view{};
    const DkImageView *color = nullptr, *depth = nullptr;
    if (descriptor->colorAttachmentCount && descriptor->colorAttachments[0].view && descriptor->colorAttachments[0].view->texture &&
        descriptor->colorAttachments[0].view->texture->image) {
        color_view = descriptor->colorAttachments[0].view->texture->image->view;
        color = &color_view;
        p->width = descriptor->colorAttachments[0].view->texture->width;
        p->height = descriptor->colorAttachments[0].view->texture->height;
    }
    const WGPURenderPassDepthStencilAttachment *d = descriptor->depthStencilAttachment;
    if (d && d->view && d->view->texture && d->view->texture->image) {
        depth_view = d->view->texture->image->view;
        depth = &depth_view;
    }
    dkCmdBufBindRenderTargets(cmd, &color, color ? 1u : 0u, depth);
    if (color && descriptor->colorAttachments[0].loadOp == WGPULoadOp_Clear) {
        const WGPUColor c = descriptor->colorAttachments[0].clearValue;
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b),
                                static_cast<float>(c.a));
    }
    if (depth && d->depthLoadOp == WGPULoadOp_Clear) dkCmdBufClearDepthStencil(cmd, true, d->depthClearValue, 0, 0);
    // Every vertex comes from the storage buffers (hw_vs builds them from the vertex index): no attributes, no buffers.
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    return p;
}

void wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline pipeline) {
    p->pipeline = pipeline;
    if (!pipeline || !pipeline->ready) return;
    DkCmdBuf cmd = p->encoder->commands;
    const DkShader *shaders[] = {&pipeline->vertex, &pipeline->fragment};
    dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindRasterizerState(cmd, &pipeline->rasterizer);
    dkCmdBufBindMultisampleState(cmd, &pipeline->multisample);
    dkCmdBufBindColorState(cmd, &pipeline->color);
    dkCmdBufBindColorWriteState(cmd, &pipeline->write);
    dkCmdBufBindDepthStencilState(cmd, &pipeline->depth);
    if (!pipeline->blends.empty()) dkCmdBufBindBlendStates(cmd, 0, pipeline->blends.data(), static_cast<std::uint32_t>(pipeline->blends.size()));
}

void wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder p, std::uint32_t, WGPUBindGroup group, std::size_t count,
                                       std::uint32_t const *offsets) {
    bind_group(p->encoder->commands, group, false, count, offsets);
}

void wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder p, float x, float y, float width, float height, float min_depth, float max_depth) {
    const DkViewport viewport{x, y, width, height, min_depth, max_depth};
    dkCmdBufSetViewports(p->encoder->commands, 0, &viewport, 1);
}

void wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder p, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
    const DkScissor scissor{x, y, width, height};
    dkCmdBufSetScissors(p->encoder->commands, 0, &scissor, 1);
}

void wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder p, WGPUColor const *color) {
    dkCmdBufSetBlendConst(p->encoder->commands, static_cast<float>(color->r), static_cast<float>(color->g), static_cast<float>(color->b),
                          static_cast<float>(color->a));
}

void wgpuRenderPassEncoderDraw(WGPURenderPassEncoder p, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first_vertex,
                              std::uint32_t first_instance) {
    if (!p || !p->pipeline || !p->pipeline->ready || !vertices) return;
    ++p->encoder->device->draws;
    dkCmdBufDraw(p->encoder->commands, p->pipeline->primitive, vertices, instances, first_vertex, first_instance);
}

void wgpuRenderPassEncoderEnd(WGPURenderPassEncoder) {}
void wgpuRenderPassEncoderRelease(WGPURenderPassEncoder p) { delete p; }

void wgpuRenderPassSetAreaHint(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    // deko3d's render targets are whole images and the scissor says what a pass touches: there is no render area to narrow.
}

WGPUCommandBuffer wgpuCommandEncoderFinish(WGPUCommandEncoder e, WGPUCommandBufferDescriptor const *) {
    if (!e) return nullptr;
    auto *c = new WGPUCommandBufferImpl;
    c->memory = e->memory;
    c->list = dkCmdBufFinishList(e->commands);
    e->commands = nullptr;  // the command buffer owns the list now; gpu.cpp releases the encoder itself
    e->memory = nullptr;
    return c;
}

void wgpuCommandBufferRelease(WGPUCommandBuffer c) { delete c; }  // its memory is held by the device until the GPU is done with it

void wgpuQueueSubmit(WGPUQueue queue, std::size_t count, WGPUCommandBuffer const *commands) {
    trace("submit %zu command buffers", static_cast<std::size_t>(count));
    if (!queue || !count) return;
    WGPUDeviceImpl *d = queue->device;
    // One list per frame on the device's queue: the previous frame is finished first, which also frees its command memory.
    DkCmdBuf main = d->gpu.begin();
    d->retired.clear();
    for (std::size_t i = 0; i < count; ++i) {
        if (!commands[i] || !commands[i]->list) continue;
        dkCmdBufCallList(main, commands[i]->list);
        d->retired.push_back(commands[i]->memory);
    }
    d->gpu.submit(false);
    // The queue waited for the frame before this one in Gpu::begin(), so every slot that was in flight is free: the next frame's encoders
    // take them again.
    d->free_slots();
    ++d->submits;
    if (d->submits % 60 == 0) {
        static double started = 0.0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (started == 0.0) started = now;
        std::fprintf(stderr, "[deko] %u submissions in %.1f s (%.1f a second): %u passes, %u draws, %u dispatches, %u skipped (no module)\n",
                     d->submits, now - started, d->submits / (now - started), d->passes, d->draws, d->dispatches, d->skipped);
    }
}

WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue queue, WGPUQueueWorkDoneCallbackInfo info) {
    if (queue) queue->device->gpu.wait();
    if (info.callback) info.callback(WGPUQueueWorkDoneStatus_Success, text(""), info.userdata1, info.userdata2);
    return WGPUFuture{1};
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Surfaces: the console's display, through the swap chain on its own window
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUSurface wgpuCreateViSurface(WGPUInstance, void *window) {
    auto *s = new WGPUSurfaceImpl;
    s->window = window;
    return s;
}

WGPUSurface wgpuInstanceCreateSurface(WGPUInstance, WGPUSurfaceDescriptor const *) {
    log_once("only the console's own display is implemented: no other kind of surface");
    return nullptr;
}

void wgpuSurfaceRelease(WGPUSurface s) { delete s; }

WGPUStatus wgpuSurfaceGetCapabilities(WGPUSurface s, WGPUAdapter, WGPUSurfaceCapabilities *caps) {
    if (!s || !caps) return WGPUStatus_Error;
    // The back buffers are RGBA8, presented from three images, and the display locks the picture to the screen's refresh.
    auto *formats = static_cast<WGPUTextureFormat *>(std::malloc(sizeof(WGPUTextureFormat)));
    auto *modes = static_cast<WGPUPresentMode *>(std::malloc(sizeof(WGPUPresentMode)));
    auto *alpha = static_cast<WGPUCompositeAlphaMode *>(std::malloc(sizeof(WGPUCompositeAlphaMode)));
    formats[0] = WGPUTextureFormat_RGBA8Unorm;
    modes[0] = WGPUPresentMode_Fifo;
    alpha[0] = WGPUCompositeAlphaMode_Auto;
    caps->usages = WGPUTextureUsage_RenderAttachment;
    caps->formatCount = 1;
    caps->formats = formats;
    caps->presentModeCount = 1;
    caps->presentModes = modes;
    caps->alphaModeCount = 1;
    caps->alphaModes = alpha;
    return WGPUStatus_Success;
}

void wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities caps) {
    std::free(const_cast<WGPUTextureFormat *>(caps.formats));
    std::free(const_cast<WGPUPresentMode *>(caps.presentModes));
    std::free(const_cast<WGPUCompositeAlphaMode *>(caps.alphaModes));
}

void wgpuSurfaceConfigure(WGPUSurface s, WGPUSurfaceConfiguration const *config) {
    trace("configure surface %ux%u", config ? unsigned(config->width) : 0u, config ? unsigned(config->height) : 0u);
    if (!s || !config || !config->device) return;
    s->device = config->device;
    s->width = config->width ? config->width : 1280;
    s->height = config->height ? config->height : 720;
    // The console's display is the default window; the window layer hands the surface a Vi target and no window handle, because there is
    // no handle on this platform - the window is libnx's.
    if (!s->window) s->window = nwindowGetDefault();
    if (!s->swapchain.create(s->device->gpu, s->window, s->width, s->height)) {
        s->device->error("the swap chain could not be made on the console's window");
        return;
    }
    std::fprintf(stderr, "[deko] surface configured: %ux%u, three back buffers\n", s->width, s->height);
}

bool wgpuSurfaceHasSwapchain(WGPUSurface s) { return s && s->swapchain.ok(); }

void wgpuSurfaceGetCurrentTexture(WGPUSurface s, WGPUSurfaceTexture *texture) {
    texture->texture = nullptr;
    texture->status = WGPUSurfaceGetCurrentTextureStatus_Error;
    if (!s || !s->swapchain.ok()) return;
    // Blocks until the compositor frees a back buffer.
    const std::shared_ptr<ps2::gs::gpu::deko::Image> image = s->swapchain.acquire();
    if (!image) return;
    auto *t = new WGPUTextureImpl;
    t->device = s->device;
    t->image = image;
    t->format = WGPUTextureFormat_RGBA8Unorm;
    t->width = s->swapchain.width();
    t->height = s->swapchain.height();
    t->borrowed = true;  // the swap chain owns it
    s->acquired = true;
    texture->texture = t;
    texture->status = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;
}

WGPUStatus wgpuSurfacePresent(WGPUSurface s) {
    if (!s || !s->swapchain.ok() || !s->acquired) return WGPUStatus_Error;
    s->acquired = false;
    s->swapchain.present();
    return WGPUStatus_Success;
}

}
