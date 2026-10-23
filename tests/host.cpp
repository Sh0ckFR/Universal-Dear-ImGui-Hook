// Host application rendering a solid color with one graphics API, then
// starting the hook in-process and checking that the overlay gets drawn and
// torn down cleanly.
#include "uih.h"

#include "host.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d9.h>
#include <dxgi1_4.h>
#include <GL/gl.h>
#define GL_APIENTRY APIENTRY
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui.h"

namespace {

template <typename T>
void Release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

// D3D9 -------------------------------------------------------------------

// Renders like many D3D9 games: several scenes per frame, one of them into a
// texture that is still bound when the frame is presented.
struct D3D9Host : Host {
    bool ex = false;
    bool srgb = false;
    IDirect3D9* d3d = nullptr;
    IDirect3D9Ex* d3dEx = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* offscreen = nullptr;
    D3DPRESENT_PARAMETERS present{};

    explicit D3D9Host(bool useEx) : ex(useEx) {}

    bool CreateTargets() {
        return SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
               SUCCEEDED(device->CreateTexture(256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr)) &&
               SUCCEEDED(texture->GetSurfaceLevel(0, &offscreen));
    }

    bool Resize(int width, int height) override {
        Release(offscreen);
        Release(texture);
        Release(backBuffer);
        present.BackBufferWidth = width;
        present.BackBufferHeight = height;
        HRESULT hr = ex ? deviceEx->ResetEx(&present, nullptr) : device->Reset(&present);
        return SUCCEEDED(hr) && CreateTargets();
    }

    UINT FullscreenRefreshRate() {
        IDirect3D9* api = ex ? d3dEx : d3d;
        UINT best = 0;
        D3DDISPLAYMODE mode{};
        for (UINT i = 0; i < api->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8); ++i)
            if (SUCCEEDED(api->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &mode)) &&
                mode.Width == static_cast<UINT>(kFullscreen[0]) && mode.Height == static_cast<UINT>(kFullscreen[1]))
                best = std::max(best, mode.RefreshRate);
        return best;
    }

    bool Fullscreen(bool on) override {
        UINT refreshRate = on ? FullscreenRefreshRate() : 0;
        if (on && !refreshRate)
            return false;
        Release(offscreen);
        Release(texture);
        Release(backBuffer);
        present.Windowed = !on;
        present.BackBufferWidth = on ? kFullscreen[0] : kWidth;
        present.BackBufferHeight = on ? kFullscreen[1] : kHeight;
        present.BackBufferFormat = on ? D3DFMT_X8R8G8B8 : D3DFMT_UNKNOWN;
        present.FullScreen_RefreshRateInHz = refreshRate;
        D3DDISPLAYMODEEX mode{sizeof(mode), present.BackBufferWidth, present.BackBufferHeight, refreshRate, D3DFMT_X8R8G8B8,
                              D3DSCANLINEORDERING_PROGRESSIVE};
        HRESULT hr = ex ? deviceEx->ResetEx(&present, on ? &mode : nullptr) : device->Reset(&present);
        return SUCCEEDED(hr) && CreateTargets();
    }

    bool Init(HWND window, const Options& options) override {
        if (options.format == "float")
            return false;
        srgb = options.format == "srgb";
        present.Windowed = TRUE;
        present.SwapEffect = D3DSWAPEFFECT_DISCARD;
        present.hDeviceWindow = window;
        if (ex) {
            if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dEx)) ||
                FAILED(d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, nullptr, &deviceEx)))
                return false;
            device = deviceEx;
            device->AddRef();
        } else {
            d3d = Direct3DCreate9(D3D_SDK_VERSION);
            if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device)))
                return false;
        }
        return CreateTargets();
    }

    void Frame() override {
        device->SetRenderTarget(0, backBuffer);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(26, 51, 77), 1.0f, 0);
        device->BeginScene();
        device->EndScene();
        device->SetRenderTarget(0, offscreen);
        device->BeginScene();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 1.0f, 0);
        device->EndScene();
        device->BeginScene();
        device->EndScene();
        if (ex)
            deviceEx->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
        else
            device->Present(nullptr, nullptr, nullptr, nullptr);
    }

    ~D3D9Host() override {
        Release(offscreen);
        Release(texture);
        Release(backBuffer);
        Release(deviceEx);
        Release(device);
        Release(d3dEx);
        Release(d3d);
    }
};

// D3D10 ------------------------------------------------------------------

DXGI_FORMAT BackBufferFormat(const Options& options) {
    if (options.format == "srgb")
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (options.format == "float")
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

DXGI_SWAP_CHAIN_DESC SwapChainDesc(HWND window, const Options& options) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = kWidth;
    desc.BufferDesc.Height = kHeight;
    desc.BufferDesc.Format = BackBufferFormat(options);
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    return desc;
}

// With UIH_TEST_D3D_DEBUG set, the D3D10, 11 and 12 hosts create their
// device with the debug layer, which also checks what the hook does on that
// device, and count the errors it stored.
bool D3DDebug() { return getenv("UIH_TEST_D3D_DEBUG") != nullptr; }

template <typename Message, typename InfoQueue>
int CountDebugErrors(InfoQueue* queue) {
    if (!queue)
        return 0;
    int errors = 0;
    UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<Message*>(buffer.data());
        // Severity 0 is corruption, 1 error.
        if (SUCCEEDED(queue->GetMessage(i, message, &size)) && static_cast<int>(message->Severity) <= 1) {
            ++errors;
            printf("DEBUG LAYER ERROR: %.*s\n", static_cast<int>(message->DescriptionByteLength), message->pDescription);
        }
    }
    queue->Release();
    return errors;
}

template <typename InfoQueue, typename Message, typename Device>
int DebugErrors(Device* device) {
    InfoQueue* queue = nullptr;
    if (!D3DDebug() || !device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        return 0;
    return CountDebugErrors<Message>(queue);
}

// Exclusive fullscreen through DXGI, as D3D10, 11 and 12 games do: the
// output's mode closest to kFullscreen, set before and after the switch.
bool DxgiFullscreen(IDXGISwapChain* swapChain, bool on) {
    if (!on)
        return SUCCEEDED(swapChain->SetFullscreenState(FALSE, nullptr));
    IDXGIOutput* output = nullptr;
    if (FAILED(swapChain->GetContainingOutput(&output)))
        return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    DXGI_MODE_DESC wanted{};
    wanted.Width = kFullscreen[0];
    wanted.Height = kFullscreen[1];
    wanted.Format = desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_MODE_DESC mode{};
    HRESULT hr = output->FindClosestMatchingMode(&wanted, &mode, nullptr);
    output->Release();
    if (FAILED(hr))
        return false;
    printf("DXGI mode %ux%u at %u/%u Hz\n", mode.Width, mode.Height, mode.RefreshRate.Numerator, mode.RefreshRate.Denominator);
    BOOL fullscreen = FALSE;
    return SUCCEEDED(swapChain->ResizeTarget(&mode)) && SUCCEEDED(swapChain->SetFullscreenState(TRUE, nullptr)) &&
           SUCCEEDED(swapChain->ResizeTarget(&mode)) && SUCCEEDED(swapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen;
}

struct D3D10Host : Host {
    ID3D10Device* device = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D10RenderTargetView* target = nullptr;

    bool CreateTarget() {
        ID3D10Texture2D* buffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
            return false;
        HRESULT hr = device->CreateRenderTargetView(buffer, nullptr, &target);
        buffer->Release();
        return SUCCEEDED(hr);
    }

    bool Resize(int width, int height) override {
        Release(target);
        return SUCCEEDED(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) && CreateTarget();
    }

    bool Fullscreen(bool on) override { return DxgiFullscreen(swapChain, on) && Resize(0, 0); }

    bool Init(HWND window, const Options& options) override {
        DXGI_SWAP_CHAIN_DESC desc = SwapChainDesc(window, options);
        D3D10_DRIVER_TYPE driver = options.warp ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE;
        UINT flags = D3DDebug() ? D3D10_CREATE_DEVICE_DEBUG : 0;
        if (FAILED(D3D10CreateDeviceAndSwapChain(nullptr, driver, nullptr, flags, D3D10_SDK_VERSION, &desc, &swapChain, &device)))
            return false;
        return CreateTarget();
    }

    void Frame() override {
        device->ClearRenderTargetView(target, kClear);
        swapChain->Present(1, 0);
    }

    int ValidationErrors() const override { return DebugErrors<ID3D10InfoQueue, D3D10_MESSAGE>(device); }

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

    bool CreateTarget() {
        ID3D11Texture2D* buffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
            return false;
        HRESULT hr = device->CreateRenderTargetView(buffer, nullptr, &target);
        buffer->Release();
        return SUCCEEDED(hr);
    }

    bool Resize(int width, int height) override {
        Release(target);
        return SUCCEEDED(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) && CreateTarget();
    }

    bool Fullscreen(bool on) override { return DxgiFullscreen(swapChain, on) && Resize(0, 0); }

    bool Init(HWND window, const Options& options) override {
        DXGI_SWAP_CHAIN_DESC desc = SwapChainDesc(window, options);
        D3D_DRIVER_TYPE driver = options.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        UINT flags = D3DDebug() ? D3D11_CREATE_DEVICE_DEBUG : 0;
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context)))
            return false;
        return CreateTarget();
    }

    void Frame() override {
        context->ClearRenderTargetView(target, kClear);
        swapChain->Present(1, 0);
    }

    int ValidationErrors() const override { return DebugErrors<ID3D11InfoQueue, D3D11_MESSAGE>(device); }

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
    // Direct queue of a second device, executing work after the presenting
    // queue every frame, like a video decoder or a tool would.
    ID3D12Device* decoyDevice = nullptr;
    ID3D12CommandQueue* decoyQueue = nullptr;
    ID3D12CommandAllocator* decoyAllocator = nullptr;
    ID3D12GraphicsCommandList* decoyList = nullptr;

    bool CreateDecoy(IDXGIFactory4* factory) {
        IDXGIAdapter* warp = nullptr;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
            return false;
        HRESULT hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&decoyDevice));
        warp->Release();
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        if (FAILED(hr) || FAILED(decoyDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&decoyQueue))) ||
            FAILED(decoyDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&decoyAllocator))) ||
            FAILED(decoyDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, decoyAllocator, nullptr, IID_PPV_ARGS(&decoyList))))
            return false;
        return SUCCEEDED(decoyList->Close());
    }

    bool Init(HWND window, const Options& options) override {
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        IDXGIAdapter* adapter = nullptr;
        if (options.warp)
            factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        ID3D12Debug* debug = nullptr;
        if (D3DDebug() && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
        }
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
        // Flip model swap chains cannot use sRGB formats.
        if (options.format == "srgb") {
            factory->Release();
            return false;
        }
        desc.Format = BackBufferFormat(options);
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBuffers;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* swapChain1 = nullptr;
        Settle(options);
        hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &swapChain1);
        // WARP is the decoy's device, which has to differ from the presenting one.
        bool decoyReady = !options.decoyQueue || (!options.warp && CreateDecoy(factory));
        factory->Release();
        if (FAILED(hr) || !decoyReady)
            return false;
        swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain));
        swapChain1->Release();

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = kBuffers;
        device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
        CreateTargets();
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&commandList));
        commandList->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void CreateTargets() {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < kBuffers; ++i) {
            swapChain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
            device->CreateRenderTargetView(buffers[i], nullptr, handle);
            targets[i] = handle;
            handle.ptr += increment;
        }
    }

    bool Resize(int width, int height) override {
        // Frame() waits for the GPU, so no buffer is in use here.
        for (ID3D12Resource*& buffer : buffers)
            Release(buffer);
        if (FAILED(swapChain->ResizeBuffers(kBuffers, width, height, DXGI_FORMAT_UNKNOWN, 0)))
            return false;
        CreateTargets();
        return true;
    }

    bool Fullscreen(bool on) override { return DxgiFullscreen(swapChain, on) && Resize(0, 0); }

    // The mingw-w64 headers of Ubuntu do not declare ID3D12Device5.
    bool RemoveDevice() override {
#ifdef __ID3D12Device5_INTERFACE_DEFINED__
        ID3D12Device5* device5 = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))))
            return false;
        device5->RemoveDevice();
        device5->Release();
        return true;
