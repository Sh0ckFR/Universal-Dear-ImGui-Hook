#include "internal.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "imgui_impl_vulkan.h"

namespace uih::detail {
namespace {

#define UIH_VK_INSTANCE_FUNCTIONS(X)              \
    X(vkDestroyInstance)                          \
    X(vkEnumeratePhysicalDevices)                 \
    X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkEnumerateDeviceExtensionProperties)       \
    X(vkCreateDevice)

#define UIH_VK_DEVICE_FUNCTIONS(X)   \
    X(vkDestroyDevice)               \
    X(vkGetDeviceQueue)              \
    X(vkQueueWaitIdle)               \
    X(vkCreateRenderPass)            \
    X(vkDestroyRenderPass)           \
    X(vkCreateImageView)             \
    X(vkDestroyImageView)            \
    X(vkCreateFramebuffer)           \
    X(vkDestroyFramebuffer)          \
    X(vkCreateCommandPool)           \
    X(vkDestroyCommandPool)          \
    X(vkAllocateCommandBuffers)      \
    X(vkCreateFence)                 \
    X(vkDestroyFence)                \
    X(vkWaitForFences)               \
    X(vkResetFences)                 \
    X(vkCreateSemaphore)             \
    X(vkDestroySemaphore)            \
    X(vkBeginCommandBuffer)          \
    X(vkEndCommandBuffer)            \
    X(vkResetCommandBuffer)          \
    X(vkCmdBeginRenderPass)          \
    X(vkCmdEndRenderPass)            \
    X(vkQueueSubmit)                 \
    X(vkGetSwapchainImagesKHR)

struct InstanceFunctions {
#define UIH_VK_DECLARE(name) PFN_##name name = nullptr;
    UIH_VK_INSTANCE_FUNCTIONS(UIH_VK_DECLARE)
};

struct DeviceFunctions {
    UIH_VK_DEVICE_FUNCTIONS(UIH_VK_DECLARE)
#undef UIH_VK_DECLARE

    void Load(PFN_vkGetDeviceProcAddr getProcAddr, VkDevice device) {
#define UIH_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(getProcAddr(device, #name));
        UIH_VK_DEVICE_FUNCTIONS(UIH_VK_LOAD)
#undef UIH_VK_LOAD
    }
};

// Device-level entry points intercepted in every installable client driver.
enum HookId {
    kGetDeviceQueue,
    kGetDeviceQueue2,
    kCreateSwapchainKHR,
    kDestroySwapchainKHR,
    kAcquireNextImageKHR,
    kAcquireNextImage2KHR,
    kQueuePresentKHR,
    kDestroyDevice,
    kHookCount,
};

const char* const kHookNames[kHookCount] = {
    "vkGetDeviceQueue",
    "vkGetDeviceQueue2",
    "vkCreateSwapchainKHR",
    "vkDestroySwapchainKHR",
    "vkAcquireNextImageKHR",
    "vkAcquireNextImage2KHR",
    "vkQueuePresentKHR",
    "vkDestroyDevice",
};

struct Icd {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice dummyDevice = VK_NULL_HANDLE;
    DeviceFunctions fn;
    void* targets[kHookCount] = {};
    void* originals[kHookCount] = {};
};

struct SwapchainInfo {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    HWND window = nullptr;
};

struct QueueInfo {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
};

struct FrameRing {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
};

struct VulkanState {
    VkDevice device = VK_NULL_HANDLE;
    DeviceFunctions fn;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t imageCount = 0;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<FrameRing> frames;
    uint32_t frameIndex = 0;

