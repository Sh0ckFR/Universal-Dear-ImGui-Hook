# How it works

The internals of Universal-Dear-ImGui-Hook, for contributors and the curious. To use the library, the [README](../README.md) is enough.

## Where the code is

| Path | Content |
| --- | --- |
| `include/uih.h` | Public API |
| `include/uih_c.h`, `src/c_api.cpp` | The same API for C and the languages that call C |
| `src/uih.cpp`, `src/internal.h` | Platform-independent core: lifecycle, frame, textures, scale |
| `src/remote.cpp`, `src/memory.cpp` | Remote control commands, and its memory commands |
| `src/code.cpp`, `src/zydis_build.c` | The code commands: disassembler, patches and signatures, with Zydis compiled into the library on x86 and x64; the pages and threads are handled in `src/windows/code.cpp` and `src/linux/code.cpp` |
| `src/advanced.cpp` | Advanced features: chat, watch list, frozen values, instruction watch, the AI Advanced Features window |
| `src/tools.cpp` | Tools of the game for the assistant, `uih::RegisterTool()`, and the small JSON reader of `uih::ToolArgument()` |
| `src/timing.cpp` | Speed of the game's clocks, the `time` command; the clocks are hooked in `src/windows/clocks.cpp` and `src/linux/clocks.cpp` |
| `src/vulkan.cpp` | Vulkan renderer, shared by both platforms |
| `src/gl_state.h` | OpenGL state saved while the overlay draws, and the frame capture shared by WGL, GLX and EGL |
| `src/windows/` | MinHook, Direct3D, WGL, Win32 input, DirectInput, XInput, remote control (`remote.cpp`), instruction watch (`breakpoints.cpp`), code patches and exports (`code.cpp`) |
| `src/linux/` | Symbol interposition, GLX, EGL, X11 input and the game's events (`events.cpp`), remote control (`remote.cpp`), instruction watch (`breakpoints.cpp`), code patches and dynamic symbols (`code.cpp`), `exports.map` |
| `remote/` | `uih_mcp.py`, the MCP server of the remote control and its DLL injector; `uih_chat.py`, the bridge between the in-game chat and Claude Code; [`PROTOCOL.md`](../remote/PROTOCOL.md) |
| `example/` | The library that gets built: `config.cpp` (settings), `menu.cpp` (the widget tour), `image.cpp` and `image.rc` (`imgui.png` embedded), `dllmain.cpp`, `proxy.cpp`, `preload.cpp` |
| `tests/` | `uih_host`, `uih_game`, `game.sh` and `game_mcp.py` for real games, the Godot project they run, the `FetchContent` consumer, `check_docs.py` for the Markdown files, `check_sources.py` for the sources of the two build systems: see [`tests/README.md`](../tests/README.md) |
| `vs/`, `Universal-ImGui-Hook.sln` | Visual Studio projects |
| `third_party/` | Dear ImGui, MinHook, Zydis 4.1.1 and Zycore (amalgamated by its `assets/amalgamate.py`, without the encoder), Vulkan headers, stb_image (example only) |

## Startup

`Start()` launches a worker thread. On Windows, it hooks every graphics and input DLL already loaded, then the others as they load (`LdrRegisterDllNotification`). The hooks are placed with MinHook on functions found through dummy objects: a hidden window, a D3D device, a Vulkan device per GPU. The DXGI hooks wait for a Direct3D 10, 11 or 12 DLL, not for `dxgi.dll`, which the library itself imports: a Vulkan game would otherwise get a dummy D3D device while it sets up Vulkan. Under Wine, that device creates an OpenGL context, which deadlocks in libxcb with the game's Vulkan surface queries on the same X connection and freezes the game before its first frame.

Your code can place its own hooks with the same MinHook (link `minhook`): the library only removes its own, including when `unloadKey` or an exception removes the overlay. Call `MH_Initialize()` before `Start()` to keep MinHook initialized after `Stop()`; otherwise `Stop()` uninitializes it. Unloading the DLL with `unloadKey` always uninitializes it.

On Linux, the library exports the hooked functions under their own names and overrides `dlsym()`, so the game calls them whether it links them directly or looks them up at run time, as SDL and GLFW do.

## Hook points

| API | Draws in | Also hooked |
| --- | --- | --- |
| D3D9 | `IDirect3DDevice9::Present`, `IDirect3DDevice9Ex::PresentEx`, `IDirect3DSwapChain9::Present` | `Reset`, `ResetEx` |
| D3D10, 11, 12 | `IDXGISwapChain::Present`, `IDXGISwapChain1::Present1`; the device behind the swap chain picks the renderer | `ResizeBuffers`, `ResizeBuffers1` |
| D3D12 | (same) | `ID3D12CommandQueue::ExecuteCommandLists` and the factory's `CreateSwapChain*`, to find the queue the swap chain presents on |
| Vulkan | `vkQueuePresentKHR` | Device, queue, swap chain and surface creation and destruction; in each driver on Windows, the loader's exports on Linux |
| OpenGL | `wglSwapBuffers`, `glXSwapBuffers`, `eglSwapBuffers` | `glXGetProcAddress`, `eglGetProcAddress` on Linux |

