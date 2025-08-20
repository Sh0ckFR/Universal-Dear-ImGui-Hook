#pragma once

#include <vulkan/vulkan.h>

namespace hooks_vk {
    extern PFN_vkCreateInstance        oCreateInstance;
    extern PFN_vkCreateDevice          oCreateDevice;
    extern PFN_vkQueuePresentKHR       oQueuePresentKHR;

    VkResult VKAPI_PTR hook_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkInstance* pInstance);
    VkResult VKAPI_PTR hook_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                           const VkDeviceCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkDevice* pDevice);
    VkResult VKAPI_PTR hook_vkQueuePresentKHR(VkQueue queue,
                                              const VkPresentInfoKHR* pPresentInfo);

    void Init();
    void release();
}

