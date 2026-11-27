# Tests

`-DUIH_BUILD_TESTS=ON` builds `uih_host`, a small application that renders a solid color with one graphics API and checks the overlay, and registers its scenarios with CTest:

```sh
cmake --preset msvc-x64 -DUIH_BUILD_TESTS=ON && cmake --build --preset msvc-x64
ctest --test-dir build/msvc-x64 -C Release
```

Some tests press real keys and move the mouse. They are skipped when the host window does not have the focus, so leave the machine alone while they run. A test whose screen check fails saves the capture as `build/tests/capture-*.bmp`.

## Without a GPU, and with the validation and debug layers

Without a GPU, as on the CI runners, Direct3D uses WARP, and Mesa's software drivers from [mesa-dist-win](https://github.com/pal1000/mesa-dist-win) cover the rest:

- **OpenGL**: copy `opengl32.dll`, `libgallium_wgl.dll` and `dxil.dll` next to `uih_host.exe` and set `GALLIUM_DRIVER=llvmpipe`, or `GALLIUM_DRIVER=d3d12` for the D3D12-based driver of Microsoft's OpenGL compatibility pack, which needs `dxil.dll` to sign its shaders.
- **Vulkan**: register `lvp_icd.*.json` under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, with a `vulkan-1.dll` from the Vulkan runtime.

`UIH_TEST_VULKAN_VALIDATION=1` runs the Vulkan host under `VK_LAYER_KHRONOS_validation` (from the Vulkan SDK on Windows, `vulkan-validationlayers` on Linux), and `UIH_TEST_D3D_DEBUG=1` creates the Direct3D 10, 11 and 12 devices with the debug layer (from the Graphics Tools optional feature of Windows). Any error they report, from the host or from the hook, fails the test.

When the Windows host crashes, it prints the exception code, module and offset, and writes a minidump to the directory named by `UIH_TEST_DUMPS`, if set. The Windows test jobs of the CI set it and build with symbols, and keep the dumps with the binaries and their PDBs when a test fails.

## `uih_host` scenarios (Windows)

One option per scenario:

