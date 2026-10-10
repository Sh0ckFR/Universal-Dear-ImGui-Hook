# Universal-Dear-ImGui-Hook

A universal [Dear ImGui](https://github.com/ocornut/imgui) overlay hook for Direct3D 9, 10, 11, 12 and Vulkan applications on Windows.

| Backend | Status |
| ------- | ------ |
| Direct3D 9 | :white_check_mark: |
| Direct3D 10 | :white_check_mark: |
| Direct3D 11 | :white_check_mark: |
| Direct3D 12 | :white_check_mark: |
| Vulkan | :white_check_mark: |

The hook detects the API the target uses at runtime: inject the DLL, press `INSERT` to open the menu and `F11` to unload it.

![screenshot](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## Building

The project uses CMake (3.21+) and ships every dependency (Dear ImGui, MinHook, Vulkan headers). The output is a Windows DLL with no runtime dependency besides the system libraries, so it can be injected into any process.

### Windows (Visual Studio 2022 or newer)

```sh
cmake --preset msvc-x64      # or msvc-x86
cmake --build --preset msvc-x64
```

The DLL is written to `build/msvc-x64/Release/Universal-ImGui-Hook.dll`. Visual Studio can also open the folder directly ("Open a local folder") and pick the preset.

### Linux (cross-compiling with MinGW-w64)

```sh
sudo apt install cmake ninja-build g++-mingw-w64-x86-64   # g++-mingw-w64-i686 for 32-bit
cmake --preset mingw-x64      # or mingw-x86
cmake --build --preset mingw-x64
```

The DLL is written to `build/mingw-x64/Universal-ImGui-Hook.dll`. The same presets work with a MinGW-w64 toolchain on Windows. Linux builds target Windows processes, including games running under Wine or Proton.

## Usage

Edit `example/menu.cpp` to draw your own interface and `example/dllmain.cpp` to change the keys or force a backend:

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

### Using the hook as a library

The repository is a CMake package that can be consumed with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(uih GIT_REPOSITORY https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git GIT_TAG master)
FetchContent_MakeAvailable(uih)

add_library(my_overlay SHARED dllmain.cpp)
target_link_libraries(my_overlay PRIVATE uih::uih)
```

`uih::uih` exposes `include/uih.h` and Dear ImGui's headers. Set `UIH_STATIC_RUNTIME` to control whether the C runtime is linked statically.

## Tests

`-DUIH_BUILD_TESTS=ON` builds `uih_host`, a small application that renders with one API, starts the hook in-process and checks that the overlay is drawn and torn down:

```sh
cmake --preset msvc-x64 -DUIH_BUILD_TESTS=ON
cmake --build --preset msvc-x64
build/msvc-x64/tests/Release/uih_host.exe --api vulkan --capture   # d3d9, d3d10, d3d11, d3d12, vulkan
```

`--inject-at 0` starts the hook before the API is initialized, `--capture` compares the pixels on screen and `--warp` uses the software rasterizer. `ctest` runs every combination.

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
