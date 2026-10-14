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
    IDirect3D9* d3d = nullptr;
    IDirect3D9Ex* d3dEx = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* offscreen = nullptr;

    explicit D3D9Host(bool useEx) : ex(useEx) {}

    bool Init(HWND window, const Options&) override {
        D3DPRESENT_PARAMETERS present{};
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
        return SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
               SUCCEEDED(device->CreateTexture(256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr)) &&
               SUCCEEDED(texture->GetSurfaceLevel(0, &offscreen));
    }

    void Frame() override {
        device->SetRenderTarget(0, backBuffer);
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
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBuffers;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* swapChain1 = nullptr;
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

    bool Init(HWND hwnd, const Options&) override {
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
        return version && version[0] >= '3';
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

void PumpMessages() {
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
    config.debugLog = true;
    uih::Start(config);
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

// Real key presses: only sent while the host window has the focus.
bool PressKey(HWND window, int key) {
    if (GetForegroundWindow() != window)
        return false;
    HoldKey(key, true);
    return true;
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
        if (frame == options.injectAt + 85)
            SetForegroundWindow(window);
        if (frame == options.injectAt + 90 && !PressKey(window, VK_INSERT))
            return Skip("the host window does not have the focus");
        if (frame == options.injectAt + 95)
            HoldKey(VK_INSERT, false);
        if (frame == options.injectAt + 180) {
            int pixels = CountOverlayPixels(window);
            printf("%d overlay pixels captured\n", pixels);
            if (pixels < kWidth * kHeight / 100)
                return Fail("overlay is not visible on screen");
            if (!PressKey(window, VK_F11))
                return Skip("the host window lost the focus");
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
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--api" && i + 1 < argc) options.api = argv[++i];
        else if (arg == "--inject-at" && i + 1 < argc) options.injectAt = atoi(argv[++i]);
        else if (arg == "--dll" && i + 1 < argc) options.dll = argv[++i];
        else if (arg == "--proxy" && i + 1 < argc) options.proxy = argv[++i];
        else if (arg == "--scale" && i + 1 < argc) options.scale = static_cast<float>(atof(argv[++i]));
        else if (arg == "--warp") options.warp = true;
        else if (arg == "--capture") options.capture = true;
        else if (arg == "--device-proc-addr") options.deviceProcAddr = true;
        else if (arg == "--decoy-queue") options.decoyQueue = true;
    }
    if (!options.proxy.empty())
        return RunProxyMode(options);
    uih::Backend expected = ExpectedBackend(options.api);
    if (expected == uih::Backend::None) {
        printf("usage: uih_host --api d3d9|d3d9ex|d3d10|d3d11|d3d12|vulkan|opengl [--inject-at N] [--scale S] [--warp] [--capture] [--device-proc-addr] [--decoy-queue] [--dll path]\n       uih_host --proxy path\n");
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
        StartHook(options);

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
    int activeImGuiFrame = 0;
    int stopFrame = -1;
    int result = 0;
    while (result == 0) {
        PumpMessages();
        if (frame == options.injectAt && frame > 0)
            StartHook(options);
        host->Frame();
        ++frame;

        if (activeFrame < 0) {
            if (uih::ActiveBackend() == expected) {
                activeFrame = frame;
                activeImGuiFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
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
