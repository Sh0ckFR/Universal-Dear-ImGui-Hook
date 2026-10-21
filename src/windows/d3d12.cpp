#include "internal.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <vector>

#include "imgui_impl_dx12.h"

namespace uih::detail {
namespace {

using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                                   IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                                    IDXGISwapChain1**);

constexpr size_t kExecuteCommandListsIndex = 10;
constexpr size_t kCreateSwapChainIndex = 10;
constexpr size_t kCreateSwapChainForHwndIndex = 15;
constexpr size_t kCreateSwapChainForCoreWindowIndex = 16;
constexpr size_t kCreateSwapChainForCompositionIndex = 24;
// Swap chains and direct queues remembered; the oldest entry goes first.
constexpr size_t kMaxSwapChains = 16;
constexpr size_t kMaxDirectQueues = 16;
// Presents of a swap chain to wait before following an application that moved to another direct queue.
constexpr int kQueueSwitchFrames = 60;

HMODULE g_module = nullptr;
HMODULE g_dxgiModule = nullptr;
ExecuteCommandListsFn g_executeCommandLists = nullptr;
CreateSwapChainFn g_createSwapChain = nullptr;
CreateSwapChainForHwndFn g_createSwapChainForHwnd = nullptr;
CreateSwapChainForCoreWindowFn g_createSwapChainForCoreWindow = nullptr;
CreateSwapChainForCompositionFn g_createSwapChainForComposition = nullptr;

// The queue a swap chain presents on is only known when the swap chain is
// created. When the hook comes later, a direct queue of the swap chain's
// device stands in for it: the one the presenting thread submitted to last,
// as engines usually present from the thread that submits the frame, or else
// the most recently used one.
struct SwapChainQueue {
    IUnknown* swapChain;
    ID3D12CommandQueue* queue;
};

struct DirectQueue {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
};

std::mutex g_queueMutex;
std::vector<SwapChainQueue> g_swapChainQueues;
std::vector<DirectQueue> g_directQueues;
// The per-thread cache outlives Stop(): it is only trusted for the session
// of the hook that filled it.
std::atomic<unsigned> g_session{0};
thread_local ID3D12CommandQueue* t_lastQueue = nullptr;
thread_local unsigned t_lastSession = 0;

ID3D12CommandQueue* LastQueueOfThread() {
    return t_lastSession == g_session.load(std::memory_order_relaxed) ? t_lastQueue : nullptr;
}

struct FrameContext {
    ID3D12CommandAllocator* allocator = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    UINT64 fenceValue = 0;
};

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D12DescriptorHeap* g_targetHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12GraphicsCommandList* g_commandList = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceValue = 0;
std::vector<FrameContext> g_frames;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
// Unused slots of the SRV heap, the lowest last.
std::vector<UINT> g_freeSrvs;
UINT g_bufferCount = 0;
int g_queueMisses = 0;
bool g_framesDirty = false;
bool g_imguiReady = false;

IUnknown* Identity(IUnknown* object) {
    IUnknown* identity = nullptr;
    if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity))))
        identity->Release();
    return identity;
}

void RecordSwapChain(IUnknown* device, IUnknown* swapChain) {
    ID3D12CommandQueue* queue = nullptr;
    if (!device || !swapChain || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        return;
    IUnknown* identity = Identity(swapChain);
    std::lock_guard<std::mutex> lock(g_queueMutex);
    for (SwapChainQueue& entry : g_swapChainQueues) {
        if (entry.swapChain == identity) {
            entry.queue->Release();
            entry.queue = queue;
            return;
        }
    }
    if (g_swapChainQueues.size() == kMaxSwapChains) {
        g_swapChainQueues.front().queue->Release();
        g_swapChainQueues.erase(g_swapChainQueues.begin());
    }
    g_swapChainQueues.push_back({identity, queue});
}

void RecordDirectQueue(ID3D12CommandQueue* queue) {
    if (queue == LastQueueOfThread())
        return;
    t_lastQueue = queue;
    t_lastSession = g_session.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_queueMutex);
    for (size_t i = 0; i < g_directQueues.size(); ++i) {
        if (g_directQueues[i].queue == queue) {
            DirectQueue entry = g_directQueues[i];
            g_directQueues.erase(g_directQueues.begin() + i);
            g_directQueues.push_back(entry);
            return;
        }
    }
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return;
    device->Release();
    if (g_directQueues.size() == kMaxDirectQueues) {
        g_directQueues.front().queue->Release();
        g_directQueues.erase(g_directQueues.begin());
    }
    queue->AddRef();
    g_directQueues.push_back({device, queue});
}

