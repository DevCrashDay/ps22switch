// The WebGPU shim driven the way gpu.cpp drives it, on the PC, against the stub deko3d (stub/deko3d_stub.cpp). It is not a rendering test:
// it runs the whole API surface the renderer uses - the same sizes, layouts, entry points, passes, submit and present - so that mistakes
// of the kind that cost a console round trip each (a string view that is not strlen's, a 4 MiB buffer bound as a uniform, a shader module
// loaded twice, a handle made from a null object) fail here in a second instead.
//
// Run it with: python scripts/deko_harness.py

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

extern "C" {
// the parts of the deko3d shim that are not in webgpu.h
WGPUSurface wgpuCreateViSurface(WGPUInstance instance, void *window);
WGPUBool wgpuSurfaceHasSwapchain(WGPUSurface surface);
WGPUStatus wgpuSurfacePresent(WGPUSurface surface);
void wgpuRenderPassSetAreaHint(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
// the stub's failure counter
int harness_failures();
}

namespace {

int g_checks = 0;

void check(bool ok, const char *what) {
    ++g_checks;
    if (!ok) std::fprintf(stderr, "[harness] FAILED: %s\n", what);
}

WGPUStringView view(const char *s) { return WGPUStringView{s, WGPU_STRLEN}; }  // what gpu.cpp does for every name

struct AdapterResult {
    WGPUAdapter adapter = nullptr;
    bool done = false;
};
struct DeviceResult {
    WGPUDevice device = nullptr;
    bool done = false;
};

WGPUBuffer make_buffer(WGPUDevice device, std::uint64_t size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = size;
    d.usage = usage;
    return wgpuDeviceCreateBuffer(device, &d);
}

WGPUTexture make_texture(WGPUDevice device, std::uint32_t size, WGPUTextureFormat format, WGPUTextureUsage usage) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = usage;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{size, size, 1};
    td.format = format;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    return wgpuDeviceCreateTexture(device, &td);
}

}

