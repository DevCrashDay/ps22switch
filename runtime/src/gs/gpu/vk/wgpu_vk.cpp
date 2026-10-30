// The part of the WebGPU C API (webgpu.h, wgpu.h) that runtime/src/gs/gpu/gpu.cpp uses, written on Vulkan, so the graphics chip's
// drawing program runs where wgpu-native does not: on the Nintendo Switch, through Mesa's NVK driver (docs: README, "Vulkan renderer").
// gpu.cpp stays as it is on the PC; only the API under it changes.
//
// What it covers: compute pipelines, storage and uniform buffers (one bind group layout with a dynamic uniform offset), buffer writes and
// copies, mapping buffers for reading, and waiting for the queue. What it does not: surfaces, render pipelines and timestamp queries.
// gpu.cpp falls back on its own when those are missing (the picture is read back and shown by the window code; no timing).
//
// WGSL is not compiled here. The shaders were turned into SPIR-V by naga when they last changed (scripts/wgsl_spirv.py) and are built
// into the program; a WGSL text is matched to its SPIR-V by a hash of the text, and a pipeline's override constants pick the variant.
//
// WebGPU's ordering rules, kept the Vulkan way:
//   - queue writes happen before the command buffers submitted after them: they are copies from a staging ring, recorded into a command
//     buffer of their own that goes first in the next submission;
//   - every dispatch and copy sees the results of the ones before it: a memory barrier follows each of them, and each command buffer
//     starts with one against the work submitted earlier;
//   - a map completes once the GPU has finished everything submitted before the map was asked for (a timeline semaphore counts the
//     submissions), and its callback runs in wgpuDevicePoll / wgpuInstanceProcessEvents;
//   - buffers start out zero, and a released object is destroyed only once the GPU is past the submissions that may use it.

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#ifndef __SWITCH__
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <pthread.h>
#endif

#ifdef _WIN32
extern "C" void *__stdcall LoadLibraryA(const char *name);
extern "C" void *__stdcall GetProcAddress(void *module, const char *name);
#elif !defined(__SWITCH__)
#include <dlfcn.h>
#endif

#include "spirv.hpp"

// ---------------------------------------------------------------------------------------------------------------------------------
// Vulkan entry points. NVK on the Switch is linked in and exports them all; elsewhere they come from the system's loader.
#define PS2_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define PS2_VK_INSTANCE(X)                                                                                                              \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)       \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define PS2_VK_DEVICE(X)                                                                                                                \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer)                    \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory)                          \
    X(vkInvalidateMappedMemoryRanges) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)                        \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkCreateSemaphore) X(vkDestroySemaphore)                         \
    X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)                    \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets)  \
    X(vkUpdateDescriptorSets) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkDestroyPipeline)                        \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

#ifndef __SWITCH__
#define PS2_VK_POINTER(name) static PFN_##name name = nullptr;
PS2_VK_GLOBAL(PS2_VK_POINTER)
PS2_VK_INSTANCE(PS2_VK_POINTER)
PS2_VK_DEVICE(PS2_VK_POINTER)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#endif

namespace {

std::recursive_mutex g_lock;  // gpu.cpp calls from one thread; this keeps the shim safe if that ever changes

void log_vk(const char *what, VkResult r) { std::fprintf(stderr, "[vulkan] %s failed: %d\n", what, static_cast<int>(r)); }

bool load_global() {
#ifdef __SWITCH__
    return true;
#else
    if (vkGetInstanceProcAddr) return true;
#ifdef _WIN32
    void *lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) return false;
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
    if (!vkGetInstanceProcAddr) return false;
#define PS2_VK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    PS2_VK_GLOBAL(PS2_VK_LOAD_GLOBAL)
    return vkCreateInstance != nullptr;
#endif
}

void load_instance([[maybe_unused]] VkInstance instance) {
#ifndef __SWITCH__
#define PS2_VK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    PS2_VK_INSTANCE(PS2_VK_LOAD_INSTANCE)
#endif
}

void load_device([[maybe_unused]] VkDevice device) {
#ifndef __SWITCH__
#define PS2_VK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    PS2_VK_DEVICE(PS2_VK_LOAD_DEVICE)
#endif
}

WGPUStringView text(const char *s) { return WGPUStringView{s, s ? std::strlen(s) : 0}; }

std::string str(WGPUStringView v) {
    if (!v.data) return {};
    return v.length == WGPU_STRLEN ? std::string(v.data) : std::string(v.data, v.length);
}

// FNV-1a over the WGSL text without carriage returns, so a CRLF checkout of gs.wgsl matches the hash made from an LF one
std::uint64_t wgsl_hash(WGPUStringView code) {
    const std::string s = str(code);
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : s) {
        if (c == '\r') continue;
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Objects

struct WGPUInstanceImpl {
    VkInstance instance = VK_NULL_HANDLE;
    std::uint32_t api = VK_API_VERSION_1_2;
};

struct WGPUAdapterImpl {
    WGPUInstance instance = nullptr;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    std::uint32_t family = 0;
};

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint8_t *map = nullptr;
    VkDeviceSize size = 0, used = 0;
    std::uint64_t free_after = 0;  // the submission that last read from it
};

struct MapRequest {
    WGPUBuffer buffer;
    std::uint64_t serial;
    WGPUBufferMapCallbackInfo info;
};

struct WorkDone {
    std::uint64_t serial;
    WGPUQueueWorkDoneCallbackInfo info;
};

