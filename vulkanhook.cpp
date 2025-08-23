#include "stdafx.h"

namespace hooks_vk {
    PFN_vkCreateInstance       oCreateInstance       = nullptr;
    PFN_vkCreateDevice         oCreateDevice         = nullptr;
    PFN_vkQueuePresentKHR      oQueuePresentKHR      = nullptr;
    PFN_vkCreateSwapchainKHR   oCreateSwapchainKHR   = nullptr;
    static PFN_vkGetInstanceProcAddr oGetInstanceProcAddr = nullptr;
    static PFN_vkGetDeviceProcAddr   oGetDeviceProcAddr   = nullptr;
    static PFN_vkGetDeviceQueue      oGetDeviceQueue      = nullptr;
    typedef VkPhysicalDevice(VKAPI_PTR* PFN_vkGetPhysicalDevice)(VkDevice device);
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
    static bool             gInitialized    = false;
    static VkRenderPass     gRenderPass     = VK_NULL_HANDLE;
    static VkFormat         gSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    static VkImageAspectFlags gSwapchainAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

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

    static void CreateDescriptorPool()
    {
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
        vkCreateDescriptorPool(gDevice, &pool_info, nullptr, &gDescriptorPool);
    }

    static void CreateCommandPool()
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = gQueueFamily;
        vkCreateCommandPool(gDevice, &info, nullptr, &gCommandPool);
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

