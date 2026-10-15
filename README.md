# Universal-Dear-ImGui-Hook

A [Dear ImGui](https://github.com/ocornut/imgui) overlay that draws your interface inside another application, whatever graphics API it uses. The API is detected at runtime: build once, inject anywhere.

| API | Windows (x86, x64) | Linux (x86-64, ARM64) |
| --- | --- | --- |
| Direct3D 9, 9Ex | :white_check_mark: | NOT REQUIRED |
| Direct3D 10, 11, 12 | :white_check_mark: | NOT REQUIRED |
| Vulkan | :white_check_mark: | :white_check_mark: |
| OpenGL 3.0+ | :white_check_mark: (WGL) | :white_check_mark: (GLX, EGL) |

Windows games running under Wine or Proton use the Windows DLL.

![screenshot](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## Contents

- [Features](#features)
- [Quick start](#quick-start)
- [Usage](#usage)
- [API](#api)
- [How it works](#how-it-works)
- [Building](#building)
- [Tests](#tests)
- [Contributing](#contributing)
- [Limitations](#limitations)

## Features

- **One library for every API**: the backend is picked from the first API that presents a frame. Inject before or after the game initializes its graphics.
- **Menu input**: `INSERT` opens the menu, `F11` removes the overlay and unloads the DLL. While the menu is open the game sees no keyboard, mouse or controller input (Windows), and an XInput controller navigates the menu.
- **Readable on any display**: the interface follows the DPI of the game window, or a fixed scale.
- **Correct colors** on sRGB and floating-point back buffers.
- **Images**: `uih::CreateTexture()` turns RGBA pixels into an image for `ImGui::Image()`, on every backend.
- **No injector needed**: the DLL can be loaded as `dinput8.dll`, and the Linux library with `LD_PRELOAD`.
- **No dependencies**: Dear ImGui, MinHook and the Vulkan headers are vendored; the C and C++ runtimes are linked statically.

## Quick start

Prebuilt libraries are attached to each [release](../../releases), and every commit's builds are kept as [workflow artifacts](../../actions). Pick the one that matches the **game**, not your machine:

| Game | File |
| --- | --- |
| Windows, 64-bit | `Universal-ImGui-Hook-msvc-x64.dll` or `-mingw-x86_64.dll` |
| Windows, 32-bit | `Universal-ImGui-Hook-msvc-Win32.dll` or `-mingw-i686.dll` |
| Linux, x86-64 or ARM64 | `Universal-ImGui-Hook-linux-x86_64.so` or `-linux-aarch64.so` (glibc 2.28 or later) |

To build it yourself on Windows, open `Universal-ImGui-Hook.sln` in Visual Studio 2022 or newer and build `Release|x64`: the DLL is `build/vs/x64/Release/Universal-ImGui-Hook.dll`. On Linux:

```sh
sudo apt install cmake ninja-build g++ pkg-config libxcb1-dev
cmake --preset linux && cmake --build --preset linux
LD_PRELOAD=$PWD/build/linux/Universal-ImGui-Hook.so ./game
```

## Usage

### Windows

Inject `Universal-ImGui-Hook.dll` with any injector or `LoadLibrary`.

To load it without an injector, rename it to `dinput8.dll` and put it next to the game's executable: it exports `DirectInput8Create` and forwards it to the system library, so any game that imports DirectInput 8 loads it. Under Wine or Proton, add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the launch options. Loaded this way, `F11` removes the overlay but the DLL stays in memory, since the game references it.

Logs (`config.debugLog`) go to `OutputDebugString`: read them with [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview).

### Linux

```sh
LD_PRELOAD=/path/to/Universal-ImGui-Hook.so ./game
```

For a Steam game, set the launch options to `LD_PRELOAD=/path/to/Universal-ImGui-Hook.so %command%`. `UIH_DEBUG=1` prints the log on stderr. `F11` removes the overlay; the library itself stays loaded.

### Writing your menu

The interface lives in `example/menu.cpp`. `DrawMenu()` is called every frame between `ImGui::NewFrame()` and `ImGui::Render()`, so any Dear ImGui code works there. The entry points, `example/dllmain.cpp` on Windows and `example/preload.cpp` on Linux, configure and start the hook:

```cpp
uih::Config config;
config.render = DrawMenu;      // every frame
config.setup = LoadFonts;      // when the ImGui context is created: fonts, style
config.scale = 0.0f;           // 0 follows the window's DPI, 1.5 = 150%
config.module = module;        // Windows: DLL unloaded by unloadKey
uih::Start(config);
```

Images are created once from RGBA pixels and drawn like any ImGui texture:

```cpp
static ImTextureData* logo = uih::CreateTexture(pixels, 64, 64);
ImGui::Image(logo->GetTexRef(), ImVec2(64, 64));
```

### Using the hook as a library

The repository is a CMake package; link `uih::uih` to get `include/uih.h` and Dear ImGui's headers:

```cmake
include(FetchContent)
FetchContent_Declare(uih GIT_REPOSITORY https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git GIT_TAG master)
FetchContent_MakeAvailable(uih)
target_link_libraries(my_overlay PRIVATE uih::uih)
```

`tests/consumer/` is a complete example, built by the CI. On Linux, linking `uih::uih` into a shared library applies `src/linux/exports.map`, which only exports the hooked functions.

## API

Everything is in `include/uih.h`, namespace `uih`.

| Function | Description |
| --- | --- |
| `Start(config)` | Installs the hooks on a worker thread and returns. Safe from `DllMain` or a library constructor. |
| `Stop()` | Removes the overlay, releases the GPU resources on the rendering thread and removes the hooks. Blocks; not from `DllMain` or a hook. |
| `ActiveBackend()`, `BackendName(backend)` | API the overlay is drawn with, `Backend::None` until the first frame. |
| `IsMenuOpen()`, `SetMenuOpen(open)` | Menu state, also toggled by `menuKey`. |
| `CreateTexture(rgba, width, height)` | Image from RGBA pixels, uploaded by the active backend on the next frame and again after a backend switch. Any thread. |
| `DestroyTexture(texture)` | Releases an image on the next frame. |

| `Config` field | Default | Description |
| --- | --- | --- |
| `render` | `nullptr` | Called every frame, between `ImGui::NewFrame()` and `ImGui::Render()` |
| `setup` | `nullptr` | Called when the ImGui context is created, again if the game switches devices or windows |
| `backend` | `None` | Restricts the hook to one API; `None` takes the first that presents |
| `menuKey`, `unloadKey` | `INSERT`, `F11` | Virtual-key codes on Windows, X11 keysyms on Linux |
| `scale` | `0` | Interface size; `0` follows the DPI of the game window |
| `module` | `nullptr` | Windows: module freed by `unloadKey`; `nullptr` disables unloading |
| `debugLog` | `false` | `OutputDebugString` on Windows, stderr on Linux |

## How it works

**Startup.** `Start()` launches a worker thread. On Windows it hooks every graphics and input DLL already loaded, then the others as they load (`LdrRegisterDllNotification`). The hooks are placed with MinHook on functions found through dummy objects: a hidden window, a D3D device, a Vulkan device per GPU. On Linux, the library exports the hooked functions under their own names and overrides `dlsym()`, so the game calls them whether it links them or looks them up like SDL and GLFW do.

**Hook points.**

| API | Draws in | Also hooked |
| --- | --- | --- |
| D3D9 | `IDirect3DDevice9::Present`, `IDirect3DDevice9Ex::PresentEx`, `IDirect3DSwapChain9::Present` | `Reset`, `ResetEx` |
| D3D10, 11, 12 | `IDXGISwapChain::Present`, `IDXGISwapChain1::Present1`; the device behind the swap chain picks the renderer | `ResizeBuffers`, `ResizeBuffers1` |
| D3D12 | (same) | `ID3D12CommandQueue::ExecuteCommandLists` and the factory's `CreateSwapChain*`, to find the queue the swap chain presents on |
| Vulkan | `vkQueuePresentKHR` | Device, queue, swap chain and surface creation and destruction; in each driver on Windows, the loader's exports on Linux |
| OpenGL | `wglSwapBuffers`, `glXSwapBuffers`, `eglSwapBuffers` | `glXGetProcAddress`, `eglGetProcAddress` on Linux |

**Each frame**, in the present hook: `uih::Frame()` registers new textures, polls the hotkeys, applies the DPI scale and runs `render`; the backend then draws the ImGui data on the back buffer, uploading textures as needed, and the game's present goes on. Vertex colors are converted to linear for back buffers that store linear values; OpenGL and D3D9 turn sRGB writes off instead. Vulkan submits its own command buffer, which waits on the game's semaphores and signals the one the present waits on.

**Input on Windows.** The game window is subclassed to feed ImGui, and its input messages are swallowed while the menu is open. `GetRawInputData`, `GetRawInputBuffer`, `GetAsyncKeyState`, `GetKeyState`, `GetKeyboardState`, DirectInput and XInput then report no input to the game, and `GetCursorPos` a frozen position. Releases always go through, so no key stays stuck; calls made by the hook itself see the real state.

**Input on Linux.** The pointer and the keyboard are polled from the X server on a separate XCB connection.

**Teardown.** `Stop()` asks the rendering thread to release the backend at its next present, or does it itself after one second, then removes the hooks once none of them is running.

## Building

| Target | Output | CMake presets |
| --- | --- | --- |
| Windows game, 64-bit | `Universal-ImGui-Hook.dll` | `msvc-x64`, `mingw-x64` |
| Windows game, 32-bit | `Universal-ImGui-Hook.dll` | `msvc-x86`, `mingw-x86` |
| Native Linux game | `Universal-ImGui-Hook.so` | `linux` |

Each preset builds into `build/<preset>`.

**Visual Studio** 2022 or newer, with the "Desktop development with C++" workload. Pick one:
- `Universal-ImGui-Hook.sln` builds the DLL and `uih_host` without CMake, into `build/vs/<x64|x86>/<configuration>/`.
- Open the folder: Visual Studio reads `CMakePresets.json`.
- Command line: `cmake --preset msvc-x64`, then `cmake --build --preset msvc-x64`.

**MinGW-w64 on Windows**, from an [MSYS2](https://www.msys2.org/) UCRT64 shell or with a [winlibs](https://winlibs.com/) toolchain in `PATH`:

```sh
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake --preset mingw-x64 && cmake --build --preset mingw-x64
```

**Windows DLL from Linux or macOS**, with the `mingw-x64` and `mingw-x86` presets and the toolchain files in `cmake/`:

| System | Packages |
| --- | --- |
| Debian, Ubuntu | `cmake ninja-build g++-mingw-w64-x86-64 g++-mingw-w64-i686` |
| Fedora | `cmake ninja-build mingw64-gcc-c++ mingw32-gcc-c++` |
| Arch | `cmake ninja mingw-w64-gcc` |
| macOS (Homebrew) | `cmake ninja mingw-w64` |

**Linux library**, with the `linux` preset:

| Distribution | Packages |
| --- | --- |
| Debian, Ubuntu | `cmake ninja-build g++ pkg-config libxcb1-dev` |
| Fedora | `cmake ninja-build gcc-c++ pkgconf-pkg-config libxcb-devel libstdc++-static` |
| Arch | `base-devel cmake ninja libxcb` |

It only needs `libxcb` and glibc at run time, but a glibc at least as recent as the build machine's: to share it or to use it in the Steam Linux Runtime, build on an old distribution or take the prebuilt one. Set `CC=clang CXX=clang++` before configuring to use Clang.

**Options**, passed at configure time, such as `-DUIH_BUILD_TESTS=ON`:

| Option | Default | Effect |
| --- | --- | --- |
| `UIH_BUILD_EXAMPLE` | `ON` at top level | The example DLL or shared object |
| `UIH_BUILD_TESTS` | `OFF` | `uih_host` and the CTest suite |
| `UIH_STATIC_RUNTIME` | `ON` at top level | Static C and C++ runtimes |

**Minimum versions**: CMake 3.21, Visual Studio 2022, mingw-w64 11 with GCC 13, GCC 13 or Clang 18 on Linux; Windows 10 at run time. The mingw-w64 headers of Ubuntu 22.04 are too old for D3D12.

## Tests

`-DUIH_BUILD_TESTS=ON` builds `uih_host`, which renders a solid color with one API and checks the overlay, and registers the scenarios with CTest:

```sh
cmake --preset msvc-x64 -DUIH_BUILD_TESTS=ON && cmake --build --preset msvc-x64
ctest --test-dir build/msvc-x64 -C Release
```

Some tests press real keys and move the mouse. They are skipped when the host window does not have the focus: leave the machine alone while they run. A test whose screen check fails saves the capture as `build/tests/capture-*.bmp`.

Without a GPU, as on the CI runners, Direct3D uses WARP, and Mesa's software drivers from [mesa-dist-win](https://github.com/pal1000/mesa-dist-win) cover the rest: copy `opengl32.dll` and `libgallium_wgl.dll` next to `uih_host.exe` with `GALLIUM_DRIVER=llvmpipe`, and register `lvp_icd.*.json` under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` with a `vulkan-1.dll` from the Vulkan runtime.

| `uih_host` option (Windows) | Scenario |
| --- | --- |
| `--api d3d9\|d3d9ex\|d3d10\|d3d11\|d3d12\|vulkan\|opengl` | Hook started in-process, overlay drawn once per frame, then `Stop()` |
| `--inject-at 0`, `--settle <ms>` | Hook started before the API; delay before the swap chain is created |
| `--capture` | Checks the overlay and an exact color on screen |
| `--format srgb\|float` | Back buffer storing linear values |
| `--decoy-queue` | A second D3D12 device executes work every frame |
| `--scale <s>`, `--warp` | Fixed interface scale; software rasterizer |
| `--input raw\|directinput` | What the game reads while the menu is open |
| `--dll <path>` | Loads the real DLL and drives it with `INSERT` and `F11` |
| `--proxy <path>` | Loads the DLL as `dinput8.dll` |

On Linux, `uih_host` does not link the hook: each test preloads the `.so`, opens the menu, checks the pixels and removes the overlay, through GLX, EGL, `dlsym()` lookups and Vulkan. It needs `libx11-dev libxtst-dev libgl-dev libegl-dev`; without a display, run `xvfb-run -a ctest --test-dir build/linux`. `tests/ddnet.sh` runs the same scenario on the game [DDNet](https://ddnet.org/).

The [CI](.github/workflows/build.yml) runs on every push and pull request:

| Job | Checks |
| --- | --- |
| MSVC x64, Win32 | Build, D3D11 on WARP, `dinput8.dll` proxy |
| Windows tests x64, Win32 | The whole CTest suite without GPU: WARP for Direct3D, Mesa's llvmpipe and lavapipe for OpenGL and Vulkan; no test may be skipped |
| Visual Studio solution x64, x86 | The `.sln` built with MSBuild, same tests |
| MinGW-w64 on Ubuntu and macOS | Cross-compiled DLLs, proxy export |
| Linux GCC, Clang x86_64, GCC aarch64 | The six preload tests under Xvfb with llvmpipe and lavapipe |
| Linux portable x86_64, aarch64 | The released libraries, built and tested against glibc 2.28 |
| DDNet OpenGL, Vulkan | The released library in a real game |
| FetchContent consumer | The repository as a CMake dependency |

Pushing a `v*` tag publishes the libraries as a release.

## Contributing

| Path | Content |
| --- | --- |
| `include/uih.h` | Public API |
| `src/uih.cpp`, `src/internal.h` | Platform-independent core: lifecycle, frame, textures, scale |
| `src/vulkan.cpp` | Vulkan renderer, shared by both platforms |
| `src/windows/` | MinHook, Direct3D, WGL, Win32 input, DirectInput, XInput |
| `src/linux/` | Symbol interposition, GLX, EGL, X11 input, `exports.map` |
| `example/` | The built library: `menu.cpp`, `dllmain.cpp`, `proxy.cpp`, `preload.cpp` |
| `tests/` | `uih_host`, the DDNet script, the `FetchContent` consumer |
| `vs/`, `Universal-ImGui-Hook.sln` | Visual Studio projects |
| `third_party/` | Dear ImGui, MinHook, Vulkan headers |

- A new source file goes into `CMakeLists.txt` **and** the matching project in `vs/`.
- A fix comes with a `uih_host` scenario that fails without it.
- Run the CTest suite on Windows; the CI covers Linux.

## Limitations

- Linux reads input from the X server: native Wayland windows get no menu input (run SDL games with `SDL_VIDEODRIVER=x11`), and the game still receives the keyboard and the mouse while the menu is open. The mouse wheel and input methods are not handled.
- Linux: 32-bit games, OpenGL ES, and games that link their GL or Vulkan loader statically are not supported.
- HDR10 swap chains (PQ color space) show the overlay with wrong colors.
- A Vulkan swap chain created before the hook is assumed to be `B8G8R8A8_UNORM` until the game recreates it.
- Native ARM64 Windows games are not supported: MinHook only handles x86 and x64.

## Built With

* [Dear ImGui](https://github.com/ocornut/imgui)
* [MinHook](https://github.com/TsudaKageyu/minhook)
* [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)

## Authors

* **Rebzzel** - *Initial work on the kiero library (no longer used)* - [Rebzzel](https://github.com/Rebzzel)
* **Sh0ckFR** | **Revan600** - *ImGui and input hooks* - [Sh0ckFR](https://github.com/Sh0ckFR) - [Revan600](https://github.com/Revan600)

## Contributors

* **primemb** - *Bug fixes* - [primemb](https://github.com/primemb)

## Support

This project is free and MIT-licensed. Building it from scratch represents roughly 33 days of senior C++ work, about €22,000 at French freelance rates. If it saved you time, consider [sponsoring](https://github.com/sponsors/Sh0ckFR) or starring the repository.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
