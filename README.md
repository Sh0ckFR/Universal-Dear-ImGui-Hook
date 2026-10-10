# Universal-Dear-ImGui-Hook

A universal [Dear ImGui](https://github.com/ocornut/imgui) overlay hook for Direct3D 9, 10, 11, 12, Vulkan and OpenGL applications on Windows.

| Backend | Status |
| ------- | ------ |
| Direct3D 9 | :white_check_mark: |
| Direct3D 10 | :white_check_mark: |
| Direct3D 11 | :white_check_mark: |
| Direct3D 12 | :white_check_mark: |
| Vulkan | :white_check_mark: |
| OpenGL 3.0+ | :white_check_mark: |

The hook detects the API the target uses at runtime: inject the DLL, press `INSERT` to open the menu and `F11` to unload it.

![screenshot](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## Building

The build uses CMake and ships every dependency (Dear ImGui, MinHook, Vulkan headers), so nothing else has to be downloaded. The output is `Universal-ImGui-Hook.dll`, a Windows DLL that only depends on system libraries. Build the architecture of the process you want to inject into: a 64-bit game needs the x64 DLL, a 32-bit game needs the x86 one.

### Minimum versions

| Tool | Minimum | Verified by the CI |
| --- | --- | --- |
| CMake | 3.21 | Versions bundled with Visual Studio and GitHub runners |
| Visual Studio | 2022 (17.x) | 2022 locally, 2026 on GitHub runners |
| MinGW-w64 / GCC | mingw-w64 8.0 with GCC 10 (C++17) | Ubuntu 22.04 (GCC 10), Ubuntu 24.04 (GCC 13), Homebrew and winlibs (GCC 14 to 16) |
| Ninja | any | |
| Windows (runtime) | Windows 10 | |

Visual Studio 2019 is not tested. The DLL imports `d3dcompiler_47.dll`, which Windows 8.1 ships as well, but only Windows 10 and 11 are exercised.

Prebuilt DLLs for every toolchain are attached to each [release](../../releases); the [build workflow](../../actions) also keeps them as artifacts for every commit.

### Windows with Visual Studio

Requirements: Visual Studio 2022 or newer with the "Desktop development with C++" workload, which includes CMake and the Windows SDK.

```sh
cmake --preset msvc-x64          # or msvc-x86
cmake --build --preset msvc-x64  # Release build
```

The DLL is written to `build/msvc-x64/Release/Universal-ImGui-Hook.dll`. You can also open the folder in Visual Studio ("Open a local folder") and pick the preset in the configuration dropdown. Visual Studio Code with the CMake Tools extension works the same way.

### Windows with MinGW-w64 (MSYS2)

```sh
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja   # UCRT64 shell
cmake --preset mingw-x64
cmake --build --preset mingw-x64
```

Use the MINGW32 shell and the `mingw-w64-i686-*` packages with the `mingw-x86` preset for a 32-bit DLL.

### Linux (cross-compiling)

The DLL is cross-compiled with MinGW-w64. Install the toolchain:

| Distribution | Packages |
| --- | --- |
| Debian, Ubuntu | `sudo apt install cmake ninja-build g++-mingw-w64-x86-64 g++-mingw-w64-i686` |
| Fedora | `sudo dnf install cmake ninja-build mingw64-gcc-c++ mingw32-gcc-c++` |
| Arch | `sudo pacman -S cmake ninja mingw-w64-gcc` |

Then:

```sh
cmake --preset mingw-x64          # or mingw-x86
cmake --build --preset mingw-x64
```

The DLL is written to `build/mingw-x64/Universal-ImGui-Hook.dll`. The presets use `cmake/mingw-w64-x86_64.cmake` and `cmake/mingw-w64-i686.cmake`, which expect the `x86_64-w64-mingw32-*` and `i686-w64-mingw32-*` compilers in `PATH`; pass `-DCMAKE_TOOLCHAIN_FILE` with your own file if your distribution names them differently.

The resulting DLL targets Windows processes, including games running under Wine or Proton (use a Windows injector inside the same prefix, or `LoadLibrary` from a wrapper DLL).

### macOS (cross-compiling)

```sh
brew install cmake ninja mingw-w64
cmake --preset mingw-x64
cmake --build --preset mingw-x64
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `UIH_BUILD_EXAMPLE` | `ON` when top-level | Builds the injectable example DLL |
| `UIH_BUILD_TESTS` | `OFF` | Builds the `uih_host` test application |
| `UIH_STATIC_RUNTIME` | `ON` when top-level | Links the C runtime statically so the DLL has no redistributable dependency |

## Usage

Inject `Universal-ImGui-Hook.dll` into the target with any injector (or `LoadLibrary`). Edit `example/menu.cpp` to draw your own interface and `example/dllmain.cpp` to change the keys or force a backend:

```cpp
uih::Config config;
config.module = module;                 // DLL to unload on `unloadKey`
config.backend = uih::Backend::None;    // None = detect automatically
config.menuKey = VK_INSERT;
config.unloadKey = VK_F11;
config.debugLog = false;                // logs go to OutputDebugString (DebugView)
config.render = DrawMenu;               // called every frame
uih::Start(config);
```

`DrawMenu` runs between `ImGui::NewFrame()` and `ImGui::Render()` on every frame; use `uih::IsMenuOpen()` to decide what to draw. While the menu is open, keyboard and mouse messages are kept away from the target application and the cursor is released.

The hook can be injected before or after the target initializes its graphics API. When injected into a running Vulkan application, the swap chain format is assumed to be `VK_FORMAT_B8G8R8A8_UNORM` until the application recreates its swap chain.

### Using the hook as a library

The repository is a CMake package that can be consumed with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(uih GIT_REPOSITORY https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git GIT_TAG master)
FetchContent_MakeAvailable(uih)

add_library(my_overlay SHARED dllmain.cpp)
target_link_libraries(my_overlay PRIVATE uih::uih)
```

`uih::uih` exposes `include/uih.h` and Dear ImGui's headers. `tests/consumer/` is a complete example of such a project and is built by the CI on every commit.

## Tests

`-DUIH_BUILD_TESTS=ON` builds `uih_host`, a small application that renders with one API, starts the hook and checks that the overlay is drawn and torn down:

```sh
cmake --preset msvc-x64 -DUIH_BUILD_TESTS=ON
cmake --build --preset msvc-x64
ctest --test-dir build/msvc-x64 -C Release
```

`uih_host --api vulkan --capture` runs one backend (`d3d9`, `d3d10`, `d3d11`, `d3d12`, `vulkan`, `opengl`); `--inject-at 0` starts the hook before the API is initialized, `--dll <path>` loads the real DLL and drives it with its hotkeys, `--capture` compares the pixels on screen and `--warp` uses the software rasterizer.

## Built With

* [Dear ImGui](https://github.com/ocornut/imgui)
* [MinHook](https://github.com/TsudaKageyu/minhook)
* [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)

## Authors

* **Rebzzel** - *Initial work on the kiero library (no longer used)* - [Rebzzel](https://github.com/Rebzzel)
* **Sh0ckFR** | **Revan600** - *ImGui and input hooks* - [Sh0ckFR](https://github.com/Sh0ckFR) - [Revan600](https://github.com/Revan600)

## Contributors

* **primemb** - *Bug fixes* - [primemb](https://github.com/primemb)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