    bool imguiReady = false;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> semaphores;
};

HMODULE g_module = nullptr;
PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_getDeviceProcAddr = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
InstanceFunctions g_vk;
std::vector<Icd> g_icds;

PFN_vkCreateDevice g_createDevice = nullptr;
PFN_vkCreateWin32SurfaceKHR g_createWin32Surface = nullptr;
PFN_vkDestroySurfaceKHR g_destroySurface = nullptr;

std::mutex g_mutex;
std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchains;
std::unordered_map<VkQueue, QueueInfo> g_queues;
std::unordered_map<VkDevice, VkPhysicalDevice> g_devices;
std::unordered_map<VkSurfaceKHR, HWND> g_surfaces;

VulkanState g_renderer;

// ICD lookup --------------------------------------------------------------

const Icd* IcdForDevice(VkDevice device) {
    if (g_icds.size() == 1 || device == VK_NULL_HANDLE)
        return g_icds.empty() ? nullptr : &g_icds[0];
    void* targets[kHookCount];
    for (int i = 0; i < kHookCount; ++i)
        targets[i] = reinterpret_cast<void*>(g_getDeviceProcAddr(device, kHookNames[i]));
    for (const Icd& icd : g_icds)
        if (std::equal(targets, targets + kHookCount, icd.targets))
            return &icd;
    Log("Vulkan device %p does not match any hooked driver", device);
    return &g_icds[0];
}

template <typename Fn>
Fn Original(const Icd* icd, HookId id) {
    return reinterpret_cast<Fn>(icd->originals[id]);
}

VkDevice DeviceForQueue(VkQueue queue, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto queueIt = g_queues.find(queue);
    if (queueIt != g_queues.end())
        return queueIt->second.device;
    auto swapchainIt = g_swapchains.find(swapchain);
    if (swapchainIt == g_swapchains.end())
        return VK_NULL_HANDLE;
    g_queues[queue] = {swapchainIt->second.device, UINT32_MAX};
    return swapchainIt->second.device;
}

// Renderer ---------------------------------------------------------------

void WaitForFrames() {
    VulkanState& r = g_renderer;
    for (FrameRing& frame : r.frames) {
        if (frame.pending)
            r.fn.vkWaitForFences(r.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
        frame.pending = false;
    }
}

void ReleaseSwapchainResources() {
    VulkanState& r = g_renderer;
    if (r.swapchain == VK_NULL_HANDLE)
        return;
    WaitForFrames();
    for (VkFramebuffer framebuffer : r.framebuffers)
        r.fn.vkDestroyFramebuffer(r.device, framebuffer, nullptr);
    for (VkImageView view : r.views)
        r.fn.vkDestroyImageView(r.device, view, nullptr);
    r.framebuffers.clear();
    r.views.clear();
    r.swapchain = VK_NULL_HANDLE;
}

void Teardown() {
    VulkanState& r = g_renderer;
    if (r.device == VK_NULL_HANDLE)
        return;
    WaitForFrames();
    if (r.queue)
        r.fn.vkQueueWaitIdle(r.queue);
    ReleaseSwapchainResources();
    if (r.imguiReady)
        ImGui_ImplVulkan_Shutdown();
    UnbindWindow();
    for (VkSemaphore semaphore : r.semaphores)
        r.fn.vkDestroySemaphore(r.device, semaphore, nullptr);
    for (FrameRing& frame : r.frames)
        r.fn.vkDestroyFence(r.device, frame.fence, nullptr);
    if (r.commandPool)
        r.fn.vkDestroyCommandPool(r.device, r.commandPool, nullptr);
    if (r.renderPass)
        r.fn.vkDestroyRenderPass(r.device, r.renderPass, nullptr);
    g_renderer = VulkanState{};
}

PFN_vkVoidFunction ImGuiLoader(const char* name, void* device) {
    if (PFN_vkVoidFunction fn = g_getDeviceProcAddr(static_cast<VkDevice>(device), name))
        return fn;
    return g_getInstanceProcAddr(g_instance, name);
}

uint32_t FindQueueFamily(VkDevice device, VkQueue queue, VkPhysicalDevice physicalDevice, const DeviceFunctions& fn) {
    uint32_t count = 0;
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());

    uint32_t fallback = UINT32_MAX;
    for (uint32_t family = 0; family < count; ++family) {
        if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        if (fallback == UINT32_MAX)
            fallback = family;
        for (uint32_t index = 0; index < families[family].queueCount; ++index) {
            VkQueue candidate = VK_NULL_HANDLE;
            fn.vkGetDeviceQueue(device, family, index, &candidate);
            if (candidate == queue)
                return family;
        }
    }
    Log("Vulkan present queue family unknown, assuming %u", fallback);
    return fallback;
}

bool CreateDeviceResources(VkDevice device, VkQueue queue, uint32_t family, VkPhysicalDevice physicalDevice, VkFormat format, uint32_t imageCount, HWND window) {
    VulkanState& r = g_renderer;
    r.device = device;
    r.fn.Load(g_getDeviceProcAddr, device);
    r.queue = queue;
    r.family = family;
    r.format = format;
    r.imageCount = imageCount;

    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = 0;
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;
    if (r.fn.vkCreateRenderPass(device, &renderPassInfo, nullptr, &r.renderPass) != VK_SUCCESS)
        return false;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    if (r.fn.vkCreateCommandPool(device, &poolInfo, nullptr, &r.commandPool) != VK_SUCCESS)
        return false;

    r.frames.resize(imageCount);
    std::vector<VkCommandBuffer> commandBuffers(imageCount);
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = r.commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = imageCount;
    if (r.fn.vkAllocateCommandBuffers(device, &allocateInfo, commandBuffers.data()) != VK_SUCCESS)
        return false;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    r.semaphores.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        r.frames[i].commandBuffer = commandBuffers[i];
        if (r.fn.vkCreateFence(device, &fenceInfo, nullptr, &r.frames[i].fence) != VK_SUCCESS ||
            r.fn.vkCreateSemaphore(device, &semaphoreInfo, nullptr, &r.semaphores[i]) != VK_SUCCESS)
            return false;
    }