| `uih_host` option | Scenario |
| --- | --- |
| `--api d3d9\|d3d9ex\|d3d10\|d3d11\|d3d12\|vulkan\|opengl` | Hook started in-process, overlay drawn once per frame, then `Stop()` |
| `--inject-at 0`, `--settle <ms>` | Hook started before the API; delay before the swap chain is created |
| `--capture` | Checks the overlay and an exact color on screen |
| `--format srgb\|float\|hdr10` | Back buffer storing linear values, or HDR10 (D3D12): a 10-bit swap chain set to the PQ color space, whose probe color must come out unchanged through Windows' conversion |
| `--decoy-queue` | A second D3D12 device executes work every frame |
| `--remove-device` | The D3D12 device is removed: the overlay must not hold up its frames, and must show on the new device the host creates |
| `--transfer-only` | The Vulkan swap chain images are created for transfers only, without `COLOR_ATTACHMENT` usage: the overlay must still show |
| `--device-proc-addr` | The Vulkan host calls the device functions it gets from `vkGetDeviceProcAddr` |
| `--composition` | The D3D11 or D3D12 swap chain is made for composition, without a window |
| `--ignore-suboptimal` | The Vulkan host recreates its swap chain only after `VK_ERROR_OUT_OF_DATE_KHR`, as many engines do: the overlay, started after the swap chain was created, must still show |
| `--scale <s>`, `--warp` | Fixed interface scale; software rasterizer |
| `--resize` | Window and back buffers resized to 960x640, then 720x560, with the menu open |
| `--fullscreen` | 800x600 exclusive fullscreen (D3D9, DXGI) or display mode change with a borderless window (Vulkan, OpenGL), then back; the window must cover the display and the overlay follow it. Skipped unless `UIH_TEST_FULLSCREEN=1`, which the CI sets |
| `--reload` | The overlay is started a second time: `Start()` after `Stop()`; with `--dll`, the DLL injected again; for a DLL named `dinput8.dll`, brought back by `INSERT` after `F11` |
| `--remove` | `F11` removes the overlay and `INSERT` brings it back, twice, without a module to free |
| `--own-hooks before\|after` | The host places its own hook with MinHook, before `Start()` or once the overlay runs: the overlay must start, and the hook must outlive `Stop()`, or `F11` and `INSERT` |
| `--second-window` | A second window with its own device presents too: the overlay must stay on one window, follow the focus, and come back when the second window stops presenting. With `--shared-device` (Vulkan, D3D12), the second window renders with the first one's device |
| `--gpu-latency ms` | The D3D12 GPU runs each frame `ms` after it was submitted, as a busy GPU does: the overlay's frames are still in flight when it moves to another swap chain |
| `--textures <n>` | `n` images created with `uih::CreateTexture()`, 300 in the tests, more than `kMaxTextures`: every image it accepts must be drawn |
| `--capture-frame` | Another thread copies a frame with `uih::CaptureFrame()` while the menu is open: the copy must have the size of the back buffer and show the game where the menu covers it |
| `--remote` | Drives the remote control as `remote/uih_mcp.py` does: game description, screenshot without the overlay, message drawn, a click on the menu's button, keys and a right click received by the window, also while another host window has the focus; the tools of `tests/remote_tools.h`, registered with `uih::RegisterTool()`, listed as JSON and run in a frame of the overlay with their arguments, an exception answering as an error; then `uih::Stop()` must end it while a client stays connected |
| `--remote-memory` | `--remote` with `remoteMemory` on: value scans of an integer and a float narrowed by change, a write through `module+offset` read back, the module list and a pattern scan must find the host's values and bytes. `--remote` alone checks that the memory commands are refused |
| `--remote-advanced` | `--remote-memory` with `aiAdvancedFeatures` on: `wait` on a still screen and on a changing one, a pointer chain from the executable and a read through it, a pointer scan that finds that pointer at module+offset, a value frozen then unfrozen, the instruction watch counting 5 writes of the host and refusing a misaligned address (on Linux, a refusal by the kernel passes only while `kernel.perf_event_paranoid` is above 2 or in a container), the chat commands, the window drawn, typing, a move and a drag received by the window, and the speed of the game's clocks (`tests/remote_time.h`): real time, paused, run by a `wait`, `time advance`, half speed, and back. Without it, the advanced commands must be refused |
| `--fault access\|exception\|draw` | The render callback writes to a null pointer or throws, or a draw callback writes to a null pointer while the backend draws: the overlay must be removed while the host keeps rendering and can resize its back buffers, then start again (MSVC) |
| `--slow-frame ms` | One frame of the menu lasts `ms` while `Stop()` is called from another thread: nothing that frame uses may be released under it. `_slow_frame` tests last 2.5 s, within the waits of `Stop()`; `_stuck_frame` tests last 6 s, beyond them. The hook must then start again |
| `--bound-framebuffer` | The OpenGL host leaves a framebuffer object and a pixel unpack buffer bound at every swap: the overlay must still show on the window |
| `--switch-api api` | Once the overlay has shown, the host renders with another API in a new window: the overlay follows it |
| `--recreate-context` | The OpenGL context is deleted with the menu open and a new one, with its own textures, takes over: the overlay comes back and leaves them alone |
| `--input raw\|directinput` | What the game reads while the menu is open, and in its window procedure: a key released while the menu is open must not stay down, Alt+F4 still closes the window, and the cursor is confined again when the menu closes, or when `uih::Stop()` closes it |
| `--gamepad <fake xinput1_4.dll>` | Controller toggling the menu and hidden from the game while it is open |
| `--c-api` | Starts the hook through the C interface of `include/uih_c.h`, from `tests/c_api.c` compiled as C, after checking its defaults, names and `uih_tool_argument()`; with `--remote`, the tools of the scenario are registered through `uih_register_tool()` and must answer as the C++ ones do (`UIH_TEST_C_TOOLS=1` does the same in `uih_remote_preload` on Linux) |
| `--resubclass` | The host subclasses its window after the overlay: with `--reload`, the overlay starts again without calling itself through the host's procedure; with `--dll`, the unload key leaves the DLL loaded and the procedures still chain |
| `--render-thread` | The host presents from its own thread while the window thread dispatches a flood of input messages, and the hook starts and stops 20 times: no access violation may occur, and on x64 freed Dear ImGui memory is made inaccessible so that any use of it faults |
| `--dll <path>` | Loads the real DLL and drives it with `INSERT` and `F11` |
| `--menu-key <vk>`, `--unload-key <vk>` | The keys used by a DLL built with other settings, such as `0x70` for F1 (X11 keysyms on Linux) |
| `--proxy <path>` | Loads the DLL as `dinput8.dll` |
| `--preview <path>` | Not a test: loads the DLL, opens its menu and renders until the window is closed, to look at a menu without a game |

