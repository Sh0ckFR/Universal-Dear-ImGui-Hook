#include "internal.h"

#include <cxxabi.h>
#include <setjmp.h>
#include <signal.h>

#include <cstdio>
#include <thread>

namespace uih::detail {
namespace {

std::atomic<NativeWindow> g_window{0};
std::atomic<bool> g_unloadRequested{false};

// Faults of the overlay jump back to RunGuarded() on the thread that raised
// them; the others go to the handler installed before this one.
constexpr int kFaultSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
struct sigaction g_previous[NSIG];
thread_local sigjmp_buf* t_guard = nullptr;
// Code that OnFault() logs for a C++ exception, as MSVC raises it.
constexpr unsigned long kCppException = 0xE06D7363;

void OnFaultSignal(int signal, siginfo_t* info, void* context) {
    if (sigjmp_buf* guard = t_guard)
        siglongjmp(*guard, signal);
    const struct sigaction& previous = g_previous[signal];
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) {
            previous.sa_sigaction(signal, info, context);
            return;
        }
    } else if (previous.sa_handler == SIG_IGN) {
        return;
    } else if (previous.sa_handler != SIG_DFL) {
        previous.sa_handler(signal);
        return;
    }
    // The default action, as the instruction faults again on return.
    struct sigaction defaults{};
    defaults.sa_handler = SIG_DFL;
    sigemptyset(&defaults.sa_mask);
    sigaction(signal, &defaults, nullptr);
}

// Once: handlers installed afterwards by the game, such as crash reporters,
// usually pass the signals they do not handle to this one.
void InstallFaultHandlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction handler{};
        handler.sa_sigaction = OnFaultSignal;
        handler.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&handler.sa_mask);
        for (int signal : kFaultSignals)
            sigaction(signal, &handler, &g_previous[signal]);
    });
}

} // namespace

bool BindWindow(NativeWindow window) {
    if (g_window == window && ImGui::GetCurrentContext())
        return true;

    if (!CreateContext())
        input::Detach();

    g_window = window;
    ImGui::GetIO().BackendPlatformName = "uih_x11";
    input::Attach(window);
    Log("Bound to window 0x%x", window);
    return true;
}

NativeWindow BoundWindow() { return g_window; }

void UnbindWindow() {
    if (!ImGui::GetCurrentContext())
        return;
    input::Detach();
    DestroyContext();
    g_window = 0;
}

namespace platform {

void WriteLog(const char* line) { fputs(line, stderr); }

bool Start() {
    g_unloadRequested = false;
    InstallFaultHandlers();
    return true;
}

void Stop() {
    // Not while a frame of the overlay still uses the resources: that thread releases them afterwards.
    if (!TeardownOnRenderThread(1000)) {
        Log("Rendering thread did not respond, releasing resources from Stop()");
        if (!ReleaseOverlayWithin(2000, TeardownVulkan))
            Log("The overlay is still drawing, it is released when the frame ends");
    }
}

void NewFrame() {
    input::NewFrame();
    remote::Listen();
}

bool IsKeyDown(int key) { return input::IsKeyDown(key); }

bool IsFocused() { return input::IsFocused(); }

float WindowScale() { return input::Scale(); }

// A preloaded library cannot be unloaded: the overlay is removed and comes
// back with the menu key.
void Unload() {
    if (g_unloadRequested.exchange(true))
        return;
    Log("Removal requested");
    std::thread(StopUntilMenuInput).detach();
}

bool MenuInputDown(const Config& config) { return config.menuKey && input::KeyDownInApplication(config.menuKey); }

bool HasFocus(NativeWindow window) { return input::HasFocus(window); }

// An OpenGL context cannot be released from another thread.
bool TeardownBackends() { return false; }

bool RunGuarded(void (*work)(void*), void* context) {
    sigjmp_buf guard;
    sigjmp_buf* outer = t_guard;
    if (int signal = sigsetjmp(guard, 1)) {
        t_guard = outer;
        OnFault(static_cast<unsigned long>(signal));
        return false;
    }
    t_guard = &guard;
    try {
        work(context);
    } catch (abi::__forced_unwind&) {
        t_guard = outer;
        throw;
    } catch (...) {
        t_guard = outer;
        OnFault(kCppException);
        return false;
    }
    t_guard = outer;
    return true;
}

} // namespace platform

} // namespace uih::detail
