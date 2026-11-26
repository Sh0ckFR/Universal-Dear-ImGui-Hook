#include "internal.h"

#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace uih::detail {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                         ID3D11DeviceContext**);

constexpr size_t kPresentIndex = 8;
constexpr size_t kResizeBuffersIndex = 13;
constexpr size_t kPresent1Index = 22;
constexpr size_t kSetColorSpace1Index = 38;
constexpr size_t kResizeBuffers1Index = 39;

HMODULE g_module = nullptr;
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resizeBuffers = nullptr;
ResizeBuffers1Fn g_resizeBuffers1 = nullptr;
SetColorSpace1Fn g_setColorSpace1 = nullptr;

// The color spaces the game set on its swap chains: HDR10 stores BT.2020
// colors encoded with the PQ curve. DXGI has no getter for the color space.
std::mutex g_pqMutex;
std::vector<std::pair<IDXGISwapChain*, bool>> g_colorSpaces;

// D3D10 devices also expose ID3D11Device, so D3D10 is probed before D3D11.
const DxgiPresenter* const kPresenters[] = {&kD3D12Presenter, &kD3D10Presenter, &kD3D11Presenter};
const DxgiPresenter* g_presenter = nullptr;
std::atomic<bool> g_deviceRemoved{false};

void Teardown() {
    if (g_presenter)
        g_presenter->Teardown();
}

// False for the swap chains of a translation layer such as DXVK.
bool ImplementedByWindows(IDXGISwapChain* swapChain) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    wchar_t windows[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(VirtualMethod(swapChain, kPresentIndex)), &module))
        return false;
    GetModuleFileNameW(module, path, MAX_PATH);
    UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    return length > 0 && length < MAX_PATH && _wcsnicmp(path, windows, length) == 0;
}

const DxgiPresenter* PresenterFor(IDXGISwapChain* swapChain) {
    for (const DxgiPresenter* presenter : kPresenters)
        if (WantBackend(presenter->backend) && presenter->Probe(swapChain))
            return presenter;
    return nullptr;
}

void OnPresent(IDXGISwapChain* swapChain, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) || InsideForeignPresent())
        return;
    // Drivers present Vulkan through a DXGI swap chain of their own, some from a thread of their own.
    if (VulkanPresenting() && ImplementedByWindows(swapChain))
        return;
    if (g_presenter && ActiveBackend() != g_presenter->backend)
        g_presenter = nullptr;
    if (!g_presenter) {
        const DxgiPresenter* presenter = PresenterFor(swapChain);
        if (!presenter || !ShouldRender(presenter->backend) || !ClaimBackend(presenter->backend))
            return;
        g_presenter = presenter;
    }
    if (ShouldRender(g_presenter->backend)) {
        DXGI_SWAP_CHAIN_DESC desc{};
        swapChain->GetDesc(&desc);
        DrawOverlay(SwapchainKey(swapChain), SwapChainWindow(swapChain, desc), [&] {
            if (PresenterFor(swapChain) == g_presenter) {
                g_presenter->Render(swapChain);
                return;
            }
            // The swap chain that takes the overlay belongs to another version of Direct3D.
            g_presenter->Teardown();
            ReleaseBackend(g_presenter->backend);
            g_presenter = nullptr;
        });
    }
}

// The application answers a removed device with a new one. D3D12 gives it
// the removed device back while references to it remain, so the overlay
// releases its own before the application sees the result.
HRESULT AfterPresent(HRESULT result) {
    bool removed = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
    if (removed && !g_deviceRemoved.exchange(true))
        Log("Device removed (0x%08lX), releasing the overlay", result);
    if (removed) {
        ReleaseOverlay(Teardown);
        ReleaseRemovedQueues();
    } else {
        g_deviceRemoved = false;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    ScopedCall call;
    Guarded([&] { OnPresent(swapChain, flags); });
    if (g_presenter)
        ServiceTeardown(g_presenter->backend, Teardown);
    return AfterPresent(g_present(swapChain, syncInterval, flags));
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    ScopedCall call;
    Guarded([&] { OnPresent(swapChain, flags); });
    if (g_presenter)
        ServiceTeardown(g_presenter->backend, Teardown);
    return AfterPresent(g_present1(swapChain, syncInterval, flags, params));
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    ScopedCall call;
    if (g_presenter)
        g_presenter->PreResize(swapChain);
    return g_resizeBuffers(swapChain, bufferCount, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                             const UINT* nodeMasks, IUnknown* const* queues) {
    ScopedCall call;
    if (g_presenter)
        g_presenter->PreResize(swapChain);
    return g_resizeBuffers1(swapChain, bufferCount, width, height, format, flags, nodeMasks, queues);
}

// For a swap chain whose color space was set before the hook: a 10-bit swap
// chain on a display in HDR mode is HDR10, as games set it. Checked again
// every 2 seconds, since the window may move to another display.
bool OnHdrOutput(IDXGISwapChain* swapChain) {
    static IDXGISwapChain* checkedChain = nullptr;
    static ULONGLONG checked = 0;
    static bool hdr = false;
    ULONGLONG now = GetTickCount64();
    if (swapChain == checkedChain && now - checked < 2000)
        return hdr;
    checkedChain = swapChain;
    checked = now;
    hdr = false;
    IDXGIOutput* output = nullptr;
    IDXGIOutput6* output6 = nullptr;
    DXGI_OUTPUT_DESC1 desc{};
    if (SUCCEEDED(swapChain->GetContainingOutput(&output)) && SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) &&
        SUCCEEDED(output6->GetDesc1(&desc)))
        hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    Release(output6);
    Release(output);
    return hdr;
}

HRESULT STDMETHODCALLTYPE HookSetColorSpace1(IDXGISwapChain3* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace) {
    ScopedCall call;
    HRESULT hr = g_setColorSpace1(swapChain, colorSpace);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(g_pqMutex);
        bool pq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        auto it = std::find_if(g_colorSpaces.begin(), g_colorSpaces.end(), [&](const auto& entry) { return entry.first == swapChain; });
        if (it != g_colorSpaces.end())
            it->second = pq;
        else
            g_colorSpaces.emplace_back(swapChain, pq);
    }
    return hr;
}

