# Compatibility

This page lists what is tested, and how. Every row of the first three tables runs in the CI on every commit, so the list cannot drift from the code.

## Games, in the CI

Each run injects or preloads the library into a real game, then checks four things:
- the overlay draws on the game's screen;
- `INSERT` opens the menu;
- the menu takes the mouse;
- the unload key removes the overlay while the game keeps running.

The screen captures are uploaded as artifacts of the run.

| Game | Engine | Platform | Graphics APIs | How the library is loaded |
| --- | --- | --- | --- | --- |
| [DDNet](https://ddnet.org/) 20.1.1 | Its own, on SDL2 | Windows x64 | OpenGL, Vulkan | Injected (`uih_game`) |
| [DDNet](https://ddnet.org/) 20.1.1 | Its own, on SDL2 | Linux x86_64 | OpenGL, Vulkan | `LD_PRELOAD` (`tests/game.sh`) |
| [Godot](https://godotengine.org/) 4.7.2 project (`tests/godot`) | Godot | Windows x64 | D3D12, Vulkan, D3D11 (Godot's OpenGL renderer drawn by ANGLE) | Injected (`uih_game`) |
| [Godot](https://godotengine.org/) 4.7.2 project (`tests/godot`) | Godot | Linux x86_64 | OpenGL, Vulkan | `LD_PRELOAD` (`tests/game.sh`) |
| [Godot](https://godotengine.org/) 4.7.2 project (`tests/godot`), Windows build | Godot | Linux, under [Wine](https://www.winehq.org/) 11 | OpenGL, D3D11 (ANGLE), Vulkan | `dinput8.dll` next to the game (`tests/wine.sh`), as a Steam Deck player does |
| DDNet and the Godot project, played by an assistant | Both | Linux x86_64 | OpenGL, Vulkan | `LD_PRELOAD`, with the remote control on, played through `remote/uih_mcp.py` (`tests/game_mcp.py`) |

The CI has no GPU. It renders with Mesa's llvmpipe and lavapipe, and on Windows with WARP and [mesa-dist-win](https://github.com/pal1000/mesa-dist-win).

## Graphics APIs, in the CI

`uih_host` is a minimal application for each API. It checks many cases against exact pixel values, and runs them on every commit; [`tests/README.md`](../tests/README.md) has the full list.

| API | Windows x64 and x86 | Linux x86_64 and aarch64 | Under Wine, in a game | Checked with |
| --- | --- | --- | --- | --- |
| Direct3D 9, 9Ex | Yes | | | sRGB back buffer |
| Direct3D 10 | Yes | | | The D3D debug layer; sRGB back buffer |
| Direct3D 11 | Yes | | Yes | The D3D debug layer; sRGB and float back buffers |
| Direct3D 12 | Yes | | | The D3D debug layer; float and HDR10 back buffers |
| Vulkan | Yes | Yes | Yes | The Khronos validation layer; sRGB back buffer |
| OpenGL | WGL | GLX and EGL | Yes | sRGB framebuffer |

## Toolchains, in the CI

| Toolchain | Targets |
| --- | --- |
| MSVC, CMake and the Visual Studio solution | Windows x64 and x86 |
| MinGW-w64, from Ubuntu and from macOS | Windows x64 and x86 |
| GCC and Clang | Linux x86_64 |
| GCC | Linux aarch64 |
| GCC, on a glibc 2.28 distribution | The portable Linux library, x86_64 and aarch64 |
| GCC with ASan and UBSan | Linux x86_64 |

## Cost of the overlay

`uih_host --api <api> --bench <frames>` measures the overlay's cost per frame with vsync off. It measures four phases, each preceded by 300 frames of warm-up:
- without the overlay;
- with the overlay, menu closed;
- with the menu open, which draws the test menu, a 500×400 window;
- without the overlay again, after `Stop()`. Comparing this phase with the first one shows how noisy the measure is.

The cost is the median frame time of a phase, minus the mean of the two phases without the overlay.

Measured with 5000 frames per phase, Release x64. The machine was an NVIDIA GeForce RTX 4070 SUPER (driver 621.40), an AMD Ryzen 7 7800X3D and Windows 11 build 26300, in an 800×600 window:

| API | Frame without the overlay | Overlay, menu closed | Overlay, menu open |
| --- | --- | --- | --- |
| Direct3D 9 | 0.079 ms | +0.070 ms | +0.031 ms |
| Direct3D 9Ex | 0.081 ms | +0.029 ms | +0.028 ms |
| Direct3D 10 | 0.066 ms | +0.012 ms | +0.009 ms |
| Direct3D 11 | 0.069 ms | +0.009 ms | +0.007 ms |
| Direct3D 12 | 0.148 ms | +0.002 ms | +0.016 ms |
| Vulkan | 0.096 ms | +0.043 ms | +0.041 ms |
| OpenGL | 0.109 ms | +0.039 ms | +0.055 ms |

How to read these numbers:
- At most 0.07 ms per frame is 0.4% of a frame at 60 fps and 1% at 144 fps.
- Each frame of the host draws nothing but a clear, so these numbers are the overlay's own cost, with no game competing for the CPU or the GPU.
- A menu that draws more costs more: the cost grows with what your `render` callback draws.
- Differences below about 0.02 ms are within the noise of the measure; compare the two phases without the overlay to see it.

To measure on your machine, build the tests and run `uih_host --api d3d11 --bench 5000` for each API.

## Known limits

The [Limitations](../README.md#limitations) section of the README lists what is not supported, such as anti-cheat systems, and the cases that work differently.

To report a game that works, or one that does not, open an issue with the engine, the graphics API and the library's log (`config.debugLog`).