## Each frame

`uih::Frame()` registers new textures, polls the hotkeys, applies the DPI scale and runs `render`. The backend then draws the ImGui data on the back buffer, uploading textures as needed, and the game's present continues.

Vertex colors are converted to linear for back buffers that store linear values; OpenGL and D3D9 turn sRGB writes off instead. On HDR10 swap chains (`IDXGISwapChain3::SetColorSpace1` with `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`, which is hooked, or `VK_COLOR_SPACE_HDR10_ST2084_EXT`), they are converted to BT.2020 and encoded with the PQ curve, white being the SDR content brightness of the display (`DisplayConfigGetDeviceInfo`, 203 nits on Linux). Vulkan submits its own command buffer, which waits on the game's semaphores and signals the one the present waits on.

**Resolution changes** are followed on every API: D3D9 `Reset`/`ResetEx`, DXGI `ResizeBuffers`, a new Vulkan swap chain, or a new size of the OpenGL window. The overlay takes the new size on the next frame.

## Input

How the game is kept from seeing input while the menu is open.

**Windows.** The game window is subclassed to feed ImGui, through a stub allocated outside the library that jumps to its window procedure. When the game, or another overlay, subclasses the window after the hook, removing the overlay points the stub at the window's original procedure instead, so the DLL can still be unloaded and injected again. Its input messages are swallowed while the menu is open, except key and button releases, keyboard and mouse raw input (filtered below) and Alt+F4. `GetRawInputData`, `GetRawInputBuffer`, `GetAsyncKeyState`, `GetKeyState`, `GetKeyboardState`, DirectInput and XInput then report no input to the game, and `GetCursorPos` a frozen position. The cursor is released from the game's `ClipCursor` rectangle, which is applied again when the menu closes. Releases always go through, so no key gets stuck; calls made by the hook itself see the real state.

**Linux.** The pointer and the keyboard are polled from the X server on a separate XCB connection. The game reads its events with `xcb_wait_for_event`, `xcb_poll_for_event` or `xcb_poll_for_queued_event`, which Xlib (and so SDL and GLFW) also calls. The library replaces these functions, and while the menu is open it drops key presses, button presses and motion, core or XInput 2, raw events included. Releases still reach the game, so no key stays down. Presses of the wheel buttons (4 to 7) scroll the menu instead.

## Teardown

What `Stop()` and the unload key do.

`Stop()` asks the rendering thread to release the backend at its next present, or does it itself after one second if no thread presents. A thread still drawing the overlay, or presenting after it, releases it once its present returns, as most devices cannot be used by two threads at once. `Stop()` then removes the hooks once none of them is running.

When the library cannot leave the process (Linux, the `dinput8.dll` proxy, or no `module`), the unload key runs `Stop()` on a thread that then polls the menu key and buttons, and restarts the hook with the menu open.

## Multiple windows or swap chains

The overlay is drawn on one swap chain, its target, by one thread at a time; the others present untouched. The target changes when it stops presenting for half a second, when another swap chain's window has the focus and the target's does not, or when the game replaces its swap chain on the same window. A Vulkan surface created before the hook is matched to the window with the swap chain's size. EGL does not tell which window a surface belongs to, so there the overlay only moves when its target stops presenting.

## Faults

Why an exception in the overlay does not crash the game.

On Windows, everything the overlay runs in the game's threads (frames, teardown, window messages) is protected by structured exception handling. An exception there, such as an access violation in the `render` callback, removes the overlay from another thread, as the unload key does without a `module`: the game keeps running, and the menu key or buttons bring the overlay back. The frame runs before the backends take a reference to the back buffer, so a fault leaks nothing. The application's draw callbacks (`ImDrawList::AddCallback`) have their own protection: after a fault in one, the backend finishes its frame and restores the device state, and the remaining callbacks of that frame are skipped.

On Linux, C++ exceptions are caught the same way, and the first `Start()` installs a handler for `SIGSEGV`, `SIGBUS`, `SIGFPE` and `SIGILL` that jumps out of the overlay when the signal comes from it; other signals go to the previously installed handler, or to the default action. Crash reporters that the game installs later usually pass on the signals they do not handle; one that does not leaves the overlay unprotected.

MinGW-w64 builds of the DLL catch C++ exceptions only.

## Frame capture