#else
        return false;
#endif
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
        if (decoyQueue) {
            ID3D12CommandList* decoyLists[] = {decoyList};
            decoyQueue->ExecuteCommandLists(1, decoyLists);
        }
        swapChain->Present(1, 0);
        queue->Signal(fence, ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    int ValidationErrors() const override { return DebugErrors<ID3D12InfoQueue, D3D12_MESSAGE>(device); }

    ~D3D12Host() override {
        Release(decoyList);
        Release(decoyAllocator);
        Release(decoyQueue);
        Release(decoyDevice);
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

// OpenGL -----------------------------------------------------------------

struct OpenGLHost : Host {
    HWND window = nullptr;
    HDC hdc = nullptr;
    HGLRC context = nullptr;
    // Framebuffer object and pixel unpack buffer left bound by the
    // bound framebuffer scenario.
    GLuint framebuffer = 0;
    GLuint unpackBuffer = 0;
    void(GL_APIENTRY* bindFramebuffer)(GLenum, GLuint) = nullptr;
    void(GL_APIENTRY* bindBuffer)(GLenum, GLuint) = nullptr;

    template <typename Proc>
    bool CreateOffscreenTarget(Proc proc) {
        constexpr GLenum kFramebuffer = 0x8D40;
        constexpr GLenum kColorAttachment0 = 0x8CE0;
        constexpr GLenum kPixelUnpackBuffer = 0x88EC;
        constexpr GLenum kStreamDraw = 0x88E0;
        auto genFramebuffers = reinterpret_cast<void(GL_APIENTRY*)(GLsizei, GLuint*)>(proc("glGenFramebuffers"));
        auto framebufferTexture2D = reinterpret_cast<void(GL_APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint)>(proc("glFramebufferTexture2D"));
        auto genBuffers = reinterpret_cast<void(GL_APIENTRY*)(GLsizei, GLuint*)>(proc("glGenBuffers"));
        auto bufferData = reinterpret_cast<void(GL_APIENTRY*)(GLenum, ptrdiff_t, const void*, GLenum)>(proc("glBufferData"));
        bindFramebuffer = reinterpret_cast<void(GL_APIENTRY*)(GLenum, GLuint)>(proc("glBindFramebuffer"));
        bindBuffer = reinterpret_cast<void(GL_APIENTRY*)(GLenum, GLuint)>(proc("glBindBuffer"));
        if (!genFramebuffers || !framebufferTexture2D || !genBuffers || !bufferData || !bindFramebuffer || !bindBuffer)
            return false;
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        genFramebuffers(1, &framebuffer);
        bindFramebuffer(kFramebuffer, framebuffer);
        framebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, texture, 0);
        bindFramebuffer(kFramebuffer, 0);
        genBuffers(1, &unpackBuffer);
        bindBuffer(kPixelUnpackBuffer, unpackBuffer);
        bufferData(kPixelUnpackBuffer, 16, nullptr, kStreamDraw);
        bindBuffer(kPixelUnpackBuffer, 0);
        return true;
    }

    // The window is cleared through the default framebuffer, then the
    // offscreen target is left bound for the swap.
    void BindForSwap(bool offscreen) {
        constexpr GLenum kFramebuffer = 0x8D40;
        constexpr GLenum kPixelUnpackBuffer = 0x88EC;
        if (!framebuffer)
            return;
        bindFramebuffer(kFramebuffer, offscreen ? framebuffer : 0);
        bindBuffer(kPixelUnpackBuffer, offscreen ? unpackBuffer : 0);
    }
    // Textures of the host, which the overlay must leave alone.
    std::vector<GLuint> textures;
    static constexpr uint32_t kTexel = 0xff996633;

    void CreateTextures() {
        textures.assign(8, 0);
        glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
        for (GLuint texture : textures) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &kTexel);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    bool TexturesIntact() override {
        bool intact = true;
        for (GLuint texture : textures) {
            if (!glIsTexture(texture)) {
                intact = false;
                continue;
            }
            // Another texture may have been created under the name of a deleted one.
            GLint width = 0;
            GLint height = 0;
            uint32_t texel = 0;
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
            if (width == 1 && height == 1)
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, &texel);
            intact = intact && texel == kTexel;
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return intact;
    }

    bool Init(HWND hwnd, const Options& options) override {
        constexpr GLenum kFramebufferSrgb = 0x8DB9;
        if (options.format == "float")
            return false;
        window = hwnd;
        hdc = GetDC(window);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        int format = ChoosePixelFormat(hdc, &pfd);
        if (!format || !SetPixelFormat(hdc, format, &pfd))
            return false;
        context = wglCreateContext(hdc);
        if (!context || !wglMakeCurrent(hdc, context))
            return false;
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if (options.format == "srgb")
            glEnable(kFramebufferSrgb);
        if (!version || version[0] < '3')
            return false;
        return !options.boundFramebuffer || CreateOffscreenTarget([](const char* name) { return reinterpret_cast<void*>(wglGetProcAddress(name)); });
    }

    bool Resize(int width, int height) override {
        glViewport(0, 0, width, height);
        return true;
    }

    void Frame() override {
        BindForSwap(false);
        glClearColor(kClear[0], kClear[1], kClear[2], kClear[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        BindForSwap(true);
        SwapBuffers(hdc);
    }

    bool RecreateContext() override {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(context);
        context = wglCreateContext(hdc);
        if (!context || !wglMakeCurrent(hdc, context))
            return false;
        CreateTextures();
        return true;
    }

    ~OpenGLHost() override {
        wglMakeCurrent(nullptr, nullptr);
        if (context)
            wglDeleteContext(context);
        if (hdc)
            ReleaseDC(window, hdc);
    }
};

// Driver -----------------------------------------------------------------

// Opaque color drawn by the test menu and checked on screen: it must come out
// unchanged whatever the format of the back buffer.
constexpr uint8_t kProbe[3] = {200, 100, 50};
constexpr int kProbeX = 650;
constexpr int kProbeY = 500;
ImTextureData* g_texture = nullptr;
// Textures of the textures scenario. The last one, of the probe color, is
// drawn on the probe.
std::vector<ImTextureData*> g_textures;
bool g_setupCalled = false;
// Set by the fault scenario.
const char* g_fault = nullptr;
// Set by the slow frame scenario: the next frame of the menu lasts that long.
std::atomic<DWORD> g_slowFrameMs{0};

void DrawTestMenu() {
    if (DWORD ms = g_slowFrameMs.exchange(0))
        Sleep(ms);
    if (!uih::IsMenuOpen())
        return;
    if (g_fault && strcmp(g_fault, "exception") == 0)
        throw std::runtime_error("menu");
    if (g_fault) {
        volatile int* volatile address = nullptr;
        *address = 1;
    }
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    ImVec2 probeMin(kProbeX - 50.0f, kProbeY - 50.0f);
    ImVec2 probeMax(kProbeX + 50.0f, kProbeY + 50.0f);
    if (g_textures.empty()) {
        foreground->AddRectFilled(probeMin, probeMax, IM_COL32(kProbe[0], kProbe[1], kProbe[2], 255));
    } else {
        for (size_t i = 0; i + 1 < g_textures.size(); ++i)
            foreground->AddImage(g_textures[i]->GetTexRef(), ImVec2(0, 0), ImVec2(1, 1));
        foreground->AddImage(g_textures.back()->GetTexRef(), probeMin, probeMax);
    }
    ImGui::SetNextWindowPos(ImVec2(50, 50));
    ImGui::SetNextWindowSize(ImVec2(500, 400));
    ImGui::Begin("uih host", nullptr, ImGuiWindowFlags_NoResize);
    ImGui::Text("Backend: %s", uih::BackendName(uih::ActiveBackend()));
    ImGui::Image(g_texture->GetTexRef(), ImVec2(64, 64));
    // Created and drawn in the same frame, as menus often do. Refused when
    // the textures scenario reached the limit.
    static ImTextureData* late = [] {
        std::vector<uint32_t> pixels(16 * 16, IM_COL32(50, 200, 100, 255));
        return uih::CreateTexture(pixels.data(), 16, 16);
    }();
    if (late) {
        ImGui::SameLine();
        ImGui::Image(late->GetTexRef(), ImVec2(64, 64));
    }
    ImGui::Button("Button", ImVec2(400, 200));
    ImGui::End();
}

struct Capture {
    int overlayPixels = 0;
    uint8_t probe[3] = {};
    int width = 0;
    int height = 0;
    // Top-down BGRA rows.
    std::vector<uint8_t> pixels;
};

// Writes the capture next to the test, for the CI to keep when a check fails.
void SaveCapture(const Capture& capture, const std::string& name) {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = capture.width;
    header.biHeight = -capture.height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(capture.pixels.size());
    std::string path = "capture-" + name + ".bmp";
    FILE* out = nullptr;
    if (fopen_s(&out, path.c_str(), "wb") == 0 && out) {
        fwrite(&file, sizeof(file), 1, out);
        fwrite(&header, sizeof(header), 1, out);
        fwrite(capture.pixels.data(), 1, capture.pixels.size(), out);
        fclose(out);
        printf("capture saved to %s\n", path.c_str());
    }
}

Capture CaptureWindow(HWND window) {
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

    Capture capture;
    const uint8_t* pixels = static_cast<const uint8_t*>(bits);
    for (int i = 0; i < width * height; ++i) {
        const uint8_t* p = pixels + i * 4;
        if (abs(p[2] - 26) > 12 || abs(p[1] - 51) > 12 || abs(p[0] - 77) > 12)
            ++capture.overlayPixels;
    }
    if (kProbeX < width && kProbeY < height) {
        const uint8_t* p = pixels + (kProbeY * width + kProbeX) * 4;
        capture.probe[0] = p[2];
        capture.probe[1] = p[1];
        capture.probe[2] = p[0];
    }
    capture.width = width;
    capture.height = height;
    capture.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return capture;
}

bool ProbeMatches(const Capture& capture) {
    printf("probe color %d %d %d, expected %d %d %d\n", capture.probe[0], capture.probe[1], capture.probe[2], kProbe[0], kProbe[1], kProbe[2]);
    for (int i = 0; i < 3; ++i)
        if (abs(capture.probe[i] - kProbe[i]) > 8)
            return false;
    return true;
}

uih::Backend ExpectedBackend(const std::string& api) {
    if (api == "d3d9" || api == "d3d9ex") return uih::Backend::D3D9;
    if (api == "d3d10") return uih::Backend::D3D10;
    if (api == "d3d11") return uih::Backend::D3D11;
    if (api == "d3d12") return uih::Backend::D3D12;
    if (api == "vulkan") return uih::Backend::Vulkan;
    if (api == "opengl") return uih::Backend::OpenGL;
    return uih::Backend::None;
}

std::unique_ptr<Host> CreateHost(const std::string& api) {
    if (api == "d3d9") return std::make_unique<D3D9Host>(false);
    if (api == "d3d9ex") return std::make_unique<D3D9Host>(true);
    if (api == "d3d10") return std::make_unique<D3D10Host>();
    if (api == "d3d11") return std::make_unique<D3D11Host>();
    if (api == "d3d12") return std::make_unique<D3D12Host>();
    if (api == "vulkan") return CreateVulkanHost();
    if (api == "opengl") return std::make_unique<OpenGLHost>();
    return nullptr;
}

// Paces the host at about 60 frames per second, so that frame counts are durations.
void Pace() {
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (next > now)
        Sleep(static_cast<DWORD>(next - now));
    next = (next > now ? next : now) + 16;
}

void PumpMessages() {
    Pace();
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void StartHook(const Options& options) {
    uih::Config config;
    config.scale = options.scale;
    config.render = DrawTestMenu;
    config.setup = [] { g_setupCalled = true; };
    config.debugLog = true;
    uih::Start(config);
}

// The hooks are installed on a worker thread, which software drivers make
// slow: detection is bounded in time rather than in frames.
constexpr ULONGLONG kDetectionTimeoutMs = 30000;

bool WaitingForBackend(ULONGLONG start) {
    return uih::ActiveBackend() == uih::Backend::None && GetTickCount64() - start < kDetectionTimeoutMs;
}

// Without an exclusive mode, games change the display mode and cover the
// screen with a borderless window.
bool CoverDisplay(HWND window, bool on) {
    if (!on) {
        ChangeDisplaySettingsW(nullptr, 0);
        SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(window, nullptr, 100, 100, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        return true;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = kFullscreen[0];
    mode.dmPelsHeight = kFullscreen[1];
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    if (ChangeDisplaySettingsW(&mode, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL)
        return false;
    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(window, HWND_TOP, 0, 0, kFullscreen[0], kFullscreen[1], SWP_FRAMECHANGED);
    return true;
}

void ResizeWindow(HWND window, int width, int height) {
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int Fail(const char* message) {
    printf("FAIL: %s\n", message);
    return 1;
}

int Skip(const char* message) {
    printf("SKIP: %s\n", message);
    return kSkip;
}

void HoldKey(int key, bool down) {
    keybd_event(static_cast<BYTE>(key), 0, down ? 0 : KEYEVENTF_KEYUP, 0);
}

// Windows only lets the foreground process hand the focus over; a key press
// lifts that restriction when the plain request is refused.
void BringToFront(HWND window) {
    SetForegroundWindow(window);
    if (GetForegroundWindow() == window)
        return;
    HoldKey(VK_MENU, true);
    HoldKey(VK_MENU, false);
    SetForegroundWindow(window);
}

// Real key presses: only sent while the host window has the focus.
bool PressKey(HWND window, int key) {
    if (GetForegroundWindow() != window)
        return false;
    HoldKey(key, true);
    return true;
}

// Procedure the host installs over the one of the overlay, as overlays and
// engines that subclass the window late do. It calls the overlay's.
WNDPROC g_previousProc = nullptr;
int g_subclassMessages = 0;

LRESULT CALLBACK HostSubclass(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
    ++g_subclassMessages;
    return CallWindowProcW(g_previousProc, window, msg, wParam, lParam);
}

void SubclassWindow(HWND window) {
    g_previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HostSubclass)));
}

// True when a message goes through the procedures of the window and back.
bool ProcedureChainWorks(HWND window) {
    int messages = g_subclassMessages;
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    return g_subclassMessages == messages + 1;
}

// Loads a library, opens its menu and renders until the window is closed:
// a quick look at a menu without a game. Like a game, the host resizes its
// back buffers with the window, so nothing is stretched.
int RunPreviewMode(HWND window, Host& host, const Options& options) {
    std::wstring path(options.preview.begin(), options.preview.end());
    if (!LoadLibraryW(path.c_str()))
        return Fail("LoadLibrary failed");
    BringToFront(window);
    RECT size{0, 0, kWidth, kHeight};
    for (int frame = 0; IsWindow(window); ++frame) {
        if (frame == 60 && PressKey(window, options.menuKey ? options.menuKey : VK_INSERT))
            HoldKey(options.menuKey ? options.menuKey : VK_INSERT, false);
        PumpMessages();
        if (!IsWindow(window))
            break;
        RECT client{};
        GetClientRect(window, &client);
        // A minimized window has no size: the back buffers keep theirs.
        if (client.right > 0 && client.bottom > 0 && (client.right != size.right || client.bottom != size.bottom)) {
            size = client;
            host.Resize(client.right, client.bottom);
        }
        host.Frame();
    }
    return 0;
}

// Loads the example DLL like an injector would and drives it with its hotkeys:
// INSERT opens the menu, F11 removes the overlay. A DLL named dinput8.dll is
// loaded the way a game loads a proxy: F11 only removes the overlay, and
// INSERT brings it back. Otherwise F11 unloads the DLL, and --reload injects
// it again.
// With --resubclass, the host subclasses the window after the overlay: the
// DLL stays loaded after F11, as the host's procedure still calls its own.
int RunDllMode(HWND window, Host& host, const Options& options) {
    const int menuKey = options.menuKey ? options.menuKey : VK_INSERT;
    const int unloadKey = options.unloadKey ? options.unloadKey : VK_F11;
    const int visible = kWidth * kHeight / 100;
    std::wstring path(options.dll.begin(), options.dll.end());
    size_t separator = path.find_last_of(L"\\/");
    std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
    const bool proxy = _wcsicmp(name.c_str(), L"dinput8.dll") == 0;
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpMessages();
            host.Frame();
        }
    };
    auto overlayPixels = [&] { return CaptureWindow(window).overlayPixels; };

    run(options.injectAt);
    HMODULE module = nullptr;
    for (int cycle = 0; cycle < (options.reload ? 2 : 1); ++cycle) {
        if (cycle == 0 || !proxy) {
            module = LoadLibraryW(path.c_str());
            if (!module)
                return Fail("LoadLibrary failed");
        }
        if (proxy && cycle == 0) {
            // The game's first call makes the DLL pin itself.
            using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, void*);
            auto create = reinterpret_cast<DirectInput8CreateFn>(reinterpret_cast<void*>(GetProcAddress(module, "DirectInput8Create")));
            IUnknown* directInput = nullptr;
            if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&directInput), nullptr)))
                return Fail("DirectInput8Create was not forwarded");
            directInput->Release();
        }
        BringToFront(window);

        // The hooks are installed on a thread of the DLL: INSERT is pressed
        // again only when the overlay did not show up within a few seconds.
        int pixels = 0;
        for (ULONGLONG start = GetTickCount64(); pixels < visible && GetTickCount64() - start < kDetectionTimeoutMs;) {
            if (!PressKey(window, menuKey))
                return Skip("the host window does not have the focus");
            run(5);
            HoldKey(menuKey, false);
            for (int wait = 0; wait < 12 && pixels < visible; ++wait) {
                run(15);
                pixels = overlayPixels();
            }
        }
        printf("cycle %d: %d overlay pixels captured\n", cycle + 1, pixels);
        if (pixels < visible)
            return Fail("overlay is not visible on screen");

        if (options.resubclass)
            SubclassWindow(window);
        if (!PressKey(window, unloadKey))
            return Skip("the host window lost the focus");
        run(5);
        HoldKey(unloadKey, false);
        if (proxy || options.resubclass) {
            for (ULONGLONG start = GetTickCount64(); pixels >= visible && GetTickCount64() - start < 10000;) {
                run(15);
                pixels = overlayPixels();
            }
            printf("cycle %d: %d overlay pixels after the unload key\n", cycle + 1, pixels);
            if (pixels >= visible)
                return Fail("overlay is still visible after the unload key");
            run(30);
            if (options.resubclass && !ProcedureChainWorks(window))
                return Fail("the procedures of the window no longer chain");
        } else {
            for (ULONGLONG start = GetTickCount64(); GetModuleHandleW(name.c_str()) && GetTickCount64() - start < 10000;)
                run(1);
            if (GetModuleHandleW(name.c_str()))
                return Fail("DLL did not unload");
        }
    }
    printf("PASS: %s via %s\n", options.api.c_str(), options.dll.c_str());
    return 0;
}