## Linux tests

On Linux, `uih_host` does not link the hook: each test preloads the `.so`, opens the menu, checks the pixels and removes the overlay. They cover:

- GLX, EGL, `dlsym()` lookups and Vulkan;
- `--resize`, `--reload`, `--second-window`, `--recreate-context` and `--bound-framebuffer` variants;
- overlay faults (`--fault`), with `tests/fault_preload.cpp`: a null pointer write in the render callback or in a draw callback, or a C++ exception, must remove the overlay while the host keeps rendering, and the menu key must bring it back;
- what the host reads while the menu is open (`--input x11`), with `tests/input_preload.cpp`: no key or button press and no motion, but the release of a key held when the menu opened, and the wheel scrolls the menu;
- the remote control (`--remote`), with `tests/remote_preload.cpp` and the hook built with it on: game description, screenshot without the assistant's message, message on screen, the tools of `tests/remote_tools.h`, keys and right clicks through XTEST with the focus and as X11 events without it, the memory and advanced commands as with `--remote-advanced` on Windows (the speed of the game read with `clock_gettime()` from the host), and typing with Shift, a move and a drag through XTEST;
- presents through `eglSwapBuffersWithDamage` that redraw and post a single pixel once `EGL_EXT_buffer_age` allows it (`--swap-with-damage`);
- Vulkan swap chain images for transfers only (`--transfer-only`) and device functions from `vkGetDeviceProcAddr` (`--device-proc-addr`);
- the Vulkan loader opened as `libvulkan.so` (`--vulkan-library`).

They need `libx11-dev libxtst-dev libgl-dev libegl-dev`. Without a display, run `xvfb-run -a ctest --test-dir build/linux`. With `-DUIH_SANITIZE=ON`, the preload tests load the sanitizer runtime before the library, and any memory error or undefined behavior in the hook fails them.

## Real games

`uih_game` on Windows, `tests/game.sh` and `tests/game_mcp.py` on Linux, `tests/wine.sh` under Wine.

`uih_game --dll <path> --backend <name> -- game.exe [arguments]` runs a game and injects the DLL once its window shows, as an injector does: the backend must become active, `INSERT` must show the menu on screen and `F11` remove it while the game keeps running. It reads the DLL's log as DebugView does, so DebugView must not run at the same time. It saves its captures as `game-before.bmp`, `game-menu.bmp` and `game-after.bmp` (`--captures <prefix>`).