struct WGPUDeviceImpl {
    WGPUAdapter adapter = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint64_t submitted = 0, completed = 0;
    std::vector<std::pair<VkCommandBuffer, std::uint64_t>> spare;  // command buffers and the submission they were last part of
    VkCommandBuffer uploads = VK_NULL_HANDLE;                     // queue writes waiting for the next submission
    std::vector<Staging> staging;
    std::size_t staging_current = SIZE_MAX;  // the block writes are appended to
    std::deque<MapRequest> maps;
    std::deque<WorkDone> work_done;
    std::deque<std::pair<std::uint64_t, std::function<void()>>> graveyard;  // destroyed once the GPU is past the submission
    std::vector<VkDescriptorPool> pools;
    WGPUUncapturedErrorCallbackInfo errors{};
    WGPUQueue queue_object = nullptr;
    std::uint64_t max_in_flight = 8;  // submissions the CPU may be ahead of the GPU (PS2_VK_IN_FLIGHT)
    // PS2_VK_PROFILE=1: GPU time of every dispatch from timestamps, summed by program and written to the log every 2 seconds
    struct Timed {
        VkCommandBuffer cmd;
        std::uint64_t serial;
        std::uint32_t first, count;
        std::vector<const char *> names;
    };
    VkQueryPool queries = VK_NULL_HANDLE;
    std::uint32_t query_next = 0;
    double tick_ns = 1.0;
    std::vector<Timed> recording, timed;  // being recorded or submitted, waiting for the GPU
    std::map<std::string, std::pair<double, std::uint64_t>> spent;  // program -> ns, dispatches
    std::uint64_t profile_submits = 0, most_in_flight = 0, waits = 0;
    double waited_ms = 0;
    std::chrono::steady_clock::time_point profile_start = std::chrono::steady_clock::now();
};

struct WGPUQueueImpl {
    WGPUDevice device = nullptr;
};

struct WGPUBufferImpl {
    WGPUDevice device = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint8_t *map = nullptr;  // MapRead buffers: host memory, mapped for good
    bool coherent = true, mapped = false;
    std::uint64_t size = 0;
};

struct WGPUBindGroupLayoutImpl {
    WGPUDevice device = nullptr;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorType> types;  // by binding
};