// Uses the library in-process without a module to free: F11 removes the
// overlay and INSERT brings it back, twice.
int RunRemoveMode(HWND window, Host& host) {
    const int visible = kWidth * kHeight / 100;
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpMessages();
            host.Frame();
        }
    };
    auto waitFor = [&](auto condition) {
        for (ULONGLONG start = GetTickCount64(); !condition() && GetTickCount64() - start < kDetectionTimeoutMs;)
            run(1);
        return condition();
    };
    auto tap = [&](int key) {
        if (!PressKey(window, key))
            return false;
        run(5);
        HoldKey(key, false);
        return true;
    };

    StartHook(Options{});
    int result = 0;
    if (!waitFor([] { return uih::ActiveBackend() != uih::Backend::None; }))
        result = Fail("backend was not detected");
    BringToFront(window);
    run(30);
    for (int cycle = 0; cycle < 2 && result == 0; ++cycle) {
        if (!tap(VK_INSERT)) {
            result = Skip("the host window does not have the focus");
            break;
        }
        if (!waitFor([] { return uih::IsMenuOpen() && uih::ActiveBackend() != uih::Backend::None; })) {
            result = Fail(cycle ? "the menu key did not bring the overlay back" : "the menu did not open");
            break;
        }
        run(30);
        int pixels = CaptureWindow(window).overlayPixels;
        printf("cycle %d: %d overlay pixels with the menu open\n", cycle + 1, pixels);
        if (pixels < visible) {
            result = Fail("overlay is not visible on screen");
            break;
        }
        if (!tap(VK_F11)) {
            result = Skip("the host window lost the focus");
            break;
        }
        if (!waitFor([] { return uih::ActiveBackend() == uih::Backend::None && !ImGui::GetCurrentContext(); })) {
            result = Fail("the unload key did not remove the overlay");
            break;
        }
        run(30);
    }
    uih::Stop();
    if (result == 0)
        printf("PASS: removal and return\n");
    return result;
}

