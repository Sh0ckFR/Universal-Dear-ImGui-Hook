// Host application rendering a solid color with one graphics API, then
// starting the hook in-process and checking that the overlay gets drawn and
// torn down cleanly.
#include "uih.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

namespace {

constexpr int kSkip = 77;
constexpr float kClear[4] = {0.1f, 0.2f, 0.3f, 1.0f};
constexpr int kWidth = 800;
constexpr int kHeight = 600;

struct Options {
    std::string api;
    std::string dll;
    int injectAt = 30;
    bool warp = false;
    bool capture = false;
    bool deviceProcAddr = false;
};

struct Host {
    virtual ~Host() = default;
    virtual bool Init(HWND window, const Options& options) = 0;
    virtual void Frame() = 0;
};

template <typename T>
void Release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

// D3D9 -------------------------------------------------------------------

struct D3D9Host : Host {
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;

    bool Init(HWND window, const Options&) override {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d)
            return false;
        D3DPRESENT_PARAMETERS present{};
        present.Windowed = TRUE;
        present.SwapEffect = D3DSWAPEFFECT_DISCARD;
        present.hDeviceWindow = window;
        return SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
    }

    void Frame() override {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(26, 51, 77), 1.0f, 0);
        device->BeginScene();
        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);
    }

    ~D3D9Host() override {
        Release(device);
        Release(d3d);
    }
};

// D3D10 ------------------------------------------------------------------

DXGI_SWAP_CHAIN_DESC SwapChainDesc(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = kWidth;
    desc.BufferDesc.Height = kHeight;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    return desc;
}

struct D3D10Host : Host {
    ID3D10Device* device = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D10RenderTargetView* target = nullptr;

    bool Init(HWND window, const Options& options) override {
        DXGI_SWAP_CHAIN_DESC desc = SwapChainDesc(window);
        D3D10_DRIVER_TYPE driver = options.warp ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE;
        if (FAILED(D3D10CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, D3D10_SDK_VERSION, &desc, &swapChain, &device)))
            return false;
        ID3D10Texture2D* buffer = nullptr;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
        HRESULT hr = device->CreateRenderTargetView(buffer, nullptr, &target);
        buffer->Release();
        return SUCCEEDED(hr);
    }

    void Frame() override {
        device->ClearRenderTargetView(target, kClear);
        swapChain->Present(1, 0);
    }

    ~D3D10Host() override {
        Release(target);
        Release(swapChain);
        Release(device);
    }
};

// D3D11 ------------------------------------------------------------------

struct D3D11Host : Host {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* target = nullptr;

    bool Init(HWND window, const Options& options) override {
        DXGI_SWAP_CHAIN_DESC desc = SwapChainDesc(window);
        D3D_DRIVER_TYPE driver = options.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context)))
            return false;
        ID3D11Texture2D* buffer = nullptr;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
        HRESULT hr = device->CreateRenderTargetView(buffer, nullptr, &target);
        buffer->Release();
        return SUCCEEDED(hr);
    }

    void Frame() override {
        context->ClearRenderTargetView(target, kClear);
        swapChain->Present(1, 0);
    }

    ~D3D11Host() override {
        Release(target);
        Release(swapChain);
        Release(context);
        Release(device);
    }
};

// D3D12 ------------------------------------------------------------------

struct D3D12Host : Host {
    static constexpr UINT kBuffers = 2;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swapChain = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* buffers[kBuffers] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE targets[kBuffers] = {};
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;