// Queue `swapChain` presents on, with a reference. `known` is false for a guess.
ID3D12CommandQueue* QueueFor(IDXGISwapChain* swapChain, bool& known) {
    IUnknown* identity = Identity(swapChain);
    ID3D12Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return nullptr;
    device->Release();

    std::lock_guard<std::mutex> lock(g_queueMutex);
    ID3D12CommandQueue* queue = nullptr;
    known = false;
    for (const SwapChainQueue& entry : g_swapChainQueues) {
        if (entry.swapChain == identity) {
            queue = entry.queue;
            known = true;
        }
    }
    for (const DirectQueue& entry : g_directQueues) {
        if (known || entry.device != device)
            continue;
        queue = entry.queue;
        if (queue == LastQueueOfThread())
            break;
    }
    if (queue)
        queue->AddRef();
    return queue;
}

void WaitForGpu() {
    if (!g_fence || !g_queue || g_fence->GetCompletedValue() >= g_fenceValue)
        return;
    if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent)))
        WaitForSingleObject(g_fenceEvent, 2000);
}

void WaitForFrame(const FrameContext& frame) {
    if (g_fence->GetCompletedValue() >= frame.fenceValue)
        return;
    if (SUCCEEDED(g_fence->SetEventOnCompletion(frame.fenceValue, g_fenceEvent)))
        WaitForSingleObject(g_fenceEvent, 2000);
}

void ReleaseFrames() {
    for (FrameContext& frame : g_frames)
        Release(frame.allocator);
    g_frames.clear();
    Release(g_targetHeap);
}

void AllocateSrv(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    IM_ASSERT(!g_freeSrvs.empty() && "SRV descriptor heap exhausted");
    UINT index = g_freeSrvs.back();
    g_freeSrvs.pop_back();
    UINT increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += static_cast<SIZE_T>(index) * increment;
    gpu->ptr += static_cast<UINT64>(index) * increment;
}

void FreeSrv(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    UINT increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    SIZE_T index = (cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / increment;
    g_freeSrvs.push_back(static_cast<UINT>(index));
}

bool Probe(IDXGISwapChain* swapChain) {
    ID3D12Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    device->Release();
    return true;
}

void Teardown() {
    if (!g_device)
        return;
    WaitForGpu();
    if (g_imguiReady)
        ImGui_ImplDX12_Shutdown();
    g_imguiReady = false;
    UnbindWindow();
    ReleaseFrames();
    Release(g_srvHeap);
    Release(g_commandList);
    Release(g_fence);
    if (g_fenceEvent) {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
    Release(g_queue);
    Release(g_device);
    g_swapChain = nullptr;
    g_format = DXGI_FORMAT_UNKNOWN;
    g_bufferCount = 0;
    g_queueMisses = 0;
}

bool CreateFrames(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC& desc) {
    ReleaseFrames();

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = desc.BufferCount;
    if (FAILED(g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_targetHeap))))
        return false;

    UINT increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE target = g_targetHeap->GetCPUDescriptorHandleForHeapStart();
    g_frames.resize(desc.BufferCount);
    for (FrameContext& frame : g_frames) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))))
            return false;
        frame.target = target;
        target.ptr += increment;
    }

    if (!g_commandList) {
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_commandList))))
            return false;
        g_commandList->Close();
    }
    g_swapChain = swapChain;
    g_bufferCount = desc.BufferCount;
    g_framesDirty = false;
    return true;
}