// A second window, with its own device and swap chain, presents after the
// first one every frame. The overlay stays on one window instead of moving at
// every present, follows the focus, and goes back to the first window when
// the second one stops presenting.
int RunSecondWindowMode(HWND window, Host& host, const Options& options) {
    constexpr int kSecondSize[2] = {400, 300};
    RECT rect{0, 0, kSecondSize[0], kSecondSize[1]};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND second = CreateWindowExW(WS_EX_TOPMOST, L"uih_host", L"uih host 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
                                  rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::unique_ptr<Host> secondHost = options.sharedDevice ? host.ShareDevice(second, options) : CreateHost(options.api);
    if (!second || !secondHost || (!options.sharedDevice && !secondHost->Init(second, options))) {
        DestroyWindow(second);
        return Fail("the second window could not render");
    }
    bool presentSecond = true;
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpMessages();
            host.Frame();
            if (presentSecond)
                secondHost->Frame();
        }
    };
    auto waitFor = [&](auto condition) {
        for (ULONGLONG start = GetTickCount64(); !condition() && GetTickCount64() - start < kDetectionTimeoutMs;)
            run(1);
        return condition();
    };
    // The size of the overlay is the client area of the window it is on.
    auto onWindow = [](HWND target) {
        RECT client{};
        GetClientRect(target, &client);
        if (!ImGui::GetCurrentContext())
            return false;
        ImVec2 display = ImGui::GetIO().DisplaySize;
        return display.x == client.right && display.y == client.bottom;
    };
    auto stays = [&](HWND target) {
        int frameCount = ImGui::GetFrameCount();
        run(60);
        int frames = ImGui::GetFrameCount() - frameCount;
        printf("%d overlay frames for 60 frames of each window\n", frames);
        return onWindow(target) && frames >= 55 && frames <= 61;
    };

    int result = 0;
    StartHook(options);
    if (!waitFor([] { return uih::ActiveBackend() != uih::Backend::None; }))
        result = Fail("backend was not detected");
    uih::SetMenuOpen(true);
    if (result == 0) {
        BringToFront(window);
        if (GetForegroundWindow() != window)
            result = Skip("the host window does not have the focus");
    }
    if (result == 0 && !waitFor([&] { return onWindow(window); }))
        result = Fail("the overlay is not on the focused window");
    if (result == 0 && !stays(window))
        result = Fail("the overlay does not stay on the focused window");
    if (result == 0) {
        BringToFront(second);
        if (GetForegroundWindow() != second)
            result = Skip("the second window does not get the focus");
    }
    if (result == 0 && !waitFor([&] { return onWindow(second); }))
        result = Fail("the overlay did not follow the focus to the second window");
    if (result == 0 && !stays(second))
        result = Fail("the overlay does not stay on the second window");
    if (result == 0) {
        presentSecond = false;
        if (!waitFor([&] { return onWindow(window); }))
            result = Fail("the overlay did not go back when the second window stopped presenting");
    }
    uih::Stop();
    secondHost.reset();
    DestroyWindow(second);
    if (result == 0)
        printf("PASS: %s with a second window\n", options.api.c_str());
    return result;
}