struct WGPUPipelineLayoutImpl {
    WGPUDevice device = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct WGPUBindGroupImpl {
    WGPUDevice device = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

struct WGPUShaderModuleImpl {
    std::uint64_t hash = 0;
};

struct WGPUComputePipelineImpl {
    WGPUDevice device = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::string name;
};

struct WGPUCommandEncoderImpl {
    WGPUDevice device = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

struct WGPUCommandBufferImpl {
    WGPUDevice device = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

struct WGPUComputePassEncoderImpl {
    WGPUCommandEncoder encoder = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const char *name = "";
};

namespace {

void report(WGPUDevice d, const std::string &message) {
    std::fprintf(stderr, "[vulkan] %s\n", message.c_str());
    if (d && d->errors.callback) d->errors.callback(&d, WGPUErrorType_Internal, WGPUStringView{message.data(), message.size()}, d->errors.userdata1, d->errors.userdata2);
}

std::uint32_t memory_type(WGPUDevice d, std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) {
    for (const VkMemoryPropertyFlags flags : {want, fallback})
        for (std::uint32_t i = 0; i < d->memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (d->memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

bool make_vk_buffer(WGPUDevice d, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback,
                    VkBuffer &buffer, VkDeviceMemory &memory, std::uint8_t **map, bool *coherent) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(size, 4);
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VkResult r = vkCreateBuffer(d->device, &bi, nullptr, &buffer); r != VK_SUCCESS) {
        log_vk("vkCreateBuffer", r);
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, buffer, &req);
    const std::uint32_t type = memory_type(d, req.memoryTypeBits, want, fallback);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(d->device, &ai, nullptr, &memory) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] no memory for a buffer of %llu bytes\n", static_cast<unsigned long long>(size));
        vkDestroyBuffer(d->device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(d->device, buffer, memory, 0);
    if (map) {
        void *p = nullptr;
        if (vkMapMemory(d->device, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            vkFreeMemory(d->device, memory, nullptr);
            vkDestroyBuffer(d->device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        *map = static_cast<std::uint8_t *>(p);
        if (coherent) *coherent = (d->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }
    return true;
}

// Everything written before is visible to everything after: transfers, compute shaders and the host.
void full_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void host_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void dispatch_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                      VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void update_completed(WGPUDevice d) {
    std::uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(d->device, d->timeline, &value) == VK_SUCCESS) d->completed = std::max(d->completed, value);
}

VkCommandBuffer begin_commands(WGPUDevice d) {
    update_completed(d);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    for (std::size_t i = 0; i < d->spare.size(); ++i)
        if (d->spare[i].second <= d->completed) {
            cmd = d->spare[i].first;
            d->spare.erase(d->spare.begin() + static_cast<std::ptrdiff_t>(i));
            vkResetCommandBuffer(cmd, 0);
            break;
        }
    if (!cmd) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = d->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (VkResult r = vkAllocateCommandBuffers(d->device, &ai, &cmd); r != VK_SUCCESS) {
            log_vk("vkAllocateCommandBuffers", r);
            return VK_NULL_HANDLE;
        }
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    full_barrier(cmd);
    return cmd;
}

// Room for `size` bytes of queue writes; the copy out of it goes into d->uploads
// Room for `size` bytes of queue writes; the copy out of it goes into d->uploads. The writes of many submissions share a block, one
// after the other, and a block is started again from the front once the GPU is past the last submission that read from it.
Staging *staging_for(WGPUDevice d, VkDeviceSize size, VkDeviceSize &offset) {
    size = (size + 15) & ~VkDeviceSize{15};
    const std::uint64_t next = d->submitted + 1;  // the submission the pending uploads go out with
    if (d->staging_current < d->staging.size()) {
        Staging &s = d->staging[d->staging_current];
        if (s.used + size <= s.size) {
            offset = s.used;
            s.used += size;
            s.free_after = next;
            return &s;
        }
    }
    update_completed(d);
    for (std::size_t i = 0; i < d->staging.size(); ++i) {
        Staging &s = d->staging[i];
        if (i != d->staging_current && s.free_after <= d->completed && size <= s.size) {
            d->staging_current = i;
            s.used = size;
            s.free_after = next;
            offset = 0;
            return &s;
        }
    }
    Staging s;
    s.size = std::max<VkDeviceSize>(size, 16u << 20);
    if (!make_vk_buffer(d, s.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s.buffer, s.memory, &s.map, nullptr))
        return nullptr;
    s.used = size;
    s.free_after = next;
    offset = 0;
    d->staging.push_back(s);
    d->staging_current = d->staging.size() - 1;
    if (d->staging.size() > 2)
        std::fprintf(stderr, "[vulkan] staging block %zu of %llu bytes (%llu submissions in flight)\n", d->staging.size(), static_cast<unsigned long long>(s.size),
                     static_cast<unsigned long long>(d->submitted - d->completed));
    return &d->staging.back();
}

VkCommandBuffer upload_commands(WGPUDevice d) {
    if (!d->uploads) d->uploads = begin_commands(d);
    return d->uploads;
}

void wait_for(WGPUDevice d, std::uint64_t serial);

constexpr std::uint32_t kQueries = 1u << 16, kQueriesPerBuffer = 4096;

WGPUDeviceImpl::Timed *timing_of(WGPUDevice d, VkCommandBuffer cmd) {
    for (WGPUDeviceImpl::Timed &t : d->recording)
        if (t.cmd == cmd) return &t;
    return nullptr;
}

// A command buffer of an encoder starts its slice of timestamps
void time_begin(WGPUDevice d, VkCommandBuffer cmd) {
    if (!d->queries) return;
    if (d->query_next + kQueriesPerBuffer > kQueries) d->query_next = 0;
    WGPUDeviceImpl::Timed t{cmd, 0, d->query_next, 1, {}};
    d->query_next += kQueriesPerBuffer;
    vkCmdResetQueryPool(cmd, d->queries, t.first, kQueriesPerBuffer);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d->queries, t.first);  // after the opening barrier: earlier work is not counted
    d->recording.push_back(std::move(t));
}

void time_after(WGPUDevice d, VkCommandBuffer cmd, const char *name) {
    WGPUDeviceImpl::Timed *t = d->queries ? timing_of(d, cmd) : nullptr;
    if (!t || t->count >= kQueriesPerBuffer) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d->queries, t->first + t->count);
    ++t->count;
    t->names.push_back(name);
}

void profile_collect(WGPUDevice d) {
    for (auto it = d->timed.begin(); it != d->timed.end();) {
        if (it->serial > d->completed) {
            ++it;
            continue;
        }
        std::vector<std::uint64_t> ticks(it->count);
        if (vkGetQueryPoolResults(d->device, d->queries, it->first, it->count, ticks.size() * 8, ticks.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (std::uint32_t i = 1; i < it->count; ++i) {
                auto &slot = d->spent[it->names[i - 1]];
                slot.first += static_cast<double>(ticks[i] - ticks[i - 1]) * d->tick_ns;
                ++slot.second;
            }
        it = d->timed.erase(it);
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - d->profile_start).count();
    if (seconds < 2.0) return;
    double busy = 0;
    for (const auto &e : d->spent) busy += e.second.first;
    std::fprintf(stderr, "[vk-profile] per second: %.0f submits, GPU busy %.1f ms, most in flight %llu, CPU waited %.1f ms (%.0f times) |",
                 d->profile_submits / seconds, busy / 1e6 / seconds, static_cast<unsigned long long>(d->most_in_flight), d->waited_ms / seconds, d->waits / seconds);
    for (const auto &e : d->spent)
        std::fprintf(stderr, " %s %.1f ms (%.0f)", e.first.c_str(), e.second.first / 1e6 / seconds, e.second.second / seconds);
    std::fprintf(stderr, "\n");
    d->spent.clear();
    d->profile_submits = d->most_in_flight = d->waits = 0;
    d->waited_ms = 0;
    d->profile_start = now;
}

// One submission: the pending queue writes first, then the given command buffers; it signals the timeline with its serial
void submit(WGPUDevice d, const std::vector<VkCommandBuffer> &given) {
    std::vector<VkCommandBuffer> all;
    if (d->uploads) {
        full_barrier(d->uploads);
        vkEndCommandBuffer(d->uploads);
        all.push_back(d->uploads);
        d->uploads = VK_NULL_HANDLE;
    }
    all.insert(all.end(), given.begin(), given.end());
    if (all.empty()) return;
    const std::uint64_t serial = d->submitted + 1;
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &serial;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.commandBufferCount = static_cast<std::uint32_t>(all.size());
    si.pCommandBuffers = all.data();
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &d->timeline;
    if (VkResult r = vkQueueSubmit(d->queue, 1, &si, VK_NULL_HANDLE); r != VK_SUCCESS) {
        report(d, "vkQueueSubmit failed: " + std::to_string(static_cast<int>(r)));
        return;
    }
    d->submitted = serial;
    for (VkCommandBuffer c : all) d->spare.emplace_back(c, serial);
    if (d->queries) {
        for (VkCommandBuffer c : all)
            for (auto it = d->recording.begin(); it != d->recording.end(); ++it)
                if (it->cmd == c) {
                    it->serial = serial;
                    d->timed.push_back(std::move(*it));
                    d->recording.erase(it);
                    break;
                }
        ++d->profile_submits;
        d->most_in_flight = std::max(d->most_in_flight, d->submitted - d->completed);
    }
    // The CPU stays at most max_in_flight submissions ahead: a backlog makes the picture late and, on the Switch, ran NVK out of room
    // for submissions (VK_ERROR_DEVICE_LOST after about 240)
    update_completed(d);
    if (d->submitted - d->completed > d->max_in_flight) {
        const auto t0 = std::chrono::steady_clock::now();
        wait_for(d, d->submitted - d->max_in_flight);
        d->waited_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++d->waits;
    }
}

void bury(WGPUDevice d, std::function<void()> destroy) { d->graveyard.emplace_back(d->submitted + 1, std::move(destroy)); }

// Callbacks of finished maps and queue waits, and objects the GPU is done with
void process(WGPUDevice d) {
    update_completed(d);
    if (d->queries) profile_collect(d);
    while (!d->graveyard.empty() && d->graveyard.front().first <= d->completed) {
        auto destroy = std::move(d->graveyard.front().second);
        d->graveyard.pop_front();
        destroy();
    }
    while (!d->maps.empty() && d->maps.front().serial <= d->completed) {
        MapRequest m = d->maps.front();
        d->maps.pop_front();
        if (!m.buffer->coherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = m.buffer->memory;
            range.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(d->device, 1, &range);
        }
        m.buffer->mapped = true;
        if (m.info.callback) m.info.callback(WGPUMapAsyncStatus_Success, text(""), m.info.userdata1, m.info.userdata2);
    }
    while (!d->work_done.empty() && d->work_done.front().serial <= d->completed) {
        WorkDone w = d->work_done.front();
        d->work_done.pop_front();
        if (w.info.callback) w.info.callback(WGPUQueueWorkDoneStatus_Success, text(""), w.info.userdata1, w.info.userdata2);
    }
}

void wait_for(WGPUDevice d, std::uint64_t serial) {
    if (serial <= d->completed) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &d->timeline;
    wi.pValues = &serial;
    if (VkResult r = vkWaitSemaphores(d->device, &wi, UINT64_MAX); r != VK_SUCCESS) log_vk("vkWaitSemaphores", r);
    update_completed(d);
}

// The driver's shader compiler on a stack of 8 MiB: on the Switch the calling thread may have 128 KiB (libnx's default) or the loader's
// main-thread stack, and NVK's compiler recurses over a program as large as gs.wgsl.
void on_big_stack(const std::function<void()> &work) {
#ifdef __SWITCH__
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t thread;
    auto *job = new std::function<void()>(work);
    if (pthread_create(&thread, &attr, [](void *arg) -> void * {
            (*static_cast<std::function<void()> *>(arg))();
            return nullptr;
        }, job) == 0) {
        pthread_join(thread, nullptr);
    } else {
        work();
    }
    delete job;
    pthread_attr_destroy(&attr);
#else
    work();
#endif
}

WGPUDevice g_device = nullptr;  // the one device (gpu.cpp makes one); wgpuInstanceProcessEvents polls it

const ps2::gs::gpu::vk::SpirvVariant *find_variant(std::uint64_t hash, const WGPUConstantEntry *constants, std::size_t count) {
    std::uint32_t scale = 1, tile = 16;
    bool fma = false;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string key = str(constants[i].key);
        if (key == "SCALE") scale = static_cast<std::uint32_t>(constants[i].value);
        else if (key == "TILE") tile = static_cast<std::uint32_t>(constants[i].value);
        else if (key == "EXPLICIT_FMA") fma = constants[i].value != 0.0;
    }
    // PS2_VK_FMA=0 or 1 picks how the shader's sums are rounded (fused or not) whatever gpu.cpp asks; unset, 1: the order the pictures of
    // the PC's compilers show, spelled out with fma(), so the driver's compiler cannot choose another one
    const char *fma_env = std::getenv("PS2_VK_FMA");
    fma = fma_env ? std::atoi(fma_env) != 0 : true;
    for (std::size_t i = 0; i < ps2::gs::gpu::vk::kSpirvVariantCount; ++i) {
        const ps2::gs::gpu::vk::SpirvVariant &v = ps2::gs::gpu::vk::kSpirvVariants[i];
        if (v.wgsl_hash == hash && v.scale == scale && v.tile == tile && v.explicit_fma == fma) return &v;
    }
    std::fprintf(stderr, "[vulkan] no SPIR-V built for this shader with SCALE=%u TILE=%u EXPLICIT_FMA=%d (scripts/wgsl_spirv.py makes them)\n", scale, tile,
                 fma ? 1 : 0);
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Instance and adapter

extern "C" {

WGPUInstance wgpuCreateInstance(WGPUInstanceDescriptor const *) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    if (!load_global()) {
        std::fprintf(stderr, "[vulkan] no Vulkan library\n");
        return nullptr;
    }
    std::uint32_t version = VK_API_VERSION_1_0;
#ifdef __SWITCH__
    version = VK_API_VERSION_1_3;
#else
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
#endif
    if (version < VK_API_VERSION_1_2) {
        std::fprintf(stderr, "[vulkan] Vulkan 1.2 is needed (timeline semaphores), this system has %u.%u\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version));
        return nullptr;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ps2run";
    app.pEngineName = "ps2-native-recomp";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (const char *v = std::getenv("PS2_VK_VALIDATE"); v && v[0] == '1') {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ci, nullptr, &instance);
    if (r != VK_SUCCESS && ci.enabledLayerCount) {
        std::fprintf(stderr, "[vulkan] no validation layer\n");
        ci.enabledLayerCount = 0;
        r = vkCreateInstance(&ci, nullptr, &instance);
    }
    if (r != VK_SUCCESS) {
        log_vk("vkCreateInstance", r);
        return nullptr;
    }
    load_instance(instance);
    auto *i = new WGPUInstanceImpl;
    i->instance = instance;
    return i;
}

void wgpuInstanceRelease(WGPUInstance instance) {
    if (!instance) return;
    vkDestroyInstance(instance->instance, nullptr);
    delete instance;
}

WGPUFuture wgpuInstanceRequestAdapter(WGPUInstance instance, WGPURequestAdapterOptions const *options, WGPURequestAdapterCallbackInfo info) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count) vkEnumeratePhysicalDevices(instance->instance, &count, devices.data());
    WGPUAdapter best = nullptr;
    int best_score = -1;
    const bool low_power = options && options->powerPreference == WGPUPowerPreference_LowPower;
    const char *pick = std::getenv("PS2_VK_DEVICE");
    for (std::uint32_t n = 0; n < count; ++n) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[n], &p);
        if (p.apiVersion < VK_API_VERSION_1_2) continue;
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[n], &families, nullptr);
        std::vector<VkQueueFamilyProperties> fp(families);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[n], &families, fp.data());
        std::uint32_t family = UINT32_MAX;
        for (std::uint32_t f = 0; f < families; ++f)
            if (fp[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                family = f;
                if (fp[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;  // the main queue when there is one
            }
        if (family == UINT32_MAX) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? (low_power ? 2 : 3)
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? (low_power ? 3 : 2)
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
        if (pick && std::atoi(pick) == static_cast<int>(n)) score = 100;
        if (score > best_score) {
            if (!best) best = new WGPUAdapterImpl;
            best->instance = instance;
            best->phys = devices[n];
            best->props = p;
            best->family = family;
            best_score = score;
        }
    }
    if (info.callback) {
        if (best) info.callback(WGPURequestAdapterStatus_Success, best, text(""), info.userdata1, info.userdata2);
        else info.callback(WGPURequestAdapterStatus_Unavailable, nullptr, text("no Vulkan 1.2 device with a compute queue"), info.userdata1, info.userdata2);
    }
    return WGPUFuture{1};
}

void wgpuAdapterRelease(WGPUAdapter adapter) { delete adapter; }

WGPUStatus wgpuAdapterGetInfo(WGPUAdapter adapter, WGPUAdapterInfo *info) {
    const auto copy = [](const char *s) {
        const std::size_t n = std::strlen(s);
        char *p = static_cast<char *>(std::malloc(n + 1));
        std::memcpy(p, s, n + 1);
        return WGPUStringView{p, n};
    };
    char version[64];
    std::snprintf(version, sizeof version, "Vulkan %u.%u.%u, driver %u", VK_VERSION_MAJOR(adapter->props.apiVersion), VK_VERSION_MINOR(adapter->props.apiVersion),
                  VK_VERSION_PATCH(adapter->props.apiVersion), adapter->props.driverVersion);
    info->vendor = copy("");
    info->architecture = copy("");
    info->device = copy(adapter->props.deviceName);
    info->description = copy(version);
    info->backendType = WGPUBackendType_Vulkan;
    const VkPhysicalDeviceType t = adapter->props.deviceType;
    info->adapterType = t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? WGPUAdapterType_DiscreteGPU
                        : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? WGPUAdapterType_IntegratedGPU
                        : t == VK_PHYSICAL_DEVICE_TYPE_CPU ? WGPUAdapterType_CPU : WGPUAdapterType_Unknown;
    info->vendorID = adapter->props.vendorID;
    info->deviceID = adapter->props.deviceID;
    return WGPUStatus_Success;
}

void wgpuAdapterInfoFreeMembers(WGPUAdapterInfo info) {
    for (WGPUStringView v : {info.vendor, info.architecture, info.device, info.description}) std::free(const_cast<char *>(v.data));
}

WGPUStatus wgpuAdapterGetLimits(WGPUAdapter adapter, WGPULimits *limits) {
    limits->maxStorageBufferBindingSize = adapter->props.limits.maxStorageBufferRange;
    limits->maxBufferSize = std::max<std::uint64_t>(adapter->props.limits.maxStorageBufferRange, 1ull << 30);
    limits->maxUniformBufferBindingSize = adapter->props.limits.maxUniformBufferRange;
    return WGPUStatus_Success;
}

WGPUBool wgpuAdapterHasFeature(WGPUAdapter, WGPUFeatureName) { return false; }  // no timestamp queries

WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter adapter, WGPUDeviceDescriptor const *descriptor, WGPURequestDeviceCallbackInfo info) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    const auto fail = [&](const char *why) {
        if (info.callback) info.callback(WGPURequestDeviceStatus_Error, nullptr, text(why), info.userdata1, info.userdata2);
        return WGPUFuture{1};
    };
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(adapter->phys, &supported);
    VkPhysicalDeviceFeatures features{};
    features.robustBufferAccess = supported.robustBufferAccess;  // WebGPU keeps out-of-range accesses harmless; the shader expects that
    if (const char *r = std::getenv("PS2_VK_ROBUST"); r && r[0] == '0') features.robustBufferAccess = VK_FALSE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = adapter->family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f12;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.pEnabledFeatures = &features;
    VkDevice device = VK_NULL_HANDLE;
    if (VkResult r = vkCreateDevice(adapter->phys, &ci, nullptr, &device); r != VK_SUCCESS) {
        log_vk("vkCreateDevice", r);
        return fail("vkCreateDevice failed");
    }
    load_device(device);
    auto *d = new WGPUDeviceImpl;
    d->adapter = adapter;
    d->device = device;
    vkGetDeviceQueue(device, adapter->family, 0, &d->queue);
    vkGetPhysicalDeviceMemoryProperties(adapter->phys, &d->memory);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = adapter->family;
    vkCreateCommandPool(device, &pi, nullptr, &d->pool);
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    ti.initialValue = 0;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &ti;
    if (VkResult r = vkCreateSemaphore(device, &si, nullptr, &d->timeline); r != VK_SUCCESS) {
        log_vk("vkCreateSemaphore (timeline)", r);
        vkDestroyDevice(device, nullptr);
        delete d;
        return fail("no timeline semaphore");
    }
    if (descriptor) d->errors = descriptor->uncapturedErrorCallbackInfo;
    if (const char *n = std::getenv("PS2_VK_IN_FLIGHT"); n && std::atoi(n) > 0) d->max_in_flight = static_cast<std::uint64_t>(std::atoi(n));
    if (const char *p = std::getenv("PS2_VK_PROFILE"); p && p[0] == '1') {
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(adapter->phys, &families, nullptr);
        std::vector<VkQueueFamilyProperties> fp(families);
        vkGetPhysicalDeviceQueueFamilyProperties(adapter->phys, &families, fp.data());
        VkQueryPoolCreateInfo qi2{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi2.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi2.queryCount = kQueries;
        if (fp[adapter->family].timestampValidBits && vkCreateQueryPool(device, &qi2, nullptr, &d->queries) == VK_SUCCESS) {
            d->tick_ns = adapter->props.limits.timestampPeriod;
            std::fprintf(stderr, "[vk-profile] timing every dispatch (%.3f ns per tick)\n", d->tick_ns);
        } else {
            std::fprintf(stderr, "[vk-profile] this queue has no timestamps\n");
        }
    }
    d->queue_object = new WGPUQueueImpl{d};
    g_device = d;
    if (!features.robustBufferAccess) std::fprintf(stderr, "[vulkan] no robust buffer access on this device\n");
    if (info.callback) info.callback(WGPURequestDeviceStatus_Success, d, text(""), info.userdata1, info.userdata2);
    return WGPUFuture{1};
}

void wgpuDeviceRelease(WGPUDevice d) {
    if (!d) return;
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    vkDeviceWaitIdle(d->device);
    d->completed = d->submitted;
    process(d);
    for (Staging &s : d->staging) {
        vkDestroyBuffer(d->device, s.buffer, nullptr);
        vkFreeMemory(d->device, s.memory, nullptr);
    }
    for (VkDescriptorPool p : d->pools) vkDestroyDescriptorPool(d->device, p, nullptr);
    if (d->queries) vkDestroyQueryPool(d->device, d->queries, nullptr);
    vkDestroySemaphore(d->device, d->timeline, nullptr);
    vkDestroyCommandPool(d->device, d->pool, nullptr);
    vkDestroyDevice(d->device, nullptr);
    delete d->queue_object;
    if (g_device == d) g_device = nullptr;
    delete d;
}

WGPUQueue wgpuDeviceGetQueue(WGPUDevice d) { return d->queue_object; }

WGPUBool wgpuDevicePoll(WGPUDevice d, WGPUBool wait, WGPUSubmissionIndex const *) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    if (wait) {
        submit(d, {});
        wait_for(d, d->submitted);
    }
    process(d);
    return d->completed >= d->submitted;
}

void wgpuInstanceProcessEvents(WGPUInstance) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    if (g_device) process(g_device);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Buffers

WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice d, WGPUBufferDescriptor const *descriptor) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    auto *b = new WGPUBufferImpl;
    b->device = d;
    b->size = descriptor->size;
    const WGPUBufferUsage usage = descriptor->usage;
    VkBufferUsageFlags vk_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (usage & WGPUBufferUsage_Storage) vk_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (usage & WGPUBufferUsage_Uniform) vk_usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    const bool readable = usage & WGPUBufferUsage_MapRead;
    bool ok;
    if (readable)
        ok = make_vk_buffer(d, b->size, vk_usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                            b->buffer, b->memory, &b->map, &b->coherent);
    else
        ok = make_vk_buffer(d, b->size, vk_usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, b->buffer, b->memory, nullptr, nullptr);
    if (!ok) {
        delete b;
        report(d, "cannot create a buffer of " + std::to_string(descriptor->size) + " bytes");
        return nullptr;
    }
    // WebGPU buffers start out as zeros
    if (b->map) std::memset(b->map, 0, b->size);
    else vkCmdFillBuffer(upload_commands(d), b->buffer, 0, VK_WHOLE_SIZE, 0);
    return b;
}

void wgpuBufferRelease(WGPUBuffer b) {
    if (!b) return;
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    WGPUDevice d = b->device;
    const VkBuffer buffer = b->buffer;
    const VkDeviceMemory memory = b->memory;
    VkDevice device = d->device;
    // a map still waiting keeps its buffer: it is dropped from the queue
    for (auto it = d->maps.begin(); it != d->maps.end();)
        if (it->buffer == b) it = d->maps.erase(it);
        else ++it;
    delete b;
    bury(d, [device, buffer, memory] {
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    });
}

void wgpuQueueWriteBuffer(WGPUQueue queue, WGPUBuffer buffer, std::uint64_t offset, void const *data, std::size_t size) {
    if (!size || !buffer) return;
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    WGPUDevice d = queue->device;
    VkDeviceSize at = 0;
    Staging *s = staging_for(d, size, at);
    if (!s) {
        report(d, "no staging memory for a write of " + std::to_string(size) + " bytes");
        return;
    }
    std::memcpy(s->map + at, data, size);
    VkBufferCopy region{at, offset, size};
    vkCmdCopyBuffer(upload_commands(d), s->buffer, buffer->buffer, 1, &region);
}

WGPUFuture wgpuBufferMapAsync(WGPUBuffer b, WGPUMapMode, std::size_t, std::size_t, WGPUBufferMapCallbackInfo info) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    WGPUDevice d = b->device;
    if (!b->map) {
        if (info.callback) info.callback(WGPUMapAsyncStatus_Error, text("the buffer is not MapRead"), info.userdata1, info.userdata2);
        return WGPUFuture{1};
    }
    submit(d, {});  // queue writes waiting for a submission count as work before the map
    d->maps.push_back(MapRequest{b, d->submitted, info});
    return WGPUFuture{1};
}

void const *wgpuBufferGetConstMappedRange(WGPUBuffer b, std::size_t offset, std::size_t) {
    if (!b || !b->map || !b->mapped) return nullptr;
    return b->map + offset;
}

void wgpuBufferUnmap(WGPUBuffer b) {
    if (b) b->mapped = false;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Layouts, bind groups, shaders, pipelines

WGPUBindGroupLayout wgpuDeviceCreateBindGroupLayout(WGPUDevice d, WGPUBindGroupLayoutDescriptor const *descriptor) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto *l = new WGPUBindGroupLayoutImpl;
    l->device = d;
    for (std::size_t i = 0; i < descriptor->entryCount; ++i) {
        const WGPUBindGroupLayoutEntry &e = descriptor->entries[i];
        VkDescriptorSetLayoutBinding b{};
        b.binding = e.binding;
        b.descriptorCount = 1;
        if (e.buffer.type == WGPUBufferBindingType_Uniform) b.descriptorType = e.buffer.hasDynamicOffset ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (e.buffer.type == WGPUBufferBindingType_Storage || e.buffer.type == WGPUBufferBindingType_ReadOnlyStorage)
            b.descriptorType = e.buffer.hasDynamicOffset ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else {
            report(d, "only buffer bindings are supported");
            delete l;
            return nullptr;
        }
        if (e.visibility & WGPUShaderStage_Compute) b.stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
        if (e.visibility & WGPUShaderStage_Fragment) b.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (e.visibility & WGPUShaderStage_Vertex) b.stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        bindings.push_back(b);
        if (l->types.size() <= e.binding) l->types.resize(e.binding + 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        l->types[e.binding] = b.descriptorType;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<std::uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    if (VkResult r = vkCreateDescriptorSetLayout(d->device, &ci, nullptr, &l->layout); r != VK_SUCCESS) {
        log_vk("vkCreateDescriptorSetLayout", r);
        delete l;
        return nullptr;
    }
    return l;
}

void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout l) {
    if (!l) return;
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    VkDevice device = l->device->device;
    const VkDescriptorSetLayout layout = l->layout;
    bury(l->device, [device, layout] { vkDestroyDescriptorSetLayout(device, layout, nullptr); });
    delete l;
}

WGPUPipelineLayout wgpuDeviceCreatePipelineLayout(WGPUDevice d, WGPUPipelineLayoutDescriptor const *descriptor) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    std::vector<VkDescriptorSetLayout> sets;
    for (std::size_t i = 0; i < descriptor->bindGroupLayoutCount; ++i) sets.push_back(descriptor->bindGroupLayouts[i]->layout);
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = static_cast<std::uint32_t>(sets.size());
    ci.pSetLayouts = sets.data();
    auto *p = new WGPUPipelineLayoutImpl;
    p->device = d;
    if (VkResult r = vkCreatePipelineLayout(d->device, &ci, nullptr, &p->layout); r != VK_SUCCESS) {
        log_vk("vkCreatePipelineLayout", r);
        delete p;
        return nullptr;
    }
    return p;
}

void wgpuPipelineLayoutRelease(WGPUPipelineLayout p) {
    if (!p) return;
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    VkDevice device = p->device->device;
    const VkPipelineLayout layout = p->layout;
    bury(p->device, [device, layout] { vkDestroyPipelineLayout(device, layout, nullptr); });
    delete p;
}

WGPUBindGroup wgpuDeviceCreateBindGroup(WGPUDevice d, WGPUBindGroupDescriptor const *descriptor) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    auto *g = new WGPUBindGroupImpl;
    g->device = d;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &descriptor->layout->layout;
    for (VkDescriptorPool p : d->pools) {
        ai.descriptorPool = p;
        if (vkAllocateDescriptorSets(d->device, &ai, &g->set) == VK_SUCCESS) {
            g->pool = p;
            break;
        }
    }
    if (!g->pool) {
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 128},
                                              {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 256}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 128;
        pi.poolSizeCount = 4;
        pi.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(d->device, &pi, nullptr, &pool) != VK_SUCCESS) {
            delete g;
            report(d, "cannot create a descriptor pool");
            return nullptr;
        }
        d->pools.push_back(pool);
        ai.descriptorPool = pool;
        if (VkResult r = vkAllocateDescriptorSets(d->device, &ai, &g->set); r != VK_SUCCESS) {
            log_vk("vkAllocateDescriptorSets", r);
            delete g;
            return nullptr;
        }
        g->pool = pool;
    }
    std::vector<VkDescriptorBufferInfo> infos(descriptor->entryCount);
    std::vector<VkWriteDescriptorSet> writes(descriptor->entryCount);
    for (std::size_t i = 0; i < descriptor->entryCount; ++i) {
        const WGPUBindGroupEntry &e = descriptor->entries[i];
        infos[i].buffer = e.buffer->buffer;
        infos[i].offset = e.offset;
        infos[i].range = e.size == WGPU_WHOLE_SIZE ? VK_WHOLE_SIZE : e.size;
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = g->set;
        writes[i].dstBinding = e.binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = descriptor->layout->types[e.binding];
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(d->device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return g;
}

void wgpuBindGroupRelease(WGPUBindGroup g) {
    if (!g) return;
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    VkDevice device = g->device->device;
    const VkDescriptorPool pool = g->pool;
    const VkDescriptorSet set = g->set;
    bury(g->device, [device, pool, set] { vkFreeDescriptorSets(device, pool, 1, &set); });
    delete g;
}

WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice d, WGPUShaderModuleDescriptor const *descriptor) {
    for (const WGPUChainedStruct *c = descriptor->nextInChain; c; c = c->next)
        if (c->sType == WGPUSType_ShaderSourceWGSL) {
            auto *m = new WGPUShaderModuleImpl;
            m->hash = wgsl_hash(reinterpret_cast<const WGPUShaderSourceWGSL *>(c)->code);
            return m;
        }
    report(d, "a shader module needs WGSL source");
    return nullptr;
}

void wgpuShaderModuleRelease(WGPUShaderModule m) { delete m; }

WGPUComputePipeline wgpuDeviceCreateComputePipeline(WGPUDevice d, WGPUComputePipelineDescriptor const *descriptor) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    const WGPUComputeState &cs = descriptor->compute;
    const ps2::gs::gpu::vk::SpirvVariant *v = find_variant(cs.module->hash, cs.constants, cs.constantCount);
    if (!v) return nullptr;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = v->word_count * 4;
    mi.pCode = v->words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(d->device, &mi, nullptr, &module); r != VK_SUCCESS) {
        log_vk("vkCreateShaderModule", r);
        return nullptr;
    }
    const std::string entry = str(cs.entryPoint);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = entry.c_str();
    ci.layout = descriptor->layout->layout;
    auto *p = new WGPUComputePipelineImpl;
    p->device = d;
    p->layout = descriptor->layout->layout;
    p->name = entry;
    VkResult r = VK_ERROR_UNKNOWN;
    on_big_stack([&] { r = vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipeline); });
    vkDestroyShaderModule(d->device, module, nullptr);
    if (r != VK_SUCCESS) {
        log_vk(("vkCreateComputePipelines (" + entry + ")").c_str(), r);
        delete p;
        return nullptr;
    }
    return p;
}

