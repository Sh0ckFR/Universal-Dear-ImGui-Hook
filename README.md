# Universal-Dear-ImGui-Hook

A universal [Dear ImGui](https://github.com/ocornut/imgui) overlay hook. It draws your interface inside another application, on top of whatever graphics API that application uses.

| Backend | Windows | Linux |
| ------- | ------- | ----- |
| Direct3D 9 | :white_check_mark: | |
| Direct3D 10 | :white_check_mark: | |
| Direct3D 11 | :white_check_mark: | |
| Direct3D 12 | :white_check_mark: | |
| Vulkan | :white_check_mark: | :white_check_mark: |
| OpenGL 3.0+ | :white_check_mark: | :white_check_mark: (GLX and EGL) |

The hook detects the API the target uses at runtime. Press `INSERT` to open the menu and `F11` to remove the overlay.

![screenshot](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## Contents

- [Quick start](#quick-start)
- [Repository layout](#repository-layout)
- [Building](#building)
  - [Windows with Visual Studio](#windows-with-visual-studio)
  - [Windows without Visual Studio (MinGW-w64)](#windows-without-visual-studio-mingw-w64)
  - [Linux: native shared object](#linux-native-shared-object)
  - [Linux and macOS: cross-compiling the Windows DLL](#linux-and-macos-cross-compiling-the-windows-dll)
  - [Build options](#build-options)
- [Usage](#usage)
  - [Windows](#windows)
  - [Linux](#linux)
  - [Writing your own menu](#writing-your-own-menu)
  - [Using the hook as a library](#using-the-hook-as-a-library)
- [Tests](#tests)

## Quick start

Prebuilt libraries for every toolchain are attached to each [release](../../releases); the [build workflow](../../actions) also keeps them as artifacts for every commit. The prebuilt Linux library is built against glibc 2.28, so it loads on any distribution at least that recent.

**Windows**, with Visual Studio 2022 or newer installed:

```sh
git clone https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git
cd Universal-Dear-ImGui-Hook
cmake --preset msvc-x64
cmake --build --preset msvc-x64
```

Inject `build/msvc-x64/Release/Universal-ImGui-Hook.dll` into a 64-bit game with any injector.

**Linux**, on Debian or Ubuntu:

```sh
sudo apt install git cmake ninja-build g++ pkg-config libxcb1-dev
git clone https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git
cd Universal-Dear-ImGui-Hook
cmake --preset linux
cmake --build --preset linux
LD_PRELOAD=$PWD/build/linux/Universal-ImGui-Hook.so ./game
```

## Repository layout

| Path | Content |
| --- | --- |
| `include/uih.h` | Public API: `uih::Config`, `uih::Start`, `uih::Stop` |
| `src/uih.cpp`, `src/internal.h` | Core shared by every platform: lifecycle, backend selection, frame loop |
| `src/vulkan.cpp` | Vulkan renderer, shared; only the way the hooks are attached differs per platform |
| `src/windows/` | Windows layer: MinHook, Direct3D 9 to 12, WGL, Win32 input |
| `src/linux/` | Linux layer: symbol interposition, GLX, EGL, X11 input |
| `example/` | The library that gets built: `menu.cpp` is the interface, `dllmain.cpp` and `proxy.cpp` the Windows entry points, `preload.cpp` the Linux one |
| `tests/` | Test applications, the DDNet script and a `FetchContent` consumer |
| `third_party/` | Dear ImGui, MinHook and the Vulkan headers |
| `cmake/` | Toolchain files for MinGW-w64 |
| `Universal-ImGui-Hook.sln`, `vs/` | Visual Studio solution for Windows, built from the same sources as CMake |

## Building

The build uses CMake and ships Dear ImGui, MinHook and the Vulkan headers, so there is nothing to download on Windows. Pick the build that matches the **target process**, not the machine you build on:

| Target | Output | Presets |
| --- | --- | --- |
| Windows game, 64-bit | `Universal-ImGui-Hook.dll` | `msvc-x64`, `mingw-x64` |
| Windows game, 32-bit | `Universal-ImGui-Hook.dll` | `msvc-x86`, `mingw-x86` |
| Windows game running under Wine or Proton | `Universal-ImGui-Hook.dll` | same as above |
| Native Linux game, 64-bit | `Universal-ImGui-Hook.so` | `linux` |

Each preset writes to its own `build/<preset>` directory, so several of them can live side by side.

### Minimum versions

| Tool | Minimum | Verified by the CI |
| --- | --- | --- |
| CMake | 3.21 | Versions bundled with Visual Studio and GitHub runners |
| Visual Studio | 2022 (17.x) | 2022 locally, 2026 on GitHub runners |
| MinGW-w64 / GCC | mingw-w64 11 with GCC 13 | Ubuntu 24.04 (GCC 13), Homebrew and winlibs (GCC 14 to 16) |
| GCC or Clang (Linux) | GCC 13, Clang 18 | Ubuntu 24.04 (GCC 13, Clang 18) |
| Ninja | any | |
| Windows (runtime) | Windows 10 | |
| Linux (runtime) | glibc, X11 or XWayland | Ubuntu 24.04 with Mesa |

Visual Studio 2019 is not tested. The mingw-w64 8.0 headers shipped by Ubuntu 22.04 are too old for the D3D12 backend; use Ubuntu 24.04, Debian 13, a current Fedora or Arch, or a toolchain from [winlibs](https://winlibs.com/) or MSYS2. The DLL imports `d3dcompiler_47.dll`, which Windows 8.1 ships as well, but only Windows 10 and 11 are exercised.

### Windows with Visual Studio

Requirements: Visual Studio 2022 or newer with the "Desktop development with C++" workload, which includes CMake and the Windows SDK. There are three ways to build; they all produce the same DLL.

**Open the solution.** Open `Universal-ImGui-Hook.sln` from the root of the repository, pick `Release` and `x64` (or `x86`), then Build > Build Solution. The `Universal-ImGui-Hook` project is the DLL, written to `build/vs/x64/Release/Universal-ImGui-Hook.dll`; the solution also builds `uih_host`, the test application described in [Tests](#tests). The solution needs no CMake and uses whichever toolset the installed Visual Studio provides. It only covers Windows: a source file added to `CMakeLists.txt` has to be added to the matching project in `vs/` too.

**Open the folder.** In Visual Studio, choose "Open a local folder" and select the repository. Pick `Visual Studio x64` (or `x86`) in the configuration dropdown, then Build > Build All. The DLL is written to `build/msvc-x64/Release/Universal-ImGui-Hook.dll`. Visual Studio Code with the CMake Tools extension works the same way.

**Command line.**

```sh
cmake --preset msvc-x64          # or msvc-x86
cmake --build --preset msvc-x64  # Release build
```

### Windows without Visual Studio (MinGW-w64)

With [MSYS2](https://www.msys2.org/), from the UCRT64 shell:

```sh
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake --preset mingw-x64
cmake --build --preset mingw-x64
```

The DLL is written to `build/mingw-x64/Universal-ImGui-Hook.dll`. Use the MINGW32 shell and the `mingw-w64-i686-*` packages with the `mingw-x86` preset for a 32-bit DLL.

A standalone toolchain from [winlibs](https://winlibs.com/) works too: add its `bin` directory to `PATH` and run the same two `cmake` commands from any terminal.

### Linux: native shared object

This builds the library that is preloaded into native Linux games. Install a compiler, CMake, Ninja and the XCB headers:

| Distribution | Packages |
| --- | --- |
| Debian, Ubuntu | `sudo apt install cmake ninja-build g++ pkg-config libxcb1-dev` |
| Fedora | `sudo dnf install cmake ninja-build gcc-c++ pkgconf-pkg-config libxcb-devel libstdc++-static` |
| Arch | `sudo pacman -S base-devel cmake ninja libxcb` |

Then:

```sh
cmake --preset linux
cmake --build --preset linux
```

The library is written to `build/linux/Universal-ImGui-Hook.so`. It links the C++ runtime statically and only needs `libxcb` and glibc at run time. To build with Clang, set `CC=clang CXX=clang++` before the first `cmake --preset linux`.

A library built this way needs a glibc at least as recent as the one of the machine it was built on. To share it, or to preload it into a game that runs in a container such as the Steam Linux Runtime, build it on an older distribution or use the prebuilt one.

### Linux and macOS: cross-compiling the Windows DLL

The Windows DLL can be built from Linux or macOS with MinGW-w64, for instance to use it under Wine or Proton:

| System | Packages |
| --- | --- |
| Debian, Ubuntu | `sudo apt install cmake ninja-build g++-mingw-w64-x86-64 g++-mingw-w64-i686` |
| Fedora | `sudo dnf install cmake ninja-build mingw64-gcc-c++ mingw32-gcc-c++` |
| Arch | `sudo pacman -S cmake ninja mingw-w64-gcc` |
| macOS | `brew install cmake ninja mingw-w64` |

```sh
cmake --preset mingw-x64          # or mingw-x86
cmake --build --preset mingw-x64
```

The DLL is written to `build/mingw-x64/Universal-ImGui-Hook.dll`. The presets use `cmake/mingw-w64-x86_64.cmake` and `cmake/mingw-w64-i686.cmake`, which expect the `x86_64-w64-mingw32-*` and `i686-w64-mingw32-*` compilers in `PATH`; pass `-DCMAKE_TOOLCHAIN_FILE` with your own file if your distribution names them differently.

### Build options

Pass them to the configure step, for example `cmake --preset linux -DUIH_BUILD_TESTS=ON`.

| Option | Default | Effect |
| --- | --- | --- |
| `UIH_BUILD_EXAMPLE` | `ON` when top-level | Builds the example library: the injectable DLL on Windows, the preloadable shared object on Linux |
| `UIH_BUILD_TESTS` | `OFF` | Builds the `uih_host` test application |
| `UIH_STATIC_RUNTIME` | `ON` when top-level | Links the C and C++ runtimes statically so the library has no redistributable dependency |

## Usage

### Windows

Inject `Universal-ImGui-Hook.dll` into the target with any injector (or `LoadLibrary`). The hook can be injected before or after the target initializes its graphics API. When injected into a running Vulkan application, the swap chain format is assumed to be `VK_FORMAT_B8G8R8A8_UNORM` until the application recreates its swap chain.

While the menu is open, keyboard and mouse messages are kept away from the target application, the cursor is released and an XInput controller navigates the menu. `F11` removes the overlay and unloads the DLL.

#### Loading without an injector

The DLL exports `DirectInput8Create` and forwards it to the system library, so a target that imports DirectInput 8 loads the hook by itself: rename the DLL to `dinput8.dll` and place it next to the target's executable. Wine and Proton also need the override to be declared:

```sh
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

Loaded this way, the unload key removes the overlay and leaves the DLL in memory, as the target still references it.

### Linux

Preload the library into the game:

```sh
LD_PRELOAD=/path/to/Universal-ImGui-Hook.so ./game
```

For a Steam game, set the launch options to `LD_PRELOAD=/path/to/Universal-ImGui-Hook.so %command%`. Set `UIH_DEBUG=1` as well to get the hook's log on the standard error output.

The library exports the presentation functions of GLX, EGL and Vulkan under their own names and forwards them to the real ones, which also covers applications that look these functions up with `dlsym()`, as SDL and GLFW do. `F11` removes the overlay; a preloaded library cannot be unloaded, so it stays in the process and does nothing.

What to expect compared to Windows:

- The pointer and the keyboard are read from the X server, so the game has to run on X11 or XWayland. Nothing can be read from a native Wayland window, hotkeys included, and that setup is not tested: start the game on X11, for instance with `SDL_VIDEODRIVER=x11` for SDL games.
- The game still receives the keyboard and the mouse while the menu is open.
- The mouse wheel, dead keys and input methods are not handled.
- OpenGL ES contexts are left alone.
- Games that link their OpenGL or Vulkan loader statically are not hooked.
- Only x86-64 with glibc is built and tested.

### Writing your own menu

Edit `example/menu.cpp` to draw your own interface: `DrawMenu` runs between `ImGui::NewFrame()` and `ImGui::Render()` on every frame, and `uih::IsMenuOpen()` tells whether the menu is shown. The entry points pass the configuration to `uih::Start`, in `example/dllmain.cpp` on Windows and `example/preload.cpp` on Linux:

```cpp
uih::Config config;
config.backend = uih::Backend::None;    // None = detect automatically
config.debugLog = false;                // OutputDebugString (DebugView) on Windows, stderr on Linux
config.render = DrawMenu;               // called every frame
#ifdef _WIN32
config.module = module;                 // DLL to unload on `unloadKey`
config.menuKey = VK_INSERT;             // virtual-key codes
config.unloadKey = VK_F11;
#else
config.menuKey = 0xff63;                // X11 keysyms: XK_Insert
config.unloadKey = 0xffc8;              // XK_F11
#endif
uih::Start(config);
```

### Using the hook as a library

The repository is a CMake package that can be consumed with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(uih GIT_REPOSITORY https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git GIT_TAG master)
FetchContent_MakeAvailable(uih)

add_library(my_overlay SHARED dllmain.cpp)
target_link_libraries(my_overlay PRIVATE uih::uih)
```

`uih::uih` exposes `include/uih.h` and Dear ImGui's headers. `tests/consumer/` is a complete example of such a project for Windows and is built by the CI on every commit. On Linux, linking `uih::uih` into a shared library also applies `src/linux/exports.map`, which keeps everything but the hooked functions out of the target's symbol lookups.

## Tests

`-DUIH_BUILD_TESTS=ON` builds `uih_host`, a small application that renders a solid color with one API, and registers the tests with CTest. The tests open a window and press the hotkeys for real, so they need a desktop session and should not be run in parallel. A test that cannot get the keyboard focus is skipped instead of sending its key presses to another application: leave the machine alone while they run.

**Windows**

```sh
cmake --preset msvc-x64 -DUIH_BUILD_TESTS=ON
cmake --build --preset msvc-x64
ctest --test-dir build/msvc-x64 -C Release
```

`uih_host --api vulkan --capture` runs one backend (`d3d9`, `d3d10`, `d3d11`, `d3d12`, `vulkan`, `opengl`) with the hook started in-process; `--inject-at 0` starts the hook before the API is initialized, `--dll <path>` loads the real DLL and drives it with its hotkeys, `--capture` compares the pixels on screen and `--warp` uses the software rasterizer. `uih_host --proxy <path>` checks that the DLL can stand in for `dinput8.dll`.

**Linux**

The host needs the Xlib, XTest, OpenGL and EGL headers on top of the build dependencies (`sudo apt install libx11-dev libxtst-dev libgl-dev libegl-dev` on Debian and Ubuntu).

```sh
cmake --preset linux -DUIH_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux
```

Here `uih_host` knows nothing about the hook: every test preloads `Universal-ImGui-Hook.so` into it, opens the menu with `INSERT`, checks the pixels of the window and removes the overlay with `F11`. `--api opengl` uses GLX, `--egl` switches to EGL, `--lookup` resolves the functions with `dlsym()` like SDL and GLFW, and `--api vulkan --device-proc-addr` goes through `vkGetDeviceProcAddr`. Without a display, run the tests under Xvfb with Mesa's software drivers: `xvfb-run -a ctest --test-dir build/linux`.

`tests/ddnet.sh` runs the same scenario against a real game, [DDNet](https://ddnet.org/):

```sh
sh tests/ddnet.sh build/linux/Universal-ImGui-Hook.so path/to/DDNet OpenGL   # or Vulkan
```

**Continuous integration**

The [build workflow](.github/workflows/build.yml) runs on every push and pull request:

| Job | What it checks |
| --- | --- |
| MSVC x64, Win32 | Build, Direct3D 11 smoke test on WARP, `dinput8.dll` proxy test |
| Visual Studio solution x64, x86 | `Universal-ImGui-Hook.sln` built with MSBuild, same two tests |
| MinGW-w64 x86_64, i686 on Ubuntu and macOS | Cross-compilation of the DLL, name of the proxy export |
| Linux GCC, Clang | Build, exported symbols, the six preload tests under Xvfb with llvmpipe and lavapipe |
| Linux portable | The released library: built and tested in a glibc 2.28 container, glibc requirement checked |
| DDNet OpenGL, Vulkan | The official DDNet build with the released library preloaded: overlay, menu hotkey, removal |
| FetchContent consumer | The repository used as a dependency of another CMake project |

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
