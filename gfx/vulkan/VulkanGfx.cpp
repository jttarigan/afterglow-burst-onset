// Vulkan 1.1 backend of ag::gfx (Android; MoltenVK on macOS for development only, never data).
//
// - The Vulkan library is loaded at run time (VK_NO_PROTOTYPES). Loader, device setup and the
//   binding-table descriptor layouts follow NeonTide/gfx/vulkan/VulkanGfx.cpp (commit 3dc7266).
// - A list is one command buffer with a timestamp pair around its work, its own fence, and (if it
//   presents) its own acquire/render semaphores. GPU ticks become lab-clock ns through
//   VK_EXT_calibrated_timestamps (CLOCK_MONOTONIC) where the driver has it; otherwise only the
//   duration is known.
// - Presentation: FIFO swapchain at the fixed internal size (D7), pre-rotated to the panel's
//   orientation (NeonSwarm/android vk_renderer.cpp, commit fa7a954: without it Samsung panels fall
//   back to GPU composition). Present times come from VK_GOOGLE_display_timing where available.
// Vulkan structs are initialised with the {sType} idiom: remaining fields zero by design.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#endif
#include <vulkan/vulkan.h>

#include "ag/KernelBindings.gen.h"
#include "ag/gfx/Clock.h"
#include "ag/gfx/Gfx.h"
#include "ag/sha256.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include <dlfcn.h>

namespace ag::gfx {
namespace {

// ----------------------------------------------------------------------------- function table
#define AG_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define AG_VK_INSTANCE(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                           \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties)                                 \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)              \
    X(vkGetDeviceProcAddr)
#define AG_VK_INSTANCE_OPT(X)                                                                                     \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)  \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)
#define AG_VK_DEVICE(X)                                                                                           \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool)                         \
    X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)             \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences)   \
    X(vkWaitForFences) X(vkGetFenceStatus) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)  \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateShaderModule)              \
    X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)                       \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline)         \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)    \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch)                        \
    X(vkCmdPipelineBarrier) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool)                     \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCreateSemaphore) X(vkDestroySemaphore)                    \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass)                       \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)
#define AG_VK_DEVICE_OPT(X)                                                                                       \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)          \
    X(vkQueuePresentKHR) X(vkGetCalibratedTimestampsEXT) X(vkGetPastPresentationTimingGOOGLE)

struct Vk {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
#if defined(__ANDROID__)
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR = nullptr;
#endif
#define AG_DECL(f) PFN_##f f = nullptr;
    AG_VK_GLOBAL(AG_DECL)
    AG_VK_INSTANCE(AG_DECL)
    AG_VK_INSTANCE_OPT(AG_DECL)
    AG_VK_DEVICE(AG_DECL)
    AG_VK_DEVICE_OPT(AG_DECL)
#undef AG_DECL
};

void* open_library(std::string& which) {
    std::vector<std::string> names;
    if (const char* env = std::getenv("AG_VULKAN_LIBRARY"); env != nullptr && *env != 0) names.emplace_back(env);
#if defined(AG_VULKAN_LIBRARY_PATH)
    names.emplace_back(AG_VULKAN_LIBRARY_PATH);
#endif
#if defined(__ANDROID__)
    names.emplace_back("libvulkan.so");
#elif defined(__APPLE__)
    names.emplace_back("libvulkan.1.dylib");
    names.emplace_back("libMoltenVK.dylib");
#else
    names.emplace_back("libvulkan.so.1");
#endif
    for (const auto& n : names) {
        if (void* h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL); h != nullptr) {
            which = n;
            return h;
        }
    }
    return nullptr;
}