void wgpuComputePipelineRelease(WGPUComputePipeline p) {
    if (!p) return;
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    VkDevice device = p->device->device;
    const VkPipeline pipeline = p->pipeline;
    bury(p->device, [device, pipeline] { vkDestroyPipeline(device, pipeline, nullptr); });
    delete p;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Commands

WGPUCommandEncoder wgpuDeviceCreateCommandEncoder(WGPUDevice d, WGPUCommandEncoderDescriptor const *) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    auto *e = new WGPUCommandEncoderImpl;
    e->device = d;
    e->cmd = begin_commands(d);
    time_begin(d, e->cmd);
    return e;
}

void wgpuCommandEncoderRelease(WGPUCommandEncoder e) { delete e; }

WGPUComputePassEncoder wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder e, WGPUComputePassDescriptor const *) {
    auto *p = new WGPUComputePassEncoderImpl;
    p->encoder = e;
    return p;
}

void wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder p, WGPUComputePipeline pipeline) {
    vkCmdBindPipeline(p->encoder->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    p->layout = pipeline->layout;
    p->name = pipeline->name.c_str();
}

void wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder p, std::uint32_t index, WGPUBindGroup group, std::size_t count, std::uint32_t const *offsets) {
    vkCmdBindDescriptorSets(p->encoder->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, index, 1, &group->set, static_cast<std::uint32_t>(count), offsets);
}

void wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder p, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!x || !y || !z) return;
    vkCmdDispatch(p->encoder->cmd, x, y, z);
    dispatch_barrier(p->encoder->cmd);  // WebGPU: each dispatch sees what the one before it wrote
    time_after(p->encoder->device, p->encoder->cmd, p->name);
}

void wgpuComputePassEncoderEnd(WGPUComputePassEncoder) {}
void wgpuComputePassEncoderRelease(WGPUComputePassEncoder p) { delete p; }

void wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer source, std::uint64_t source_offset, WGPUBuffer destination,
                                          std::uint64_t destination_offset, std::uint64_t size) {
    if (!size) return;
    VkBufferCopy region{source_offset, destination_offset, size};
    vkCmdCopyBuffer(e->cmd, source->buffer, destination->buffer, 1, &region);
    dispatch_barrier(e->cmd);
    time_after(e->device, e->cmd, "copy");
}

WGPUCommandBuffer wgpuCommandEncoderFinish(WGPUCommandEncoder e, WGPUCommandBufferDescriptor const *) {
    host_barrier(e->cmd);
    vkEndCommandBuffer(e->cmd);
    auto *c = new WGPUCommandBufferImpl;
    c->device = e->device;
    c->cmd = e->cmd;
    e->cmd = VK_NULL_HANDLE;
    return c;
}