When `CaptureFrame()` waits, the backend copies its back buffer at the next present, before drawing the overlay: `GetRenderTargetData` on D3D9, a staging texture on D3D10 and 11, a readback buffer on D3D12 and Vulkan, `glReadPixels` on OpenGL. Multisampled back buffers are resolved first. To allow the copy, Vulkan swap chains are created with `TRANSFER_SRC` usage, and created again without it if the surface refuses.

## Remote control

With `remoteControl` set, a library thread serves the named pipe with overlapped I/O on Windows, or the Unix socket with `poll()` and an `eventfd` on Linux, so that `Stop()` ends a connection or a command immediately. The [protocol](../remote/PROTOCOL.md) is one text command per line.

Messages are drawn after the `render` callback, in the overlay's frame. Keys and clicks go through `SendInput` (Windows) or XTEST on a dedicated connection to the X server (Linux) when the game has the focus, and as window messages or X11 events otherwise. On Windows, a click also moves the cursor with `SendInput`, since the `SetCursorPos` hook holds it still while the menu is open.

**Tools of the game.** The server thread hands a `tool` command to the next frame of the overlay, which runs it after the `render` callback, and waits up to 10 seconds for its answer: a tool can then use the game's objects on the thread that renders, as the menu does. One request is served at a time, so one tool call waits at most.

With `aiAdvancedFeatures`, `wait` compares frames copied by `uih::CaptureFrame()`, shrunk to 160 pixels. Frozen values are written in the overlay's frame, only when they changed, and their pointer chains are resolved again each time.

**Speed of the game.** With `aiAdvancedFeatures`, the clocks the game reads are hooked: `RtlQueryPerformanceCounter`, which `QueryPerformanceCounter` jumps to, `GetTickCount`, `GetTickCount64` and `timeGetTime` with MinHook on Windows, and `clock_gettime()` by interposition on Linux. Each clock keeps its real reading and the game's at the last change of speed, and the game's reading goes on from there at the new speed, so its time never jumps back. Only the calls of the game's code are scaled, by the module of the return address: Windows and the drivers (under the Windows directory, except the C and C++ runtimes, which run the game's code), the graphics, audio and windowing libraries on Linux, and the library itself keep real time, so presenting, sound and the remote control keep working while the game is paused. Code generated at run time, such as a JIT's, counts as the game's. The overlay's frame and the server thread are marked to keep real time, since they also read the clocks through the C++ runtime. The host tests read the clock from a stub generated at run time, as the game's code would.

**Instruction watch.** On Windows, it sets debug register 3 of each thread from a dedicated thread, which suspends them one at a time, since a thread cannot change its own debug registers. A vectored exception handler counts the instructions that hit the address in a lock-free table, and new threads are picked up twice a second. On Linux, `src/linux/breakpoints.cpp` opens a perf events hardware breakpoint on each thread listed in `/proc/self/task`; it records the address of each instruction that hits it in a one-page ring buffer, read every frame, and the game gets no signal. On ARM64, where the processor stops on the instruction itself, 4 is added to its address so the hits read the same as on x86.

**Code.** `disasm` decodes with Zydis, which `src/zydis_build.c` compiles on x86 and x64 only. The instructions before an address are found by decoding from up to 15 bytes per instruction before it, then from one byte closer at a time, until the decoding lands on the address: x86 code falls in step within a few instructions. The symbols are the exports of the module on Windows, matched on x64 to the start of the function in its unwind data, and `dladdr1()` on Linux. A signature grows by whole instructions: its first pattern found less than 4096 times in the module, the matches are checked again as it grows, until the address alone is left, then the pattern search confirms it.

A patch keeps the original bytes and refuses the library's module, unless the library is linked into the executable, and the code of its MinHook hooks, recorded by `CreateHook()`. On Windows, the pages are made `PAGE_EXECUTE_READWRITE` one by one with `VirtualProtect()`, every other thread is suspended (`SuspendThread()`, then `GetThreadContext()` waits for the suspension), and the bytes are written once none of them has its instruction pointer inside them, after the first byte; otherwise the threads are resumed and the write tried again, 50 times 2 ms. Nothing is allocated while the threads are suspended. On Linux, `mprotect()` makes the pages writable and executable for the write, then gives them their protection of `/proc/self/maps` back; when the system refuses writable code, the write goes through `/proc/self/mem`. Up to 8 bytes in an aligned 8-byte word are written with one atomic store, so a thread running there sees the old or the new instruction, not a mix. `Stop()` restores the patches after the remote control stopped.

**Linux socket.** Every process the game starts preloads the library too, so the socket is opened at the overlay's first frame, by the process that draws it. The socket file is only open to its owner (`0600`), and the server also checks each client's user (`SO_PEERCRED`), which matters for the `/tmp` fallback. A file left by a game that crashed is replaced when nobody answers on it.
