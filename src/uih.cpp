#include "internal.h"

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "imgui_internal.h"

namespace uih {
namespace detail {
namespace {

enum class State { Stopped, Running, Stopping };

Config g_config;
std::mutex g_lifecycle;
std::atomic<State> g_state{State::Stopped};
std::atomic<Backend> g_activeBackend{Backend::None};
std::atomic<bool> g_menuOpen{false};
std::atomic<int> g_inFlight{0};

std::atomic<bool> g_teardownPending{false};
std::mutex g_teardownMutex;
std::condition_variable g_teardownSignal;
bool g_teardownDone = false;
// Backend whose overlay the thread drawing it releases once its frame ends.
std::atomic<Backend> g_abandonedBackend{Backend::None};

float g_appliedScale = 1.0f;
// Textures created with CreateTexture(). They are registered with each ImGui
// context in turn, on the rendering thread.
struct UserTexture {
    ImTextureData* data;
    bool registered;
    bool destroy;
};
std::mutex g_texturesMutex;
std::vector<UserTexture> g_textures;
// A texture created by the render callback is drawn in the same frame, so it
// is registered at once instead of before the next frame.
thread_local bool t_inRenderCallback = false;

bool g_menuButtonsDown = false;
bool g_menuKeyDown = false;
// Cleared by Start() and Stop() to end a wait for the menu input.
std::atomic<bool> g_waitingForMenuInput{false};
// Set when the overlay raised an exception, until the next Start().
std::atomic<bool> g_faulted{false};

// The swap chain the overlay is drawn on, guarded by g_frameMutex, which is
// held while the overlay draws or releases its resources.
struct OverlayTarget {
    uint64_t swapchain = 0;
    NativeWindow window{};
    std::chrono::steady_clock::time_point lastPresent{};
};
std::timed_mutex g_frameMutex;
OverlayTarget g_target;
std::chrono::steady_clock::time_point g_lastFocusCheck{};
// Thread that drew the last overlay frame, the one to release its resources.
std::atomic<std::thread::id> g_renderThread{};
constexpr auto kStalledTarget = std::chrono::milliseconds(500);
constexpr auto kFocusCheckInterval = std::chrono::milliseconds(250);

bool IsOverlayTarget(uint64_t swapchain, NativeWindow window) {
    auto now = std::chrono::steady_clock::now();
    OverlayTarget& target = g_target;
    if (swapchain == target.swapchain) {
        target.lastPresent = now;
        return true;
    }
    bool move = target.swapchain == 0 || (window && window == target.window) || now - target.lastPresent > kStalledTarget;
    // Focus queries may reach the X server: they are spaced out.
    if (!move && window && now - g_lastFocusCheck >= kFocusCheckInterval) {
        g_lastFocusCheck = now;
        move = platform::HasFocus(window) && !platform::HasFocus(target.window);
    }
    if (!move)
        return false;
    if (target.swapchain)
        Log("Overlay moves to swap chain 0x%llx", static_cast<unsigned long long>(swapchain));
    target = {swapchain, window, now};
    return true;
}
bool g_unloadKeyDown = false;

bool KeyPressed(int key, bool& wasDown, bool focused) {
    bool down = platform::IsKeyDown(key);
    bool pressed = down && !wasDown && focused;
    wasDown = down;
    return pressed;
}

// Runs before ImGui::NewFrame(), which starts the uploads and releases.
void UpdateTextures() {
    std::lock_guard<std::mutex> lock(g_texturesMutex);
    for (size_t i = 0; i < g_textures.size();) {
        UserTexture& texture = g_textures[i];
        if (texture.destroy) {
            if (!texture.registered || texture.data->Status == ImTextureStatus_Destroyed) {
                if (texture.registered)
                    ImGui::UnregisterUserTexture(texture.data);
                delete texture.data;
                g_textures.erase(g_textures.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            texture.data->WantDestroyNextFrame = true;
        } else if (!texture.registered) {
            ImGui::RegisterUserTexture(texture.data);
            texture.registered = true;
        }
        ++i;
    }
}

// Reads the controller through the platform backend, which feeds the ImGui gamepad keys.
bool MenuButtonsDown() {
    static const struct {
        unsigned button;
        ImGuiKey key;
    } kButtons[] = {
        {GamepadDpadUp, ImGuiKey_GamepadDpadUp},
        {GamepadDpadDown, ImGuiKey_GamepadDpadDown},
        {GamepadDpadLeft, ImGuiKey_GamepadDpadLeft},
        {GamepadDpadRight, ImGuiKey_GamepadDpadRight},
        {GamepadStart, ImGuiKey_GamepadStart},
        {GamepadBack, ImGuiKey_GamepadBack},
        {GamepadL3, ImGuiKey_GamepadL3},
        {GamepadR3, ImGuiKey_GamepadR3},
        {GamepadL1, ImGuiKey_GamepadL1},
        {GamepadR1, ImGuiKey_GamepadR1},
        {GamepadA, ImGuiKey_GamepadFaceDown},
        {GamepadB, ImGuiKey_GamepadFaceRight},
        {GamepadX, ImGuiKey_GamepadFaceLeft},
        {GamepadY, ImGuiKey_GamepadFaceUp},
    };
    unsigned wanted = g_config.menuButtons;
    if (!wanted || !(ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad))
        return false;
    for (const auto& entry : kButtons)
        if ((wanted & entry.button) && !ImGui::IsKeyDown(entry.key))
            return false;
    return true;
}

void PollKeys() {
    bool focused = platform::IsFocused();
    bool buttonsDown = MenuButtonsDown();
    bool buttonsPressed = buttonsDown && !g_menuButtonsDown && focused;
    g_menuButtonsDown = buttonsDown;
    if (KeyPressed(g_config.menuKey, g_menuKeyDown, focused) || buttonsPressed)
        SetMenuOpen(!g_menuOpen);
    if (KeyPressed(g_config.unloadKey, g_unloadKeyDown, focused))
        platform::Unload();
}

void ApplyScale() {
    float scale = g_config.scale > 0.0f ? g_config.scale : platform::WindowScale();
    if (scale <= 0.0f || scale == g_appliedScale)
        return;
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale / g_appliedScale);
    style.FontScaleDpi = scale;
    g_appliedScale = scale;
    Log("Interface scale %.2f", scale);
}

} // namespace

void Log(const char* fmt, ...) {
    if (!g_config.debugLog)
        return;
    char buffer[1024];
    int offset = snprintf(buffer, sizeof(buffer), "[uih] ");
    va_list args;
    va_start(args, fmt);
    offset += vsnprintf(buffer + offset, sizeof(buffer) - offset - 1, fmt, args);
    va_end(args);
    if (offset < 0 || offset >= static_cast<int>(sizeof(buffer)) - 1)
        offset = static_cast<int>(sizeof(buffer)) - 2;
    buffer[offset] = '\n';
    buffer[offset + 1] = '\0';
    platform::WriteLog(buffer);
}

ScopedCall::ScopedCall() { ++g_inFlight; }
ScopedCall::~ScopedCall() { --g_inFlight; }

bool WaitForIdleHooks(unsigned timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (g_inFlight > 0) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::recursive_mutex g_inputMutex;
thread_local int t_inputLocks = 0;

InputLock::InputLock() : depth_(t_inputLocks) {
    g_inputMutex.lock();
    ++t_inputLocks;
}

InputLock::~InputLock() { ReleaseInputLocks(depth_); }

int InputLockDepth() { return t_inputLocks; }

void ReleaseInputLocks(int depth) {
    for (; t_inputLocks > depth; --t_inputLocks)
        g_inputMutex.unlock();
}

thread_local int g_foreignPresentDepth = 0;

ForeignPresentScope::ForeignPresentScope() { ++g_foreignPresentDepth; }
ForeignPresentScope::~ForeignPresentScope() { --g_foreignPresentDepth; }

bool InsideForeignPresent() { return g_foreignPresentDepth > 0; }

const Config& GetConfig() { return g_config; }

bool WantBackend(Backend backend) {
    return g_config.backend == Backend::None || g_config.backend == backend;
}

bool ShouldRender(Backend backend) {
    if (g_state != State::Running || g_faulted || !WantBackend(backend))
        return false;
    Backend active = g_activeBackend;
    return active == Backend::None || active == backend;
}

bool ClaimBackend(Backend backend) {
    Backend expected = Backend::None;
    if (g_activeBackend.compare_exchange_strong(expected, backend) || expected == backend) {
        Log("%s backend active", BackendName(backend));
        return true;
    }
    return false;
}

void ReleaseBackend(Backend backend) {
    Backend expected = backend;
    if (g_activeBackend.compare_exchange_strong(expected, Backend::None))
        Log("%s backend released", BackendName(backend));
}

bool BeginOverlayFrame(uint64_t swapchain, NativeWindow window) {
    // Another thread draws the overlay: this one does not wait for it.
    if (!g_frameMutex.try_lock())
        return false;
    if (!IsOverlayTarget(swapchain, window)) {
        g_frameMutex.unlock();
        return false;
    }
    g_renderThread = std::this_thread::get_id();
    return true;
}

void EndOverlayFrame() { g_frameMutex.unlock(); }

void ServiceTeardown(Backend backend, void (*teardown)()) {
    std::thread::id renderThread = g_renderThread;
    if (renderThread != std::thread::id() && renderThread != std::this_thread::get_id())
        return;
    // Stop() gave up waiting for the frame this thread was drawing.
    Backend abandoned = backend;
    if (g_abandonedBackend.compare_exchange_strong(abandoned, Backend::None)) {
        ReleaseOverlay(teardown);
        return;
    }
    if (g_activeBackend != backend || !g_teardownPending.exchange(false))
        return;
    ReleaseOverlay(teardown);
    {
        std::lock_guard<std::mutex> lock(g_teardownMutex);
        g_teardownDone = true;
    }
    g_teardownSignal.notify_all();
}

void ReleaseOverlay(void (*release)()) {
    std::lock_guard<std::timed_mutex> lock(g_frameMutex);
    Guarded([&] { release(); });
}

bool ReleaseOverlayWithin(unsigned timeoutMs, void (*release)()) {
    std::unique_lock<std::timed_mutex> lock(g_frameMutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(timeoutMs))) {
        g_abandonedBackend = g_activeBackend.load();
        // The frame may have ended before its thread could see that.
        if (!lock.try_lock())
            return false;
        if (g_abandonedBackend.exchange(Backend::None) == Backend::None)
            return true;
    }
    Guarded([&] { release(); });
    return true;
}

bool TeardownOnRenderThread(unsigned timeoutMs) {
    if (g_activeBackend == Backend::None)
        return true;
    std::unique_lock<std::mutex> lock(g_teardownMutex);
    g_teardownDone = false;
    g_teardownPending = true;
    if (g_teardownSignal.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_teardownDone; }))
        return true;
    return !g_teardownPending.exchange(false);
}

bool CreateContext() {
    if (ImGui::GetCurrentContext())
        return false;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    g_appliedScale = 1.0f;
    if (g_config.setup)
        g_config.setup();
    return true;
}

void DestroyContext() {
    {
        std::lock_guard<std::mutex> lock(g_texturesMutex);
        for (size_t i = 0; i < g_textures.size();) {
            UserTexture& texture = g_textures[i];
            if (texture.registered)
                ImGui::UnregisterUserTexture(texture.data);
            texture.registered = false;
            if (texture.destroy) {
                delete texture.data;
                g_textures.erase(g_textures.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }
    ImGui::DestroyContext();
}

void StopUntilMenuInput() {
    Config config = GetConfig();
    uih::Stop();
    g_waitingForMenuInput = true;
    Log("Overlay removed until the menu input is pressed");
    // Input already held when the wait starts needs a fresh press.
    bool wasDown = platform::MenuInputDown(config);
    while (g_waitingForMenuInput) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bool down = platform::MenuInputDown(config);
        if (down && !wasDown)
            break;
        wasDown = down;
    }
    if (!g_waitingForMenuInput.exchange(false))
        return;
    // The press that brought the hook back opens the menu, and must not close it again.
    g_menuKeyDown = true;
    g_menuButtonsDown = true;
    uih::Start(config);
    SetMenuOpen(true);
}

void OnFault(unsigned long code) {
    t_inRenderCallback = false;
    Log("Exception 0x%08lX in the overlay", code);
    if (g_faulted.exchange(true) || g_state != State::Running)
        return;
    // Stop() waits for the hooks to return.
    std::thread(StopUntilMenuInput).detach();
}

ImDrawData* Frame() {
    UpdateTextures();
    {
        // The input events queued by the window procedure are consumed here.
        InputLock lock;
        platform::NewFrame();
        PollKeys();
        ApplyScale();
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = g_menuOpen;
        // The game keeps its controller while the menu is closed.
        if (g_menuOpen)
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        ImGui::NewFrame();
    }
    t_inRenderCallback = true;
    if (g_config.render)
        g_config.render();
    t_inRenderCallback = false;
    ImGui::Render();
    return ImGui::GetDrawData();
}

void AbandonRenderer() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    // As the backend's shutdown leaves them: created again by the next backend.
    for (ImTextureData* texture : platformIo.Textures) {
        if (texture->RefCount == 1) {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    platformIo.ClearRendererHandlers();
}

void LinearizeColors(ImDrawData* drawData) {
    static const std::array<ImU32, 256> kLinear = [] {
        std::array<ImU32, 256> table{};
        for (int i = 0; i < 256; ++i) {
            float c = i / 255.0f;
            float linear = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            table[i] = static_cast<ImU32>(linear * 255.0f + 0.5f);
        }
        return table;
    }();
    for (ImDrawList* list : drawData->CmdLists) {
        for (ImDrawVert& vertex : list->VtxBuffer) {
            ImU32 c = vertex.col;
            vertex.col = (c & IM_COL32_A_MASK) | kLinear[(c >> IM_COL32_R_SHIFT) & 0xFF] << IM_COL32_R_SHIFT |
                         kLinear[(c >> IM_COL32_G_SHIFT) & 0xFF] << IM_COL32_G_SHIFT | kLinear[(c >> IM_COL32_B_SHIFT) & 0xFF] << IM_COL32_B_SHIFT;
        }
    }
}

} // namespace detail

using namespace detail;

void Start(const Config& config) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    g_waitingForMenuInput = false;
    if (g_state != State::Stopped)
        return;

    g_config = config;
    g_activeBackend = Backend::None;
    g_menuOpen = false;
    g_teardownPending = false;
    g_faulted = false;
    g_target = {};
    g_renderThread = std::thread::id();
    g_state = State::Running;
    if (!platform::Start())
        g_state = State::Stopped;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    g_waitingForMenuInput = false;
    if (g_state != State::Running)
        return;
    g_state = State::Stopping;
    Log("Stopping");
    // The application gets its input and its cursor back.
    SetMenuOpen(false);

    platform::Stop();

    g_activeBackend = Backend::None;
    g_state = State::Stopped;
    Log("Stopped");
}

Backend ActiveBackend() { return g_activeBackend; }

bool IsMenuOpen() { return g_menuOpen; }

void SetMenuOpen(bool open) {
    if (g_menuOpen.exchange(open) == open)
        return;
    Log("Menu %s", open ? "opened" : "closed");
    input::OnMenuToggled(open);
}

ImTextureData* CreateTexture(const void* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_texturesMutex);
    // Textures waiting for their release still hold a descriptor.
    if (g_textures.size() >= static_cast<size_t>(kMaxTextures)) {
        Log("CreateTexture: %d textures already exist", kMaxTextures);
        return nullptr;
    }
    auto* texture = new ImTextureData();
    texture->Create(ImTextureFormat_RGBA32, width, height);
    memcpy(texture->GetPixels(), rgba, static_cast<size_t>(width) * height * 4);
    texture->UseColors = true;
    g_textures.push_back({texture, t_inRenderCallback, false});
    if (t_inRenderCallback)
        ImGui::RegisterUserTexture(texture);
    return texture;
}

void DestroyTexture(ImTextureData* texture) {
    std::lock_guard<std::mutex> lock(g_texturesMutex);
    for (size_t i = 0; i < g_textures.size(); ++i) {
        if (g_textures[i].data != texture)
            continue;
        // No backend knows about it: nothing to wait for.
        if (!g_textures[i].registered) {
            delete texture;
            g_textures.erase(g_textures.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            g_textures[i].destroy = true;
        }
        return;
    }
}

const char* BackendName(Backend backend) {
    switch (backend) {
    case Backend::D3D9: return "D3D9";
    case Backend::D3D10: return "D3D10";
    case Backend::D3D11: return "D3D11";
    case Backend::D3D12: return "D3D12";
    case Backend::Vulkan: return "Vulkan";
    case Backend::OpenGL: return "OpenGL";
    default: return "None";
    }
}

} // namespace uih