void wgpuCommandBufferRelease(WGPUCommandBuffer c) { delete c; }

void wgpuQueueSubmit(WGPUQueue queue, std::size_t count, WGPUCommandBuffer const *commands) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    std::vector<VkCommandBuffer> list;
    for (std::size_t i = 0; i < count; ++i) list.push_back(commands[i]->cmd);
    submit(queue->device, list);
}

WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue queue, WGPUQueueWorkDoneCallbackInfo info) {
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    WGPUDevice d = queue->device;
    submit(d, {});
    d->work_done.push_back(WorkDone{d->submitted, info});
    return WGPUFuture{1};
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Not available here: gpu.cpp does without them (no timing, the picture is shown by the window code).

WGPUQuerySet wgpuDeviceCreateQuerySet(WGPUDevice, WGPUQuerySetDescriptor const *) { return nullptr; }
void wgpuCommandEncoderResolveQuerySet(WGPUCommandEncoder, WGPUQuerySet, std::uint32_t, std::uint32_t, WGPUBuffer, std::uint64_t) {}
WGPUSurface wgpuInstanceCreateSurface(WGPUInstance, WGPUSurfaceDescriptor const *) { return nullptr; }
void wgpuSurfaceRelease(WGPUSurface) {}
WGPUStatus wgpuSurfaceGetCapabilities(WGPUSurface, WGPUAdapter, WGPUSurfaceCapabilities *) { return WGPUStatus_Error; }
void wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities) {}
void wgpuSurfaceConfigure(WGPUSurface, WGPUSurfaceConfiguration const *) {}
void wgpuSurfaceGetCurrentTexture(WGPUSurface, WGPUSurfaceTexture *texture) {
    texture->texture = nullptr;
    texture->status = WGPUSurfaceGetCurrentTextureStatus_Error;
}
WGPUStatus wgpuSurfacePresent(WGPUSurface) { return WGPUStatus_Error; }
WGPUTextureView wgpuTextureCreateView(WGPUTexture, WGPUTextureViewDescriptor const *) { return nullptr; }
void wgpuTextureRelease(WGPUTexture) {}
void wgpuTextureViewRelease(WGPUTextureView) {}
WGPURenderPipeline wgpuDeviceCreateRenderPipeline(WGPUDevice, WGPURenderPipelineDescriptor const *) { return nullptr; }
WGPURenderPassEncoder wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder, WGPURenderPassDescriptor const *) { return nullptr; }
void wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder, float, float, float, float, float, float) {}
void wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder, WGPURenderPipeline) {}
void wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder, std::uint32_t, WGPUBindGroup, std::size_t, std::uint32_t const *) {}
void wgpuRenderPassEncoderDraw(WGPURenderPassEncoder, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void wgpuRenderPassEncoderEnd(WGPURenderPassEncoder) {}
void wgpuRenderPassEncoderRelease(WGPURenderPassEncoder) {}

}  // extern "C"