`tests/game.sh` runs the same scenario on a Linux game, such as [DDNet](https://ddnet.org/) or a [Godot](https://godotengine.org/) project.

`tests/wine.sh Universal-ImGui-Hook.dll <backend> <prefix> game.exe [arguments]` runs it on a Windows game under Wine, as a Linux or Steam Deck player does: the DLL is copied next to the game as `dinput8.dll` and loaded with `WINEDLLOVERRIDES="dinput8=n,b"`. It reads the DLL's log on Wine's debug output (`WINEDEBUG=+debugstr`), checks with `+loaddll` that Wine loaded this `dinput8.dll` rather than its own, and saves its screen captures as `<prefix>-before.png`, `<prefix>-menu.png` and `<prefix>-after.png`. It needs Wine, an X server, xdotool and ImageMagick.

`tests/game_mcp.py libuih_remote_preload.so <backend> mcp.png game [arguments]` plays a Linux game through `remote/uih_mcp.py`, as an assistant does, with the remote control on: the tool list (memory and advanced tools, and the tools `tests/remote_preload.cpp` registers, included), a call of `test_add`, game description, `wait_for_change`, `game_speed` pausing, advancing, slowing down and resuming the game, screenshot, message, `INSERT` to open the menu, a click in the menu and `F11` to remove the overlay, after which the socket must be gone and the game still running.

## The CI

The [CI](../.github/workflows/build.yml) runs on every pull request, on `master` and on tags, and manually on any branch from the Actions tab:

| Job | Checks |
| --- | --- |
| MSVC x64, Win32 | Build, D3D11 on WARP, `dinput8.dll` proxy |
| Windows tests x64, Win32 | The whole CTest suite without a GPU: WARP for Direct3D, Mesa's llvmpipe and lavapipe for OpenGL and Vulkan, then the OpenGL scenarios again on Mesa's D3D12-based driver, the Direct3D 10 to 12 ones under the debug layers and, on x64, the Vulkan ones under the validation layer; no test may be skipped |
| Visual Studio solution x64, x86 | The `.sln` built with MSBuild, same tests |
| MinGW-w64 on Ubuntu and macOS | Cross-compiled DLLs, proxy export |
| Linux GCC, Clang x86_64, GCC aarch64 | The preload tests under Xvfb with llvmpipe and lavapipe, including resizes, the overlay brought back after `F11` and, with `kernel.perf_event_paranoid` set to 2, the instruction watch; then the Vulkan ones under the validation layer |
| Linux sanitizers | The preload tests with the library built with AddressSanitizer and UndefinedBehaviorSanitizer |
| Linux portable x86_64, aarch64 | The released libraries, built and tested against glibc 2.28 |
| DDNet, Godot on Linux | The released library preloaded into [DDNet](https://ddnet.org/) and an empty [Godot](https://godotengine.org/) project (`tests/godot`), with OpenGL and Vulkan; then the same games played through the MCP server (`tests/game_mcp.py`) |
| DDNet, Godot on Windows | The DLL injected into the same games once they render, without a GPU: DDNet with OpenGL and Vulkan, Godot with Direct3D 12, Vulkan and Direct3D 11 (its OpenGL renderer through ANGLE) |
| Godot under Wine | The released x64 DLL as `dinput8.dll` next to the Windows build of Godot, run with `WINEDLLOVERRIDES="dinput8=n,b"` under Wine 11 and Xvfb (`tests/wine.sh`), with OpenGL, Direct3D 11 (ANGLE on Wine's Direct3D) and Vulkan on llvmpipe and lavapipe: Wine must load the DLL, the backend become active, `INSERT` show the menu on screen and `F11` remove it while the game keeps running |
| FetchContent consumer | The repository as a CMake dependency |
| Docs and sources | `tests/check_docs.py` on every Markdown file: relative links and anchors, tables, control characters, code spans; `tests/check_sources.py`: CMake and the Visual Studio solution build the same sources |

Pushing a `v*` tag publishes the MSVC and Linux portable libraries as a release once every job above has passed, with their SHA256 in `SHA256SUMS` and a build provenance attestation. The message of an annotated tag becomes the highlights of the release notes: see [CONTRIBUTING.md](../CONTRIBUTING.md#releases).

## Documentation

`python tests/check_docs.py` checks the Markdown files as the CI does, without fetching external links: relative links to missing files or anchors, table rows cut off from their table or with the wrong number of cells, tabs and other control characters, unclosed code spans and unbalanced `<details>` blocks.

`python tests/check_sources.py` checks that `CMakeLists.txt` and the projects of `vs/` build the same sources: each source of `src/` in both (`src/linux/` in CMake only), the Windows sources of `example/` and of `uih_host` in both, and no file named by either that does not exist. A source added to one build system only links on one side and fails on the other.