// The render callback faults with the menu open: the overlay is removed, the
// host keeps rendering, and the overlay comes back when the hook starts again.
int RunFaultMode(HWND window, Host& host, const Options& options) {
    const int visible = kWidth * kHeight / 100;
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpMessages();
            host.Frame();
        }
    };
    auto waitFor = [&](auto condition) {
        for (ULONGLONG start = GetTickCount64(); !condition() && GetTickCount64() - start < kDetectionTimeoutMs;)
            run(1);
        return condition();
    };

    int result = 0;
    for (int cycle = 0; cycle < 2 && result == 0; ++cycle) {
        StartHook(options);
        if (!waitFor([] { return uih::ActiveBackend() != uih::Backend::None; })) {
            result = Fail(cycle ? "the hook did not start again after the fault" : "backend was not detected");
            break;
        }
        uih::SetMenuOpen(true);
        run(30);
        int pixels = CaptureWindow(window).overlayPixels;
        printf("cycle %d: %d overlay pixels with the menu open\n", cycle + 1, pixels);
        if (pixels < visible) {
            result = Fail("overlay is not visible on screen");
            break;
        }
        if (cycle == 1)
            break;
        g_fault = options.fault.c_str();
        bool removed = waitFor([] { return uih::ActiveBackend() == uih::Backend::None && !ImGui::GetCurrentContext(); });
        g_fault = nullptr;
        if (!removed) {
            result = Fail("the fault did not remove the overlay");
            break;
        }
        run(30);
        pixels = CaptureWindow(window).overlayPixels;
        printf("after the fault: %d overlay pixels, the host still renders\n", pixels);
        if (pixels >= visible)
            result = Fail("overlay is still visible after the fault");
    }
    uih::Stop();
    if (result == 0)
        printf("PASS: %s fault\n", options.fault.c_str());
    return result;
}

// Gives every allocation of Dear ImGui pages of its own, made inaccessible
// when it is freed: a thread that uses memory another thread released, as
// when both grow the same vector, crashes at once. 64-bit only: the address
// space is never reused.
void UseGuardedAllocator() {
#ifdef _WIN64
    static constexpr size_t kHeader = 16;
    ImGui::SetAllocatorFunctions(
        [](size_t size, void*) -> void* {
            auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, size + kHeader, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!block)
                return nullptr;
            *reinterpret_cast<size_t*>(block) = size + kHeader;
            return block + kHeader;
        },
        [](void* pointer, void*) {
            if (!pointer)
                return;
            uint8_t* block = static_cast<uint8_t*>(pointer) - kHeader;
            VirtualFree(block, *reinterpret_cast<size_t*>(block), MEM_DECOMMIT);
        });
#endif
}

// Access violations raised in the process, even those the overlay recovers from.
std::atomic<int> g_accessViolations{0};

LONG CALLBACK CountAccessViolations(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        ++g_accessViolations;
    return EXCEPTION_CONTINUE_SEARCH;
}

// uih::Stop() is called while a frame of the overlay outlasts its wait for
// the thread that presents: it must not release what that frame still uses.
int RunSlowFrameMode(HWND window, Host& host, const Options& options) {
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpMessages();
            host.Frame();
        }
    };
    StartHook(options);
    for (ULONGLONG start = GetTickCount64(); WaitingForBackend(start);)
        run(1);
    if (uih::ActiveBackend() == uih::Backend::None) {
        uih::Stop();
        return Fail("backend was not detected");
    }
    uih::SetMenuOpen(true);
    run(30);

    PVOID handler = AddVectoredExceptionHandler(1, CountAccessViolations);
    g_slowFrameMs = static_cast<DWORD>(options.slowFrameMs);
    std::thread stop([] {
        // Once the slow frame started.
        Sleep(200);
        uih::Stop();
    });
    run(1);
    stop.join();
    run(30);
    RemoveVectoredExceptionHandler(handler);

    int result = 0;
    if (g_accessViolations > 0)
        result = Fail("the overlay raised an access violation");
    else if (ImGui::GetCurrentContext() || uih::ActiveBackend() != uih::Backend::None)
        result = Fail("hook did not release its resources");
    if (result != 0)
        return result;

    StartHook(options);
    for (ULONGLONG start = GetTickCount64(); WaitingForBackend(start);)
        run(1);
    uih::SetMenuOpen(true);
    run(30);
    bool back = ImGui::GetCurrentContext() && ImGui::GetIO().MetricsRenderVertices > 0;
    uih::Stop();
    if (!back)
        return Fail("the overlay did not come back after the slow frame");
    printf("PASS: %s, stopped during a frame of %d ms\n", options.api.c_str(), options.slowFrameMs);
    return result;
}

// The host renders on a thread of its own while the thread of the window
// dispatches a flood of input messages, and the hook starts and stops in the
// middle of it: the window procedure must not meet the overlay in Dear ImGui.
int RunRenderThreadMode(HWND window, const Options& options) {
    std::atomic<int> started{0};
    std::atomic<bool> done{false};
    std::thread render([&] {
        std::unique_ptr<Host> host = CreateHost(options.api);
        if (!host->Init(window, options)) {
            started = -1;
            return;
        }
        started = 1;
        while (!done)
            host->Frame();
    });
    auto pumpUntil = [](auto condition) {
        for (ULONGLONG start = GetTickCount64(); !condition() && GetTickCount64() - start < kDetectionTimeoutMs;)
            PumpMessages();
        return condition();
    };
    // Mouse moves, characters and keys, which Dear ImGui queues as events.
    auto flood = [&](auto until) {
        for (int i = 0; !until(); ++i) {
            PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(100 + i % 200, 100 + i % 50));
            PostMessageW(window, WM_CHAR, 'a' + i % 26, 0);
            PostMessageW(window, i % 2 ? WM_KEYUP : WM_KEYDOWN, VK_F13, 0);
            if (i % 200 != 199)
                continue;
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    };

    PVOID handler = AddVectoredExceptionHandler(1, CountAccessViolations);
    int result = 0;
    pumpUntil([&] { return started != 0; });
    if (started != 1)
        result = Skip("the host could not render on its thread");
    for (int cycle = 0; cycle < 20 && result == 0; ++cycle) {
        // The input keeps coming while the overlay creates its context.
        StartHook(options);
        ULONGLONG start = GetTickCount64();
        auto active = [] { return uih::ActiveBackend() != uih::Backend::None && ImGui::GetCurrentContext(); };
        flood([&] { return active() || GetTickCount64() - start > kDetectionTimeoutMs; });
        if (!active()) {
            result = Fail("backend was not detected");
            break;
        }
        uih::SetMenuOpen(true);
        start = GetTickCount64();
        flood([&] { return GetTickCount64() - start > 250; });
        // Stopped from another thread, as the unload key does, while the input keeps coming.
        std::atomic<bool> stopped{false};
        std::thread stop([&] {
            uih::Stop();
            stopped = true;
        });
        flood([&] { return stopped.load(); });
        stop.join();
        if (g_accessViolations > 0)
            result = Fail("the overlay raised an access violation");
        else if (ImGui::GetCurrentContext())
            result = Fail("hook did not release its resources");
    }
    uih::Stop();
    done = true;
    render.join();
    RemoveVectoredExceptionHandler(handler);
    if (result == 0)
        printf("PASS: %s with a render thread\n", options.api.c_str());
    return result;
}

// Input the application reads while the menu is open ----------------------

struct RawInputSeen {
    bool keyPressed = false;
    long mouseMotion = 0;
};

RawInputSeen g_rawInput;

// Reads WM_INPUT before dispatching it, like engines that poll their message queue.
void PumpRawInput() {
    Pace();
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_INPUT) {
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                if (input.header.dwType == RIM_TYPEKEYBOARD && input.data.keyboard.VKey == VK_F13 && !(input.data.keyboard.Flags & RI_KEY_BREAK))
                    g_rawInput.keyPressed = true;
                if (input.header.dwType == RIM_TYPEMOUSE)
                    g_rawInput.mouseMotion += labs(input.data.mouse.lLastX) + labs(input.data.mouse.lLastY);
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// What the window procedure of the host saw, like games that read their
// input there rather than in their message loop.
struct ProcedureInput {
    bool keyDown = false;
    bool rawKeyDown = false;
    bool closeRequested = false;
};

ProcedureInput g_procedureInput;

LRESULT CALLBACK HostInputProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wParam == VK_F13)
        g_procedureInput.keyDown = msg == WM_KEYDOWN;
    if (msg == WM_INPUT) {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            input.header.dwType == RIM_TYPEKEYBOARD) {
            if (input.data.keyboard.Flags & RI_KEY_BREAK) {
                if (input.data.keyboard.VKey == VK_F13)
                    g_procedureInput.rawKeyDown = false;
            } else if (input.data.keyboard.VKey == VK_F13) {
                g_procedureInput.rawKeyDown = true;
            }
        }
    }
    if (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE) {
        g_procedureInput.closeRequested = true;
        return 0;
    }
    return DefWindowProcW(window, msg, wParam, lParam);
}

struct InputState {
    bool asyncKey = false;
    bool keyState = false;
    bool keyboardState = false;
    bool directInputKey = false;
    RawInputSeen raw;
    POINT cursor{};
};