    static void CreateRenderPass()
    {
        if (gRenderPass)
            return;
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

        vkCreateRenderPass(gDevice, &rp_info, nullptr, &gRenderPass);
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

    static void CreateFrameResources(uint32_t count)
    {
        gFrames.resize(count);
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = gCommandPool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = count;
        std::vector<VkCommandBuffer> cmds(count);
        vkAllocateCommandBuffers(gDevice, &alloc_info, cmds.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            gFrames[i].cmd = cmds[i];
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(gDevice, &fence_info, nullptr, &gFrames[i].fence);
        }
    }

    PFN_vkVoidFunction VKAPI_PTR hook_vkGetDeviceProcAddr(VkDevice device, const char* pName)
    {
        PFN_vkVoidFunction func = oGetDeviceProcAddr(device, pName);
        if (!func)
            return func;

        if (strcmp(pName, "vkQueuePresentKHR") == 0)
        {
            if (oQueuePresentKHR == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkQueuePresentKHR, (void**)&oQueuePresentKHR);
                MH_EnableHook((void*)func);
            }
            if (gDevice == VK_NULL_HANDLE)
                gDevice = device;
            if (gQueue == VK_NULL_HANDLE)
            {
                PFN_vkGetDeviceQueue getQueue = (PFN_vkGetDeviceQueue)oGetDeviceProcAddr(device, "vkGetDeviceQueue");
                if (getQueue)
                    getQueue(device, 0, 0, &gQueue);
            }
        }
        else if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        {
            if (oCreateSwapchainKHR == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkCreateSwapchainKHR, (void**)&oCreateSwapchainKHR);
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
            if (oQueuePresentKHR == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkQueuePresentKHR, (void**)&oQueuePresentKHR);
                MH_EnableHook((void*)func);
            }
        }
        else if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        {
            if (oCreateSwapchainKHR == nullptr)
            {
                MH_CreateHook((void*)func, (void*)hook_vkCreateSwapchainKHR, (void**)&oCreateSwapchainKHR);
                MH_EnableHook((void*)func);
            }
        }
        return func;
    }

    VkResult VKAPI_PTR hook_vkCreateSwapchainKHR(VkDevice device,
                                                 const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkSwapchainKHR* pSwapchain)
    {
        if (pCreateInfo)
        {
            gSwapchainFormat = pCreateInfo->imageFormat;
            gSwapchainAspectMask = GetAspectMask(gSwapchainFormat);
        }
        return oCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    VkResult VKAPI_PTR hook_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkInstance* pInstance)
    {
        VkResult res = oCreateInstance(pCreateInfo, pAllocator, pInstance);
        if (res == VK_SUCCESS)
        {
            gInstance = *pInstance;
        }
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
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, nullptr);
            std::vector<VkExtensionProperties> props(ext_count);
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, props.data());
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
            dyn_features.pNext = create_info.pNext;
            create_info.pNext = &dyn_features;
        }

        VkResult res = oCreateDevice(physicalDevice, &create_info, pAllocator, pDevice);
        if (res != VK_SUCCESS)
            return res;

        gUseDynamicRendering = has_dynamic;
        gDevice = *pDevice;
        gPhysicalDevice = physicalDevice;
        gQueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;

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

        if (oQueuePresentKHR == nullptr)
        {
            oQueuePresentKHR = (PFN_vkQueuePresentKHR)oGetDeviceProcAddr(gDevice, "vkQueuePresentKHR");
            if (oQueuePresentKHR)
            {
                MH_CreateHook((void*)oQueuePresentKHR, (void*)hook_vkQueuePresentKHR, (void**)&oQueuePresentKHR);
                MH_EnableHook((void*)oQueuePresentKHR);
            }
        }

        CreateDescriptorPool();
        CreateCommandPool();
        DebugLog("[vulkanhook] Device initialized.\n");
        return res;
    }

    VkResult VKAPI_PTR hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        if (gDevice == VK_NULL_HANDLE || gQueue == VK_NULL_HANDLE)
        {
            void*** dispatch_ptr = reinterpret_cast<void***>(queue);
            if (dispatch_ptr && *dispatch_ptr)
            {
                PFN_vkGetDeviceProcAddr getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>((*dispatch_ptr)[0]);
                if (!oGetDeviceProcAddr)
                    oGetDeviceProcAddr = getDeviceProcAddr;

                VkDevice device = VK_NULL_HANDLE;
                void** queue_ptr = reinterpret_cast<void**>(queue);
                if (queue_ptr)
                    device = reinterpret_cast<VkDevice>(queue_ptr[1]);

                if (device != VK_NULL_HANDLE)
                {
                    gDevice = device;
                    if (!oGetDeviceQueue)
                        oGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(oGetDeviceProcAddr(gDevice, "vkGetDeviceQueue"));

                    PFN_vkGetPhysicalDevice getPhysicalDevice =
                        reinterpret_cast<PFN_vkGetPhysicalDevice>(oGetDeviceProcAddr(gDevice, "vkGetPhysicalDevice"));
                    if (getPhysicalDevice)
                        gPhysicalDevice = getPhysicalDevice(gDevice);

                    if (gPhysicalDevice != VK_NULL_HANDLE && oGetDeviceQueue)
                    {
                        uint32_t family_count = 0;
                        vkGetPhysicalDeviceQueueFamilyProperties(gPhysicalDevice, &family_count, nullptr);
                        for (uint32_t i = 0; i < family_count; ++i)
                        {
                            VkQueue q = VK_NULL_HANDLE;
                            oGetDeviceQueue(gDevice, i, 0, &q);
                            if (q == queue)
                            {
                                gQueueFamily = i;
                                break;
                            }
                        }
                    }
                }
            }

            if (gQueue == VK_NULL_HANDLE)
                gQueue = queue;
        }

        if (gQueue == VK_NULL_HANDLE)
            gQueue = queue;

        if (pPresentInfo && pPresentInfo->swapchainCount > 0 &&
            gSwapchain != VK_NULL_HANDLE && pPresentInfo->pSwapchains[0] != gSwapchain)
        {
            vkDeviceWaitIdle(gDevice);
            if (globals::mainWindow)
                inputhook::Remove(globals::mainWindow);
            if (gInitialized)
            {
                ImGui_ImplVulkan_Shutdown();
                if (globals::mainWindow)
                    ImGui_ImplWin32_Shutdown();
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
            if (gDescriptorPool == VK_NULL_HANDLE)
                CreateDescriptorPool();
            if (gCommandPool == VK_NULL_HANDLE)
                CreateCommandPool();
            gSwapchain = pPresentInfo->pSwapchains[0];
            vkGetSwapchainImagesKHR(gDevice, gSwapchain, &gImageCount, nullptr);
            gSwapchainImages.resize(gImageCount);
            vkGetSwapchainImagesKHR(gDevice, gSwapchain, &gImageCount, gSwapchainImages.data());
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
                vkCreateImageView(gDevice, &view_info, nullptr, &gImageViews[i]);
            }
            if (!gUseDynamicRendering)
                CreateRenderPass();
            DestroyFrameResources();
            CreateFrameResources(gImageCount);

            ImGui::CreateContext();
            if (globals::mainWindow)
            {
                ImGui_ImplWin32_Init(globals::mainWindow);
                inputhook::Init(globals::mainWindow);
            }
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
            vkWaitForFences(gDevice, 1, &fr.fence, VK_TRUE, UINT64_MAX);
            vkResetFences(gDevice, 1, &fr.fence);
            vkResetCommandBuffer(fr.cmd, 0);

            ImGui_ImplVulkan_NewFrame();
            if (globals::mainWindow)
                ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (menu::isOpen)
                menu::Init();
            ImGui::EndFrame();
            ImGui::Render();

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(fr.cmd, &begin_info);
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
                    vkCreateFramebuffer(gDevice, &fb_info, nullptr, &gFrames[image_index].fb);
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
            vkEndCommandBuffer(fr.cmd);

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &fr.cmd;
            vkQueueSubmit(gQueue, 1, &submit, fr.fence);

            gFrameIndex = (gFrameIndex + 1) % gFrames.size();
        }

        return oQueuePresentKHR(queue, pPresentInfo);
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
        if (oCreateInstance)
        {
            MH_CreateHook((void*)oCreateInstance, (void*)hook_vkCreateInstance, (void**)&oCreateInstance);
            MH_EnableHook((void*)oCreateInstance);
        }
        if (oCreateDevice)
        {
            MH_CreateHook((void*)oCreateDevice, (void*)hook_vkCreateDevice, (void**)&oCreateDevice);
            MH_EnableHook((void*)oCreateDevice);
        }
        if (oGetDeviceProcAddr)
        {
            MH_CreateHook((void*)oGetDeviceProcAddr, (void*)hook_vkGetDeviceProcAddr, (void**)&oGetDeviceProcAddr);
            MH_EnableHook((void*)oGetDeviceProcAddr);
        }
        if (oGetInstanceProcAddr)
        {
            MH_CreateHook((void*)oGetInstanceProcAddr, (void*)hook_vkGetInstanceProcAddr, (void**)&oGetInstanceProcAddr);
            MH_EnableHook((void*)oGetInstanceProcAddr);
        }
        if (oQueuePresentKHR)
        {
            MH_CreateHook((void*)oQueuePresentKHR, (void*)hook_vkQueuePresentKHR, (void**)&oQueuePresentKHR);
            MH_EnableHook((void*)oQueuePresentKHR);
        }
        if (oCreateSwapchainKHR)
        {
            MH_CreateHook((void*)oCreateSwapchainKHR, (void*)hook_vkCreateSwapchainKHR, (void**)&oCreateSwapchainKHR);
            MH_EnableHook((void*)oCreateSwapchainKHR);
        }
        DebugLog("[vulkanhook] Hooks placed for Vulkan procs\n");
    }

    void release()
    {
        DebugLog("[vulkanhook] Releasing resources\n");
        if (globals::mainWindow)
            inputhook::Remove(globals::mainWindow);

        if (gInitialized)
        {
            ImGui_ImplVulkan_Shutdown();
            if (globals::mainWindow)
                ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            gInitialized = false;
        }

        if (gDevice != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(gDevice);
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