bool Init(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue) {
    DXGI_SWAP_CHAIN_DESC desc{};
    ID3D12Device* device = nullptr;
    if (FAILED(swapChain->GetDesc(&desc)) || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return false;

    DXGI_FORMAT format = RenderTargetFormat(desc.BufferDesc.Format);
    if (g_device && (device != g_device || queue != g_queue || format != g_format || desc.BufferCount != g_bufferCount))
        Teardown();
    if (!BindWindow(desc.OutputWindow)) {
        device->Release();
        return false;
    }

    if (!g_device) {
        g_device = device;
        g_queue = queue;
        g_queue->AddRef();
        g_format = format;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kTextureDescriptors;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_srvHeap))) ||
            FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))) || !g_fenceEvent ||
            !CreateFrames(swapChain, desc)) {
            Log("D3D12 resource creation failed");
            Teardown();
            return false;
        }
        g_freeSrvs.clear();
        for (UINT index = kTextureDescriptors; index-- > 0;)
            g_freeSrvs.push_back(index);
        g_fenceValue = 0;

        ImGui_ImplDX12_InitInfo info{};
        info.Device = g_device;
        info.CommandQueue = g_queue;
        info.NumFramesInFlight = static_cast<int>(desc.BufferCount);
        info.RTVFormat = g_format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = g_srvHeap;
        info.SrvDescriptorAllocFn = AllocateSrv;
        info.SrvDescriptorFreeFn = FreeSrv;
        g_imguiReady = ImGui_ImplDX12_Init(&info);
        if (!g_imguiReady)
            Teardown();
        return g_imguiReady;
    }

    device->Release();
    if (!CreateFrames(swapChain, desc)) {
        Log("D3D12 frame resource creation failed");
        Teardown();
        return false;
    }
    return true;
}

void Render(IDXGISwapChain* swapChain) {
    bool known = false;
    ID3D12CommandQueue* queue = QueueFor(swapChain, known);
    if (!queue)
        return;
    if (!known && swapChain == g_swapChain && queue != g_queue && ++g_queueMisses < kQueueSwitchFrames) {
        queue->Release();
        queue = g_queue;
        queue->AddRef();
    } else {
        g_queueMisses = 0;
    }
    bool ok = true;
    if (swapChain != g_swapChain || queue != g_queue || g_framesDirty)
        ok = Init(swapChain, queue);
    queue->Release();
    if (!ok)
        return;

    IDXGISwapChain3* swapChain3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
        return;
    UINT index = swapChain3->GetCurrentBackBufferIndex();
    swapChain3->Release();
    if (index >= g_frames.size())
        return;

    // Before any reference to the back buffer, which an exception in the frame would leak.
    ImGui_ImplDX12_NewFrame();
    ImDrawData* drawData = Frame();
    if (StoresLinear(g_format))
        LinearizeColors(drawData);

    ID3D12Resource* buffer = nullptr;
    if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&buffer))))
        return;

    FrameContext& frame = g_frames[index];
    WaitForFrame(frame);
    if (FAILED(frame.allocator->Reset()) || FAILED(g_commandList->Reset(frame.allocator, nullptr))) {
        buffer->Release();
        return;
    }

    D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
    viewDesc.Format = g_format;
    viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(buffer, &viewDesc, frame.target);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_commandList->ResourceBarrier(1, &barrier);
    g_commandList->OMSetRenderTargets(1, &frame.target, FALSE, nullptr);
    g_commandList->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(drawData, g_commandList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);
    g_commandList->Close();

    ID3D12CommandList* lists[] = {g_commandList};
    g_executeCommandLists(g_queue, 1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    frame.fenceValue = g_fenceValue;
    buffer->Release();
}

void PreResize(IDXGISwapChain* swapChain) {
    if (swapChain != g_swapChain)
        return;
    WaitForGpu();
    g_framesDirty = true;
}