    if (!BindWindow(window))
        return false;
    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, ImGuiLoader, device);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_0;
    info.Instance = g_instance;
    info.PhysicalDevice = physicalDevice;
    info.Device = device;
    info.QueueFamily = family;
    info.Queue = queue;
    info.DescriptorPoolSize = 16;
    info.MinImageCount = 2;
    info.ImageCount = imageCount;
    info.PipelineInfoMain.RenderPass = r.renderPass;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    r.imguiReady = ImGui_ImplVulkan_Init(&info);
    return r.imguiReady;
}

bool CreateSwapchainResources(VkSwapchainKHR swapchain, VkExtent2D extent) {
    VulkanState& r = g_renderer;
    uint32_t count = 0;
    if (r.fn.vkGetSwapchainImagesKHR(r.device, swapchain, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;
    std::vector<VkImage> images(count);
    if (r.fn.vkGetSwapchainImagesKHR(r.device, swapchain, &count, images.data()) != VK_SUCCESS)
        return false;

    r.swapchain = swapchain;
    r.extent = extent;
    r.views.resize(count);
    r.framebuffers.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = r.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (r.fn.vkCreateImageView(r.device, &viewInfo, nullptr, &r.views[i]) != VK_SUCCESS)
            return false;
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = r.renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &r.views[i];
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        if (r.fn.vkCreateFramebuffer(r.device, &framebufferInfo, nullptr, &r.framebuffers[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

VkExtent2D WindowExtent(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    return {static_cast<uint32_t>(std::max<LONG>(rect.right - rect.left, 1)), static_cast<uint32_t>(std::max<LONG>(rect.bottom - rect.top, 1))};
}

bool Setup(VkDevice device, VkQueue queue, VkSwapchainKHR swapchain) {
    SwapchainInfo swapchainInfo;
    QueueInfo queueInfo;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto swapchainIt = g_swapchains.find(swapchain);
        if (swapchainIt != g_swapchains.end())
            swapchainInfo = swapchainIt->second;
        auto queueIt = g_queues.find(queue);
        if (queueIt != g_queues.end())
            queueInfo = queueIt->second;
        auto deviceIt = g_devices.find(device);
        if (deviceIt != g_devices.end())
            physicalDevice = deviceIt->second;
    }
    if (physicalDevice == VK_NULL_HANDLE)
        physicalDevice = IcdForDevice(device)->physicalDevice;
    if (swapchainInfo.format == VK_FORMAT_UNDEFINED) {
        swapchainInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        Log("Vulkan swap chain %p was created before injection, assuming VK_FORMAT_B8G8R8A8_UNORM", swapchain);
    }
    if (!swapchainInfo.window)
        swapchainInfo.window = input::FindProcessWindow();
    if (swapchainInfo.extent.width == 0 || swapchainInfo.extent.height == 0)
        swapchainInfo.extent = WindowExtent(swapchainInfo.window);

    DeviceFunctions fn;
    fn.Load(g_getDeviceProcAddr, device);
    uint32_t imageCount = 0;
    if (fn.vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr) != VK_SUCCESS || imageCount == 0)
        return false;
    imageCount = std::max<uint32_t>(imageCount, 2);

    VulkanState& r = g_renderer;
    if (r.device && (r.device != device || r.queue != queue || r.format != swapchainInfo.format || r.imageCount != imageCount))
        Teardown();
    ReleaseSwapchainResources();

    if (!r.device) {
        if (!ClaimBackend(Backend::Vulkan))
            return false;
        uint32_t family = queueInfo.family != UINT32_MAX ? queueInfo.family : FindQueueFamily(device, queue, physicalDevice, fn);
        if (family == UINT32_MAX || !CreateDeviceResources(device, queue, family, physicalDevice, swapchainInfo.format, imageCount, swapchainInfo.window)) {
            Log("Vulkan renderer initialization failed");
            Teardown();
            return false;
        }
    }
    if (!CreateSwapchainResources(swapchain, swapchainInfo.extent)) {
        Log("Vulkan swap chain resource creation failed");
        ReleaseSwapchainResources();
        return false;
    }
    return true;
}

const VkPresentInfoKHR* Render(VkQueue queue, VkDevice device, const VkPresentInfoKHR* info, VkPresentInfoKHR& patched) {
    VkSwapchainKHR swapchain = info->pSwapchains[0];
    uint32_t imageIndex = info->pImageIndices[0];
    VulkanState& r = g_renderer;
    if ((r.swapchain != swapchain || r.device != device || r.queue != queue) && !Setup(device, queue, swapchain))
        return info;
    if (imageIndex >= r.framebuffers.size())
        return info;

    FrameRing& frame = r.frames[r.frameIndex];
    if (frame.pending) {
        r.fn.vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
        frame.pending = false;
    }
    r.fn.vkResetFences(device, 1, &frame.fence);

    ImGui_ImplVulkan_NewFrame();
    ImDrawData* drawData = Frame();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r.fn.vkResetCommandBuffer(frame.commandBuffer, 0);
    if (r.fn.vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS)
        return info;
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = r.renderPass;
    renderPassInfo.framebuffer = r.framebuffers[imageIndex];
    renderPassInfo.renderArea.extent = r.extent;
    r.fn.vkCmdBeginRenderPass(frame.commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, frame.commandBuffer);
    r.fn.vkCmdEndRenderPass(frame.commandBuffer);
    if (r.fn.vkEndCommandBuffer(frame.commandBuffer) != VK_SUCCESS)
        return info;

    std::vector<VkPipelineStageFlags> waitStages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = info->waitSemaphoreCount;
    submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &r.semaphores[imageIndex];
    if (r.fn.vkQueueSubmit(queue, 1, &submit, frame.fence) != VK_SUCCESS) {
        Log("vkQueueSubmit failed");
        return info;
    }
    frame.pending = true;
    r.frameIndex = (r.frameIndex + 1) % r.frames.size();

    patched = *info;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &r.semaphores[imageIndex];
    return &patched;
}

// Hooks ------------------------------------------------------------------

void RecordQueue(VkDevice device, uint32_t family, VkQueue queue) {
    if (queue == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queues[queue] = {device, family};
}

void VKAPI_CALL HookGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    ScopedCall call;
    Original<PFN_vkGetDeviceQueue>(IcdForDevice(device), kGetDeviceQueue)(device, family, index, queue);
    if (queue)
        RecordQueue(device, family, *queue);
}

void VKAPI_CALL HookGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    ScopedCall call;
    Original<PFN_vkGetDeviceQueue2>(IcdForDevice(device), kGetDeviceQueue2)(device, info, queue);
    if (queue && info)
        RecordQueue(device, info->queueFamilyIndex, *queue);
}

VkResult VKAPI_CALL HookCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    ScopedCall call;
    VkResult result = Original<PFN_vkCreateSwapchainKHR>(IcdForDevice(device), kCreateSwapchainKHR)(device, info, allocator, swapchain);
    if (result == VK_SUCCESS && info && swapchain) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto surface = g_surfaces.find(info->surface);
        g_swapchains[*swapchain] = {device, info->imageFormat, info->imageExtent, surface != g_surfaces.end() ? surface->second : nullptr};
    }
    return result;
}

void VKAPI_CALL HookDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) {
    ScopedCall call;
    if (swapchain != VK_NULL_HANDLE && g_renderer.swapchain == swapchain)
        ReleaseSwapchainResources();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_swapchains.erase(swapchain);
    }
    Original<PFN_vkDestroySwapchainKHR>(IcdForDevice(device), kDestroySwapchainKHR)(device, swapchain, allocator);
}