bool Install() {
    g_module = LoadLibraryW(L"d3d11.dll");
    auto create = GetProc<D3D11CreateDeviceAndSwapChainFn>(g_module, "D3D11CreateDeviceAndSwapChain");
    if (!create)
        return false;

    HWND window = CreateHiddenWindow();
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE driver : drivers) {
        hr = create(nullptr, driver, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, nullptr);
        if (SUCCEEDED(hr))
            break;
    }

    bool hooked = false;
    if (SUCCEEDED(hr)) {
        hooked = CreateHook("IDXGISwapChain::Present", VirtualMethod(swapChain, kPresentIndex),
                            reinterpret_cast<void*>(HookPresent), reinterpret_cast<void**>(&g_present));
        CreateHook("IDXGISwapChain::ResizeBuffers", VirtualMethod(swapChain, kResizeBuffersIndex),
                   reinterpret_cast<void*>(HookResizeBuffers), reinterpret_cast<void**>(&g_resizeBuffers));

        IDXGISwapChain1* swapChain1 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1)))) {
            CreateHook("IDXGISwapChain1::Present1", VirtualMethod(swapChain1, kPresent1Index),
                       reinterpret_cast<void*>(HookPresent1), reinterpret_cast<void**>(&g_present1));
            swapChain1->Release();
        }
        IDXGISwapChain3* swapChain3 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
            CreateHook("IDXGISwapChain3::ResizeBuffers1", VirtualMethod(swapChain3, kResizeBuffers1Index),
                       reinterpret_cast<void*>(HookResizeBuffers1), reinterpret_cast<void**>(&g_resizeBuffers1));
            CreateHook("IDXGISwapChain3::SetColorSpace1", VirtualMethod(swapChain3, kSetColorSpace1Index),
                       reinterpret_cast<void*>(HookSetColorSpace1), reinterpret_cast<void**>(&g_setColorSpace1));
            swapChain3->Release();
        }
        swapChain->Release();
        device->Release();
    } else {
        Log("D3D11 dummy swap chain creation failed: 0x%08lX", hr);
    }

    DestroyWindow(window);
    return hooked;
}

void Uninstall() {
    g_presenter = nullptr;
    g_present = nullptr;
    g_present1 = nullptr;
    g_resizeBuffers = nullptr;
    g_resizeBuffers1 = nullptr;
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

const Renderer kDxgiRenderer = {"DXGI", L"dxgi.dll", Install, Uninstall, Teardown};

DXGI_FORMAT RenderTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

void EncodeColors(IDXGISwapChain* swapChain, DXGI_FORMAT format, ImDrawData* drawData) {
    if (StoresLinear(format)) {
        LinearizeColors(drawData);
        return;
    }
    if (format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R10G10B10A2_TYPELESS)
        return;
    int set = -1;
    {
        std::lock_guard<std::mutex> lock(g_pqMutex);
        for (const auto& [chain, pq] : g_colorSpaces)
            if (chain == swapChain)
                set = pq;
    }
    if (set < 0)
        set = OnHdrOutput(swapChain);
    if (set)
        EncodePqColors(drawData, platform::SdrWhiteNits());
}

bool StoresLinear(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

TexelLayout DxgiTexelLayout(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return TexelLayout::RGBA8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return TexelLayout::BGRA8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return TexelLayout::RGB10A2;
    default:
        return TexelLayout::Unsupported;
    }
}

HWND SwapChainWindow(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC& desc) {
    if (desc.OutputWindow)
        return desc.OutputWindow;
    static std::mutex mutex;
    static IDXGISwapChain* known = nullptr;
    static HWND window = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (known != swapChain || !IsWindow(window)) {
        known = swapChain;
        window = input::FindProcessWindow(desc.BufferDesc.Width, desc.BufferDesc.Height);
    }
    return window;
}

HWND CreateHiddenWindow() {
    return CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
}

} // namespace uih::detail