void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    ScopedCall call;
    if (count && lists && lists[0] && lists[0]->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
        RecordDirectQueue(queue);
    g_executeCommandLists(queue, count, lists);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
    ScopedCall call;
    HRESULT hr = g_createSwapChain(factory, device, desc, swapChain);
    if (SUCCEEDED(hr) && swapChain)
        RecordSwapChain(device, *swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** swapChain) {
    ScopedCall call;
    HRESULT hr = g_createSwapChainForHwnd(factory, device, window, desc, fullscreen, output, swapChain);
    if (SUCCEEDED(hr) && swapChain)
        RecordSwapChain(device, *swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output, IDXGISwapChain1** swapChain) {
    ScopedCall call;
    HRESULT hr = g_createSwapChainForCoreWindow(factory, device, window, desc, output, swapChain);
    if (SUCCEEDED(hr) && swapChain)
        RecordSwapChain(device, *swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            IDXGIOutput* output, IDXGISwapChain1** swapChain) {
    ScopedCall call;
    HRESULT hr = g_createSwapChainForComposition(factory, device, desc, output, swapChain);
    if (SUCCEEDED(hr) && swapChain)
        RecordSwapChain(device, *swapChain);
    return hr;
}

void HookFactory() {
    g_dxgiModule = LoadLibraryW(L"dxgi.dll");
    auto createFactory = GetProc<CreateDXGIFactory1Fn>(g_dxgiModule, "CreateDXGIFactory1");
    IDXGIFactory2* factory = nullptr;
    if (!createFactory || FAILED(createFactory(IID_PPV_ARGS(&factory)))) {
        Log("DXGI factory creation failed, swap chain queues will be guessed");
        return;
    }
    CreateHook("IDXGIFactory::CreateSwapChain", VirtualMethod(factory, kCreateSwapChainIndex),
               reinterpret_cast<void*>(HookCreateSwapChain), reinterpret_cast<void**>(&g_createSwapChain));
    CreateHook("IDXGIFactory2::CreateSwapChainForHwnd", VirtualMethod(factory, kCreateSwapChainForHwndIndex),
               reinterpret_cast<void*>(HookCreateSwapChainForHwnd), reinterpret_cast<void**>(&g_createSwapChainForHwnd));
    CreateHook("IDXGIFactory2::CreateSwapChainForCoreWindow", VirtualMethod(factory, kCreateSwapChainForCoreWindowIndex),
               reinterpret_cast<void*>(HookCreateSwapChainForCoreWindow), reinterpret_cast<void**>(&g_createSwapChainForCoreWindow));
    CreateHook("IDXGIFactory2::CreateSwapChainForComposition", VirtualMethod(factory, kCreateSwapChainForCompositionIndex),
               reinterpret_cast<void*>(HookCreateSwapChainForComposition), reinterpret_cast<void**>(&g_createSwapChainForComposition));
    factory->Release();
}

ID3D12Device* CreateDummyDevice() {
    auto createDevice = GetProc<D3D12CreateDeviceFn>(g_module, "D3D12CreateDevice");
    if (!createDevice)
        return nullptr;

    ID3D12Device* device = nullptr;
    if (SUCCEEDED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return device;

    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    auto createFactory = dxgi ? GetProc<CreateDXGIFactory1Fn>(dxgi, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory4* factory = nullptr;
    if (!createFactory || FAILED(createFactory(IID_PPV_ARGS(&factory))))
        return nullptr;
    IDXGIAdapter* warp = nullptr;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        createDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        warp->Release();
    }
    factory->Release();
    return device;
}

bool InstallQueueHook() {
    g_module = LoadLibraryW(L"d3d12.dll");
    HookFactory();
    ID3D12Device* device = CreateDummyDevice();
    if (!device) {
        Log("D3D12 dummy device creation failed");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC desc{};
    ID3D12CommandQueue* queue = nullptr;
    bool hooked = false;
    if (SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
        hooked = CreateHook("ID3D12CommandQueue::ExecuteCommandLists", VirtualMethod(queue, kExecuteCommandListsIndex),
                            reinterpret_cast<void*>(HookExecuteCommandLists), reinterpret_cast<void**>(&g_executeCommandLists));
        queue->Release();
    }
    device->Release();
    return hooked;
}

void UninstallQueueHook() {
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (SwapChainQueue& entry : g_swapChainQueues)
            entry.queue->Release();
        for (DirectQueue& entry : g_directQueues)
            entry.queue->Release();
        g_swapChainQueues.clear();
        g_directQueues.clear();
        ++g_session;
    }
    g_executeCommandLists = nullptr;
    g_createSwapChain = nullptr;
    g_createSwapChainForHwnd = nullptr;
    g_createSwapChainForCoreWindow = nullptr;
    g_createSwapChainForComposition = nullptr;
    for (HMODULE* module : {&g_module, &g_dxgiModule}) {
        if (*module) {
            FreeLibrary(*module);
            *module = nullptr;
        }
    }
}

void NoTeardown() {}

} // namespace

const DxgiPresenter kD3D12Presenter = {Backend::D3D12, Probe, Render, PreResize, Teardown};
const Renderer kD3D12QueueRenderer = {"D3D12", L"d3d12.dll", InstallQueueHook, UninstallQueueHook, NoTeardown};

} // namespace uih::detail