void RecordSwapchainDevice(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_swapchains[swapchain].device = device;
}

VkResult VKAPI_CALL HookAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    ScopedCall call;
    RecordSwapchainDevice(device, swapchain);
    return Original<PFN_vkAcquireNextImageKHR>(IcdForDevice(device), kAcquireNextImageKHR)(device, swapchain, timeout, semaphore, fence, index);
}

VkResult VKAPI_CALL HookAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* index) {
    ScopedCall call;
    if (info)
        RecordSwapchainDevice(device, info->swapchain);
    return Original<PFN_vkAcquireNextImage2KHR>(IcdForDevice(device), kAcquireNextImage2KHR)(device, info, index);
}

VkResult VKAPI_CALL HookQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    ScopedCall call;
    ForeignPresentScope presentScope;
    VkDevice device = VK_NULL_HANDLE;
    const VkPresentInfoKHR* forward = info;
    VkPresentInfoKHR patched;
    if (info && info->swapchainCount > 0 && info->pSwapchains && info->pImageIndices) {
        device = DeviceForQueue(queue, info->pSwapchains[0]);
        if (device != VK_NULL_HANDLE && ShouldRender(Backend::Vulkan))
            forward = Render(queue, device, info, patched);
    }
    VkResult result = Original<PFN_vkQueuePresentKHR>(IcdForDevice(device), kQueuePresentKHR)(queue, forward);
    ServiceTeardown(Backend::Vulkan, Teardown);
    return result;
}

