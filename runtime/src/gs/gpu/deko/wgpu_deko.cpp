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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

WGPUStringView text(const char *s) { return WGPUStringView{s, std::strlen(s)}; }

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
    // The code of every shader module made on this device, one module after the other in one Code memory block.
    std::shared_ptr<ps2::gs::gpu::deko::Memory> code_memory;
    std::uint32_t code_used = 0;
    // Command memory the GPU may still be reading: freed when it has finished with it.
    std::vector<std::shared_ptr<ps2::gs::gpu::deko::Memory>> retired;
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
    bool borrowed = false;  // a swap chain back buffer: the swap chain owns it
};

struct WGPUTextureViewImpl {
    WGPUDeviceImpl *device = nullptr;
    WGPUTextureImpl *texture = nullptr;
};

struct WGPUBindGroupLayoutImpl {
    struct Entry {
        std::uint32_t binding = 0;
        bool buffer = false;
        bool uniform = false;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    };
    std::vector<Entry> entries;
};

struct WGPUPipelineLayoutImpl {
    std::vector<WGPUBindGroupLayoutImpl *> groups;
};

struct WGPUBindGroupImpl {
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
    WGPUShaderModuleImpl *module = nullptr;
    std::string entry_point;
};

struct WGPURenderPipelineImpl {
    WGPUShaderModuleImpl *vertex = nullptr;
    WGPUShaderModuleImpl *fragment = nullptr;
    std::string vertex_entry, fragment_entry;
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

WGPUQueue wgpuDeviceGetQueue(WGPUDevice d) { return d ? d->queue.get() : nullptr; }

WGPUBool wgpuDevicePoll(WGPUDevice d, WGPUBool wait, WGPUSubmissionIndex const *) {
    if (d && wait) d->gpu.wait();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice d, WGPUBufferDescriptor const *descriptor) {
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
    if (!t) return nullptr;
    auto *v = new WGPUTextureViewImpl;
    v->device = t->device;
    v->texture = t;
    return v;
}

void wgpuTextureViewRelease(WGPUTextureView v) { delete v; }

// ---------------------------------------------------------------------------------------------------------------------------------
// Bind groups, layouts and pipelines (the next slice; these keep gpu.cpp's objects and say what is missing)
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUBindGroupLayout wgpuDeviceCreateBindGroupLayout(WGPUDevice, WGPUBindGroupLayoutDescriptor const *descriptor) {
    auto *l = new WGPUBindGroupLayoutImpl;
    for (std::size_t i = 0; i < descriptor->entryCount; ++i) {
        const WGPUBindGroupLayoutEntry &e = descriptor->entries[i];
        WGPUBindGroupLayoutImpl::Entry entry;
        entry.binding = e.binding;
        entry.buffer = e.buffer.type != WGPUBufferBindingType_BindingNotUsed;
        entry.uniform = e.buffer.type == WGPUBufferBindingType_Uniform;
        l->entries.push_back(entry);
    }
    return l;
}

void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout l) { delete l; }

WGPUPipelineLayout wgpuDeviceCreatePipelineLayout(WGPUDevice, WGPUPipelineLayoutDescriptor const *descriptor) {
    auto *p = new WGPUPipelineLayoutImpl;
    for (std::size_t i = 0; i < descriptor->bindGroupLayoutCount; ++i) p->groups.push_back(descriptor->bindGroupLayouts[i]);
    return p;
}

void wgpuPipelineLayoutRelease(WGPUPipelineLayout p) { delete p; }

WGPUBindGroup wgpuDeviceCreateBindGroup(WGPUDevice, WGPUBindGroupDescriptor const *descriptor) {
    auto *g = new WGPUBindGroupImpl;
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

WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice, WGPUShaderModuleDescriptor const *descriptor) {
    // gpu.cpp passes the WGSL text and the entry point through a chained WGSL descriptor; the entry point name is what finds the module
    // uam compiled (dksh_table.cpp). Nothing is compiled here.
    auto *m = new WGPUShaderModuleImpl;
    m->entry_point = "unknown";
    for (const WGPUChainedStruct *c = descriptor->nextInChain; c; c = c->next) {
        if (c->sType == WGPUSType_ShaderSourceWGSL) {
            const auto *wgsl = reinterpret_cast<const WGPUShaderSourceWGSL *>(c);
            (void)wgsl;
        }
    }
    return m;
}

