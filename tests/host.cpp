// Host application rendering a solid color with one graphics API, then
// starting the hook in-process and checking that the overlay gets drawn and
// torn down cleanly.
#include "uih.h"

#include "host.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>
#include <dxgi1_4.h>
#include <GL/gl.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

    bool Init(HWND window, const Options& options) override {
        DXGI_SWAP_CHAIN_DESC desc = SwapChainDesc(window, options);
        D3D10_DRIVER_TYPE driver = options.warp ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE;
        if (FAILED(D3D10CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, D3D10_SDK_VERSION, &desc, &swapChain, &device)))
            return false;
        return CreateTarget();
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

    bool Init(HWND window, const Options& options) override {
        DXGI_SWAP_CHAIN_DESC desc = SwapChainDesc(window, options);
        D3D_DRIVER_TYPE driver = options.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context)))
            return false;
        return CreateTarget();
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
        return version && version[0] >= '3';
    }

    bool Resize(int width, int height) override {
        glViewport(0, 0, width, height);
        return true;
    }

    void Frame() override {
        glClearColor(kClear[0], kClear[1], kClear[2], kClear[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(hdc);
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
bool g_setupCalled = false;

void DrawTestMenu() {
    if (!uih::IsMenuOpen())
        return;
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(kProbeX - 50.0f, kProbeY - 50.0f), ImVec2(kProbeX + 50.0f, kProbeY + 50.0f),
                                                  IM_COL32(kProbe[0], kProbe[1], kProbe[2], 255));
    ImGui::SetNextWindowPos(ImVec2(50, 50));
    ImGui::SetNextWindowSize(ImVec2(500, 400));
    ImGui::Begin("uih host", nullptr, ImGuiWindowFlags_NoResize);
    ImGui::Text("Backend: %s", uih::BackendName(uih::ActiveBackend()));
    ImGui::Image(g_texture->GetTexRef(), ImVec2(64, 64));
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

// Loads the example DLL like an injector would and drives it with its hotkeys:
// INSERT opens the menu, F11 removes the overlay. A DLL named dinput8.dll is
// loaded the way a game loads a proxy: F11 only removes the overlay, and
// INSERT brings it back. Otherwise F11 unloads the DLL, and --reload injects
// it again.
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

        if (!PressKey(window, unloadKey))
            return Skip("the host window lost the focus");
        run(5);
        HoldKey(unloadKey, false);
        if (proxy) {
            for (ULONGLONG start = GetTickCount64(); pixels >= visible && GetTickCount64() - start < 10000;) {
                run(15);
                pixels = overlayPixels();
            }
            printf("cycle %d: %d overlay pixels after the unload key\n", cycle + 1, pixels);
            if (pixels >= visible)
                return Fail("overlay is still visible after the unload key");
            run(30);
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

// Checks that the keyboard and the mouse reach the host while the menu is
// closed, and that the host reads nothing while it is open. DirectInput
// registers the keyboard for raw input itself, and a process has one raw
// input target per device class, so raw input and DirectInput are checked
// in separate runs.
int RunInputMode(HWND window, Host& host, bool useDirectInput) {
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
    BringToFront(window);
    run(30);

    int result = 0;
    const WORD menuButtons = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    if (uih::ActiveBackend() == uih::Backend::None) {
        result = Fail("backend was not detected");
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
        else if (arg == "--proxy" && i + 1 < argc) options.proxy = argv[++i];
        else if (arg == "--scale" && i + 1 < argc) options.scale = static_cast<float>(atof(argv[++i]));
        else if (arg == "--menu-key" && i + 1 < argc) options.menuKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--unload-key" && i + 1 < argc) options.unloadKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--resize") options.resize = true;
        else if (arg == "--reload") options.reload = true;
        else if (arg == "--remove") options.remove = true;
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
        printf("usage: uih_host --api d3d9|d3d9ex|d3d10|d3d11|d3d12|vulkan|opengl [--inject-at N] [--scale S] [--resize] [--reload] [--remove] [--warp] [--capture] [--device-proc-addr] [--decoy-queue] [--format srgb|float] [--settle ms] [--input raw|directinput] [--gamepad fake-xinput.dll] [--dll path [--menu-key vk] [--unload-key vk]]\n       uih_host --proxy path\n");
        return 2;
    }

    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW), CS_OWNDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"uih_host", nullptr};
    RegisterClassExW(&windowClass);
    RECT rect{0, 0, kWidth, kHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    // Screen captures need the window above everything else.
    DWORD exStyle = options.capture || !options.dll.empty() ? WS_EX_TOPMOST : 0;
    HWND window = CreateWindowExW(exStyle, L"uih_host", L"uih host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    PumpMessages();

    // Created before the hook starts, registered with its context on the first frame.
    std::vector<uint32_t> pixels(32 * 32, IM_COL32(200, 100, 50, 255));
    g_texture = uih::CreateTexture(pixels.data(), 32, 32);

    if (options.injectAt == 0 && options.dll.empty())
        StartHook(options);

    std::unique_ptr<Host> host = CreateHost(options.api);
    if (!host->Init(window, options)) {
        printf("SKIP: %s is not available on this machine\n", options.api.c_str());
        uih::Stop();
        return kSkip;
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
    if (!options.input.empty()) {
        int result = RunInputMode(window, *host, options.input == "directinput");
        host.reset();
        DestroyWindow(window);
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
            } else if (frame > options.injectAt && GetTickCount64() - injectTime > kDetectionTimeoutMs) {
                result = Fail("backend was not detected");
            }
        } else if (stopFrame < 0 && frame == activeFrame + 60) {
            if (!ImGui::GetCurrentContext() || ImGui::GetIO().MetricsRenderVertices == 0) {
                result = Fail("ImGui did not render any geometry");
                break;
            }
            printf("ImGui rendered %d vertices\n", ImGui::GetIO().MetricsRenderVertices);
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
                    ImVec2 display = ImGui::GetIO().DisplaySize;
                    printf("resized to %dx%d: overlay %.0fx%.0f, %d vertices\n", size[0], size[1], display.x, display.y,
                           ImGui::GetIO().MetricsRenderVertices);
                    if (display.x != size[0] || display.y != size[1] || ImGui::GetIO().MetricsRenderVertices == 0) {
                        result = Fail("the overlay did not follow the new size");
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
    host.reset();
    DestroyWindow(window);
    if (result == 0)
        printf("PASS: %s\n", options.api.c_str());
    return result;
}