void VKAPI_CALL HookDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    ScopedCall call;
    const Icd* icd = IcdForDevice(device);
    if (g_renderer.device == device)
        Teardown();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices.erase(device);
        for (auto it = g_queues.begin(); it != g_queues.end();)
            it = it->second.device == device ? g_queues.erase(it) : std::next(it);
        for (auto it = g_swapchains.begin(); it != g_swapchains.end();)
            it = it->second.device == device ? g_swapchains.erase(it) : std::next(it);
    }
    Original<PFN_vkDestroyDevice>(icd, kDestroyDevice)(device, allocator);
}

VkResult VKAPI_CALL HookCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device) {
    ScopedCall call;
    VkResult result = g_createDevice(physicalDevice, info, allocator, device);
    if (result == VK_SUCCESS && device) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices[*device] = physicalDevice;
    }
    return result;
}

VkResult VKAPI_CALL HookCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    ScopedCall call;
    VkResult result = g_createWin32Surface(instance, info, allocator, surface);
    if (result == VK_SUCCESS && info && surface) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_surfaces[*surface] = info->hwnd;
    }
    return result;
}

void VKAPI_CALL HookDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator) {
    ScopedCall call;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_surfaces.erase(surface);
    }
    g_destroySurface(instance, surface, allocator);
}

void* const kDetours[kHookCount] = {
    reinterpret_cast<void*>(HookGetDeviceQueue),
    reinterpret_cast<void*>(HookGetDeviceQueue2),
    reinterpret_cast<void*>(HookCreateSwapchainKHR),
    reinterpret_cast<void*>(HookDestroySwapchainKHR),
    reinterpret_cast<void*>(HookAcquireNextImageKHR),
    reinterpret_cast<void*>(HookAcquireNextImage2KHR),
    reinterpret_cast<void*>(HookQueuePresentKHR),
    reinterpret_cast<void*>(HookDestroyDevice),
};

// Installation -----------------------------------------------------------

bool CreateDummyInstance() {
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(g_getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance)
        return false;
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "uih";
    appInfo.apiVersion = VK_API_VERSION_1_0;
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &appInfo;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    VkResult result = createInstance(&info, nullptr, &g_instance);
    if (result != VK_SUCCESS) {
        Log("vkCreateInstance failed: %d", result);
        return false;
    }
#define UIH_VK_LOAD(name) g_vk.name = reinterpret_cast<PFN_##name>(g_getInstanceProcAddr(g_instance, #name));
    UIH_VK_INSTANCE_FUNCTIONS(UIH_VK_LOAD)
#undef UIH_VK_LOAD
    return true;
}

