#pragma once

#include "uih.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "imgui.h"

namespace uih::detail {

#ifdef _WIN32
using NativeWindow = HWND;
#else
// X11 window, 0 when unknown.
using NativeWindow = uint32_t;
#endif

void Log(const char* fmt, ...);

// Every hook keeps one of these alive while it runs so Stop() can wait for
// in-flight calls before the hooks are removed.
class ScopedCall {
public:
    ScopedCall();
    ~ScopedCall();
    ScopedCall(const ScopedCall&) = delete;
    ScopedCall& operator=(const ScopedCall&) = delete;
};
bool WaitForIdleHooks(unsigned timeoutMs);

// Drivers may implement presentation of one API on top of another (Vulkan on
// a DXGI swap chain). Hooks of the outer API mark the thread so the inner API
// ignores the nested present.
class ForeignPresentScope {
public:
    ForeignPresentScope();
    ~ForeignPresentScope();
    ForeignPresentScope(const ForeignPresentScope&) = delete;
    ForeignPresentScope& operator=(const ForeignPresentScope&) = delete;
};
bool InsideForeignPresent();

// Frame orchestration ----------------------------------------------------

const Config& GetConfig();
// True when `backend` is allowed by the configuration.
bool WantBackend(Backend backend);
// True when hooks are running and `backend` is either active or no backend
// has claimed the process yet.
bool ShouldRender(Backend backend);
// Marks `backend` as the active one. Fails when another backend already owns the process.
bool ClaimBackend(Backend backend);
// Runs `teardown` when Stop() asked the active backend to release its resources
// on the rendering thread.
void ServiceTeardown(Backend backend, void (*teardown)());
// False when the rendering thread did not answer and the caller has to release the resources.
bool TeardownOnRenderThread(unsigned timeoutMs);

// False when the ImGui context already exists.
bool CreateContext();
// Creates the ImGui context on first use and attaches the platform backend
// and input hooks to `window`.
bool BindWindow(NativeWindow window);
// Platform NewFrame, key polling, user callback and ImGui::Render().
ImDrawData* Frame();
// Detaches the platform backend and destroys the ImGui context.
void UnbindWindow();

// Implemented once per platform -------------------------------------------

namespace platform {
void WriteLog(const char* line);
bool Start();
void Stop();
void NewFrame();
bool IsKeyDown(int key);
bool IsFocused();
void Unload();
} // namespace platform

namespace input {
void Attach(NativeWindow window);
void Detach();
// Called when the menu opens or closes: releases the cursor and hides or
// restores the input the application sees.
void OnMenuToggled(bool open);
// Best-effort lookup of the main window of this process.
NativeWindow FindProcessWindow();
} // namespace input

} // namespace uih::detail

#ifdef _WIN32
#include "windows/platform.h"
#else
#include "linux/platform.h"
#endif