    bool Init(HWND window, const Options& options) override {
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        IDXGIAdapter* adapter = nullptr;
        if (options.warp)
            factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        Release(adapter);
        if (FAILED(hr)) {
            factory->Release();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBuffers;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* swapChain1 = nullptr;
        hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &swapChain1);
        factory->Release();
        if (FAILED(hr))
            return false;
        swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain));
        swapChain1->Release();

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = kBuffers;
        device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < kBuffers; ++i) {
            swapChain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
            device->CreateRenderTargetView(buffers[i], nullptr, handle);
            targets[i] = handle;
            handle.ptr += increment;
        }
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&commandList));
        commandList->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Frame() override {
        UINT index = swapChain->GetCurrentBackBufferIndex();
        allocator->Reset();
        commandList->Reset(allocator, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffers[index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);
        commandList->ClearRenderTargetView(targets[index], kClear, 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &barrier);
        commandList->Close();
        ID3D12CommandList* lists[] = {commandList};
        queue->ExecuteCommandLists(1, lists);
        swapChain->Present(1, 0);
        queue->Signal(fence, ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    ~D3D12Host() override {
        if (fenceEvent)
            CloseHandle(fenceEvent);
        Release(fence);
        Release(commandList);
        Release(allocator);
        for (ID3D12Resource*& buffer : buffers)
            Release(buffer);
        Release(heap);
        Release(swapChain);
        Release(queue);
        Release(device);
    }
};

// Vulkan -----------------------------------------------------------------

#define HOST_VK_INSTANCE_FUNCTIONS(X)            \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkCreateWin32SurfaceKHR)                   \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)

#define HOST_VK_DEVICE_FUNCTIONS(X) \
    X(vkGetDeviceQueue)             \
    X(vkCreateSwapchainKHR)         \
    X(vkGetSwapchainImagesKHR)      \
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
    std::vector<VkImage> images;
    std::vector<VkSemaphore> renderDone;
    VkCommandBuffer commandBuffers[kFrames] = {};
    VkSemaphore acquired[kFrames] = {};
    VkFence fences[kFrames] = {};
    uint32_t frame = 0;

    bool Init(HWND window, const Options& options) override {
        HMODULE module = LoadLibraryW(L"vulkan-1.dll");
        if (!module)
            return false;
        getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
        auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.apiVersion = VK_API_VERSION_1_0;
        const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        if (createInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
            return false;
#define HOST_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(getInstanceProcAddr(instance, #name));
        HOST_VK_INSTANCE_FUNCTIONS(HOST_VK_LOAD)
        HOST_VK_DEVICE_FUNCTIONS(HOST_VK_LOAD)
#undef HOST_VK_LOAD

        VkWin32SurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = window;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS)
            return false;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> physicalDevices(count);
        vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data());
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
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
        VkSurfaceFormatKHR format = formats[0];
        for (const VkSurfaceFormatKHR& candidate : formats)
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM)
                format = candidate;

        VkSwapchainCreateInfoKHR swapchainInfo{};
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
        vkQueuePresentKHR(queue, &present);
        ++frame;
    }

    ~VulkanHost() override {
        if (device)
            vkDeviceWaitIdle(device);
    }
};

// Driver -----------------------------------------------------------------

void DrawTestMenu() {
    if (!uih::IsMenuOpen())
        return;
    ImGui::SetNextWindowPos(ImVec2(50, 50));
    ImGui::SetNextWindowSize(ImVec2(500, 400));
    ImGui::Begin("uih host", nullptr, ImGuiWindowFlags_NoResize);
    ImGui::Text("Backend: %s", uih::BackendName(uih::ActiveBackend()));
    ImGui::Button("Button", ImVec2(400, 200));
    ImGui::End();
}

int CountOverlayPixels(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    POINT origin{};
    ClientToScreen(window, &origin);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);

    int count = 0;
    const uint8_t* pixels = static_cast<const uint8_t*>(bits);
    for (int i = 0; i < width * height; ++i) {
        const uint8_t* p = pixels + i * 4;
        if (abs(p[2] - 26) > 12 || abs(p[1] - 51) > 12 || abs(p[0] - 77) > 12)
            ++count;
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return count;
}

uih::Backend ExpectedBackend(const std::string& api) {
    if (api == "d3d9") return uih::Backend::D3D9;
    if (api == "d3d10") return uih::Backend::D3D10;
    if (api == "d3d11") return uih::Backend::D3D11;
    if (api == "d3d12") return uih::Backend::D3D12;
    if (api == "vulkan") return uih::Backend::Vulkan;
    return uih::Backend::None;
}

std::unique_ptr<Host> CreateHost(const std::string& api) {
    if (api == "d3d9") return std::make_unique<D3D9Host>();
    if (api == "d3d10") return std::make_unique<D3D10Host>();
    if (api == "d3d11") return std::make_unique<D3D11Host>();
    if (api == "d3d12") return std::make_unique<D3D12Host>();
    if (api == "vulkan") return std::make_unique<VulkanHost>();
    return nullptr;
}

void PumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void StartHook() {
    uih::Config config;
    config.render = DrawTestMenu;
    config.debugLog = true;
    uih::Start(config);
}

int Fail(const char* message) {
    printf("FAIL: %s\n", message);
    return 1;
}

void HoldKey(int key, bool down) {
    keybd_event(static_cast<BYTE>(key), 0, down ? 0 : KEYEVENTF_KEYUP, 0);
}