bool SupportsSwapchain(VkPhysicalDevice physicalDevice) {
    uint32_t count = 0;
    g_vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    g_vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data());
    for (const VkExtensionProperties& extension : extensions)
        if (strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
            return true;
    return false;
}

VkDevice CreateDummyDevice(VkPhysicalDevice physicalDevice) {
    if (!SupportsSwapchain(physicalDevice))
        return VK_NULL_HANDLE;
    uint32_t count = 0;
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        ++family;
    if (family == count)
        return VK_NULL_HANDLE;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    VkDevice device = VK_NULL_HANDLE;
    VkResult result = g_vk.vkCreateDevice(physicalDevice, &info, nullptr, &device);
    if (result != VK_SUCCESS)
        Log("Vulkan dummy device creation failed: %d", result);
    return device;
}

bool Install() {
    g_module = LoadLibraryW(L"vulkan-1.dll");
    g_getInstanceProcAddr = GetProc<PFN_vkGetInstanceProcAddr>(g_module, "vkGetInstanceProcAddr");
    g_getDeviceProcAddr = GetProc<PFN_vkGetDeviceProcAddr>(g_module, "vkGetDeviceProcAddr");
    if (!g_getInstanceProcAddr || !g_getDeviceProcAddr || !CreateDummyInstance())
        return false;

    uint32_t count = 0;
    g_vk.vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(count);
    g_vk.vkEnumeratePhysicalDevices(g_instance, &count, physicalDevices.data());

    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        Icd icd;
        icd.physicalDevice = physicalDevice;
        icd.dummyDevice = CreateDummyDevice(physicalDevice);
        if (icd.dummyDevice == VK_NULL_HANDLE)
            continue;
        icd.fn.Load(g_getDeviceProcAddr, icd.dummyDevice);
        for (int i = 0; i < kHookCount; ++i)
            icd.targets[i] = reinterpret_cast<void*>(g_getDeviceProcAddr(icd.dummyDevice, kHookNames[i]));
        bool duplicate = false;
        for (Icd& existing : g_icds) {
            if (std::equal(icd.targets, icd.targets + kHookCount, existing.targets)) {
                icd.fn.vkDestroyDevice(icd.dummyDevice, nullptr);
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            g_icds.push_back(icd);
    }
    if (g_icds.empty()) {
        Log("No Vulkan device supports presentation");
        return false;
    }

    bool hooked = false;
    for (Icd& icd : g_icds) {
        for (int i = 0; i < kHookCount; ++i) {
            if (!icd.targets[i])
                continue;
            for (const Icd& other : g_icds) {
                if (&other != &icd && other.targets[i] == icd.targets[i] && other.originals[i]) {
                    icd.originals[i] = other.originals[i];
                    break;
                }
            }
            if (!icd.originals[i])
                hooked |= CreateHook(kHookNames[i], icd.targets[i], kDetours[i], &icd.originals[i]);
        }
    }

    struct InstanceHook {
        const char* name;
        void* detour;
        void** original;
    } instanceHooks[] = {
        {"vkCreateDevice", reinterpret_cast<void*>(HookCreateDevice), reinterpret_cast<void**>(&g_createDevice)},
        {"vkCreateWin32SurfaceKHR", reinterpret_cast<void*>(HookCreateWin32SurfaceKHR), reinterpret_cast<void**>(&g_createWin32Surface)},
        {"vkDestroySurfaceKHR", reinterpret_cast<void*>(HookDestroySurfaceKHR), reinterpret_cast<void**>(&g_destroySurface)},
    };
    for (const InstanceHook& hook : instanceHooks)
        if (void* target = reinterpret_cast<void*>(g_getInstanceProcAddr(g_instance, hook.name)))
            CreateHook(hook.name, target, hook.detour, hook.original);
    return hooked;
}

void Uninstall() {
    for (Icd& icd : g_icds)
        icd.fn.vkDestroyDevice(icd.dummyDevice, nullptr);
    g_icds.clear();
    if (g_instance) {
        g_vk.vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
    }
    g_swapchains.clear();
    g_queues.clear();
    g_devices.clear();
    g_surfaces.clear();
    g_createDevice = nullptr;
    g_createWin32Surface = nullptr;
    g_destroySurface = nullptr;
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

const Renderer kVulkanRenderer = {"Vulkan", L"vulkan-1.dll", Install, Uninstall, Teardown};

} // namespace uih::detail