void wgpuShaderModuleRelease(WGPUShaderModule m) { delete m; }

WGPUComputePipeline wgpuDeviceCreateComputePipeline(WGPUDevice, WGPUComputePipelineDescriptor const *descriptor) {
    auto *p = new WGPUComputePipelineImpl;
    p->module = descriptor->compute.module;
    if (descriptor->compute.entryPoint.data) p->entry_point.assign(descriptor->compute.entryPoint.data, descriptor->compute.entryPoint.length);
    return p;
}

void wgpuComputePipelineRelease(WGPUComputePipeline p) { delete p; }

WGPURenderPipeline wgpuDeviceCreateRenderPipeline(WGPUDevice, WGPURenderPipelineDescriptor const *descriptor) {
    auto *p = new WGPURenderPipelineImpl;
    p->vertex = descriptor->vertex.module;
    if (descriptor->vertex.entryPoint.data) p->vertex_entry.assign(descriptor->vertex.entryPoint.data, descriptor->vertex.entryPoint.length);
    if (descriptor->fragment) {
        p->fragment = descriptor->fragment->module;
        if (descriptor->fragment->entryPoint.data)
            p->fragment_entry.assign(descriptor->fragment->entryPoint.data, descriptor->fragment->entryPoint.length);
    }
    return p;
}

void wgpuRenderPipelineRelease(WGPURenderPipeline p) { delete p; }

// ---------------------------------------------------------------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------------------------------------------------------------

WGPUCommandEncoder wgpuDeviceCreateCommandEncoder(WGPUDevice d, WGPUCommandEncoderDescriptor const *) {
    auto *e = new WGPUCommandEncoderImpl;
    e->device = d;
    // Each encoder records into its own memory: gpu.cpp keeps several encoders alive and submits them in one go.
    e->memory = d->gpu.allocate(256 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!e->memory) {
        delete e;
        d->error("no command memory for an encoder");
        return nullptr;
    }
    e->commands = dk::CmdBufMaker{d->gpu.device()}.create();
    dkCmdBufAddMemory(e->commands, e->memory->block, 0, e->memory->size);
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

void wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder p, WGPUComputePipeline pipeline) { p->pipeline = pipeline; }

void wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder, std::uint32_t, WGPUBindGroup, std::size_t, std::uint32_t const *) {}

void wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder, std::uint32_t, std::uint32_t, std::uint32_t) {
    log_once("compute dispatches are the next slice: the VRAM moves are not running on deko3d yet");
}

void wgpuComputePassEncoderEnd(WGPUComputePassEncoder) {}
void wgpuComputePassEncoderRelease(WGPUComputePassEncoder p) { delete p; }

WGPURenderPassEncoder wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e, WGPURenderPassDescriptor const *descriptor) {
    auto *p = new WGPURenderPassEncoderImpl;
    p->encoder = e;
    // The attachment says what to do with the image before and after the pass; the clear is a deko3d clear on the bound target.
    for (std::size_t i = 0; i < descriptor->colorAttachmentCount; ++i) {
        const WGPURenderPassColorAttachment &a = descriptor->colorAttachments[i];
        if (!a.view || !a.view->texture || !a.view->texture->image) continue;
        const DkImageView &view = a.view->texture->image->view;
        dkCmdBufBindRenderTarget(e->commands, &view, nullptr);
        if (a.loadOp == WGPULoadOp_Clear) {
            dkCmdBufClearColorFloat(e->commands, static_cast<std::uint32_t>(i), DkColorMask_RGBA, static_cast<float>(a.clearValue.r),
                                    static_cast<float>(a.clearValue.g), static_cast<float>(a.clearValue.b), static_cast<float>(a.clearValue.a));
        }
        p->width = a.view->texture->width;
        p->height = a.view->texture->height;
    }
    log_once("render passes clear and set state, but nothing is drawn yet: pipelines bind in the next slice");
    return p;
}

void wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline pipeline) { p->pipeline = pipeline; }

void wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder, std::uint32_t, WGPUBindGroup, std::size_t, std::uint32_t const *) {}

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

void wgpuRenderPassEncoderDraw(WGPURenderPassEncoder, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    log_once("draws are the next slice: nothing is on screen from the game yet");
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
    if (!s || !config || !config->device) return;
    s->device = config->device;
    s->width = config->width ? config->width : 1280;
    s->height = config->height ? config->height : 720;
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