// Loads the example DLL like an injector would and drives it with its hotkeys:
// INSERT opens the menu, F11 unloads the DLL.
int RunDllMode(HWND window, Host& host, const Options& options) {
    std::wstring path(options.dll.begin(), options.dll.end());
    size_t separator = path.find_last_of(L"\\/");
    std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
    for (int frame = 0;; ++frame) {
        PumpMessages();
        if (frame == options.injectAt && !LoadLibraryW(path.c_str()))
            return Fail("LoadLibrary failed");
        if (frame == options.injectAt + 90) {
            SetForegroundWindow(window);
            HoldKey(VK_INSERT, true);
        }
        if (frame == options.injectAt + 95)
            HoldKey(VK_INSERT, false);
        if (frame == options.injectAt + 180) {
            int pixels = CountOverlayPixels(window);
            printf("%d overlay pixels captured\n", pixels);
            if (pixels < kWidth * kHeight / 100)
                return Fail("overlay is not visible on screen");
            HoldKey(VK_F11, true);
        }
        if (frame == options.injectAt + 185)
            HoldKey(VK_F11, false);
        if (frame == options.injectAt + 400) {
            if (GetModuleHandleW(name.c_str()))
                return Fail("DLL did not unload");
            printf("PASS: %s via %s\n", options.api.c_str(), options.dll.c_str());
            return 0;
        }
        host.Frame();
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--api" && i + 1 < argc) options.api = argv[++i];
        else if (arg == "--inject-at" && i + 1 < argc) options.injectAt = atoi(argv[++i]);
        else if (arg == "--dll" && i + 1 < argc) options.dll = argv[++i];
        else if (arg == "--warp") options.warp = true;
        else if (arg == "--capture") options.capture = true;
        else if (arg == "--device-proc-addr") options.deviceProcAddr = true;
    }
    uih::Backend expected = ExpectedBackend(options.api);
    if (expected == uih::Backend::None) {
        printf("usage: uih_host --api d3d9|d3d10|d3d11|d3d12|vulkan [--inject-at N] [--warp] [--capture] [--device-proc-addr] [--dll path]\n");
        return 2;
    }

    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW), CS_OWNDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"uih_host", nullptr};
    RegisterClassExW(&windowClass);
    RECT rect{0, 0, kWidth, kHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, L"uih_host", L"uih host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    PumpMessages();

    if (options.injectAt == 0 && options.dll.empty())
        StartHook();

    std::unique_ptr<Host> host = CreateHost(options.api);
    if (!host->Init(window, options)) {
        printf("SKIP: %s is not available on this machine\n", options.api.c_str());
        uih::Stop();
        return kSkip;
    }
    if (!options.dll.empty()) {
        int result = RunDllMode(window, *host, options);
        host.reset();
        DestroyWindow(window);
        return result;
    }

    int frame = 0;
    int activeFrame = -1;
    int stopFrame = -1;
    int result = 0;
    while (result == 0) {
        PumpMessages();
        if (frame == options.injectAt && frame > 0)
            StartHook();
        host->Frame();
        ++frame;

        if (activeFrame < 0) {
            if (uih::ActiveBackend() == expected) {
                activeFrame = frame;
                printf("%s backend active after %d frames\n", options.api.c_str(), frame - options.injectAt);
                uih::SetMenuOpen(true);
            } else if (frame > options.injectAt + 600) {
                result = Fail("backend was not detected");
            }
        } else if (stopFrame < 0 && frame == activeFrame + 60) {
            if (!ImGui::GetCurrentContext() || ImGui::GetIO().MetricsRenderVertices == 0) {
                result = Fail("ImGui did not render any geometry");
                break;
            }
            printf("ImGui rendered %d vertices\n", ImGui::GetIO().MetricsRenderVertices);
            if (options.capture) {
                int pixels = CountOverlayPixels(window);
                printf("%d overlay pixels captured\n", pixels);
                if (pixels < kWidth * kHeight / 100) {
                    result = Fail("overlay is not visible on screen");
                    break;
                }
            }
            uih::Stop();
            stopFrame = frame;
            if (ImGui::GetCurrentContext() || uih::ActiveBackend() != uih::Backend::None)
                result = Fail("hook did not release its resources");
        } else if (stopFrame >= 0 && frame == stopFrame + 60) {
            break;
        }
    }

    uih::Stop();
    host.reset();
    DestroyWindow(window);
    if (result == 0)
        printf("PASS: %s\n", options.api.c_str());
    return result;
}