std::string version_string(u32 v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

const gen::KernelInfo* find_kernel(const char* name) {
    for (const auto& k : gen::kKernels) {
        if (std::strcmp(k.name, name) == 0) return &k;
    }
    return nullptr;
}

bool read_file(const std::string& path, std::vector<u8>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    u8 buf[1 << 15];
    for (;;) {
        const usize n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

constexpr u32 kSlots = 24;          // lists in flight (a frame uses 2-4)
constexpr i64 kPresentWaitNs = 250000000;

class VulkanGfx final : public Gfx {
public:
    ~VulkanGfx() override { shutdown(); }

    bool init(const InitDesc& desc, std::string& err) override {
        kernelDir_ = desc.kernelDir;
        wantW_ = desc.width;
        wantH_ = desc.height;
        std::string libName;
        lib_ = open_library(libName);
        if (lib_ == nullptr) return fail(err, "Vulkan library not found");
        vk_.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib_, "vkGetInstanceProcAddr"));
        if (vk_.vkGetInstanceProcAddr == nullptr) return fail(err, libName + ": no vkGetInstanceProcAddr");
#define AG_LOAD_G(f) vk_.f = reinterpret_cast<PFN_##f>(vk_.vkGetInstanceProcAddr(VK_NULL_HANDLE, #f));
        AG_VK_GLOBAL(AG_LOAD_G)
#undef AG_LOAD_G
        vk_.vkEnumerateInstanceVersion =
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vk_.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        u32 loaderVersion = VK_API_VERSION_1_0;
        if (vk_.vkEnumerateInstanceVersion != nullptr) vk_.vkEnumerateInstanceVersion(&loaderVersion);
        if (loaderVersion < VK_API_VERSION_1_1) return fail(err, "Vulkan 1.1 required (instance " + version_string(loaderVersion) + ")");

        const bool wantSurface = desc.surface == SurfaceKind::AndroidWindow && desc.nativeSurface != nullptr;
#if !defined(__ANDROID__)
        if (desc.surface != SurfaceKind::None) return fail(err, "this Vulkan build presents only to an Android window");
#endif
        u32 n = 0;
        vk_.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> iexts(n);
        vk_.vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
        auto hasIExt = [&](const char* name) {
            return std::any_of(iexts.begin(), iexts.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        std::vector<const char*> iext;
        VkInstanceCreateFlags iflags = 0;
        if (hasIExt("VK_KHR_portability_enumeration")) {
            iext.push_back("VK_KHR_portability_enumeration");
            iflags |= 0x00000001;   // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
        }
        if (wantSurface) {
            iext.push_back("VK_KHR_surface");
            iext.push_back("VK_KHR_android_surface");
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Afterglow";
        app.pEngineName = "Afterglow";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.flags = iflags;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = static_cast<u32>(iext.size());
        ici.ppEnabledExtensionNames = iext.data();
        if (VkResult r = vk_.vkCreateInstance(&ici, nullptr, &instance_); r != VK_SUCCESS) {
            return fail(err, "vkCreateInstance failed (" + std::to_string(r) + ")");
        }
#define AG_LOAD_I(f) vk_.f = reinterpret_cast<PFN_##f>(vk_.vkGetInstanceProcAddr(instance_, #f));
        AG_VK_INSTANCE(AG_LOAD_I)
        AG_VK_INSTANCE_OPT(AG_LOAD_I)
#undef AG_LOAD_I

        vk_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        if (n == 0) return fail(err, "no Vulkan physical device");
        std::vector<VkPhysicalDevice> pds(n);
        vk_.vkEnumeratePhysicalDevices(instance_, &n, pds.data());
        phys_ = pds[0];
        vk_.vkGetPhysicalDeviceProperties(phys_, &props_);
        if (props_.apiVersion < VK_API_VERSION_1_1) {
            return fail(err, std::string(props_.deviceName) + " supports only Vulkan " + version_string(props_.apiVersion));
        }
        vk_.vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);

#if defined(__ANDROID__)
        if (wantSurface) {
            window_ = static_cast<ANativeWindow*>(desc.nativeSurface);
            vk_.vkCreateAndroidSurfaceKHR =
                reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(vk_.vkGetInstanceProcAddr(instance_, "vkCreateAndroidSurfaceKHR"));
            VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            sci.window = window_;
            if (vk_.vkCreateAndroidSurfaceKHR == nullptr ||
                vk_.vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_) != VK_SUCCESS) {
                return fail(err, "vkCreateAndroidSurfaceKHR failed");
            }
        }
#endif

        // Extensions.
        vk_.vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> dexts(n);
        vk_.vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, dexts.data());
        auto hasExt = [&](const char* name) {
            return std::any_of(dexts.begin(), dexts.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        std::vector<const char*> dext;
        if (hasExt("VK_KHR_portability_subset")) dext.push_back("VK_KHR_portability_subset");
        if (surface_ != VK_NULL_HANDLE) {
            if (!hasExt("VK_KHR_swapchain")) return fail(err, "VK_KHR_swapchain missing");
            dext.push_back("VK_KHR_swapchain");
            displayTiming_ = hasExt("VK_GOOGLE_display_timing");
            if (displayTiming_) dext.push_back("VK_GOOGLE_display_timing");
        }
        calibrated_ = hasExt("VK_EXT_calibrated_timestamps") && vk_.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT != nullptr;
        if (calibrated_) {
            u32 dn = 0;
            vk_.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(phys_, &dn, nullptr);
            std::vector<VkTimeDomainEXT> doms(dn);
            vk_.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(phys_, &dn, doms.data());
            const bool dev = std::find(doms.begin(), doms.end(), VK_TIME_DOMAIN_DEVICE_EXT) != doms.end();
            const bool mono = std::find(doms.begin(), doms.end(), VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) != doms.end();
            calibrated_ = dev && mono;
            if (calibrated_) dext.push_back("VK_EXT_calibrated_timestamps");
        }

        // Queue: graphics + compute (+ present).
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qf(n);
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, qf.data());
        family_ = UINT32_MAX;
        for (u32 i = 0; i < n && family_ == UINT32_MAX; ++i) {
            const VkQueueFlags want = VK_QUEUE_COMPUTE_BIT | (surface_ != VK_NULL_HANDLE ? VK_QUEUE_GRAPHICS_BIT : 0u);
            if ((qf[i].queueFlags & want) != want) continue;
            if (surface_ != VK_NULL_HANDLE) {
                VkBool32 ok = VK_FALSE;
                vk_.vkGetPhysicalDeviceSurfaceSupportKHR(phys_, i, surface_, &ok);
                if (!ok) continue;
            }
            family_ = i;
        }
        if (family_ == UINT32_MAX) return fail(err, "no queue family with compute (+ graphics and present)");
        timestampBits_ = qf[family_].timestampValidBits;
        if (timestampBits_ == 0) return fail(err, "the queue has no timestamps (docs/03 §6 needs GPU timestamps)");
        tsMask_ = timestampBits_ >= 64 ? ~u64{0} : ((u64{1} << timestampBits_) - 1);

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<u32>(dext.size());
        dci.ppEnabledExtensionNames = dext.data();
        if (VkResult r = vk_.vkCreateDevice(phys_, &dci, nullptr, &device_); r != VK_SUCCESS) {
            return fail(err, "vkCreateDevice failed (" + std::to_string(r) + ")");
        }
#define AG_LOAD_D(f) vk_.f = reinterpret_cast<PFN_##f>(vk_.vkGetDeviceProcAddr(device_, #f));
        AG_VK_DEVICE(AG_LOAD_D)
        AG_VK_DEVICE_OPT(AG_LOAD_D)
#undef AG_LOAD_D
        vk_.vkGetDeviceQueue(device_, family_, 0, &queue_);
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = family_;
        if (vk_.vkCreateCommandPool(device_, &cpi, nullptr, &cmdPool_) != VK_SUCCESS) return fail(err, "vkCreateCommandPool failed");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 128;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (vk_.vkCreateDescriptorPool(device_, &dpi, nullptr, &descPool_) != VK_SUCCESS) return fail(err, "descriptor pool");
        for (Slot& s : slots_) {
            if (!createSlot(s)) return fail(err, "command slot creation failed");
        }
        if (surface_ != VK_NULL_HANDLE && !createSwapchain(err)) return false;
        if (calibrated_) calibrate();

        // MoltenVK translates SPIR-V to Metal: development only, never research data (validate.py).
        info_.api = libName.find("MoltenVK") != std::string::npos ? "Vulkan (MoltenVK, development only)" : "Vulkan";
        info_.deviceName = props_.deviceName;
        char raw[16];
        std::snprintf(raw, sizeof(raw), "0x%08x", props_.driverVersion);
        info_.driverVersion = version_string(props_.driverVersion) + " (raw " + raw + ")";
        auto& p = info_.props;
        char hex[16];
        p.emplace_back("vulkan_library", libName);
        p.emplace_back("vulkan_api_version", version_string(props_.apiVersion) + " (instance " + version_string(loaderVersion) + ")");
        std::snprintf(hex, sizeof(hex), "0x%04x", props_.vendorID);
        p.emplace_back("vendor_id", hex);
        std::snprintf(hex, sizeof(hex), "0x%04x", props_.deviceID);
        p.emplace_back("device_id", hex);
        char tp[48];
        std::snprintf(tp, sizeof(tp), "%.6f", static_cast<double>(props_.limits.timestampPeriod));
        p.emplace_back("timestamp_period_ns", tp);
        p.emplace_back("timestamp_valid_bits", std::to_string(timestampBits_));
        p.emplace_back("gpu_time_domain", calibrated_ ? "VK_EXT_calibrated_timestamps -> CLOCK_MONOTONIC, correlated"
                                                      : "durations only (no VK_EXT_calibrated_timestamps)");
        p.emplace_back("present_timing", displayTiming_ ? "VK_GOOGLE_display_timing" : "n/a: VK_GOOGLE_display_timing unsupported");
        if (surface_ != VK_NULL_HANDLE) {
            p.emplace_back("swapchain", std::to_string(swapExtent_.width) + "x" + std::to_string(swapExtent_.height) +
                                            " FIFO, pre-rotation " + std::to_string(preRotate_ * 90) + " deg, images " +
                                            std::to_string(swapImages_.size()));
        }
        return true;
    }

    const DeviceInfo& deviceInfo() const override { return info_; }

    void resize(u32 width, u32 height) override {
        if (width > 0 && height > 0) {
            wantW_ = width;
            wantH_ = height;
        }
        needRecreate_ = true;
    }

    // ------------------------------------------------------------------ resources
    BufferHandle createBuffer(u64 bytes, Memory memory, const char* name) override {
        Buf b{};
        b.bytes = bytes;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vk_.vkCreateBuffer(device_, &bi, nullptr, &b.buf) != VK_SUCCESS) {
            error(std::string("vkCreateBuffer failed: ") + name);
            return {};
        }
        VkMemoryRequirements mr;
        vk_.vkGetBufferMemoryRequirements(device_, b.buf, &mr);
        const VkMemoryPropertyFlags want = memory == Memory::HostShared
                                               ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const u32 type = memoryType(mr.memoryTypeBits, want);
        if (type == UINT32_MAX) {
            error(std::string("no memory type for ") + name);
            return {};
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (vk_.vkAllocateMemory(device_, &ai, nullptr, &b.mem) != VK_SUCCESS ||
            vk_.vkBindBufferMemory(device_, b.buf, b.mem, 0) != VK_SUCCESS) {
            error(std::string("buffer memory failed: ") + name);
            return {};
        }
        if (memory == Memory::HostShared) vk_.vkMapMemory(device_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        bufs_.push_back(b);
        return {static_cast<u32>(bufs_.size())};
    }

    void* map(BufferHandle b) override { return b.valid() && b.id <= bufs_.size() ? bufs_[b.id - 1].mapped : nullptr; }

    PipelineHandle createPipeline(const char* kernel, std::string& err) override {
        const gen::KernelInfo* ki = find_kernel(kernel);
        if (ki == nullptr) {
            err = std::string("kernel not in the binding table: ") + kernel;
            return {};
        }
        const std::string path = kernelDir_ + "/spirv/" + kernel + ".spv";
        std::vector<u8> code;
        if (!read_file(path, code) || code.size() % 4 != 0) {
            err = "cannot read " + path;
            return {};
        }
        Pipe pp{};
        pp.info = ki;
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (u32 i = 0; i < ki->bindingCount; ++i) {
            const gen::KernelBinding& kb = ki->bindings[i];
            if (kb.kind == gen::BindKind::PushConstants) continue;
            if (kb.kind != gen::BindKind::Buffer && kb.kind != gen::BindKind::RWBuffer) {
                err = std::string(kernel) + ": only buffer bindings are supported in M1";
                return {};
            }
            VkDescriptorSetLayoutBinding b{};
            b.binding = kb.vkBinding;
            b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            lb.push_back(b);
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = static_cast<u32>(lb.size());
        dl.pBindings = lb.data();
        vk_.vkCreateDescriptorSetLayout(device_, &dl, nullptr, &pp.dsl);
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, ki->pushBytes};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &pp.dsl;
        pl.pushConstantRangeCount = ki->pushBytes > 0 ? 1u : 0u;
        pl.pPushConstantRanges = &pr;
        vk_.vkCreatePipelineLayout(device_, &pl, nullptr, &pp.layout);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = code.size();
        smi.pCode = reinterpret_cast<const u32*>(code.data());
        VkShaderModule sm = VK_NULL_HANDLE;
        if (vk_.vkCreateShaderModule(device_, &smi, nullptr, &sm) != VK_SUCCESS) {
            err = "vkCreateShaderModule failed: " + path;
            return {};
        }
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = sm;
        cpi.stage.pName = kernel;
        cpi.layout = pp.layout;
        const VkResult r = vk_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pp.pipe);
        vk_.vkDestroyShaderModule(device_, sm, nullptr);
        if (r != VK_SUCCESS) {
            err = std::string("vkCreateComputePipelines failed for ") + kernel;
            return {};
        }
        pipes_.push_back(pp);
        pipeInfos_.push_back({kernel, path, sha256_file_hex(path)});
        return {static_cast<u32>(pipes_.size())};
    }

    const std::vector<PipelineInfo>& pipelines() const override { return pipeInfos_; }

    // ------------------------------------------------------------------ recording
    void begin() override {
        if (cur_ != nullptr) {
            error("begin() while a list is open");
            return;
        }
        cur_ = freeSlot();
        Slot& s = *cur_;
        s.presents = false;
        vk_.vkResetCommandBuffer(s.cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.vkBeginCommandBuffer(s.cb, &bi);
        vk_.vkCmdResetQueryPool(s.cb, s.qp, 0, 2);
        // Lists run strictly one after another (D41): a full barrier against everything earlier on
        // the queue, and the start timestamp at ALL_COMMANDS, i.e. once that earlier work is done.
        VkMemoryBarrier full{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        full.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        full.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vk_.vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &full,
                                 0, nullptr, 0, nullptr);
        vk_.vkCmdWriteTimestamp(s.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s.qp, 0);
    }

    void dispatch(PipelineHandle p, const Bind* binds, u32 bindCount, const void* push, u32 pushBytes, u32 groupsX) override {
        if (cur_ == nullptr || !p.valid() || p.id > pipes_.size()) {
            error("dispatch outside a list or with an invalid pipeline");
            return;
        }
        const Pipe& pp = pipes_[p.id - 1];
        const gen::KernelInfo& ki = *pp.info;
        if (ki.pushBytes != pushBytes || (pushBytes > 0 && push == nullptr)) {
            error(std::string(ki.name) + ": push constants missing or wrong size");
            return;
        }
        // Descriptor sets are cached per (pipeline, buffers): their contents never change.
        std::vector<u32> key{p.id};
        std::vector<std::pair<u32, VkBuffer>> wb;
        for (u32 i = 0; i < ki.bindingCount; ++i) {
            const gen::KernelBinding& kb = ki.bindings[i];
            if (kb.kind == gen::BindKind::PushConstants) continue;
            const Bind* bnd = nullptr;
            for (u32 j = 0; j < bindCount; ++j) {
                if (std::strcmp(binds[j].name, kb.name) == 0) bnd = &binds[j];
            }
            if (bnd == nullptr || !bnd->buffer.valid() || bnd->buffer.id > bufs_.size()) {
                error(std::string(ki.name) + ": parameter '" + kb.name + "' not bound");
                return;
            }
            key.push_back(bnd->buffer.id);
            wb.emplace_back(kb.vkBinding, bufs_[bnd->buffer.id - 1].buf);
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (auto it = sets_.find(key); it != sets_.end()) {
            set = it->second;
        } else {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = descPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &pp.dsl;
            if (vk_.vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
                error("descriptor set allocation failed");
                return;
            }
            std::vector<VkDescriptorBufferInfo> infos(wb.size());
            std::vector<VkWriteDescriptorSet> writes(wb.size());
            for (usize i = 0; i < wb.size(); ++i) {
                infos[i] = {wb[i].second, 0, VK_WHOLE_SIZE};
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = set;
                writes[i].dstBinding = wb[i].first;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &infos[i];
            }
            vk_.vkUpdateDescriptorSets(device_, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
            sets_[key] = set;
        }
        VkCommandBuffer cb = cur_->cb;
        vk_.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp.pipe);
        vk_.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp.layout, 0, 1, &set, 0, nullptr);
        if (pushBytes > 0) vk_.vkCmdPushConstants(cb, pp.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, push);
        vk_.vkCmdDispatch(cb, groupsX, 1, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        vk_.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
    }

    bool clearAndPresent(u8 r, u8 g, u8 b) override {
        if (cur_ == nullptr || surface_ == VK_NULL_HANDLE) return false;
        if (needRecreate_) {
            std::string err;
            if (!recreateSwapchain(err)) {
                error(err);
                return false;
            }
        }
        if (swapchain_ == VK_NULL_HANDLE) return false;
        u32 index = 0;
        const VkResult ar = vk_.vkAcquireNextImageKHR(device_, swapchain_, 1000000000ull, cur_->acquire, VK_NULL_HANDLE, &index);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            needRecreate_ = true;
            return false;
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return false;
        if (ar == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
        VkClearValue cv{};
        cv.color.float32[0] = static_cast<float>(r) / 255.0f;
        cv.color.float32[1] = static_cast<float>(g) / 255.0f;
        cv.color.float32[2] = static_cast<float>(b) / 255.0f;
        cv.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = renderPass_;
        rbi.framebuffer = framebuffers_[index];
        rbi.renderArea.extent = swapExtent_;
        rbi.clearValueCount = 1;
        rbi.pClearValues = &cv;
        vk_.vkCmdBeginRenderPass(cur_->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vk_.vkCmdEndRenderPass(cur_->cb);
        cur_->presents = true;
        cur_->imageIndex = index;
        return true;
    }

    ListId submit() override {
        if (cur_ == nullptr) {
            error("submit() without begin()");
            return 0;
        }
        Slot& s = *cur_;
        cur_ = nullptr;
        vk_.vkCmdWriteTimestamp(s.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.qp, 1);
        vk_.vkEndCommandBuffer(s.cb);
        vk_.vkResetFences(device_, 1, &s.fence);
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cb;
        if (s.presents) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &s.acquire;
            si.pWaitDstStageMask = &waitStage;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &s.render;
        }
        if (vk_.vkQueueSubmit(queue_, 1, &si, s.fence) != VK_SUCCESS) error("vkQueueSubmit failed");
        s.id = ++lastId_;
        s.inFlight = true;
        s.presentId = 0;
        if (s.presents) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &s.render;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain_;
            pi.pImageIndices = &s.imageIndex;
            VkPresentTimeGOOGLE pt{};
            VkPresentTimesInfoGOOGLE pti{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
            if (displayTiming_) {
                pt.presentID = ++lastPresentId_;
                pt.desiredPresentTime = 0;
                pti.swapchainCount = 1;
                pti.pTimes = &pt;
                pi.pNext = &pti;
                s.presentId = pt.presentID;
            }
            const VkResult pr = vk_.vkQueuePresentKHR(queue_, &pi);
            if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
            else if (pr != VK_SUCCESS) error("vkQueuePresentKHR failed (" + std::to_string(pr) + ")");
        }
        return s.id;
    }

    // ------------------------------------------------------------------ completion
    ListTiming timing(ListId id) override {
        harvest();
        pollPresentTimes();
        auto it = results_.find(id);
        if (it == results_.end()) {
            ListTiming t;
            for (const Slot& s : slots_) {
                if (s.inFlight && s.id == id) return t;   // still running
            }
            t.done = true;
            t.error = true;
            return t;
        }
        Result& r = it->second;
        if (r.presentId != 0 && r.t.presentedNs < 0) {
            if (auto pt = presentTimes_.find(r.presentId); pt != presentTimes_.end()) {
                r.t.presentedNs = pt->second;
            } else if (now_ns() - r.harvestedNs < kPresentWaitNs) {
                ListTiming pending;
                return pending;
            }
        }
        r.t.done = true;
        return r.t;
    }

    void wait(ListId id) override {
        for (Slot& s : slots_) {
            if (s.inFlight && s.id == id) vk_.vkWaitForFences(device_, 1, &s.fence, VK_TRUE, UINT64_MAX);
        }
        harvest();
    }

    void waitIdle() override {
        if (device_ != VK_NULL_HANDLE) vk_.vkDeviceWaitIdle(device_);
        harvest();
    }

    u64 errorCount() const override { return errors_; }
    const std::string& firstError() const override { return firstError_; }

private:
    struct Buf {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* mapped = nullptr;
        u64 bytes = 0;
    };
    struct Pipe {
        VkPipeline pipe = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        const gen::KernelInfo* info = nullptr;
    };
    struct Slot {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool qp = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE, render = VK_NULL_HANDLE;
        ListId id = 0;
        bool inFlight = false;
        bool presents = false;
        u32 imageIndex = 0;
        u32 presentId = 0;
    };
    struct Result {
        ListTiming t;
        u32 presentId = 0;
        i64 harvestedNs = 0;
    };

    bool fail(std::string& err, const std::string& msg) {
        err = msg;
        return false;
    }
    void error(const std::string& msg) {
        if (errors_++ == 0) firstError_ = msg;
    }

    u32 memoryType(u32 bits, VkMemoryPropertyFlags want) const {
        for (u32 i = 0; i < memProps_.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        return UINT32_MAX;
    }

    bool createSlot(Slot& s) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        return vk_.vkAllocateCommandBuffers(device_, &ai, &s.cb) == VK_SUCCESS &&
               vk_.vkCreateFence(device_, &fi, nullptr, &s.fence) == VK_SUCCESS &&
               vk_.vkCreateQueryPool(device_, &qi, nullptr, &s.qp) == VK_SUCCESS &&
               vk_.vkCreateSemaphore(device_, &si, nullptr, &s.acquire) == VK_SUCCESS &&
               vk_.vkCreateSemaphore(device_, &si, nullptr, &s.render) == VK_SUCCESS;
    }

    Slot* freeSlot() {
        harvest();
        for (Slot& s : slots_) {
            if (!s.inFlight) return &s;
        }
        // All in flight: wait for the oldest.
        Slot* oldest = &slots_[0];
        for (Slot& s : slots_) {
            if (s.id < oldest->id) oldest = &s;
        }
        vk_.vkWaitForFences(device_, 1, &oldest->fence, VK_TRUE, UINT64_MAX);
        harvest();
        return oldest;
    }

    /// Reads the timestamps of every finished list and frees its slot.
    void harvest() {
        for (Slot& s : slots_) {
            if (!s.inFlight || vk_.vkGetFenceStatus(device_, s.fence) != VK_SUCCESS) continue;
            Result r;
            r.presentId = s.presentId;
            r.harvestedNs = now_ns();
            u64 ts[2] = {0, 0};
            if (vk_.vkGetQueryPoolResults(device_, s.qp, 0, 2, sizeof(ts), ts, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const u64 dt = (ts[1] - ts[0]) & tsMask_;
                r.t.durationNs = static_cast<i64>(static_cast<double>(dt) * static_cast<double>(props_.limits.timestampPeriod));
                if (calibrated_ && calOk_) {
                    r.t.startNs = gpuToCpu(ts[0]);
                    r.t.endNs = r.t.startNs + r.t.durationNs;
                }
            } else {
                r.t.error = true;
                error("timestamp query unavailable");
            }
            results_[s.id] = r;
            order_.push_back(s.id);
            while (order_.size() > 1024) {
                results_.erase(order_.front());
                order_.pop_front();
            }
            s.inFlight = false;
        }
        if (calibrated_ && now_ns() - lastCal_ > 2000000000) calibrate();
    }

    void pollPresentTimes() {
        if (!displayTiming_ || swapchain_ == VK_NULL_HANDLE || vk_.vkGetPastPresentationTimingGOOGLE == nullptr) return;
        u32 n = 0;
        if (vk_.vkGetPastPresentationTimingGOOGLE(device_, swapchain_, &n, nullptr) != VK_SUCCESS || n == 0) return;
        std::vector<VkPastPresentationTimingGOOGLE> past(n);
        if (vk_.vkGetPastPresentationTimingGOOGLE(device_, swapchain_, &n, past.data()) != VK_SUCCESS) return;
        for (u32 i = 0; i < n; ++i) presentTimes_[past[i].presentID] = static_cast<i64>(past[i].actualPresentTime);
        while (presentTimes_.size() > 1024) presentTimes_.erase(presentTimes_.begin());
    }

    /// GPU ticks -> CLOCK_MONOTONIC ns (the Android lab clock), from the last calibration.
    i64 gpuToCpu(u64 ticks) const {
        const i64 dTicks = static_cast<i64>((ticks - calGpu_) & tsMask_);
        const i64 signedTicks = dTicks > static_cast<i64>(tsMask_ >> 1) ? dTicks - static_cast<i64>(tsMask_) - 1 : dTicks;
        return calCpu_ + static_cast<i64>(static_cast<double>(signedTicks) * static_cast<double>(props_.limits.timestampPeriod));
    }

    void calibrate() {
        lastCal_ = now_ns();
        if (vk_.vkGetCalibratedTimestampsEXT == nullptr) return;
        VkCalibratedTimestampInfoEXT infos[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT},
                                                 {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT}};
        infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
        infos[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
        u64 best[2] = {0, 0};
        u64 bestDev = UINT64_MAX;
        for (int i = 0; i < 5; ++i) {
            u64 ts[2];
            u64 dev = 0;
            if (vk_.vkGetCalibratedTimestampsEXT(device_, 2, infos, ts, &dev) != VK_SUCCESS) continue;
            if (dev < bestDev) {
                bestDev = dev;
                best[0] = ts[0];
                best[1] = ts[1];
            }
        }
        if (bestDev == UINT64_MAX) return;
        calGpu_ = best[0] & tsMask_;
        calCpu_ = static_cast<i64>(best[1]);
        calOk_ = true;
    }

    bool createSwapchain(std::string& err) {
        VkSurfaceCapabilitiesKHR caps;
        if (vk_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) return fail(err, "surface caps");
        u32 fn = 0;
        vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn);
        vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, fmts.data());
        if (fmts.empty()) return fail(err, "no surface formats");
        VkSurfaceFormatKHR chosen = fmts[0];
        for (const auto& f : fmts) {
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
                chosen = f;
                break;
            }
        }
        const VkSurfaceTransformFlagBitsKHR cur = caps.currentTransform;
        preRotate_ = !(caps.supportedTransforms & cur)                ? 0
                     : cur == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR  ? 1
                     : cur == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2
                     : cur == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3
                                                                      : 0;
        // Fixed internal resolution (D7), in the panel's orientation when pre-rotating; the
        // compositor scales it to the window.
        VkExtent2D ext{wantW_ > 0 ? wantW_ : caps.currentExtent.width, wantH_ > 0 ? wantH_ : caps.currentExtent.height};
        if (preRotate_ == 1 || preRotate_ == 3) std::swap(ext.width, ext.height);
        ext.width = std::clamp(ext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        swapExtent_ = ext;
        u32 count = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface_;
        sci.minImageCount = count;
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = ext;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = preRotate_ != 0 ? cur
                           : (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                                 : cur;
        sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                 : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = swapchain_;
        VkSwapchainKHR sc = VK_NULL_HANDLE;
        if (vk_.vkCreateSwapchainKHR(device_, &sci, nullptr, &sc) != VK_SUCCESS) return fail(err, "vkCreateSwapchainKHR failed");
        destroySwapchainViews();
        if (swapchain_ != VK_NULL_HANDLE) vk_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = sc;
        swapFormat_ = chosen.format;
        u32 in = 0;
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &in, nullptr);
        swapImages_.resize(in);
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &in, swapImages_.data());
        if (renderPass_ == VK_NULL_HANDLE) {
            VkAttachmentDescription att{};
            att.format = swapFormat_;
            att.samples = VK_SAMPLE_COUNT_1_BIT;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &ref;
            VkSubpassDependency dep{};
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            rpi.attachmentCount = 1;
            rpi.pAttachments = &att;
            rpi.subpassCount = 1;
            rpi.pSubpasses = &sub;
            rpi.dependencyCount = 1;
            rpi.pDependencies = &dep;
            if (vk_.vkCreateRenderPass(device_, &rpi, nullptr, &renderPass_) != VK_SUCCESS) return fail(err, "render pass");
        }
        for (VkImage img : swapImages_) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swapFormat_;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            vk_.vkCreateImageView(device_, &vi, nullptr, &view);
            views_.push_back(view);
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = renderPass_;
            fi.attachmentCount = 1;
            fi.pAttachments = &view;
            fi.width = swapExtent_.width;
            fi.height = swapExtent_.height;
            fi.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            vk_.vkCreateFramebuffer(device_, &fi, nullptr, &fb);
            framebuffers_.push_back(fb);
        }
        needRecreate_ = false;
        return true;
    }

    bool recreateSwapchain(std::string& err) {
        vk_.vkDeviceWaitIdle(device_);
        harvest();
        return createSwapchain(err);
    }

    void destroySwapchainViews() {
        for (VkFramebuffer fb : framebuffers_) vk_.vkDestroyFramebuffer(device_, fb, nullptr);
        for (VkImageView v : views_) vk_.vkDestroyImageView(device_, v, nullptr);
        framebuffers_.clear();
        views_.clear();
    }

    void shutdown() {
        if (device_ != VK_NULL_HANDLE) {
            vk_.vkDeviceWaitIdle(device_);
            destroySwapchainViews();
            if (swapchain_ != VK_NULL_HANDLE) vk_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            if (renderPass_ != VK_NULL_HANDLE) vk_.vkDestroyRenderPass(device_, renderPass_, nullptr);
            for (Slot& s : slots_) {
                if (s.fence != VK_NULL_HANDLE) vk_.vkDestroyFence(device_, s.fence, nullptr);
                if (s.qp != VK_NULL_HANDLE) vk_.vkDestroyQueryPool(device_, s.qp, nullptr);
                if (s.acquire != VK_NULL_HANDLE) vk_.vkDestroySemaphore(device_, s.acquire, nullptr);
                if (s.render != VK_NULL_HANDLE) vk_.vkDestroySemaphore(device_, s.render, nullptr);
            }
            for (Pipe& p : pipes_) {
                vk_.vkDestroyPipeline(device_, p.pipe, nullptr);
                vk_.vkDestroyPipelineLayout(device_, p.layout, nullptr);
                vk_.vkDestroyDescriptorSetLayout(device_, p.dsl, nullptr);
            }
            for (Buf& b : bufs_) {
                vk_.vkDestroyBuffer(device_, b.buf, nullptr);
                vk_.vkFreeMemory(device_, b.mem, nullptr);
            }
            if (descPool_ != VK_NULL_HANDLE) vk_.vkDestroyDescriptorPool(device_, descPool_, nullptr);
            if (cmdPool_ != VK_NULL_HANDLE) vk_.vkDestroyCommandPool(device_, cmdPool_, nullptr);
            vk_.vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE && vk_.vkDestroySurfaceKHR != nullptr) vk_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (instance_ != VK_NULL_HANDLE) vk_.vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    Vk vk_;
    void* lib_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 family_ = 0;
    u32 timestampBits_ = 0;
    u64 tsMask_ = ~u64{0};
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    std::array<Slot, kSlots> slots_{};
    Slot* cur_ = nullptr;
    ListId lastId_ = 0;
    std::map<ListId, Result> results_;
    std::deque<ListId> order_;
    std::map<std::vector<u32>, VkDescriptorSet> sets_;
    std::vector<Buf> bufs_;
    std::vector<Pipe> pipes_;
    std::vector<PipelineInfo> pipeInfos_;
    std::string kernelDir_;
    DeviceInfo info_;
    // presentation
#if defined(__ANDROID__)
    ANativeWindow* window_ = nullptr;
#endif
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent_{0, 0};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    u32 wantW_ = 0, wantH_ = 0;
    int preRotate_ = 0;
    bool needRecreate_ = false;
    bool displayTiming_ = false;
    u32 lastPresentId_ = 0;
    std::map<u32, i64> presentTimes_;
    // clock correlation
    bool calibrated_ = false;
    bool calOk_ = false;
    u64 calGpu_ = 0;
    i64 calCpu_ = 0;
    i64 lastCal_ = 0;
    // audit
    u64 errors_ = 0;
    std::string firstError_;
};

} // namespace

std::unique_ptr<Gfx> create_gfx() { return std::make_unique<VulkanGfx>(); }

} // namespace ag::gfx