// Presses F13, which no application uses, and moves the mouse, then reports what the host reads.
InputState PressAndRead(Host& host, IDirectInputDevice8W* keyboard) {
    g_rawInput = {};
    keybd_event(VK_F13, static_cast<BYTE>(MapVirtualKeyW(VK_F13, MAPVK_VK_TO_VSC)), 0, 0);
    INPUT motion{};
    motion.type = INPUT_MOUSE;
    motion.mi.dx = 40;
    motion.mi.dy = 40;
    motion.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &motion, sizeof(motion));
    for (int frame = 0; frame < 10; ++frame) {
        PumpRawInput();
        host.Frame();
    }

    InputState state;
    state.asyncKey = (GetAsyncKeyState(VK_F13) & 0x8000) != 0;
    state.keyState = (GetKeyState(VK_F13) & 0x8000) != 0;
    BYTE keys[256] = {};
    GetKeyboardState(keys);
    state.keyboardState = (keys[VK_F13] & 0x80) != 0;
    BYTE directInputKeys[256] = {};
    if (keyboard && SUCCEEDED(keyboard->GetDeviceState(sizeof(directInputKeys), directInputKeys)))
        state.directInputKey = (directInputKeys[DIK_F13] & 0x80) != 0;
    state.raw = g_rawInput;
    GetCursorPos(&state.cursor);

    keybd_event(VK_F13, static_cast<BYTE>(MapVirtualKeyW(VK_F13, MAPVK_VK_TO_VSC)), KEYEVENTF_KEYUP, 0);
    for (int frame = 0; frame < 5; ++frame) {
        PumpRawInput();
        host.Frame();
    }
    return state;
}

void PrintInput(const char* label, const InputState& state) {
    printf("%s: GetAsyncKeyState %d, GetKeyState %d, GetKeyboardState %d, DirectInput %d, raw key %d, raw motion %ld, cursor %ld,%ld\n", label,
           state.asyncKey, state.keyState, state.keyboardState, state.directInputKey, state.raw.keyPressed, state.raw.mouseMotion, state.cursor.x,
           state.cursor.y);
}

// A key held when the menu opens and released while it is open must not
// stay down in the application, and Alt+F4 still closes its window.
int CheckReleaseWithMenuOpen(HWND window, Host& host, bool useDirectInput) {
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpRawInput();
            host.Frame();
        }
    };
    BYTE scanCode = static_cast<BYTE>(MapVirtualKeyW(VK_F13, MAPVK_VK_TO_VSC));
    g_procedureInput = {};
    keybd_event(VK_F13, scanCode, 0, 0);
    run(10);
    bool rawSeen = useDirectInput || g_procedureInput.rawKeyDown;
    if (!g_procedureInput.keyDown || !rawSeen) {
        keybd_event(VK_F13, scanCode, KEYEVENTF_KEYUP, 0);
        run(5);
        return Skip("the key press did not reach the window procedure of the host");
    }
    uih::SetMenuOpen(true);
    run(5);
    keybd_event(VK_F13, scanCode, KEYEVENTF_KEYUP, 0);
    run(10);
    // Alt+F4, as the system reports it to the window with the focus.
    PostMessageW(window, WM_SYSKEYDOWN, VK_F4, 1 | (1 << 29));
    run(5);
    uih::SetMenuOpen(false);
    run(5);
    printf("procedure: key down %d, raw key down %d, close requested %d\n", g_procedureInput.keyDown, g_procedureInput.rawKeyDown,
           g_procedureInput.closeRequested);
    if (g_procedureInput.keyDown)
        return Fail("a key released while the menu was open stays down in the application");
    if (g_procedureInput.rawKeyDown)
        return Fail("a key released while the menu was open stays down in the raw input of the application");
    if (!g_procedureInput.closeRequested)
        return Fail("Alt+F4 does not close the window while the menu is open");
    return 0;
}

// The menu frees the cursor of a game that confines it to its window, and
// confines it again when it closes, to the rectangle the game asked for last.
int CheckCursorClip(HWND window, Host& host) {
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpRawInput();
            host.Frame();
        }
    };
    RECT client{};
    GetClientRect(window, &client);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
    RECT clip{};
    ClipCursor(&client);
    GetClipCursor(&clip);
    if (!EqualRect(&clip, &client)) {
        ClipCursor(nullptr);
        return Skip("the host cannot confine the cursor");
    }
    uih::SetMenuOpen(true);
    run(5);
    RECT open{};
    GetClipCursor(&open);
    RECT inner = client;
    InflateRect(&inner, -10, -10);
    ClipCursor(&inner);
    run(5);
    uih::SetMenuOpen(false);
    run(5);
    GetClipCursor(&clip);
    ClipCursor(nullptr);
    printf("cursor clip: %ld,%ld %ld,%ld with the menu open, %ld,%ld %ld,%ld after it closed\n", open.left, open.top, open.right, open.bottom, clip.left,
           clip.top, clip.right, clip.bottom);
    if (EqualRect(&open, &client))
        return Fail("the cursor stays confined while the menu is open");
    if (!EqualRect(&clip, &inner))
        return Fail("the cursor is not confined again when the menu closes");
    return 0;
}

// Checks that the keyboard and the mouse reach the host while the menu is
// closed, and that the host reads nothing while it is open. DirectInput
// registers the keyboard for raw input itself, and a process has one raw
// input target per device class, so raw input and DirectInput are checked
// in separate runs.
int RunInputMode(HWND window, Host& host, bool useDirectInput) {
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HostInputProc));
    RAWINPUTDEVICE devices[] = {{0x01, 0x06, 0, window}, {0x01, 0x02, 0, window}};
    if (!useDirectInput)
        RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
    IDirectInput8W* directInput = nullptr;
    IDirectInputDevice8W* keyboard = nullptr;
    if (useDirectInput && SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&directInput), nullptr)) &&
        SUCCEEDED(directInput->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr))) {
        keyboard->SetDataFormat(&c_dfDIKeyboard);
        keyboard->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        keyboard->Acquire();
    }

    StartHook(Options{});
    int result = 0;
    for (ULONGLONG start = GetTickCount64(); WaitingForBackend(start);) {
        PumpRawInput();
        host.Frame();
    }
    BringToFront(window);
    for (int frame = 0; frame < 30; ++frame) {
        PumpRawInput();
        host.Frame();
    }

    if (uih::ActiveBackend() == uih::Backend::None) {
        result = Fail("backend was not detected");
    } else if (GetForegroundWindow() != window) {
        result = Skip("the host window does not have the focus");
    } else {
        InputState closed = PressAndRead(host, keyboard);
        PrintInput("menu closed", closed);
        bool reached = useDirectInput ? closed.directInputKey : closed.raw.keyPressed && closed.raw.mouseMotion != 0;
        if (!closed.asyncKey || !closed.keyState || !closed.keyboardState || !reached) {
            result = Skip("the key press or the mouse motion did not reach the host");
        } else {
            uih::SetMenuOpen(true);
            POINT before{};
            GetCursorPos(&before);
            InputState open = PressAndRead(host, keyboard);
            PrintInput("menu open", open);
            uih::SetMenuOpen(false);
            if (open.asyncKey || open.keyState || open.keyboardState)
                result = Fail("the application reads the keyboard state while the menu is open");
            else if (open.directInputKey)
                result = Fail("DirectInput reports the key while the menu is open");
            else if (open.raw.keyPressed || open.raw.mouseMotion != 0)
                result = Fail("raw input reaches the application while the menu is open");
            else if (open.cursor.x != before.x || open.cursor.y != before.y)
                result = Fail("the cursor moves for the application while the menu is open");
        }
    }
    if (result == 0)
        result = CheckReleaseWithMenuOpen(window, host, useDirectInput);
    if (result == 0)
        result = CheckCursorClip(window, host);

    uih::Stop();
    if (keyboard) {
        keyboard->Unacquire();
        keyboard->Release();
    }
    if (directInput)
        directInput->Release();
    if (result == 0)
        printf("PASS: input via %s\n", useDirectInput ? "DirectInput" : "raw input");
    return result;
}

// Controller ---------------------------------------------------------------

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using FakeXInputSetButtonsFn = void(WINAPI*)(WORD);