int main() {
    // --- instance, adapter, device: what gpu::init does first ---
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    check(instance != nullptr, "an instance");
    if (!instance) return 1;
    AdapterResult ar;
    WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView, void *out, void *done) {
        if (status == WGPURequestAdapterStatus_Success) *static_cast<WGPUAdapter *>(out) = adapter;
        *static_cast<bool *>(done) = true;
    };
    acb.userdata1 = &ar.adapter;
    acb.userdata2 = &ar.done;
    wgpuInstanceRequestAdapter(instance, nullptr, acb);
    check(ar.done && ar.adapter, "an adapter");
    if (!ar.adapter) return 1;

    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    check(wgpuAdapterGetInfo(ar.adapter, &info) == WGPUStatus_Success, "adapter info");
    std::fprintf(stderr, "[harness] adapter: %.*s\n", static_cast<int>(info.device.length), info.device.data);
    wgpuAdapterInfoFreeMembers(info);
    WGPULimits limits = WGPU_LIMITS_INIT;
    check(wgpuAdapterGetLimits(ar.adapter, &limits) == WGPUStatus_Success, "adapter limits");
    check(limits.maxStorageBufferBindingSize >= (4u << 20), "a storage binding limit at least as large as video memory");

    DeviceResult dr;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredLimits = &limits;
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = WGPUCallbackMode_AllowSpontaneous;
    dcb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView, void *out, void *done) {
        if (status == WGPURequestDeviceStatus_Success) *static_cast<WGPUDevice *>(out) = device;
        *static_cast<bool *>(done) = true;
    };
    dcb.userdata1 = &dr.device;
    dcb.userdata2 = &dr.done;
    wgpuAdapterRequestDevice(ar.adapter, &dd, dcb);
    check(dr.done && dr.device, "a device");
    if (!dr.device) return 1;
    WGPUDevice device = dr.device;
    WGPUQueue queue = wgpuDeviceGetQueue(device);
    check(queue != nullptr, "a queue");

    // --- the renderer's buffers, with the sizes gpu.cpp uses ---
    const std::uint64_t sizes[] = {4194304, 180224, 4194304, 8388608, 16777216, 16777216, 4194304};
    std::vector<WGPUBuffer> buffers;
    for (std::uint64_t size : sizes) {
        WGPUBuffer b = make_buffer(device, size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
        check(b != nullptr, "a buffer");
        buffers.push_back(b);
    }
    WGPUBuffer readback = make_buffer(device, 1u << 20, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    check(readback != nullptr, "a MapRead buffer");
    std::vector<std::uint32_t> tables(1024, 0);
    wgpuQueueWriteBuffer(queue, buffers[1], 0, tables.data(), tables.size() * 4);

    // --- the bind group layout gpu.cpp's compute path declares: a writable SSBO, a read-only one, a dynamic UBO, more SSBOs ---
    WGPUBindGroupLayoutEntry le[8] = {};
    for (int i = 0; i < 8; ++i) {
        le[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        le[i].binding = static_cast<std::uint32_t>(i);
        le[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        le[i].buffer.type = i == 2 ? WGPUBufferBindingType_Uniform : WGPUBufferBindingType_ReadOnlyStorage;
        le[i].buffer.hasDynamicOffset = i == 2;
        le[i].buffer.minBindingSize = i == 2 ? 256 : 0;
    }
    le[0].buffer.type = WGPUBufferBindingType_Storage;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 8;
    ld.entries = le;
    WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
    check(layout != nullptr, "a bind group layout");
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &layout;
    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pld);
    check(pipeline_layout != nullptr, "a pipeline layout");

    // --- the shader module, then every program the hardware path creates, by name with WGPU_STRLEN ---
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view("// the harness never compiles this");
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &md);
    check(module != nullptr, "a shader module");

    const char *entries[] = {"upload", "copy_read", "copy_write", "load_clut", "display", "resolve", "snapshot", "decode"};
    std::vector<WGPUComputePipeline> compute;
    for (const char *entry : entries) {
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.layout = pipeline_layout;
        cd.compute.module = module;
        cd.compute.entryPoint = view(entry);
        WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(device, &cd);
        check(pipeline != nullptr, entry);
        compute.push_back(pipeline);
    }

    // --- the hardware path's images: a colour target that is also a storage image, and a depth target ---
    WGPUTexture colour = make_texture(device, 2048, WGPUTextureFormat_RGBA8Unorm,
                                      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding);
    WGPUTexture depth = make_texture(device, 2048, WGPUTextureFormat_Depth32Float,
                                     WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
    check(colour != nullptr && depth != nullptr, "the colour and depth targets");
    WGPUTextureView colour_view = wgpuTextureCreateView(colour, nullptr);
    WGPUTextureView depth_view = wgpuTextureCreateView(depth, nullptr);
    check(colour_view != nullptr && depth_view != nullptr, "views of them");

    // a storage-image group and a sampled-texture group, as hw.wgsl's bindings 0 and 0/1 need
    WGPUBindGroupLayoutEntry storage = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    storage.binding = 0;
    storage.visibility = WGPUShaderStage_Compute;
    storage.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
    storage.storageTexture.format = WGPUTextureFormat_RGBA8Unorm;
    storage.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor sld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    sld.entryCount = 1;
    sld.entries = &storage;
    WGPUBindGroupLayout storage_layout = wgpuDeviceCreateBindGroupLayout(device, &sld);
    WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
    be.binding = 0;
    be.textureView = colour_view;
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = storage_layout;
    bgd.entryCount = 1;
    bgd.entries = &be;
    WGPUBindGroup storage_group = wgpuDeviceCreateBindGroup(device, &bgd);
    check(storage_group != nullptr, "a group holding a storage image");

    WGPUBindGroupLayoutEntry sampled[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    for (int i = 0; i < 2; ++i) {
        sampled[i].binding = static_cast<std::uint32_t>(i);
        sampled[i].visibility = WGPUShaderStage_Compute;
        sampled[i].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        sampled[i].texture.viewDimension = WGPUTextureViewDimension_2D;
    }
    WGPUBindGroupLayoutDescriptor tld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    tld.entryCount = 2;
    tld.entries = sampled;
    WGPUBindGroupLayout texture_layout = wgpuDeviceCreateBindGroupLayout(device, &tld);
    WGPUBindGroupEntry te[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    te[0].binding = 0;
    te[0].textureView = colour_view;
    te[1].binding = 1;
    te[1].textureView = depth_view;
    WGPUBindGroupDescriptor tgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    tgd.layout = texture_layout;
    tgd.entryCount = 2;
    tgd.entries = te;
    WGPUBindGroup texture_group = wgpuDeviceCreateBindGroup(device, &tgd);
    check(texture_group != nullptr, "a group holding sampled textures");

    // the buffers' group: every binding, the uniform one with a dynamic offset
    WGPUBindGroupEntry ge[8] = {};
    for (int i = 0; i < 8; ++i) {
        ge[i] = WGPU_BIND_GROUP_ENTRY_INIT;
        ge[i].binding = static_cast<std::uint32_t>(i);
        ge[i].buffer = buffers[i == 7 ? 0 : i];
        ge[i].size = WGPU_WHOLE_SIZE;
    }
    WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    gd.layout = layout;
    gd.entryCount = 8;
    gd.entries = ge;
    WGPUBindGroup buffer_group = wgpuDeviceCreateBindGroup(device, &gd);
    check(buffer_group != nullptr, "a group holding eight buffers");

    // --- a render pipeline shaped like the hardware path's: blend, depth test, dual-source factors ---
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.srcFactor = WGPUBlendFactor_Src1Alpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrc1Alpha;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_Zero;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = WGPUTextureFormat_RGBA8Unorm;
    target.writeMask = WGPUColorWriteMask_All;
    target.blend = &blend;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = view("hw_fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.depthCompare = WGPUCompareFunction_Less;
    WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rd.layout = pipeline_layout;
    rd.vertex.module = module;
    rd.vertex.entryPoint = view("hw_vs");
    rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rd.primitive.cullMode = WGPUCullMode_None;
    rd.depthStencil = &ds;
    rd.multisample.count = 1;
    rd.multisample.mask = 0xFFFFFFFFu;
    rd.fragment = &fragment;
    WGPURenderPipeline render_pipeline = wgpuDeviceCreateRenderPipeline(device, &rd);
    check(render_pipeline != nullptr, "a render pipeline with dual-source blending");

    // --- a frame: a compute pass, a render pass, submit, read back ---
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    check(encoder != nullptr, "an encoder");
    WGPUComputePassEncoder compute_pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
    wgpuComputePassEncoderSetPipeline(compute_pass, compute[0]);
    std::uint32_t offset = 256;
    wgpuComputePassEncoderSetBindGroup(compute_pass, 0, buffer_group, 1, &offset);
    wgpuComputePassEncoderDispatchWorkgroups(compute_pass, 2, 2, 1);
    wgpuComputePassEncoderSetPipeline(compute_pass, compute[4]);
    wgpuComputePassEncoderSetBindGroup(compute_pass, 0, buffer_group, 1, &offset);
    wgpuComputePassEncoderSetBindGroup(compute_pass, 1, storage_group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(compute_pass, 4, 4, 1);
    wgpuComputePassEncoderEnd(compute_pass);
    wgpuComputePassEncoderRelease(compute_pass);

    WGPURenderPassColorAttachment colour_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colour_attachment.view = colour_view;
    colour_attachment.loadOp = WGPULoadOp_Clear;
    colour_attachment.storeOp = WGPUStoreOp_Store;
    colour_attachment.clearValue = WGPUColor{0.1, 0.2, 0.3, 1.0};
    WGPURenderPassDepthStencilAttachment depth_attachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth_attachment.view = depth_view;
    depth_attachment.depthLoadOp = WGPULoadOp_Clear;
    depth_attachment.depthStoreOp = WGPUStoreOp_Store;
    depth_attachment.depthClearValue = 1.0f;
    WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rpd.colorAttachmentCount = 1;
    rpd.colorAttachments = &colour_attachment;
    rpd.depthStencilAttachment = &depth_attachment;
    wgpuRenderPassSetAreaHint(0, 0, 320, 224);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rpd);
    wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, 2048.0f, 2048.0f, 0.0f, 1.0f);
    wgpuRenderPassEncoderSetScissorRect(pass, 16, 16, 320, 224);
    const WGPUColor constant{0.5, 0.5, 0.5, 0.5};
    wgpuRenderPassEncoderSetBlendConstant(pass, &constant);
    wgpuRenderPassEncoderSetPipeline(pass, render_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, buffer_group, 1, &offset);
    wgpuRenderPassEncoderDraw(pass, 6u * 97u, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    wgpuCommandEncoderCopyBufferToBuffer(encoder, buffers[0], 0, readback, 0, 4096);
    WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, nullptr);
    check(command != nullptr, "a command buffer");
    wgpuCommandEncoderRelease(encoder);
    wgpuQueueSubmit(queue, 1, &command);
    wgpuCommandBufferRelease(command);
    wgpuDevicePoll(device, true, nullptr);

    // --- reading a buffer back, as the download and snapshot paths do ---
    struct MapResult {
        bool done = false;
        bool success = false;
    } map;
    WGPUBufferMapCallbackInfo mcb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mcb.mode = WGPUCallbackMode_AllowSpontaneous;
    mcb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *out, void *done) {
        *static_cast<bool *>(out) = status == WGPUMapAsyncStatus_Success;
        *static_cast<bool *>(done) = true;
    };
    mcb.userdata1 = &map.success;
    mcb.userdata2 = &map.done;
    wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, 4096, mcb);
    check(map.done && map.success, "a buffer map that calls back");
    check(wgpuBufferGetConstMappedRange(readback, 0, 4096) != nullptr, "a mapped range");
    wgpuBufferUnmap(readback);

    // --- what gpu.cpp does with lifetimes, which WebGPU allows: it releases the layout, the pipeline layout and the buffer as soon as the
    // group and the pipeline that were made from them exist, because those keep them alive. The shim used to read the freed layout at
    // every bind, and the blit group's was the first one that faulted on the console (a data abort in bind_group, 0x4e51d1002); here it
    // is a use-after-free that --asan reports. Binding the group with a dynamic offset needs the layout's flags (uniform, dynamic) and
    // the buffer's bytes, so both have to still be there.
    {
        WGPUBindGroupLayoutEntry late[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
        late[0].binding = 0;
        late[0].visibility = WGPUShaderStage_Compute;
        late[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        late[1].binding = 1;
        late[1].visibility = WGPUShaderStage_Fragment;
        late[1].buffer.type = WGPUBufferBindingType_Uniform;
        late[1].buffer.hasDynamicOffset = true;  // the frame's parameters, as gpu.cpp binds them
        WGPUBindGroupLayoutDescriptor late_layout_descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        late_layout_descriptor.entryCount = 2;
        late_layout_descriptor.entries = late;
        WGPUBindGroupLayout born = wgpuDeviceCreateBindGroupLayout(device, &late_layout_descriptor);
        WGPUPipelineLayoutDescriptor late_pipeline_descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        late_pipeline_descriptor.bindGroupLayoutCount = 1;
        late_pipeline_descriptor.bindGroupLayouts = &born;
        WGPUPipelineLayout late_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &late_pipeline_descriptor);
        WGPUBuffer short_lived = make_buffer(device, 4096, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        WGPUBindGroupEntry late_entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        late_entries[0].binding = 0;
        late_entries[0].buffer = buffers[1];
        late_entries[0].size = WGPU_WHOLE_SIZE;
        late_entries[1].binding = 1;
        late_entries[1].buffer = short_lived;
        late_entries[1].size = 4096;
        WGPUBindGroupDescriptor late_group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        late_group_descriptor.layout = born;
        late_group_descriptor.entryCount = 2;
        late_group_descriptor.entries = late_entries;
        WGPUBindGroup late_group = wgpuDeviceCreateBindGroup(device, &late_group_descriptor);
        check(late_group != nullptr, "a bind group");
        wgpuPipelineLayoutRelease(late_pipeline_layout);
        wgpuBindGroupLayoutRelease(born);
        wgpuBufferRelease(short_lived);
        WGPUCommandEncoder late_encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        WGPUComputePassEncoder late_pass = wgpuCommandEncoderBeginComputePass(late_encoder, nullptr);
        wgpuComputePassEncoderSetPipeline(late_pass, compute[0]);
        std::uint32_t late_offset = 1024;
        wgpuComputePassEncoderSetBindGroup(late_pass, 0, late_group, 1, &late_offset);
        wgpuComputePassEncoderDispatchWorkgroups(late_pass, 1, 1, 1);
        wgpuComputePassEncoderEnd(late_pass);
        wgpuComputePassEncoderRelease(late_pass);
        WGPUCommandBuffer late_command = wgpuCommandEncoderFinish(late_encoder, nullptr);
        check(late_command != nullptr, "a command buffer from a group whose layout and buffer are gone");
        wgpuQueueSubmit(queue, 1, &late_command);
        wgpuCommandBufferRelease(late_command);
        wgpuCommandEncoderRelease(late_encoder);
        wgpuDevicePoll(device, true, nullptr);
        wgpuBindGroupRelease(late_group);
    }

    // --- the surface, the swap chain and present, as the window layer drives them ---
    WGPUSurface surface = wgpuCreateViSurface(instance, nullptr);
    check(surface != nullptr, "a surface");
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    check(wgpuSurfaceGetCapabilities(surface, ar.adapter, &caps) == WGPUStatus_Success && caps.formatCount, "surface capabilities");
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device;
    config.format = WGPUTextureFormat_RGBA8Unorm;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = 1280;
    config.height = 720;
    config.presentMode = WGPUPresentMode_Fifo;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(surface, &config);
    check(wgpuSurfaceHasSwapchain(surface), "a swap chain on the window");

    for (int frame = 0; frame < 3; ++frame) {
        WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(surface, &st);
        check(st.texture != nullptr && st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal, "a back buffer");
        if (!st.texture) break;
        WGPUTextureView target_view = wgpuTextureCreateView(st.texture, nullptr);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
        WGPURenderPassColorAttachment a = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        a.view = target_view;
        a.loadOp = WGPULoadOp_Clear;
        a.storeOp = WGPUStoreOp_Store;
        a.clearValue = WGPUColor{0.0, 0.5, 0.0, 1.0};
        WGPURenderPassDescriptor d = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        d.colorAttachmentCount = 1;
        d.colorAttachments = &a;
        WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &d);
        wgpuRenderPassEncoderSetViewport(p, 0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f);
        wgpuRenderPassEncoderSetPipeline(p, render_pipeline);
        wgpuRenderPassEncoderSetBindGroup(p, 0, buffer_group, 1, &offset);
        wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(p);
        wgpuRenderPassEncoderRelease(p);
        WGPUCommandBuffer c = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuCommandEncoderRelease(enc);
        wgpuQueueSubmit(queue, 1, &c);
        wgpuCommandBufferRelease(c);
        check(wgpuSurfacePresent(surface) == WGPUStatus_Success, "a present");
        wgpuTextureViewRelease(target_view);
        wgpuTextureRelease(st.texture);
    }

    // --- release everything, in the order gpu.cpp releases it ---
    wgpuRenderPipelineRelease(render_pipeline);
    wgpuBindGroupRelease(buffer_group);
    wgpuBindGroupRelease(texture_group);
    wgpuBindGroupRelease(storage_group);
    wgpuBindGroupLayoutRelease(texture_layout);
    wgpuBindGroupLayoutRelease(storage_layout);
    wgpuTextureViewRelease(depth_view);
    wgpuTextureViewRelease(colour_view);
    wgpuTextureRelease(depth);
    wgpuTextureRelease(colour);
    for (WGPUComputePipeline pipeline : compute) wgpuComputePipelineRelease(pipeline);
    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuBindGroupLayoutRelease(layout);
    for (WGPUBuffer b : buffers) wgpuBufferRelease(b);
    wgpuBufferRelease(readback);
    wgpuSurfaceRelease(surface);
    wgpuDeviceRelease(device);
    wgpuAdapterRelease(ar.adapter);
    wgpuInstanceRelease(instance);

    const int failures = harness_failures();
    std::fprintf(stderr, "[harness] %d checks, %d failures\n", g_checks, failures);
    std::fprintf(stderr, failures ? "[harness] FAIL\n" : "[harness] PASS: the whole API surface the renderer uses\n");
    return failures ? 1 : 0;
}
