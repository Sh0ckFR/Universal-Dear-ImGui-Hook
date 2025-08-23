#include "stdafx.h"
#include <unordered_map>
#include <Psapi.h>
#include <vulkan/vulkan_win32.h>

namespace hooks_vk {
    PFN_vkCreateInstance       oCreateInstance       = nullptr;
    PFN_vkCreateDevice         oCreateDevice         = nullptr;
    PFN_vkQueuePresentKHR      oQueuePresentKHR      = nullptr;
    PFN_vkCreateSwapchainKHR   oCreateSwapchainKHR   = nullptr;
    PFN_vkCreateWin32SurfaceKHR oCreateWin32SurfaceKHR = nullptr;
    static PFN_vkGetInstanceProcAddr oGetInstanceProcAddr = nullptr;
    static PFN_vkGetDeviceProcAddr   oGetDeviceProcAddr   = nullptr;
    static PFN_vkGetDeviceQueue      oGetDeviceQueue      = nullptr;
    static PFN_vkCmdBeginRenderingKHR fpBeginRendering = nullptr;
    static PFN_vkCmdEndRenderingKHR   fpEndRendering   = nullptr;
    static bool                      gUseDynamicRendering = false;

    static VkInstance       gInstance       = VK_NULL_HANDLE;
    static VkPhysicalDevice gPhysicalDevice = VK_NULL_HANDLE;
    static VkDevice         gDevice         = VK_NULL_HANDLE;
    static VkQueue          gQueue          = VK_NULL_HANDLE;
    static uint32_t         gQueueFamily    = 0;
    static VkDescriptorPool gDescriptorPool = VK_NULL_HANDLE;
    static VkCommandPool    gCommandPool    = VK_NULL_HANDLE;
    static VkSwapchainKHR   gSwapchain      = VK_NULL_HANDLE;
    static VkSurfaceKHR     gSurface        = VK_NULL_HANDLE;
    static bool             gInitialized    = false;
    static bool             gWin32Initialized = false;
    static VkRenderPass     gRenderPass     = VK_NULL_HANDLE;
    static VkFormat         gSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    static VkImageAspectFlags gSwapchainAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

    struct DeviceInfo
    {
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t        queueFamily = 0;
    };
    static std::unordered_map<VkDevice, DeviceInfo> gDeviceMap;