// Drives a fake xinput1_4.dll, loaded before the hook so the game and the
// Win32 backend both use it: the menu buttons toggle the menu, and the game
// reads an idle controller while the menu is open.
int RunGamepadMode(HWND window, Host& host, const std::string& fake) {
    std::wstring path(fake.begin(), fake.end());
    HMODULE xinput = LoadLibraryW(path.c_str());
    auto setButtons = reinterpret_cast<FakeXInputSetButtonsFn>(reinterpret_cast<void*>(GetProcAddress(xinput, "FakeXInputSetButtons")));
    auto getState = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")));
    if (!xinput || !setButtons || !getState)
        return Fail("the fake XInput library did not load");

    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpMessages();
            host.Frame();
        }
    };
    auto gameButtons = [&] {
        XINPUT_STATE state{};
        getState(0, &state);
        return state.Gamepad.wButtons;
    };
    auto press = [&](WORD buttons) {
        setButtons(buttons);
        run(5);
        setButtons(0);
        run(5);
    };

    StartHook(Options{});
    for (ULONGLONG start = GetTickCount64(); WaitingForBackend(start);)
        run(1);
    // XInput is hooked after the graphics API, by MinHook, which writes a jump at the start of the function.
    auto hooked = [&] {
        uint8_t first = *reinterpret_cast<const volatile uint8_t*>(reinterpret_cast<void*>(getState));
        return first == 0xE9 || first == 0xEB;
    };
    for (ULONGLONG start = GetTickCount64(); !hooked() && GetTickCount64() - start < kDetectionTimeoutMs;)
        run(1);
    BringToFront(window);
    run(30);

    int result = 0;
    const WORD menuButtons = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    if (uih::ActiveBackend() == uih::Backend::None) {
        result = Fail("backend was not detected");
    } else if (!hooked()) {
        result = Fail("XInput was not hooked");
    } else if (GetForegroundWindow() != window) {
        result = Skip("the host window does not have the focus");
    } else {
        setButtons(XINPUT_GAMEPAD_A);
        run(5);
        bool readClosed = gameButtons() == XINPUT_GAMEPAD_A;
        setButtons(0);
        run(5);
        press(menuButtons);
        bool opened = uih::IsMenuOpen();
        setButtons(XINPUT_GAMEPAD_A);
        run(5);
        bool readOpen = gameButtons() != 0;
        bool navigated = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
        setButtons(0);
        run(5);
        press(menuButtons);
        printf("game reads A with the menu closed: %d, menu opened: %d, game reads A with the menu open: %d, menu reads A: %d, menu closed: %d\n",
               readClosed, opened, readOpen, navigated, !uih::IsMenuOpen());
        if (!readClosed)
            result = Fail("the game does not read the controller while the menu is closed");
        else if (!opened)
            result = Fail("the menu buttons did not open the menu");
        else if (readOpen)
            result = Fail("the game reads the controller while the menu is open");
        else if (!navigated)
            result = Fail("the menu does not read the controller");
        else if (uih::IsMenuOpen())
            result = Fail("the menu buttons did not close the menu");
    }

    uih::Stop();
    FreeLibrary(xinput);
    if (result == 0)
        printf("PASS: gamepad\n");
    return result;
}

