// Vulkan application shared by the Windows and the Linux test hosts.
#include "host.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <vulkan/vulkan.h>
#ifndef _WIN32
#include <vulkan/vulkan_xlib.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// With UIH_TEST_VULKAN_VALIDATION set, the host enables the Khronos
// validation layer, which also checks the commands the hook records on the
// host's device, and counts its errors.
int g_validationErrors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_validationErrors;
        printf("VALIDATION ERROR: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

bool HasLayer(PFN_vkGetInstanceProcAddr getInstanceProcAddr, const char* name) {
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
    uint32_t count = 0;
    if (!enumerate || enumerate(&count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkLayerProperties> layers(count);
    enumerate(&count, layers.data());
    for (const VkLayerProperties& layer : layers)
        if (strcmp(layer.layerName, name) == 0)
            return true;
    return false;
}

#ifdef _WIN32
const char* const kSurfaceExtension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;

PFN_vkGetInstanceProcAddr LoadGetInstanceProcAddr() {
    HMODULE module = LoadLibraryW(L"vulkan-1.dll");
    return module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr"))) : nullptr;
}

VkSurfaceKHR CreateSurface(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, HostWindow window) {
    auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(getInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    return create && create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}
#else
const char* const kSurfaceExtension = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;

PFN_vkGetInstanceProcAddr LoadGetInstanceProcAddr() {
    void* module = dlopen("libvulkan.so.1", RTLD_NOW);
    return module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr")) : nullptr;
}

VkSurfaceKHR CreateSurface(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, HostWindow window) {
    auto create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(getInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
    VkXlibSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = window.display;
    info.window = window.window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    return create && create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}
#endif

#define HOST_VK_INSTANCE_FUNCTIONS(X)            \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)

#define HOST_VK_DEVICE_FUNCTIONS(X) \
    X(vkGetDeviceQueue)             \
    X(vkCreateSwapchainKHR)         \
    X(vkGetSwapchainImagesKHR)      \
    X(vkDestroySwapchainKHR)        \
    X(vkDestroySemaphore)           \
    X(vkCreateCommandPool)          \
    X(vkAllocateCommandBuffers)     \
    X(vkCreateSemaphore)            \
    X(vkCreateFence)                \
    X(vkAcquireNextImageKHR)        \
    X(vkWaitForFences)              \
    X(vkResetFences)                \
    X(vkResetCommandBuffer)         \
    X(vkBeginCommandBuffer)         \
    X(vkCmdPipelineBarrier)         \
    X(vkCmdClearColorImage)         \
    X(vkEndCommandBuffer)           \
    X(vkQueueSubmit)                \
    X(vkQueuePresentKHR)            \
    X(vkDeviceWaitIdle)

struct VulkanHost : Host {
    static constexpr uint32_t kFrames = 2;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
#define HOST_VK_DECLARE(name) PFN_##name name = nullptr;
    HOST_VK_INSTANCE_FUNCTIONS(HOST_VK_DECLARE)
    HOST_VK_DEVICE_FUNCTIONS(HOST_VK_DECLARE)
#undef HOST_VK_DECLARE

    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR swapchainInfo{};
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> renderDone;
    VkCommandBuffer commandBuffers[kFrames] = {};
    VkSemaphore acquired[kFrames] = {};
    VkFence fences[kFrames] = {};
    uint32_t frame = 0;

    bool Init(HostWindow window, const Options& options) override {
        getInstanceProcAddr = LoadGetInstanceProcAddr();
        if (!getInstanceProcAddr)
            return false;
        auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.apiVersion = VK_API_VERSION_1_0;
        std::vector<const char*> instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, kSurfaceExtension};
        const char* const validationLayer = "VK_LAYER_KHRONOS_validation";
        const bool validation = getenv("UIH_TEST_VULKAN_VALIDATION") != nullptr;
        if (validation) {
            if (!HasLayer(getInstanceProcAddr, validationLayer)) {
                printf("VK_LAYER_KHRONOS_validation is not installed\n");
                return false;
            }
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
        instanceInfo.enabledLayerCount = validation ? 1 : 0;
        instanceInfo.ppEnabledLayerNames = &validationLayer;
        if (createInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
            return false;
        if (validation) {
            auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(getInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
            messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            messengerInfo.pfnUserCallback = OnValidationMessage;
            VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
            if (!createMessenger || createMessenger(instance, &messengerInfo, nullptr, &messenger) != VK_SUCCESS)
                return false;
        }
#define HOST_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(getInstanceProcAddr(instance, #name));
        HOST_VK_INSTANCE_FUNCTIONS(HOST_VK_LOAD)
        HOST_VK_DEVICE_FUNCTIONS(HOST_VK_LOAD)
#undef HOST_VK_LOAD

        VkSurfaceKHR surface = CreateSurface(getInstanceProcAddr, instance, window);
        if (surface == VK_NULL_HANDLE)
            return false;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> physicalDevices(count);
        vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data());
        uint32_t family = 0;
        for (VkPhysicalDevice candidate : physicalDevices) {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t i = 0; i < familyCount && !physicalDevice; ++i) {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && supported) {
                    physicalDevice = candidate;
                    family = i;
                }
            }
            if (physicalDevice)
                break;
        }
        if (!physicalDevice)
            return false;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS)
            return false;
        if (options.deviceProcAddr) {
#define HOST_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
            HOST_VK_DEVICE_FUNCTIONS(HOST_VK_LOAD)
#undef HOST_VK_LOAD
        }
        vkGetDeviceQueue(device, family, 0, &queue);

        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
        VkFormat wanted = VK_FORMAT_B8G8R8A8_UNORM;
        if (options.format == "srgb")
            wanted = VK_FORMAT_B8G8R8A8_SRGB;
        else if (!options.format.empty())
            return false;
        VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        for (const VkSurfaceFormatKHR& candidate : formats)
            if (candidate.format == wanted)
                format = candidate;
        if (format.format == VK_FORMAT_UNDEFINED)
            return false;

        swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchainInfo.surface = surface;
        swapchainInfo.minImageCount = std::max<uint32_t>(2, capabilities.minImageCount);
        swapchainInfo.imageFormat = format.format;
        swapchainInfo.imageColorSpace = format.colorSpace;
        swapchainInfo.imageExtent = capabilities.currentExtent;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = capabilities.currentTransform;
        swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;
        Settle(options);
        if (vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain) != VK_SUCCESS)
            return false;
        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        images.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = pool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = kFrames;
        vkAllocateCommandBuffers(device, &allocateInfo, commandBuffers);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < kFrames; ++i) {
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired[i]);
            vkCreateFence(device, &fenceInfo, nullptr, &fences[i]);
        }
        renderDone.resize(imageCount);
        for (VkSemaphore& semaphore : renderDone)
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
        return true;
    }

    bool Resize(int, int) override {
        vkDeviceWaitIdle(device);
        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, swapchainInfo.surface, &capabilities);
        swapchainInfo.imageExtent = capabilities.currentExtent;
        swapchainInfo.oldSwapchain = swapchain;
        VkSwapchainKHR replacement = VK_NULL_HANDLE;
        VkResult result = vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &replacement);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
        swapchain = replacement;
        if (result != VK_SUCCESS)
            return false;
        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        images.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkSemaphore semaphore : renderDone)
            vkDestroySemaphore(device, semaphore, nullptr);
        renderDone.resize(imageCount);
        for (VkSemaphore& semaphore : renderDone)
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
        return true;
    }

    void Frame() override {
        uint32_t slot = frame % kFrames;
        vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX);
        uint32_t imageIndex = 0;
        if (vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[slot], VK_NULL_HANDLE, &imageIndex) < 0)
            return;
        vkResetFences(device, 1, &fences[slot]);

        VkCommandBuffer cmd = commandBuffers[slot];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[imageIndex];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkClearColorValue color{};
        memcpy(color.float32, kClear, sizeof(kClear));
        vkCmdClearColorImage(cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired[slot];
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderDone[imageIndex];
        vkQueueSubmit(queue, 1, &submit, fences[slot]);

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderDone[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        VkResult presented = vkQueuePresentKHR(queue, &present);
        ++frame;
        if (presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR) {
            printf("swap chain recreated after %s\n", presented == VK_SUBOPTIMAL_KHR ? "VK_SUBOPTIMAL_KHR" : "VK_ERROR_OUT_OF_DATE_KHR");
            Resize(0, 0);
        }
    }

    int ValidationErrors() const override { return g_validationErrors; }

    ~VulkanHost() override {
        if (device)
            vkDeviceWaitIdle(device);
    }
};

} // namespace

std::unique_ptr<Host> CreateVulkanHost() { return std::make_unique<VulkanHost>(); }