    static bool IsPlausibleDevice(VkDevice dev)
    {
        if (dev == VK_NULL_HANDLE)
            return false;
        if (!gDeviceMap.empty() && gDeviceMap.find(dev) == gDeviceMap.end())
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)dev, &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        // Some applications provide stale or placeholder device handles that pass the
        // memory checks above. Query vkGetDeviceQueue for the device and attempt a
        // dummy call to ensure the device is valid.
        if (oGetDeviceProcAddr)
        {
            auto stub = oGetDeviceProcAddr(VK_NULL_HANDLE, "vkGetDeviceQueue");
            PFN_vkGetDeviceQueue p = (PFN_vkGetDeviceQueue)oGetDeviceProcAddr(dev, "vkGetDeviceQueue");
            if (p == nullptr || p == (PFN_vkGetDeviceQueue)stub)
            {
                DebugLog("[vulkanhook] device %p rejected (stub dispatch)\n", dev);
                return false;
            }
            __try
            {
                VkQueue q = VK_NULL_HANDLE;
                p(dev, 0, 0, &q);
                DebugLog("[vulkanhook] vkGetDeviceQueue call succeeded for %p (queue=%p)\n", dev, q);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DebugLog("[vulkanhook] vkGetDeviceQueue call failed for %p\n", dev);
                return false;
            }
        }
        return true;
    }

    VkResult VKAPI_PTR hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    void     VKAPI_PTR hook_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);
    VkResult VKAPI_PTR hook_vkCreateSwapchainKHR(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain);
    VkResult VKAPI_PTR hook_vkCreateWin32SurfaceKHR(
        VkInstance instance,
        const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface);

    struct FrameData {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        VkFramebuffer   fb   = VK_NULL_HANDLE;
    };
    static std::vector<VkImage>      gSwapchainImages;
    static std::vector<VkImageView>  gImageViews;
    static std::vector<FrameData>    gFrames;
    static uint32_t                  gImageCount = 0;
    static uint32_t                  gFrameIndex = 0;

    static VkResult CreateDescriptorPool()
    {
        if (!IsPlausibleDevice(gDevice))
        {
            DebugLog("[vulkanhook] CreateDescriptorPool: invalid device %p\n", gDevice);
            return VK_ERROR_DEVICE_LOST;
        }
        VkDescriptorPoolSize pool_sizes[] = {
            { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
        };
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        VkResult res = vkCreateDescriptorPool(gDevice, &pool_info, nullptr, &gDescriptorPool);
        if (res != VK_SUCCESS)
            DebugLog("[vulkanhook] vkCreateDescriptorPool failed: %d\n", res);
        return res;
    }

    static VkResult CreateCommandPool()
    {
        if (!IsPlausibleDevice(gDevice))
        {
            DebugLog("[vulkanhook] CreateCommandPool: invalid device %p\n", gDevice);
            return VK_ERROR_DEVICE_LOST;
        }
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = gQueueFamily;
        VkResult res = vkCreateCommandPool(gDevice, &info, nullptr, &gCommandPool);
        if (res != VK_SUCCESS)
            DebugLog("[vulkanhook] vkCreateCommandPool failed: %d\n", res);
        return res;
    }

    static void HookQueuePresent(VkDevice device, VkQueue queue)
    {
        if (!device || !oGetDeviceProcAddr)
            return;

        PFN_vkQueuePresentKHR func = (PFN_vkQueuePresentKHR)oGetDeviceProcAddr(device, "vkQueuePresentKHR");
        if (!func)
            return;

        if (oQueuePresentKHR && oQueuePresentKHR != func)
        {
            MH_DisableHook((void*)oQueuePresentKHR);
            MH_RemoveHook((void*)oQueuePresentKHR);
        }

        MH_STATUS mh;
        if (oQueuePresentKHR != func)
        {
            mh = MH_CreateHook((void*)func, (void*)hook_vkQueuePresentKHR, (void**)&oQueuePresentKHR);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkQueuePresentKHR failed: %s\n", MH_StatusToString(mh));
        }

        mh = MH_EnableHook((void*)func);
        if (mh != MH_OK)
            DebugLog("[vulkanhook] MH_EnableHook vkQueuePresentKHR failed: %s\n", MH_StatusToString(mh));
        oQueuePresentKHR = func;

        if (gDevice == VK_NULL_HANDLE)
            gDevice = device;
        if (queue != VK_NULL_HANDLE && gQueue == VK_NULL_HANDLE)
            gQueue = queue;
    }

    static VkImageAspectFlags GetAspectMask(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    static VkResult CreateRenderPass()
    {
        if (gRenderPass)
            return VK_SUCCESS;
        VkAttachmentDescription attachment{};
        attachment.format = gSwapchainFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        VkRenderPassCreateInfo rp_info{};
        rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_info.attachmentCount = 1;
        rp_info.pAttachments = &attachment;
        rp_info.subpassCount = 1;
        rp_info.pSubpasses = &subpass;

        VkResult res = vkCreateRenderPass(gDevice, &rp_info, nullptr, &gRenderPass);
        if (res != VK_SUCCESS)
            DebugLog("[vulkanhook] vkCreateRenderPass failed: %d\n", res);
        return res;
    }

    static void DestroyFrameResources()
    {
        if (gDevice == VK_NULL_HANDLE || gCommandPool == VK_NULL_HANDLE)
        {
            gFrames.clear();
            return;
        }

        std::vector<VkCommandBuffer> cmds;
        cmds.reserve(gFrames.size());
        for (auto& fr : gFrames)
        {
            if (fr.fence)
            {
                vkDestroyFence(gDevice, fr.fence, nullptr);
                fr.fence = VK_NULL_HANDLE;
            }
            if (fr.fb)
            {
                vkDestroyFramebuffer(gDevice, fr.fb, nullptr);
                fr.fb = VK_NULL_HANDLE;
            }
            if (fr.cmd)
            {
                cmds.push_back(fr.cmd);
                fr.cmd = VK_NULL_HANDLE;
            }
        }
        if (!cmds.empty())
            vkFreeCommandBuffers(gDevice, gCommandPool, (uint32_t)cmds.size(), cmds.data());
        gFrames.clear();
        gFrameIndex = 0;
    }

    static VkResult CreateFrameResources(uint32_t count)
    {
        gFrames.resize(count);
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = gCommandPool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = count;
        std::vector<VkCommandBuffer> cmds(count);
        VkResult res = vkAllocateCommandBuffers(gDevice, &alloc_info, cmds.data());
        if (res != VK_SUCCESS)
        {
            DebugLog("[vulkanhook] vkAllocateCommandBuffers failed: %d\n", res);
            return res;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            gFrames[i].cmd = cmds[i];
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            res = vkCreateFence(gDevice, &fence_info, nullptr, &gFrames[i].fence);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkCreateFence failed: %d\n", res);
                return res;
            }
        }
        return VK_SUCCESS;
    }

    PFN_vkVoidFunction VKAPI_PTR hook_vkGetDeviceProcAddr(VkDevice device, const char* pName)
    {
        PFN_vkVoidFunction func = oGetDeviceProcAddr(device, pName);
        if (!func)
            return func;

        if (strcmp(pName, "vkQueuePresentKHR") == 0)
        {
            if (gDevice == VK_NULL_HANDLE)
                gDevice = device;
            if (gQueue == VK_NULL_HANDLE)
            {
                PFN_vkGetDeviceQueue getQueue = (PFN_vkGetDeviceQueue)oGetDeviceProcAddr(device, "vkGetDeviceQueue");
                if (getQueue)
                    getQueue(device, 0, 0, &gQueue);
            }
            HookQueuePresent(device, gQueue);
        }
        else if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        {
            if (oCreateSwapchainKHR == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkCreateSwapchainKHR, (void**)&oCreateSwapchainKHR);
                MH_EnableHook((void*)func);
            }
        }
        else if (strcmp(pName, "vkCreateWin32SurfaceKHR") == 0)
        {
            if (oCreateWin32SurfaceKHR == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkCreateWin32SurfaceKHR, (void**)&oCreateWin32SurfaceKHR);
                MH_EnableHook((void*)func);
            }
        }
        else if (strcmp(pName, "vkGetDeviceQueue") == 0)
        {
            if (oGetDeviceQueue == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkGetDeviceQueue, (void**)&oGetDeviceQueue);
                MH_EnableHook((void*)func);
            }
        }
        return func;
    }

    PFN_vkVoidFunction VKAPI_PTR hook_vkGetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        PFN_vkVoidFunction func = oGetInstanceProcAddr(instance, pName);
        if (!func)
            return func;

        if (gInstance == VK_NULL_HANDLE)
            gInstance = instance;
        if (strcmp(pName, "vkQueuePresentKHR") == 0)
        {
            HookQueuePresent(gDevice, gQueue);
        }
        else if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        {
            if (oCreateSwapchainKHR == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkCreateSwapchainKHR, (void**)&oCreateSwapchainKHR);
                MH_EnableHook((void*)func);
            }
        }
        else if (strcmp(pName, "vkGetDeviceQueue") == 0)
        {
            if (oGetDeviceQueue == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkGetDeviceQueue, (void**)&oGetDeviceQueue);
                MH_EnableHook((void*)func);
            }
        }
        return func;
    }

    void VKAPI_PTR hook_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue)
    {
        oGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue)
        {
            gDevice = device;
            gQueueFamily = queueFamilyIndex;
            gDeviceMap[device].queueFamily = queueFamilyIndex;
            HookQueuePresent(device, *pQueue);
            gQueue = *pQueue;
        }
    }

    VkResult VKAPI_PTR hook_vkCreateSwapchainKHR(VkDevice device,
                                                 const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkSwapchainKHR* pSwapchain)
    {
        if (pCreateInfo)
        {
            gSurface = pCreateInfo->surface;
            gSwapchainFormat = pCreateInfo->imageFormat;
            gSwapchainAspectMask = GetAspectMask(gSwapchainFormat);
        }
        VkResult res = oCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        if (res != VK_SUCCESS)
            DebugLog("[vulkanhook] vkCreateSwapchainKHR failed: %d\n", res);
        return res;
    }

    VkResult VKAPI_PTR hook_vkCreateWin32SurfaceKHR(VkInstance instance,
                                                    const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkSurfaceKHR* pSurface)
    {
        if (pCreateInfo)
            globals::mainWindow = pCreateInfo->hwnd;
        VkResult res = oCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
        if (res == VK_SUCCESS && pSurface)
            gSurface = *pSurface;
        if (res != VK_SUCCESS)
            DebugLog("[vulkanhook] vkCreateWin32SurfaceKHR failed: %d\n", res);
        return res;
    }

    VkResult VKAPI_PTR hook_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkInstance* pInstance)
    {
        VkResult res = oCreateInstance(pCreateInfo, pAllocator, pInstance);
        if (res != VK_SUCCESS)
        {
            DebugLog("[vulkanhook] vkCreateInstance failed: %d\n", res);
            return res;
        }
        gInstance = *pInstance;
        return res;
    }

    VkResult VKAPI_PTR hook_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                           const VkDeviceCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkDevice* pDevice)
    {
        bool has_dynamic = false;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
        {
            if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], "VK_KHR_dynamic_rendering") == 0)
            {
                has_dynamic = true;
                break;
            }
        }

        bool has_dynamic_feature = false;
        for (const VkBaseInStructure* p = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             p; p = p->pNext)
        {
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR)
            {
                has_dynamic_feature = true;
                break;
            }
        }

        std::vector<const char*> extensions;
        VkDeviceCreateInfo create_info = *pCreateInfo;
        if (!has_dynamic)
        {
            uint32_t ext_count = 0;
            VkResult res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, nullptr);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkEnumerateDeviceExtensionProperties failed: %d\n", res);
                return res;
            }
            std::vector<VkExtensionProperties> props(ext_count);
            res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, props.data());
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkEnumerateDeviceExtensionProperties failed: %d\n", res);
                return res;
            }
            for (const auto& e : props)
            {
                if (strcmp(e.extensionName, "VK_KHR_dynamic_rendering") == 0)
                {
                    has_dynamic = true;
                    extensions.assign(pCreateInfo->ppEnabledExtensionNames,
                                      pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
                    extensions.push_back("VK_KHR_dynamic_rendering");
                    create_info.enabledExtensionCount = (uint32_t)extensions.size();
                    create_info.ppEnabledExtensionNames = extensions.data();
                    break;
                }
            }
        }

        VkPhysicalDeviceDynamicRenderingFeaturesKHR dyn_features{};
        if (!has_dynamic_feature)
        {
            dyn_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
            dyn_features.dynamicRendering = VK_TRUE;
            dyn_features.pNext = const_cast<void*>(create_info.pNext);
            create_info.pNext = &dyn_features;
        }

        VkResult res = oCreateDevice(physicalDevice, &create_info, pAllocator, pDevice);
        if (res != VK_SUCCESS)
        {
            DebugLog("[vulkanhook] vkCreateDevice failed: %d\n", res);
            return res;
        }

        gUseDynamicRendering = has_dynamic;
        gDevice = *pDevice;
        gPhysicalDevice = physicalDevice;
        gQueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
        gDeviceMap[gDevice] = { gPhysicalDevice, gQueueFamily };

        if (!oGetDeviceProcAddr)
            oGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkGetDeviceProcAddr");

        oGetDeviceQueue = (PFN_vkGetDeviceQueue)oGetDeviceProcAddr(gDevice, "vkGetDeviceQueue");
        if (oGetDeviceQueue)
            oGetDeviceQueue(gDevice, gQueueFamily, 0, &gQueue);

        if (gUseDynamicRendering)
        {
            fpBeginRendering = (PFN_vkCmdBeginRenderingKHR)oGetDeviceProcAddr(gDevice, "vkCmdBeginRenderingKHR");
            fpEndRendering   = (PFN_vkCmdEndRenderingKHR)oGetDeviceProcAddr(gDevice, "vkCmdEndRenderingKHR");
        }

        HookQueuePresent(gDevice, gQueue);

        VkResult init_res = CreateDescriptorPool();
        if (init_res != VK_SUCCESS)
        {
            DebugLog("[vulkanhook] initialization aborted; gDevice=%p, gQueue=%p, res=%d\n", gDevice, gQueue, init_res);
            return res;
        }
        init_res = CreateCommandPool();
        if (init_res != VK_SUCCESS)
        {
            DebugLog("[vulkanhook] initialization aborted; gDevice=%p, gQueue=%p, res=%d\n", gDevice, gQueue, init_res);
            return res;
        }
        DebugLog("[vulkanhook] Device initialized.\n");
        return res;
    }

    static bool FallbackScanDispatchTables()
    {
        if (!oGetDeviceQueue || !oGetDeviceProcAddr)
            return false;

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        MEMORY_BASIC_INFORMATION mbi{};
        uint8_t* addr = (uint8_t*)si.lpMinimumApplicationAddress;
        while (addr < (uint8_t*)si.lpMaximumApplicationAddress)
        {
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi))
            {
                bool readable = (mbi.State == MEM_COMMIT) &&
                                !(mbi.Protect & PAGE_GUARD) &&
                                (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                                                PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                                                PAGE_EXECUTE_READ | PAGE_EXECUTE_WRITECOPY));
                if (readable)
                {
                    uint8_t* start = (uint8_t*)mbi.BaseAddress;
                    uint8_t* end   = start + mbi.RegionSize;
                    for (uint8_t* p = start; p < end; p += sizeof(void*))
                    {
                        if (*(void**)p == (void*)oGetDeviceProcAddr)
                        {
                            VkDevice dev = (VkDevice)p;
                            if (!IsPlausibleDevice(dev))
                            {
                                DebugLog("[vulkanhook] Rejected candidate VkDevice %p during fallback scan\n", dev);
                                continue;
                            }
                            VkQueue q = VK_NULL_HANDLE;
                            __try { oGetDeviceQueue(dev, 0, 0, &q); }
                            __except (EXCEPTION_EXECUTE_HANDLER)
                            {
                                DebugLog("[vulkanhook] Exception while querying queue for VkDevice %p during fallback scan\n", dev);
                            }
                            if (q)
                            {
                                gDevice = dev;
                                gQueue  = q;
                                HookQueuePresent(gDevice, gQueue);
                                gDeviceMap[dev] = { VK_NULL_HANDLE, 0 };
                                return true;
                            }
                            else
                            {
                                DebugLog("[vulkanhook] No queue returned for VkDevice %p during fallback scan\n", dev);
                            }
                        }
                    }
                }
                addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
            }
            else
            {
                addr += si.dwPageSize;
            }
        }
        return false;
    }

    VkResult VKAPI_PTR hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        static bool logged = false;
        if (!logged)
        {
            DebugLog("[vulkanhook] vkQueuePresentKHR intercepted\\n");
            logged = true;
        }
        if (gDevice == VK_NULL_HANDLE || gQueue == VK_NULL_HANDLE)
        {
            void*** dispatch_ptr = reinterpret_cast<void***>(queue);
            if (dispatch_ptr && *dispatch_ptr)
            {
                PFN_vkGetDeviceProcAddr getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>((*dispatch_ptr)[0]);
                if (!oGetDeviceProcAddr)
                    oGetDeviceProcAddr = getDeviceProcAddr;

                VkDevice        found_device = VK_NULL_HANDLE;
                void**          queue_ptr    = reinterpret_cast<void**>(queue);
                if (queue_ptr)
                {
                    // Search through up to the first 64 bytes (8 pointers on 64-bit
                    // builds) of the queue object for a plausible VkDevice pointer.
                    // Adjust MAX_DEVICE_OFFSETS if Vulkan internals change.
                    constexpr int MAX_DEVICE_OFFSETS = 64 / sizeof(void*);
                    DebugLog("[vulkanhook] probing %d candidate device offsets\n", MAX_DEVICE_OFFSETS);
                    for (int i = 0; i < MAX_DEVICE_OFFSETS; ++i)
                    {
                        VkDevice device = reinterpret_cast<VkDevice>(queue_ptr[i]);
                        if (device == VK_NULL_HANDLE)
                            continue;
                        bool ok = IsPlausibleDevice(device);
                        DebugLog("[vulkanhook] candidate device[%d] %p %s validation\n", i, device, ok ? "passed" : "failed");
                        if (ok)
                        {
                            found_device = device;
                            break;
                        }
                    }
                    if (found_device == VK_NULL_HANDLE)
                        DebugLog("[vulkanhook] all candidate devices failed; initialization deferred\n");
                }

                if (found_device != VK_NULL_HANDLE)
                {
                    gDevice = found_device;
                    if (!oGetDeviceQueue)
                        oGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(oGetDeviceProcAddr(gDevice, "vkGetDeviceQueue"));

                    auto it = gDeviceMap.find(gDevice);
                    if (it != gDeviceMap.end())
                    {
                        gPhysicalDevice = it->second.physical;
                        gQueueFamily    = it->second.queueFamily;
                    }

                    if (gPhysicalDevice != VK_NULL_HANDLE && oGetDeviceQueue)
                    {
                        oGetDeviceQueue(gDevice, gQueueFamily, 0, &gQueue);
                        DebugLog("[vulkanhook] oGetDeviceQueue(%p) returned %p\n", gDevice, gQueue);
                    }
                }
            }

            if (gQueue == VK_NULL_HANDLE)
                gQueue = queue;
        }

        if (gQueue == VK_NULL_HANDLE)
            gQueue = queue;

        static int noDeviceFrames = 0;
        if (!IsPlausibleDevice(gDevice) || gQueue == VK_NULL_HANDLE)
        {
            if (++noDeviceFrames >= 10)
            {
                DebugLog("[vulkanhook] attempting fallback dispatch table scan after %d frames\n", noDeviceFrames);
                bool ok = FallbackScanDispatchTables();
                DebugLog("[vulkanhook] fallback dispatch table scan %s\n", ok ? "succeeded" : "failed");
                noDeviceFrames = 0;
            }
        }
        else
        {
            noDeviceFrames = 0;
        }

        if (pPresentInfo && pPresentInfo->swapchainCount > 0 &&
            IsPlausibleDevice(gDevice) &&
            gSwapchain == VK_NULL_HANDLE && gSwapchainFormat == VK_FORMAT_B8G8R8A8_UNORM)
        {
            gSwapchain = pPresentInfo->pSwapchains[0];
            if (gPhysicalDevice != VK_NULL_HANDLE && gSurface != VK_NULL_HANDLE)
            {
                uint32_t format_count = 0;
                VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(gPhysicalDevice, gSurface, &format_count, nullptr);
                if (res == VK_SUCCESS && format_count > 0)
                {
                    std::vector<VkSurfaceFormatKHR> formats(format_count);
                    res = vkGetPhysicalDeviceSurfaceFormatsKHR(gPhysicalDevice, gSurface, &format_count, formats.data());
                    if (res == VK_SUCCESS)
                    {
                        gSwapchainFormat = formats[0].format;
                        gSwapchainAspectMask = GetAspectMask(gSwapchainFormat);
                        for (auto view : gImageViews)
                            vkDestroyImageView(gDevice, view, nullptr);
                        gImageViews.clear();
                        if (gRenderPass)
                        {
                            vkDestroyRenderPass(gDevice, gRenderPass, nullptr);
                            gRenderPass = VK_NULL_HANDLE;
                        }
                    }
                }
            }
        }

        if (pPresentInfo && pPresentInfo->swapchainCount > 0 &&
            IsPlausibleDevice(gDevice) &&
            gSwapchain != VK_NULL_HANDLE && pPresentInfo->pSwapchains[0] != gSwapchain)
        {
            VkResult res = vkDeviceWaitIdle(gDevice);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkDeviceWaitIdle failed: %d\n", res);
                return res;
            }
            HWND hwnd = globals::mainWindow;
            if (globals::mainWindow)
                inputhook::Remove(globals::mainWindow);
            globals::mainWindow = hwnd;
            if (gWin32Initialized)
                ImGui_ImplWin32_Shutdown();
            gWin32Initialized = false;
            if (gInitialized)
            {
                ImGui_ImplVulkan_Shutdown();
                ImGui::DestroyContext();
                gInitialized = false;
            }
            DestroyFrameResources();
            if (gCommandPool)
            {
                vkDestroyCommandPool(gDevice, gCommandPool, nullptr);
                gCommandPool = VK_NULL_HANDLE;
            }
            if (gDescriptorPool)
            {
                vkDestroyDescriptorPool(gDevice, gDescriptorPool, nullptr);
                gDescriptorPool = VK_NULL_HANDLE;
            }
            for (auto view : gImageViews)
                vkDestroyImageView(gDevice, view, nullptr);
            gImageViews.clear();
            gSwapchainImages.clear();
            if (gRenderPass)
            {
                vkDestroyRenderPass(gDevice, gRenderPass, nullptr);
                gRenderPass = VK_NULL_HANDLE;
            }
            gSwapchain = VK_NULL_HANDLE;
            gImageCount = 0;
        }

        if (!gInitialized && pPresentInfo && pPresentInfo->swapchainCount > 0)
        {
            if (!IsPlausibleDevice(gDevice) || gQueue == VK_NULL_HANDLE || gPhysicalDevice == VK_NULL_HANDLE)
            {
                DebugLog("[vulkanhook] initialization postponed; gDevice=%p, gQueue=%p, gPhysicalDevice=%p\n", gDevice, gQueue, gPhysicalDevice);
                return oQueuePresentKHR(queue, pPresentInfo);
            }
            VkResult res;
            if (gDescriptorPool == VK_NULL_HANDLE)
            {
                res = CreateDescriptorPool();
                if (res != VK_SUCCESS)
                {
                    DebugLog("[vulkanhook] initialization aborted; gDevice=%p, gQueue=%p, res=%d\n", gDevice, gQueue, res);
                    return oQueuePresentKHR(queue, pPresentInfo);
                }
            }
            if (gCommandPool == VK_NULL_HANDLE)
            {
                res = CreateCommandPool();
                if (res != VK_SUCCESS)
                {
                    DebugLog("[vulkanhook] initialization aborted; gDevice=%p, gQueue=%p, res=%d\n", gDevice, gQueue, res);
                    return oQueuePresentKHR(queue, pPresentInfo);
                }
            }
            gSwapchain = pPresentInfo->pSwapchains[0];
            res = vkGetSwapchainImagesKHR(gDevice, gSwapchain, &gImageCount, nullptr);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkGetSwapchainImagesKHR failed: %d\n", res);
                return res;
            }
            gSwapchainImages.resize(gImageCount);
            res = vkGetSwapchainImagesKHR(gDevice, gSwapchain, &gImageCount, gSwapchainImages.data());
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkGetSwapchainImagesKHR failed: %d\n", res);
                return res;
            }
            gImageViews.resize(gImageCount);
            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                VkImageViewCreateInfo view_info{};
                view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view_info.format = gSwapchainFormat;
                view_info.image = gSwapchainImages[i];
                view_info.subresourceRange.aspectMask = gSwapchainAspectMask;
                view_info.subresourceRange.levelCount = 1;
                view_info.subresourceRange.layerCount = 1;
                res = vkCreateImageView(gDevice, &view_info, nullptr, &gImageViews[i]);
                if (res != VK_SUCCESS)
                {
                    DebugLog("[vulkanhook] vkCreateImageView failed: %d\n", res);
                    return res;
                }
            }
            if (!gUseDynamicRendering)
            {
                res = CreateRenderPass();
                if (res != VK_SUCCESS)
                    return res;
            }
            DestroyFrameResources();
            res = CreateFrameResources(gImageCount);
            if (res != VK_SUCCESS)
                return res;

            ImGui::CreateContext();
            ImGui_ImplVulkan_InitInfo init_info{};
            init_info.Instance = gInstance;
            init_info.PhysicalDevice = gPhysicalDevice;
            init_info.Device = gDevice;
            init_info.QueueFamily = gQueueFamily;
            init_info.Queue = gQueue;
            init_info.DescriptorPool = gDescriptorPool;
            init_info.MinImageCount = gImageCount;
            init_info.ImageCount = gImageCount;
            init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            init_info.UseDynamicRendering = gUseDynamicRendering;
            if (!gUseDynamicRendering)
                init_info.RenderPass = gRenderPass;
#ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
            if (gUseDynamicRendering)
            {
                VkFormat color_format = gSwapchainFormat;
                init_info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
                init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
            }
#endif
            ImGui_ImplVulkan_Init(&init_info);
            gInitialized = true;
            globals::activeBackend = globals::Backend::Vulkan;
            DebugLog("[vulkanhook] ImGui initialized.\n");
        }

        if (GetAsyncKeyState(globals::openMenuKey) & 1)
        {
            menu::isOpen = !menu::isOpen;
            DebugLog("[vulkanhook] Toggle menu: %d\n", menu::isOpen);
        }

        if (GetAsyncKeyState(globals::uninjectKey) & 1)
        {
            Uninject();
            return oQueuePresentKHR(queue, pPresentInfo);
        }

        if (gInitialized && pPresentInfo && pPresentInfo->pImageIndices)
        {
            uint32_t image_index = pPresentInfo->pImageIndices[0];
            FrameData& fr = gFrames[image_index % gFrames.size()];
            VkResult res = vkWaitForFences(gDevice, 1, &fr.fence, VK_TRUE, UINT64_MAX);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkWaitForFences failed: %d\n", res);
                return res;
            }
            res = vkResetFences(gDevice, 1, &fr.fence);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkResetFences failed: %d\n", res);
                return res;
            }
            res = vkResetCommandBuffer(fr.cmd, 0);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkResetCommandBuffer failed: %d\n", res);
                return res;
            }

            ImGui_ImplVulkan_NewFrame();
            if (gWin32Initialized)
                ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (menu::isOpen)
                menu::Init();
            ImGui::EndFrame();
            ImGui::Render();

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            res = vkBeginCommandBuffer(fr.cmd, &begin_info);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkBeginCommandBuffer failed: %d\n", res);
                return res;
            }
#ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
            if (gUseDynamicRendering && fpBeginRendering && fpEndRendering)
            {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.image = gSwapchainImages[image_index];
                barrier.subresourceRange.aspectMask = gSwapchainAspectMask;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                vkCmdPipelineBarrier(fr.cmd,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                VkRenderingAttachmentInfo color_attachment{};
                color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                color_attachment.imageView = gImageViews[image_index];
                color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                VkRenderingInfo render_info{};
                render_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                render_info.renderArea.extent.width = (uint32_t)ImGui::GetDrawData()->DisplaySize.x;
                render_info.renderArea.extent.height = (uint32_t)ImGui::GetDrawData()->DisplaySize.y;
                render_info.layerCount = 1;
                render_info.colorAttachmentCount = 1;
                render_info.pColorAttachments = &color_attachment;

                fpBeginRendering(fr.cmd, &render_info);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fr.cmd);
                fpEndRendering(fr.cmd);

                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = 0;
                vkCmdPipelineBarrier(fr.cmd,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            else
#endif
            {
                if (gFrames[image_index].fb == VK_NULL_HANDLE)
                {
                    VkImageView attachment = gImageViews[image_index];
                    VkFramebufferCreateInfo fb_info{};
                    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                    fb_info.renderPass = gRenderPass;
                    fb_info.attachmentCount = 1;
                    fb_info.pAttachments = &attachment;
                    fb_info.width = (uint32_t)ImGui::GetDrawData()->DisplaySize.x;
                    fb_info.height = (uint32_t)ImGui::GetDrawData()->DisplaySize.y;
                    fb_info.layers = 1;
                    res = vkCreateFramebuffer(gDevice, &fb_info, nullptr, &gFrames[image_index].fb);
                    if (res != VK_SUCCESS)
                    {
                        DebugLog("[vulkanhook] vkCreateFramebuffer failed: %d\n", res);
                        return res;
                    }
                }

                VkRenderPassBeginInfo rp_begin{};
                rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rp_begin.renderPass = gRenderPass;
                rp_begin.framebuffer = gFrames[image_index].fb;
                rp_begin.renderArea.extent.width = (uint32_t)ImGui::GetDrawData()->DisplaySize.x;
                rp_begin.renderArea.extent.height = (uint32_t)ImGui::GetDrawData()->DisplaySize.y;
                vkCmdBeginRenderPass(fr.cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fr.cmd);
                vkCmdEndRenderPass(fr.cmd);
            }
            res = vkEndCommandBuffer(fr.cmd);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkEndCommandBuffer failed: %d\n", res);
                return res;
            }

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &fr.cmd;
            res = vkQueueSubmit(gQueue, 1, &submit, fr.fence);
            if (res != VK_SUCCESS)
            {
                DebugLog("[vulkanhook] vkQueueSubmit failed: %d\n", res);
                return res;
            }

            gFrameIndex = (gFrameIndex + 1) % gFrames.size();
        }

        VkResult pres = oQueuePresentKHR(queue, pPresentInfo);
        if (pres == VK_SUCCESS && !gWin32Initialized && globals::mainWindow)
        {
            inputhook::Init(globals::mainWindow);
            ImGui_ImplWin32_Init(globals::mainWindow);
            gWin32Initialized = true;
        }
        if (pres != VK_SUCCESS)
            DebugLog("[vulkanhook] vkQueuePresentKHR failed: %d\n", pres);
        return pres;
    }

    void Init()
    {
        DebugLog("[vulkanhook] Init starting\n");
        HMODULE mod = GetModuleHandleA("vulkan-1.dll");
        if (!mod)
        {
            DebugLog("[vulkanhook] vulkan-1.dll not found\n");
            return;
        }
        oCreateInstance       = (PFN_vkCreateInstance)GetProcAddress(mod, "vkCreateInstance");
        oCreateDevice         = (PFN_vkCreateDevice)GetProcAddress(mod, "vkCreateDevice");
        oGetDeviceProcAddr    = (PFN_vkGetDeviceProcAddr)GetProcAddress(mod, "vkGetDeviceProcAddr");
        oGetInstanceProcAddr  = (PFN_vkGetInstanceProcAddr)GetProcAddress(mod, "vkGetInstanceProcAddr");
        oQueuePresentKHR      = (PFN_vkQueuePresentKHR)GetProcAddress(mod, "vkQueuePresentKHR");
        oCreateSwapchainKHR   = (PFN_vkCreateSwapchainKHR)GetProcAddress(mod, "vkCreateSwapchainKHR");
        oCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)GetProcAddress(mod, "vkCreateWin32SurfaceKHR");
        oGetDeviceQueue       = (PFN_vkGetDeviceQueue)GetProcAddress(mod, "vkGetDeviceQueue");
        MH_STATUS mh;
        if (oCreateInstance)
        {
            auto target = oCreateInstance;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkCreateInstance), reinterpret_cast<void**>(&oCreateInstance));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkCreateInstance failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkCreateInstance failed: %s\n", MH_StatusToString(mh));
        }
        if (oCreateDevice)
        {
            auto target = oCreateDevice;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkCreateDevice), reinterpret_cast<void**>(&oCreateDevice));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkCreateDevice failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkCreateDevice failed: %s\n", MH_StatusToString(mh));
        }
        if (oGetDeviceProcAddr)
        {
            auto target = oGetDeviceProcAddr;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkGetDeviceProcAddr), reinterpret_cast<void**>(&oGetDeviceProcAddr));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkGetDeviceProcAddr failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkGetDeviceProcAddr failed: %s\n", MH_StatusToString(mh));
        }
        if (oGetInstanceProcAddr)
        {
            auto target = oGetInstanceProcAddr;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkGetInstanceProcAddr), reinterpret_cast<void**>(&oGetInstanceProcAddr));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkGetInstanceProcAddr failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkGetInstanceProcAddr failed: %s\n", MH_StatusToString(mh));
        }
        if (oGetDeviceQueue)
        {
            auto target = oGetDeviceQueue;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkGetDeviceQueue), reinterpret_cast<void**>(&oGetDeviceQueue));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkGetDeviceQueue failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkGetDeviceQueue failed: %s\n", MH_StatusToString(mh));
        }
        if (oQueuePresentKHR)
        {
            auto target = oQueuePresentKHR;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkQueuePresentKHR), reinterpret_cast<void**>(&oQueuePresentKHR));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkQueuePresentKHR failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkQueuePresentKHR failed: %s\n", MH_StatusToString(mh));
        }
        if (oCreateSwapchainKHR)
        {
            auto target = oCreateSwapchainKHR;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkCreateSwapchainKHR), reinterpret_cast<void**>(&oCreateSwapchainKHR));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkCreateSwapchainKHR failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkCreateSwapchainKHR failed: %s\n", MH_StatusToString(mh));
        }
        if (oCreateWin32SurfaceKHR)
        {
            auto target = oCreateWin32SurfaceKHR;
            mh = MH_CreateHook(target, reinterpret_cast<void*>(hook_vkCreateWin32SurfaceKHR), reinterpret_cast<void**>(&oCreateWin32SurfaceKHR));
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_CreateHook vkCreateWin32SurfaceKHR failed: %s\n", MH_StatusToString(mh));
            mh = MH_EnableHook(target);
            if (mh != MH_OK)
                DebugLog("[vulkanhook] MH_EnableHook vkCreateWin32SurfaceKHR failed: %s\n", MH_StatusToString(mh));
        }
        if (oGetDeviceQueue && !gDeviceMap.empty())
        {
            for (const auto& kv : gDeviceMap)
            {
                VkQueue q = VK_NULL_HANDLE;
                oGetDeviceQueue(kv.first, kv.second.queueFamily, 0, &q);
                if (q)
                    HookQueuePresent(kv.first, q);
            }
        }
        else if (gDevice != VK_NULL_HANDLE && gQueue != VK_NULL_HANDLE)
        {
            HookQueuePresent(gDevice, gQueue);
        }
        else if (oGetDeviceQueue && oGetDeviceProcAddr)
        {
            DWORD processes[1024];
            DWORD bytes = 0;
            if (EnumProcesses(processes, sizeof(processes), &bytes))
            {
                DWORD current = GetCurrentProcessId();
                for (DWORD i = 0; i < bytes / sizeof(DWORD); ++i)
                {
                    if (processes[i] != current)
                        continue;

                    HMODULE mods[1024];
                    DWORD   cbMods = 0;
                    EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cbMods);

                    SYSTEM_INFO si{};
                    GetSystemInfo(&si);
                    MEMORY_BASIC_INFORMATION mbi{};
                    uint8_t* addr = (uint8_t*)si.lpMinimumApplicationAddress;
                    while (addr < (uint8_t*)si.lpMaximumApplicationAddress)
                    {
                        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi))
                        {
                            bool readable = (mbi.State == MEM_COMMIT) &&
                                !(mbi.Protect & PAGE_GUARD) &&
                                (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                                                PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                                                PAGE_EXECUTE_READ | PAGE_EXECUTE_WRITECOPY));
                            if (readable)
                            {
                                uint8_t* start = (uint8_t*)mbi.BaseAddress;
                                uint8_t* end   = start + mbi.RegionSize;
                                for (uint8_t* p = start; p < end; p += sizeof(void*))
                                {
                                    if (*(void**)p == (void*)oGetDeviceProcAddr)
                                    {
                                        VkDevice dev = (VkDevice)p;
                                        if (!IsPlausibleDevice(dev))
                                        {
                                            DebugLog("[vulkanhook] Rejected candidate VkDevice %p during memory scan\n", dev);
                                            continue;
                                        }
                                        VkQueue  q   = VK_NULL_HANDLE;
                                        __try
                                        {
                                            oGetDeviceQueue(dev, 0, 0, &q);
                                        }
                                        __except (EXCEPTION_EXECUTE_HANDLER)
                                        {
                                            DebugLog("[vulkanhook] Exception while querying queue for VkDevice %p\n", dev);
                                        }
                                        if (q)
                                        {
                                            HookQueuePresent(dev, q);
                                            gDeviceMap[dev] = { VK_NULL_HANDLE, 0 };
                                            addr = (uint8_t*)si.lpMaximumApplicationAddress;
                                            break;
                                        }
                                        else
                                        {
                                            DebugLog("[vulkanhook] No queue returned for VkDevice %p during memory scan\n", dev);
                                        }
                                    }
                                }
                            }
                            addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
                        }
                        else
                        {
                            addr += si.dwPageSize;
                        }
                    }
                }
            }
        }
        DebugLog("[vulkanhook] Hooks placed for Vulkan procs\n");
    }

    void release()
    {
        DebugLog("[vulkanhook] Releasing resources\n");
        if (globals::mainWindow)
            inputhook::Remove(globals::mainWindow);
        if (gWin32Initialized)
            ImGui_ImplWin32_Shutdown();
        gWin32Initialized = false;

        if (gInitialized)
        {
            ImGui_ImplVulkan_Shutdown();
            ImGui::DestroyContext();
            gInitialized = false;
        }

        if (gDevice != VK_NULL_HANDLE)
        {
            VkResult res = vkDeviceWaitIdle(gDevice);
            if (res != VK_SUCCESS)
                DebugLog("[vulkanhook] vkDeviceWaitIdle failed during release: %d\n", res);
            DestroyFrameResources();
            if (gCommandPool) vkDestroyCommandPool(gDevice, gCommandPool, nullptr);
            if (gDescriptorPool) vkDestroyDescriptorPool(gDevice, gDescriptorPool, nullptr);
            for (auto view : gImageViews)
                vkDestroyImageView(gDevice, view, nullptr);
            if (gRenderPass) vkDestroyRenderPass(gDevice, gRenderPass, nullptr);
        }

        gImageViews.clear();
        gSwapchainImages.clear();
        gDescriptorPool = VK_NULL_HANDLE;
        gCommandPool = VK_NULL_HANDLE;
        gRenderPass = VK_NULL_HANDLE;
        gDevice = VK_NULL_HANDLE;
        gInstance = VK_NULL_HANDLE;
        gPhysicalDevice = VK_NULL_HANDLE;
        gQueue = VK_NULL_HANDLE;
        gQueueFamily = 0;
        gDeviceMap.clear();

        if (oQueuePresentKHR) {
            MH_DisableHook((void*)oQueuePresentKHR);
            MH_RemoveHook((void*)oQueuePresentKHR);
            oQueuePresentKHR = nullptr;
        }
        if (oCreateSwapchainKHR) {
            MH_DisableHook((void*)oCreateSwapchainKHR);
            MH_RemoveHook((void*)oCreateSwapchainKHR);
            oCreateSwapchainKHR = nullptr;
        }
        if (oCreateWin32SurfaceKHR) {
            MH_DisableHook((void*)oCreateWin32SurfaceKHR);
            MH_RemoveHook((void*)oCreateWin32SurfaceKHR);
            oCreateWin32SurfaceKHR = nullptr;
        }
        if (oGetDeviceProcAddr) {
            MH_DisableHook((void*)oGetDeviceProcAddr);
            MH_RemoveHook((void*)oGetDeviceProcAddr);
            oGetDeviceProcAddr = nullptr;
        }
        if (oGetInstanceProcAddr) {
            MH_DisableHook((void*)oGetInstanceProcAddr);
            MH_RemoveHook((void*)oGetInstanceProcAddr);
            oGetInstanceProcAddr = nullptr;
        }
        if (oGetDeviceQueue) {
            MH_DisableHook((void*)oGetDeviceQueue);
            MH_RemoveHook((void*)oGetDeviceQueue);
            oGetDeviceQueue = nullptr;
        }
        if (oCreateInstance) {
            MH_DisableHook((void*)oCreateInstance);
            MH_RemoveHook((void*)oCreateInstance);
            oCreateInstance = nullptr;
        }
        if (oCreateDevice) {
            MH_DisableHook((void*)oCreateDevice);
            MH_RemoveHook((void*)oCreateDevice);
            oCreateDevice = nullptr;
        }
    }

    bool IsInitialized()
    {
        return gInitialized;
    }
}