// Calls the export a target imports when the DLL is renamed to dinput8.dll.
int RunProxyMode(const Options& options) {
    using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, void*);
    constexpr GUID kDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    constexpr DWORD kDirectInputVersion = 0x0800;

    std::wstring path(options.proxy.begin(), options.proxy.end());
    HMODULE module = LoadLibraryW(path.c_str());
    if (!module)
        return Fail("LoadLibrary failed");
    auto create = reinterpret_cast<DirectInput8CreateFn>(reinterpret_cast<void*>(GetProcAddress(module, "DirectInput8Create")));
    if (!create)
        return Fail("DirectInput8Create is not exported");
    IUnknown* directInput = nullptr;
    HRESULT result = create(GetModuleHandleW(nullptr), kDirectInputVersion, kDirectInput8W, reinterpret_cast<void**>(&directInput), nullptr);
    if (FAILED(result) || !directInput)
        return Fail("DirectInput8Create was not forwarded to the system library");
    directInput->Release();
    FreeLibrary(module);
    if (!GetModuleHandleW(path.c_str()))
        return Fail("DLL unloaded while used as a proxy");
    printf("PASS: proxy via %s\n", options.proxy.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Keeps the output of a test that crashes.
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--api" && i + 1 < argc) options.api = argv[++i];
        else if (arg == "--inject-at" && i + 1 < argc) options.injectAt = atoi(argv[++i]);
        else if (arg == "--dll" && i + 1 < argc) options.dll = argv[++i];
        else if (arg == "--preview" && i + 1 < argc) options.preview = argv[++i];
        else if (arg == "--proxy" && i + 1 < argc) options.proxy = argv[++i];
        else if (arg == "--scale" && i + 1 < argc) options.scale = static_cast<float>(atof(argv[++i]));
        else if (arg == "--menu-key" && i + 1 < argc) options.menuKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--unload-key" && i + 1 < argc) options.unloadKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--resize") options.resize = true;
        else if (arg == "--reload") options.reload = true;
        else if (arg == "--fullscreen") options.fullscreen = true;
        else if (arg == "--remove") options.remove = true;
        else if (arg == "--fault" && i + 1 < argc) options.fault = argv[++i];
        else if (arg == "--second-window") options.secondWindow = true;
        else if (arg == "--shared-device") options.sharedDevice = true;
        else if (arg == "--textures" && i + 1 < argc) options.textures = atoi(argv[++i]);
        else if (arg == "--remove-device") options.removeDevice = true;
        else if (arg == "--transfer-only") options.transferOnly = true;
        else if (arg == "--render-thread") options.renderThread = true;
        else if (arg == "--resubclass") options.resubclass = true;
        else if (arg == "--recreate-context") options.recreateContext = true;
        else if (arg == "--slow-frame" && i + 1 < argc) options.slowFrameMs = atoi(argv[++i]);
        else if (arg == "--bound-framebuffer") options.boundFramebuffer = true;
        else if (arg == "--warp") options.warp = true;
        else if (arg == "--capture") options.capture = true;
        else if (arg == "--device-proc-addr") options.deviceProcAddr = true;
        else if (arg == "--decoy-queue") options.decoyQueue = true;
        else if (arg == "--format" && i + 1 < argc) options.format = argv[++i];
        else if (arg == "--settle" && i + 1 < argc) options.settleMs = atoi(argv[++i]);
        else if (arg == "--input" && i + 1 < argc) options.input = argv[++i];
        else if (arg == "--gamepad" && i + 1 < argc) options.gamepad = argv[++i];
    }
    if (!options.proxy.empty())
        return RunProxyMode(options);
    uih::Backend expected = ExpectedBackend(options.api);
    if (expected == uih::Backend::None) {
        printf("usage: uih_host --api d3d9|d3d9ex|d3d10|d3d11|d3d12|vulkan|opengl [--inject-at N] [--scale S] [--resize] [--reload] [--remove] [--fault access|exception] [--second-window [--shared-device]] [--textures N] [--remove-device] [--transfer-only] [--render-thread] [--resubclass] [--recreate-context] [--slow-frame ms] [--bound-framebuffer] [--warp] [--capture] [--device-proc-addr] [--decoy-queue] [--format srgb|float] [--settle ms] [--input raw|directinput] [--gamepad fake-xinput.dll] [--dll path [--menu-key vk] [--unload-key vk]] [--preview path]\n       uih_host --proxy path\n");
        return 2;
    }

    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW), CS_OWNDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"uih_host", nullptr};
    RegisterClassExW(&windowClass);
    // Before the first allocation of Dear ImGui.
    if (options.renderThread)
        UseGuardedAllocator();
    RECT rect{0, 0, kWidth, kHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    // Screen captures need the window above everything else.
    DWORD exStyle = options.capture || !options.dll.empty() || !options.fault.empty() ? WS_EX_TOPMOST : 0;
    HWND window = CreateWindowExW(exStyle, L"uih_host", L"uih host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    PumpMessages();

    // Created before the hook starts, registered with its context on the first frame.
    std::vector<uint32_t> pixels(32 * 32, IM_COL32(200, 100, 50, 255));
    g_texture = uih::CreateTexture(pixels.data(), 32, 32);
    if (options.textures > 0) {
        std::vector<uint32_t> probe(4 * 4, IM_COL32(kProbe[0], kProbe[1], kProbe[2], 255));
        for (int i = 0; i < options.textures; ++i)
            if (ImTextureData* texture = uih::CreateTexture(probe.data(), 4, 4))
                g_textures.push_back(texture);
        size_t expected = std::min<size_t>(options.textures, uih::kMaxTextures - 1);
        printf("%zu textures created, %zu expected\n", g_textures.size(), expected);
        if (g_textures.size() != expected)
            return Fail("the texture limit was not applied");
    }

    if (options.injectAt == 0 && options.dll.empty())
        StartHook(options);

    if (options.fullscreen && !getenv("UIH_TEST_FULLSCREEN"))
        return Skip("changes the display mode: set UIH_TEST_FULLSCREEN=1 to run it");

    if (options.renderThread) {
        int result = RunRenderThreadMode(window, options);
        DestroyWindow(window);
        return result;
    }

    std::unique_ptr<Host> host = CreateHost(options.api);
    if (!host->Init(window, options)) {
        printf("SKIP: %s is not available on this machine\n", options.api.c_str());
        uih::Stop();
        return kSkip;
    }
    if (options.secondWindow) {
        int result = RunSecondWindowMode(window, *host, options);
        host.reset();
        DestroyWindow(window);
        return result;
    }
    if (!options.fault.empty()) {
        int result = RunFaultMode(window, *host, options);
        host.reset();
        DestroyWindow(window);
        return result;
    }
    if (options.remove) {
        int result = RunRemoveMode(window, *host);
        host.reset();
        DestroyWindow(window);
        return result;
    }
    if (!options.gamepad.empty()) {
        int result = RunGamepadMode(window, *host, options.gamepad);
        host.reset();
        DestroyWindow(window);
        return result;
    }
    if (options.slowFrameMs > 0) {
        int result = RunSlowFrameMode(window, *host, options);
        host.reset();
        DestroyWindow(window);
        return result;
    }
    if (!options.input.empty()) {
        int result = RunInputMode(window, *host, options.input == "directinput");
        host.reset();
        DestroyWindow(window);
        return result;
    }
    if (!options.preview.empty()) {
        int result = RunPreviewMode(window, *host, options);
        host.reset();
        return result;
    }
    if (!options.dll.empty()) {
        int result = RunDllMode(window, *host, options);
        host.reset();
        DestroyWindow(window);
        return result;
    }

    int frame = 0;
    ULONGLONG injectTime = GetTickCount64();
    int activeFrame = -1;
    int activeImGuiFrame = 0;
    int stopFrame = -1;
    int cycle = 1;
    int result = 0;
    while (result == 0) {
        PumpMessages();
        if (frame == options.injectAt && frame > 0) {
            StartHook(options);
            injectTime = GetTickCount64();
        }
        host->Frame();
        ++frame;

        if (activeFrame < 0) {
            if (uih::ActiveBackend() == expected) {
                activeFrame = frame;
                activeImGuiFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
                printf("%s backend active after %d frames\n", options.api.c_str(), frame - options.injectAt);
                uih::SetMenuOpen(true);
                if (options.resubclass && cycle == 1)
                    SubclassWindow(window);
            } else if (frame > options.injectAt && GetTickCount64() - injectTime > kDetectionTimeoutMs) {
                result = Fail("backend was not detected");
            }
        } else if (stopFrame < 0 && frame == activeFrame + 60) {
            if (!ImGui::GetCurrentContext() || ImGui::GetIO().MetricsRenderVertices == 0) {
                result = Fail("ImGui did not render any geometry");
                break;
            }
            printf("ImGui rendered %d vertices\n", ImGui::GetIO().MetricsRenderVertices);
            if (options.resubclass && !ProcedureChainWorks(window)) {
                result = Fail("the procedures of the window no longer chain");
                break;
            }
            int imguiFrames = ImGui::GetFrameCount() - activeImGuiFrame;
            if (imguiFrames > frame - activeFrame + 1) {
                printf("ImGui ran %d frames for %d presents\n", imguiFrames, frame - activeFrame);
                result = Fail("the overlay is drawn more than once per frame");
                break;
            }
            if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad)) {
                result = Fail("gamepad navigation is off while the menu is open");
                break;
            }
            if (options.scale > 0.0f && ImGui::GetStyle().FontScaleDpi != options.scale) {
                result = Fail("the interface scale was not applied");
                break;
            }
            if (!g_setupCalled) {
                result = Fail("the setup callback was not called");
                break;
            }
            if (g_texture->Status != ImTextureStatus_OK || g_texture->GetTexID() == ImTextureID_Invalid) {
                result = Fail("the texture was not uploaded");
                break;
            }
            for (ImTextureData* texture : g_textures)
                if (result == 0 && (texture->Status != ImTextureStatus_OK || texture->GetTexID() == ImTextureID_Invalid))
                    result = Fail("a texture was not uploaded");
            if (result)
                break;
            if (options.removeDevice) {
                if (!host->RemoveDevice()) {
                    result = Skip("the host cannot remove its device");
                    break;
                }
                ULONGLONG start = GetTickCount64();
                for (int i = 0; i < 30; ++i) {
                    PumpMessages();
                    host->Frame();
                }
                ULONGLONG elapsed = GetTickCount64() - start;
                printf("30 frames on the removed device in %llu ms\n", elapsed);
                if (elapsed > 3000) {
                    result = Fail("the overlay holds up the frames of the removed device");
                    break;
                }
                host.reset();
                host = CreateHost(options.api);
                if (!host->Init(window, options)) {
                    result = Fail("the host could not create a new device");
                    break;
                }
                for (int i = 0; i < 60; ++i) {
                    PumpMessages();
                    host->Frame();
                }
                int vertices = ImGui::GetCurrentContext() ? ImGui::GetIO().MetricsRenderVertices : 0;
                printf("new device: %d vertices\n", vertices);
                if (vertices == 0 || g_texture->Status != ImTextureStatus_OK) {
                    result = Fail("the overlay did not come back on the new device");
                    break;
                }
            }
            if (options.recreateContext) {
                if (!host->RecreateContext()) {
                    result = Fail("the host could not create a new context");
                    break;
                }
                // Beyond the frames the overlay waits before it follows another context.
                for (int i = 0; i < 120; ++i) {
                    PumpMessages();
                    host->Frame();
                }
                int vertices = ImGui::GetCurrentContext() ? ImGui::GetIO().MetricsRenderVertices : 0;
                printf("new context: %d vertices\n", vertices);
                if (vertices == 0) {
                    result = Fail("the overlay did not come back in the new context");
                    break;
                }
                if (!host->TexturesIntact()) {
                    result = Fail("the overlay deleted or changed textures of the new context");
                    break;
                }
            }
            if (options.resize) {
                for (const auto& size : kResizes) {
                    ResizeWindow(window, size[0], size[1]);
                    if (!host->Resize(size[0], size[1])) {
                        result = Fail("the host could not resize its back buffers");
                        break;
                    }
                    for (int i = 0; i < 30; ++i) {
                        PumpMessages();
                        host->Frame();
                    }
                    // A screen too small for the window makes the system shrink it.
                    RECT client{};
                    GetClientRect(window, &client);
                    ImVec2 display = ImGui::GetIO().DisplaySize;
                    printf("resized to %ldx%ld: overlay %.0fx%.0f, %d vertices\n", client.right, client.bottom, display.x, display.y,
                           ImGui::GetIO().MetricsRenderVertices);
                    if (display.x != client.right || display.y != client.bottom || ImGui::GetIO().MetricsRenderVertices == 0) {
                        result = Fail("the overlay did not follow the new size");
                        break;
                    }
                }
                if (result)
                    break;
            }
            if (options.fullscreen) {
                const bool exclusive = options.api != "vulkan" && options.api != "opengl";
                for (bool on : {true, false}) {
                    bool switched = exclusive ? host->Fullscreen(on) : CoverDisplay(window, on) && host->Resize(kFullscreen[0], kFullscreen[1]);
                    if (!on) {
                        ResizeWindow(window, kWidth, kHeight);
                        switched = host->Resize(kWidth, kHeight) && switched;
                    }
                    if (!switched) {
                        result = Fail(on ? "the host could not switch to fullscreen" : "the host could not leave fullscreen");
                        break;
                    }
                    for (int i = 0; i < 60; ++i) {
                        PumpMessages();
                        host->Frame();
                    }
                    RECT client{};
                    GetClientRect(window, &client);
                    MONITORINFO monitor{sizeof(monitor)};
                    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
                    long monitorWidth = monitor.rcMonitor.right - monitor.rcMonitor.left;
                    long monitorHeight = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
                    ImVec2 display = ImGui::GetIO().DisplaySize;
                    printf("%s: monitor %ldx%ld, window %ldx%ld, overlay %.0fx%.0f, %d vertices\n", on ? "fullscreen" : "windowed", monitorWidth,
                           monitorHeight, client.right, client.bottom, display.x, display.y, ImGui::GetIO().MetricsRenderVertices);
                    // Virtual machines may emulate DXGI's exclusive mode without a mode change: the window covers the monitor either way.
                    if (on && (client.right != monitorWidth || client.bottom != monitorHeight)) {
                        result = Fail("the window does not cover the display in fullscreen");
                        break;
                    }
                    if (display.x != client.right || display.y != client.bottom || ImGui::GetIO().MetricsRenderVertices == 0) {
                        result = Fail("the overlay did not follow the display mode");
                        break;
                    }
                }
                if (result)
                    break;
            }
            if (options.capture) {
                Capture capture = CaptureWindow(window);
                printf("%d overlay pixels captured\n", capture.overlayPixels);
                // The clear color of a linear back buffer is not the reference one.
                if (options.format.empty() && capture.overlayPixels < kWidth * kHeight / 100) {
                    result = Fail("overlay is not visible on screen");
                    break;
                }
                if (!ProbeMatches(capture)) {
                    SaveCapture(capture, options.api + (options.format.empty() ? "" : "-" + options.format));
                    result = Fail("overlay colors are altered");
                    break;
                }
            }
            uih::Stop();
            stopFrame = frame;
            if (ImGui::GetCurrentContext() || uih::ActiveBackend() != uih::Backend::None)
                result = Fail("hook did not release its resources");
            else if (g_texture->GetTexID() != ImTextureID_Invalid)
                result = Fail("the texture was not released");
            for (ImTextureData* texture : g_textures)
                if (result == 0 && texture->GetTexID() != ImTextureID_Invalid)
                    result = Fail("a texture was not released");
        } else if (stopFrame >= 0 && frame == stopFrame + 60) {
            if (!options.reload || cycle == 2)
                break;
            printf("starting the hook again\n");
            ++cycle;
            StartHook(options);
            injectTime = GetTickCount64();
            activeFrame = -1;
            stopFrame = -1;
        }
    }

    uih::Stop();
    uih::DestroyTexture(g_texture);
    for (ImTextureData* texture : g_textures)
        uih::DestroyTexture(texture);
    int validationErrors = host->ValidationErrors();
    if (result == 0 && validationErrors > 0)
        result = Fail("the validation layer reported errors");
    host.reset();
    DestroyWindow(window);
    if (result == 0)
        printf("PASS: %s\n", options.api.c_str());
    return result;
}
