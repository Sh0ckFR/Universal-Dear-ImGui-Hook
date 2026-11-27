<div align="center">

# Universal-Dear-ImGui-Hook

**Draw your [Dear ImGui](https://github.com/ocornut/imgui) menu inside any game, whatever its graphics API, and let Claude see and play it.**

[![build](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/Sh0ckFR/Universal-Dear-ImGui-Hook)](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/releases/latest)
[![license](https://img.shields.io/github/license/Sh0ckFR/Universal-Dear-ImGui-Hook)](LICENSE)
![platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Steam%20Deck-blue)
![MCP](https://img.shields.io/badge/AI-MCP%20server-8A2BE2)

[Quick start](#quick-start) · [Chat with Claude](#talk-to-claude-from-the-game) · [AI remote control](#remote-control-by-an-ai-assistant) · [API](#api) · [Building](#building) · [Compatibility](docs/COMPATIBILITY.md)

![The example menu next to the Dear ImGui demo window, drawn by the hook in a Direct3D 11 application](imgui.png)

</div>

The graphics API is detected at run time: build once, inject anywhere. The same library also lets an AI assistant such as Claude Code [see and play the game](#remote-control-by-an-ai-assistant) through MCP, read and change its memory and code, and browse the objects of [Unity and Unreal Engine games](#engine-tools). You can even [chat with Claude from inside the game](#talk-to-claude-from-the-game).

| API | Windows (x86, x64) | Linux (x86-64, ARM64) |
| --- | :---: | :---: |
| Direct3D 9, 9Ex | ✅ | ✅ Windows DLL under Wine or Proton |
| Direct3D 10, 11, 12 | ✅ | ✅ Windows DLL under Wine or Proton |
| Vulkan | ✅ | ✅ |
| OpenGL 3.0+ | ✅ WGL | ✅ GLX, EGL |

On Linux, the overlay works in X11, XWayland and native Wayland games.

> [!IMPORTANT]
> **Use it on single-player games and your own software.** Anti-cheat systems detect injected DLLs and can ban your account. While the remote control is on, any program running on your machine can control the game, and with the memory tools, read and write its memory.

## Contents

| Get started | Go further | Project |
| --- | --- | --- |
| [Features](#features) | [Remote control by an AI assistant](#remote-control-by-an-ai-assistant) | [Tests](#tests) |
| [Quick start](#quick-start) | [API](#api) | [Contributing](#contributing) |
| [Usage](#usage): [Windows](#windows), [Linux](#linux), [settings](#settings) | [How it works](#how-it-works) | [Limitations](#limitations) |
| [Writing your menu](#writing-your-menu), [as a library](#using-the-hook-as-a-library) | [Building](#building) | [Credits](#credits) |
| [Talk to Claude from the game](#talk-to-claude-from-the-game) | [Compatibility and cost of the overlay](docs/COMPATIBILITY.md) | [Support](#support), [License](#license) |

Sections marked with ▶ expand on click. What changed in each version is listed in the [releases](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/releases).

## Features

**The overlay**

- 🎨 **One library for every API.** The backend is picked from the first API that presents a frame. Inject before or after the game initializes its graphics.
- ⌨️ **Menu input.** `INSERT`, or L3 + R3 on a controller, opens the menu, and `F11` removes the overlay and unloads the DLL; both keys are [settings](#settings). While the menu is open, the game receives no keyboard, mouse or controller input, and an XInput controller navigates the menu. On Linux, the game gets no key or button presses and no motion, on X11 and native Wayland alike.
- 🛡️ **The overlay can't crash the game** (MSVC builds on Windows, and Linux). An access violation or a C++ exception in the overlay, in a renderer backend or in your menu code, removes the overlay instead of taking the game down. The menu key brings it back.
- 🖼️ **Images and frame capture.** `uih::CreateTexture()` turns RGBA pixels into an image for `ImGui::Image()`, and `uih::CaptureFrame()` copies the game's frame, without the overlay, on every backend.
- 🪟 **Multiple windows.** When the game presents to more than one window or swap chain, the overlay stays on one of them and follows the focus.
- 🖥️ **Readable on any display.** The interface follows the game window's DPI, or a fixed scale. Colors are right on sRGB, floating-point and HDR10 back buffers, where the overlay is as bright as the SDR content Windows shows.

**The AI assistant**

- 💬 **Chat with Claude in the game.** A [chat window over the game](#talk-to-claude-from-the-game) where you tell Claude what to do. It looks at the game, plays it, reads or changes its memory, and answers in the chat.
- 🤖 **AI remote control.** Through [MCP](#remote-control-by-an-ai-assistant), an assistant such as Claude Code sees the game, shows messages to the player, presses keys and clicks. Your menu can give it [tools of your own](#tools-of-your-own), such as *teleport* or *spawn an item*, which it calls like the built-in ones.
- 🔬 **Memory and code tools.** With the [memory](#memory-tools) and [advanced](#advanced-features-and-the-ai-advanced-features-window) tools, it scans values, follows pointer chains, freezes values and finds the code that writes them. It also disassembles and patches that code, and builds signatures that survive an update.
- ⏸️ **Plays at its own pace.** It types text, drags the mouse, waits for the screen to change, and pauses or slows down the game while it thinks. The *AI Advanced Features* window shows everything it does.
- 🧩 **Unity and Unreal Engine objects.** With the [engine tools](#engine-tools), the assistant lists the classes of a Unity (Mono or IL2CPP) or Unreal Engine 4 or 5 game, finds their live objects, and reads and writes their fields by name instead of by address. On Unreal, the **Engine** tab of the AI Advanced Features window does the same in the game, calls its functions, and writes a [C++ SDK](#a-c-sdk-of-an-unreal-engine-game) of the game for a mod, a tool or an automated test of your own.

**Shipping it**

- 📦 **No injector needed.** The DLL can be loaded as `dinput8.dll`, and the Linux library with `LD_PRELOAD`.
- 🧱 **No dependencies.** Dear ImGui, MinHook, Zydis and the Vulkan headers are vendored, and the C and C++ runtimes are linked statically: you ship one file.
- ✅ **Tested in real games.** On every commit, the CI loads the library into [DDNet](https://ddnet.org/) and a [Godot](https://godotengine.org/) project on Windows and Linux, with Direct3D 11, Direct3D 12, Vulkan and OpenGL, and plays them through the MCP server on Linux. It also runs the Windows build of Godot under Wine with the DLL as `dinput8.dll`, with OpenGL, Direct3D 11 and Vulkan. [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) has the full list.

## Quick start

### 1. Get the library

Prebuilt libraries are attached to each [release](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/releases), and every commit's builds are kept as [workflow artifacts](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/actions). Pick the one that matches the **game**, not your machine:

| Game | File |
| --- | --- |
| Windows, 64-bit | `Universal-ImGui-Hook-msvc-x64.dll` |
| Windows, 32-bit | `Universal-ImGui-Hook-msvc-Win32.dll` |
| Linux, x86-64 or ARM64 | `Universal-ImGui-Hook-linux-x86_64.so` or `-linux-aarch64.so` (glibc 2.28 or later) |

These are the `Universal-ImGui-Hook.dll` and `.so` that the build below produces, renamed after their platform; the rest of this page uses the short name.

Each file also comes in four variants with some settings on, named with a suffix, such as `Universal-ImGui-Hook-msvc-x64-ai.dll`:

| Suffix | Settings on | What it gives |
| --- | --- | --- |
| none | none | The menu alone |
| `-remote` | `remoteControl` | The [remote control](#remote-control-by-an-ai-assistant): an assistant such as Claude sees the game, shows messages and presses keys |
| `-memory` | `remoteControl`, `remoteMemory` | The remote control and the [memory tools](#memory-tools), with the [engine tools](#engine-tools) of Unity and Unreal games |
| `-advanced` | `remoteControl`, `aiAdvancedFeatures` | The remote control and the [advanced tools](#advanced-features-and-the-ai-advanced-features-window): typing, mouse, game speed, and the [chat with Claude](#talk-to-claude-from-the-game), without access to the memory |
| `-ai` | all three | Every tool of the assistant, with the chat and the advanced memory tools |

> [!WARNING]
> Use the variants for single-player games only: any program you run can then control the game, and with `-memory` or `-ai` its memory too.

<details>
<summary>Check a file you downloaded</summary>

Each release lists the SHA256 of its libraries in `SHA256SUMS`, and GitHub attests that this repository's CI built them:

```sh
gh attestation verify Universal-ImGui-Hook-msvc-x64.dll --repo Sh0ckFR/Universal-Dear-ImGui-Hook
```

</details>

### 2. Or build it

**Windows**: open `Universal-ImGui-Hook.sln` in Visual Studio 2022 or newer and build `Release|x64`, or `Release|Win32` for a 32-bit game. The DLL is `build/vs/x64/Release/Universal-ImGui-Hook.dll`, or in `build/vs/x86/Release/` for Win32.

**Linux**:

```sh
sudo apt install cmake ninja-build g++ pkg-config libxcb1-dev
cmake --preset linux && cmake --build --preset linux
LD_PRELOAD=$PWD/build/linux/Universal-ImGui-Hook.so ./game
```

[Building](#building) covers MinGW-w64, cross-compiling and the CMake options.

### 3. Make it yours

You only edit the example; the hook itself, in `src/`, needs no changes:

| To change | Edit |
| --- | --- |
| The menu | `example/menu.cpp`, plain Dear ImGui code run every frame |
| Hotkeys, controller buttons, graphics API, interface scale | `example/config.cpp` |
| AI remote control and its pipe or socket name | `example/config.cpp`: see [Remote control by an AI assistant](#remote-control-by-an-ai-assistant) |
| Tools of your own for the assistant | `RegisterExampleTools()` in `example/menu.cpp`: see [Tools of your own](#tools-of-your-own) |
| Fonts and style | A `config.setup` callback, run when the ImGui context is created, set next to `config.render` in `example/dllmain.cpp` (Windows) or `example/preload.cpp` (Linux) |

## Usage

### Windows

| How to load it | What to do | What `F11` does |
| --- | --- | --- |
| **With an injector** | Inject `Universal-ImGui-Hook.dll` with any injector, with `LoadLibrary`, or with the script of this repository (see [step 2](#step-2-load-the-library-into-the-game) of the chat) | Unloads the DLL; inject it again to bring the overlay back |
| **As `dinput8.dll`**, no injector | Rename the DLL to `dinput8.dll` and put it next to the game's executable. Under Wine or Proton, add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the launch options | Removes the overlay, and `INSERT` brings it back: the game holds a reference to the DLL, which stays loaded |

As `dinput8.dll`, the DLL exports `DirectInput8Create` and forwards it to the system library, so any game that imports DirectInput 8, or loads `dinput8.dll` as Godot does, loads it.

> [!TIP]
> Logs (`config.debugLog`) go to `OutputDebugString`; read them with [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview). The example DLL writes them when the game runs with the environment variable `UIH_DEBUG=1`.

### Linux

```sh
LD_PRELOAD=/path/to/Universal-ImGui-Hook.so ./game
```

- **Steam game**: set the launch options to `LD_PRELOAD=/path/to/Universal-ImGui-Hook.so %command%`.
- **Logs**: `UIH_DEBUG=1` prints them on stderr.
- **`F11`**: a preloaded library cannot leave the process, so `F11` removes the overlay and its hooks, and `INSERT` brings them back.

**Steam Deck.** SteamOS is Linux:

| Game | What to do |
| --- | --- |
| Native Linux game | Set its launch options to `LD_PRELOAD=/path/to/Universal-ImGui-Hook.so %command%` |
| Windows game under Proton | Use the Windows DLL as `dinput8.dll` next to the game's executable, with `WINEDLLOVERRIDES="dinput8=n,b" %command%` in the launch options (see [Windows](#windows)) |

The controller buttons that open the menu (`menuButtons`) are read through XInput, which only the Windows DLL uses. In a native game, bind the menu key, `INSERT` by default, to a button of the Deck with Steam Input.

### Settings

The example library's settings are compiled into it: edit `example/config.cpp`, next to `dllmain.cpp`, and rebuild.

```cpp
void ApplySettings(uih::Config& config) {
    config.menuKey = VK_INSERT;                              // opens and closes the menu
    config.unloadKey = VK_F11;                               // removes the overlay, unloads the DLL
    config.menuButtons = uih::GamepadL3 | uih::GamepadR3;    // controller buttons held together, 0 to disable
    config.backend = uih::Backend::None;                     // graphics API to hook, None detects it
    config.scale = 0.0f;                                     // interface size, 0 follows the DPI
    config.remoteControl = false;                            // AI remote control
    config.remotePipe = "universal-imgui-hook";              // name of its pipe (Windows) or socket (Linux)
    config.remoteMemory = false;                             // memory tools of the remote control
    config.aiAdvancedFeatures = false;                       // advanced tools and the AI Advanced Features window
}
```

<details>
<summary>Values of each setting</summary>

| Setting | Values |
| --- | --- |
| `menuKey`, `unloadKey` | Windows: [virtual-key codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) such as `VK_F1` or `'M'`. Linux: X11 keysyms from `X11/keysymdef.h`, such as `0xffbe` for `XK_F1`; the file has its own `#else` branch for them. `0` disables the key. |
| `menuButtons` | `uih::GamepadA`, `B`, `X`, `Y`, `L1`, `R1`, `L3`, `R3`, `Start`, `Back`, `DpadUp`, `DpadDown`, `DpadLeft`, `DpadRight`, combined with `\|`; first XInput controller, Windows only. |
| `backend` | `uih::Backend::None` detects the API from the first one that presents a frame, which is right for nearly every game. `D3D9`, `D3D10`, `D3D11`, `D3D12`, `Vulkan` or `OpenGL` forces one, for a game that uses several. |
| `scale` | `0` follows the DPI of the game window; `1.5` draws the interface at 150%. |
| `remoteControl`, `remotePipe` | `true` serves the [remote control](#remote-control-by-an-ai-assistant) on the named pipe `\\.\pipe\<remotePipe>` on Windows, or the Unix socket `$XDG_RUNTIME_DIR/<remotePipe>` on Linux. |
| `remoteMemory` | `true`, with `remoteControl`, also gives the assistant the [memory tools](#memory-tools): modules, read, write, value and pattern scans, and the [engine tools](#engine-tools), which see the objects of Unity and Unreal Engine games. |
| `aiAdvancedFeatures` | `true`, with `remoteControl`, gives the assistant the [advanced tools](#advanced-features-and-the-ai-advanced-features-window) and adds the *AI Advanced Features* button to the menu; the advanced memory tools also need `remoteMemory`. |

</details>

## Writing your menu

The interface lives in `example/menu.cpp`. `DrawMenu()` is called every frame between `ImGui::NewFrame()` and `ImGui::Render()`, so any Dear ImGui code works there. The entry points, `example/dllmain.cpp` on Windows and `example/preload.cpp` on Linux, apply the settings and start the hook:

```cpp
uih::Config config;
ApplySettings(config);         // example/config.cpp
config.render = DrawMenu;      // every frame
config.setup = LoadFonts;      // when the ImGui context is created: fonts, style
config.module = module;        // Windows: DLL unloaded by unloadKey
uih::Start(config);
```

Images are created once from RGBA pixels and drawn like any ImGui texture:

```cpp
static ImTextureData* logo = uih::CreateTexture(pixels, 64, 64);
ImGui::Image(logo->GetTexRef(), ImVec2(64, 64));
```

The example menu is a tour of the common widgets: buttons, combo and list boxes, inputs, sliders, colors, tabs, trees, tables, plots, popups and draw lists, with `imgui.png` shown under the slider. Its *Menu* bar opens Dear ImGui's own demo window, which covers every function. `example/image.cpp` embeds the picture in the library and decodes it with [stb_image](https://github.com/nothings/stb).

### Using the hook as a library

The repository is a CMake package. Link `uih::uih` to get `include/uih.h` and Dear ImGui's headers:

```cmake
include(FetchContent)
FetchContent_Declare(uih
    GIT_REPOSITORY https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git
    GIT_TAG v2.1.0)   # a release tag, for reproducible builds
FetchContent_MakeAvailable(uih)
target_link_libraries(my_overlay PRIVATE uih::uih)
```

`tests/consumer/` is a complete example, built by the CI. On Linux, linking `uih::uih` into a shared library applies `src/linux/exports.map`, which exports only the hooked functions.

## Talk to Claude from the game

Chat with [Claude](https://claude.com/claude-code) in a window drawn over the game: ask it what it sees, or tell it what to do. It looks at the game, plays it, reads or changes its memory, and answers in the chat as it works.

```mermaid
flowchart LR
    A["Chat tab of the<br/>AI Advanced Features window"] -- "your messages" --> B["remote/uih_chat.py<br/>(the bridge)"]
    B -- "claude -p, one for<br/>the conversation" --> C["Claude Code"]
    C -- "game tools" --> D["remote/uih_mcp.py"]
    D -- "screenshots, keys,<br/>clicks, memory" --> E["The library in the game"]
    B -- "the answers" --> A
```

### What you need

| | |
| --- | --- |
| **Claude Code** | [Installed](https://claude.com/claude-code) and signed in: `claude` must start in a terminal. The chat uses your Claude Code plan. |
| **Python** | 3.8 or newer, from [python.org](https://www.python.org) on Windows. Nothing to install with `pip`. The commands below use `python`; on Linux, type `python3`. |
| **This repository** | Cloned, to build the library and run the scripts in `remote/`. |
| **A single-player game** | Anti-cheat systems detect injected DLLs. |

### Step 1: get the library with the chat on

Download the `-ai` library of a [release](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/releases) that matches the game, such as `Universal-ImGui-Hook-msvc-x64-ai.dll` (see [Quick start](#quick-start)): it has the settings below on.

Or build it. In `example/config.cpp`, set these settings to `true`, or configure CMake with `-DUIH_EXAMPLE_VARIANT=ai`, which turns them all on (`remote`, `memory` and `advanced` build the other [variants](#1-get-the-library)):

```cpp
config.remoteControl = true;        // the game can be controlled through a local pipe or socket
config.remoteMemory = true;         // optional: Claude can read and change the game's memory
config.aiAdvancedFeatures = true;   // the AI Advanced Features window and its chat
```

Then build, as in [Quick start](#2-or-build-it): `build/vs/x64/Release/Universal-ImGui-Hook.dll` on Windows (`build/vs/x86/Release/` for Win32), `build/linux/Universal-ImGui-Hook.so` on Linux.

### Step 2: load the library into the game

**Windows**: the DLL must be loaded into the game, by any means. **If it already is**, with your own injector or as `dinput8.dll` next to the game, skip to the check below. Otherwise, start the game, wait for its window, then run this in a terminal opened in the repository:

```sh
python remote/uih_mcp.py inject game.exe build/vs/x64/Release/Universal-ImGui-Hook.dll
```

`game.exe` is the process name shown in the Task Manager (*Details* tab), or its PID. Give the process that shows the game window, not its launcher. `inject` only loads the DLL and exits: nothing else needs it.

**Linux**: start the game with the library preloaded:

```sh
LD_PRELOAD=$PWD/build/linux/Universal-ImGui-Hook.so /path/to/game
```

The socket opens with the overlay's first frame, in the process that draws it, not in the launch scripts that preload the library too.

**Check.** The overlay shows up with the menu open: that is how you know the library is loaded. To check that the game also answers on its pipe or socket, however the library got in:

```sh
python remote/uih_mcp.py check
```

```text
The game answers on universal-imgui-hook. Graphics API: Vulkan, window 1024x768, keyboard focus: True, overlay menu open: True, memory tools: on, advanced tools: on.
```

Add `--pipe <name>` if `config.remotePipe` is not the default.

### Step 3: start the bridge

In a terminal opened in the repository, which you leave open while you play:

```sh
python remote/uih_chat.py
```

It prints `Connected` once it has found the game. Start it before or after the game, in any order: it waits for the game, and for it to come back after a restart.

> [!NOTE]
> You never start `remote/uih_mcp.py` yourself for the chat: the bridge hands it to Claude Code, which starts it once and keeps it for the conversation. Run without arguments, it is an MCP server waiting for a client on its standard input, and does nothing on its own.

### Step 4: chat

In the game, press `INSERT` to open the menu, click **AI Advanced Features**, and stay on the **Chat** tab. The top of the tab says **Claude is listening** in green. Type, then press Enter:

- *What do you see?*
- *Find my health and keep it at 100.*
- *Walk to the door and open it.*
- *Type "gg" in the game's chat.*
- *Why can't I buy this item?*

Claude writes in the chat as it works, and the line *Claude is working* shows which tool it is using. **Stop** ends its answer, and **New conversation** makes it forget what was said.

To play, Claude closes the menu, since the game gets no keys or clicks while it is open. Its answers then show at the top of the screen, and `INSERT` brings the chat back.

### Bridge options

| Option | Default | Effect |
| --- | --- | --- |
| `--pipe <name>` | `universal-imgui-hook` | The game's `config.remotePipe`, if you changed it |
| `--model <model>` | Claude Code's default | `sonnet` answers faster, `opus` thinks more |
| `--claude <path>` | `claude` on the `PATH` | The Claude Code executable |

<details>
<summary>How the bridge runs Claude Code</summary>

- It runs Claude Code headless (`claude -p`) with the tools of `remote/uih_mcp.py` and no other tool: no shell, no file access.
- It starts it as soon as it finds the game and keeps it for the whole conversation, writing it each of your messages, so answers start without waiting for Claude Code to load. **New conversation** starts a new one.
- It runs from its own folder in the temporary directory, so none of your `CLAUDE.md` files apply. The conversation is kept there: if Claude Code stops, the next message resumes it.

</details>

### Troubleshooting

| You see | Do |
| --- | --- |
| `INSERT` shows nothing | The library is not in the game: inject it into the process that shows the window, with the same architecture (x64 DLL for a 64-bit game). Some launchers start the game as a second process, such as Godot's `_console.exe`. |
| The bridge stays on `Waiting for the game` | Same as above, or `config.remoteControl` is `false`, or another game already holds the pipe: close it, or give each game its own `remotePipe` and `--pipe`. |
| `The game refuses the chat` | `config.aiAdvancedFeatures` is `false`: set it and rebuild. |
| **Claude is not connected** in the chat | The bridge is not running, or runs with another `--pipe`. |
| `Claude Code is not on the PATH` | Install Claude Code, or give its path with `--claude`. |
| `Claude Code failed` in the chat | Run `claude` once in a terminal and sign in. |
| Claude cannot press keys or read the memory | The switches at the top of the window are off, or `config.remoteMemory` is `false`. |

## Remote control by an AI assistant

The library can let an AI assistant such as [Claude Code](https://claude.com/claude-code) see the game, show messages to the player, press keys and click, and optionally read and write the game's memory. It works with every graphics API, and on Linux with X11, XWayland and native Wayland games.

The assistant speaks the [Model Context Protocol](https://modelcontextprotocol.io) (MCP) with `remote/uih_mcp.py` over standard input and output, and the script talks to the library over a local named pipe (Windows) or Unix socket (Linux):

```mermaid
flowchart LR
    A["Assistant, such as Claude Code"] -- "MCP on stdin and stdout" --> B["remote/uih_mcp.py"]
    B -- "local named pipe or Unix socket" --> C["The library in the game"]
```

### Set it up

**1. Turn it on and load the library into the game**, as in steps [1](#step-1-get-the-library-with-the-chat-on) and [2](#step-2-load-the-library-into-the-game) of the chat. Only `config.remoteControl` is required; the other settings add tools, as the table below shows. `config.remotePipe` names the pipe, `\\.\pipe\universal-imgui-hook` by default, or the socket, `$XDG_RUNTIME_DIR/universal-imgui-hook`. `python remote/uih_mcp.py check` tells you whether the game answers.

**2. Register the MCP server with the assistant**, once. You do not start `uih_mcp.py` yourself: the assistant does when a session starts, and talks to it over its standard input and output. With Claude Code:

```sh
claude mcp add game -- python C:/path/to/Universal-Dear-ImGui-Hook/remote/uih_mcp.py
```

On Linux, use `python3 /path/to/Universal-Dear-ImGui-Hook/remote/uih_mcp.py`. With another `remotePipe`, add `--pipe <name>` after the script. Other MCP clients, such as Claude Desktop, take the same command in their configuration:

```json
{
  "mcpServers": {
    "game": { "command": "python", "args": ["C:/path/to/Universal-Dear-ImGui-Hook/remote/uih_mcp.py"] }
  }
}
```

**3. Ask.** Start a new session of the assistant, or run `/mcp` in Claude Code, and talk about the game:

| Ask | Needs |
| --- | --- |
| *Look at the game and tell me what you see.* | `remoteControl` |
| *Write "Well done!" in the game for 5 seconds.* | `remoteControl` |
| *Walk right to the edge of the platform, then jump.* | `remoteControl` |
| *Open the overlay menu and describe it.* | `remoteControl` |
| *Click on "New game" in the main menu.* | `remoteControl` |
| *Find my money in memory and set it to 99999.* | `remoteMemory` |
| *Type "gg" in the chat, then wait for the next round to start.* | `aiAdvancedFeatures` |
| *Find my health, keep it at 100, and find a pointer to it that survives a restart.* | `remoteMemory` and `aiAdvancedFeatures` |
| *Find the instruction that takes my health away, show me the code around it, and patch it out.* | `remoteMemory` and `aiAdvancedFeatures` |

### Which tools each setting gives

| With | The assistant gets |
| --- | --- |
| `remoteControl` | The [tools](#tools): screenshot, message, keys, clicks, game info, and [tools of your own](#tools-of-your-own) |
| `remoteMemory` too | The [memory tools](#memory-tools) and the [engine tools](#engine-tools) |
| `aiAdvancedFeatures` too | The [advanced tools](#advanced-features-and-the-ai-advanced-features-window): typing, mouse, waiting, game speed, and the *AI Advanced Features* window with the chat |
| All three | The advanced memory tools: pointers, watch list, what writes, disassembler, patches, signatures |

### Tools

| Tool | Effect |
| --- | --- |
| `screenshot` | The current frame, without the overlay, at most 1280 pixels wide or high by default |
| `show_text` | A message to the player at the top of the screen, drawn by the overlay, for 10 seconds by default |
| `press_key` | Holds keys for a duration, 150 ms by default; `right+space` holds them together |
| `click` | Clicks at a point of the last screenshot, with the left, right or middle button |
| `game_info` | Graphics API, window size, keyboard focus, menu state, whether the memory and advanced tools are on, and the speed of the game when it is not real time |

### Memory tools

With `remoteMemory`, the assistant also gets memory tools, like those of a memory editor such as Cheat Engine:

| Tool | Effect |
| --- | --- |
| `memory_find`, `memory_next`, `memory_results` | Value scan of the writable memory (`i8` to `i64`, `f32`, `f64`), narrowed by a new value or by `changed`, `unchanged`, `increased`, `decreased`, with the module+offset of each address |
| `memory_read`, `memory_write` | Hexadecimal dump, with the first bytes decoded as numbers and as a pointer; writes a value or bytes, code included |
| `memory_pattern` | Byte pattern with `??` wildcards in a module, the executable by default |
| `memory_modules` | The game's modules with their base address and size |

Addresses are hexadecimal or `module+offset`, such as `game.exe+1a2b3c`. With `aiAdvancedFeatures`, they can also be pointer chains: `[game.exe+1a2b3c]+10` is the address 0x10 bytes after the one stored at `game.exe+1a2b3c`.

### Engine tools

With `remoteMemory`, the assistant also sees the objects of Unity and Unreal Engine games, by their classes and fields instead of bytes:
- **Unity**: the library finds the runtime of the game's scripts, Mono or IL2CPP, and calls its C API, which it does not link.
- **Unreal Engine 4.22 to 5.x**: it finds the engine's name table and object array in the game's memory, and reads the classes, structs, enums, properties and functions from them. It calls functions through `UObject::ProcessEvent`, which it finds in the virtual table of the objects.

| Tool | Effect |
| --- | --- |
| `engine_info` | The engine, its scripting runtime and its version, such as Unity 2022.3.10f1 with IL2CPP, or Unreal Engine 5.3 |
| `engine_classes` | The assemblies of the game's scripts (the packages on Unreal), or their classes whose name contains a filter, such as `player` in `Assembly-CSharp` |
| `engine_search` | The methods and fields (functions and properties on Unreal) of every class whose name contains a text, such as `heal`: each as `Class::Name` with its signature, or its offset and type, those named exactly so first |
| `engine_class` | A class: its parents, its fields with their type and offset (or static, const), inherited ones included, and its methods with their parameters |
| `engine_instances` | The live objects of a class, such as every `Game.Player`, found by a scan of the memory checked against the layout of the class, or in the object array on Unreal |
| `engine_field` | Reads a field of an object or a static field, and writes it when given a value: numbers, booleans, strings, enums, structs such as `Vector3` (`position.x`), references to other objects |
| `engine_call` | Calls a method on the game's main thread and returns its result, or the exception it threw; on Unreal, a function of an object or a static one, with structs as JSON objects (`{"X": 1, "Y": 2, "Z": 3}`), and its out parameters after its result. Needs `aiAdvancedFeatures` too |
| `engine_dump` | Unreal: writes a [C++ SDK](#a-c-sdk-of-an-unreal-engine-game) of the game, its own packages by default or `all` of them, to a header in the temporary directory, and gives its full path |

Ask, for example: *Find the player object, tell me its health, and set its speed to 10.* The assistant lists the classes of `Assembly-CSharp`, describes the player's class, finds its instance and reads or writes the fields.

Unreal names may have spaces, such as the blueprint function `Get Health Ratio`: in the commands of the [protocol](remote/PROTOCOL.md), they go between double quotes.

### A C++ SDK of an Unreal Engine game

`engine_dump`, or the **Dump C++ SDK** button of the **Engine** tab, writes the classes, structs, enums and functions of the running game to one header, such as `%TEMP%\MyGame-SDK-5.3-20261127-164021.hpp`, and gives its full path. The header needs nothing else, neither Unreal's headers nor this library: include it in a DLL of your own, C++17, built with MSVC, MinGW-w64, GCC or Clang, and inject that DLL into the game: a mod, a tool or an automated test.

- `uesdk::WaitForInit()` finds the engine again at each launch: the name table, the object array and `ProcessEvent`, and checks they are those of the game that was dumped, the same build. Until then, and when it fails, `uesdk::LastError()` says why.
- Each property is an accessor, `player->Health()`, that reads and writes the game's memory; each function a wrapper, `player->Heal(50)`, whose out parameters are references. Offsets are found again by name when the game's are not those of the dump. An accessor reads the object's memory directly, as the game does: use objects found again recently, since the game frees those it destroys.
- `uesdk::Instances<SDK::APlayer>()`, `uesdk::FindObject()` and `SDK::APlayer::StaticClass()` find the objects.
- Functions must run on the game thread: `uesdk::GameThread::Run()` runs code there and waits for it, `Post()` does not wait. A call that faults in the game's code returns false with MSVC builds, and the game goes on.
- Call `uesdk::Shutdown()` before the DLL unloads.

```cpp
#include "MyGame-SDK-5.3-20261127-164021.hpp"

DWORD WINAPI TestThread(void* module) {
    if (!uesdk::WaitForInit(30000))
        return 1;  // uesdk::LastError() says why
    while (!(GetAsyncKeyState(VK_END) & 1)) {
        for (SDK::APlayerCharacter_C* player : uesdk::Instances<SDK::APlayerCharacter_C>())
            player->Health() = player->Health_Max();  // keeps the player alive while testing a level
        if (GetAsyncKeyState(VK_F5) & 1)  // moves the player up, to reach a spot to test
            uesdk::GameThread::Run([] {
                if (auto* player = uesdk::FirstInstance<SDK::APlayerCharacter_C>()) {
                    SDK::FVector at = player->K2_GetActorLocation();
                    at.Z() += 500;
                    SDK::FHitResult hit{};  // an out parameter
                    player->K2_SetActorLocation(at, false, hit, true);
                }
            });
        Sleep(100);
    }
    uesdk::Shutdown();
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH)
        CloseHandle(CreateThread(nullptr, 0, TestThread, module, 0, nullptr));
    return TRUE;
}
```

The names are the game's: a property `Health Max` becomes `Health_Max()`, a blueprint class gets the prefix `A` for actors, `U` for other objects and `F` for structs, as in Unreal's C++. Dump the SDK again after each update of the game: the header refuses another build. On Linux, the header works the same, except `GameThread`, which has nothing to run on: call the functions from a hook of the game's own.

### Advanced features and the AI Advanced Features window

With `config.aiAdvancedFeatures = true`, the assistant also gets these tools:

| Tool | Effect |
| --- | --- |
| `type_text` | Types Unicode text, for chat boxes and text fields; a new line presses Enter |
| `mouse_move`, `drag` | Moves the cursor to a point of the last screenshot, or drags from one point to another with a button held |
| `wait_for_change` | Waits until a share of the frame's pixels has changed, 1% by default, or for a timeout: use it after a loading screen or an animation, instead of taking screenshots in a loop |
| `game_speed` | Pauses the game while the assistant thinks, slows it down (0.25 for a fast fight) or brings it back to real time; while it is paused, each key, click or wait runs it for its duration, and `advance_ms` runs it for a set time. The overlay keeps drawing and the screenshots keep working |

and, with `remoteMemory` too:

| Tool | Effect |
| --- | --- |
| `memory_pointer` | Follows a pointer chain, `[[game.exe+1a2b]+10]+8`, and shows each pointer it reads |
| `memory_pointer_scan` | Finds the pointers to an address, those stored in a module first: `[game.exe+offset]+delta` finds the value again after a restart |
| `memory_watch`, `memory_freeze` | A watch list of values, each with a type and a label; a frozen value is written again every frame |
| `memory_what_writes` | The instructions that write a value, or read it, decoded, with their hit counts: hardware breakpoints on every thread, like a debugger's data breakpoint |
| `memory_disassemble` | x86 and x64 instructions at an address, and optionally those before it, with module+offset, bytes, exported functions, and the module+offset and symbol of the addresses they branch to or read |
| `memory_patch`, `memory_unpatch` | Changes the game's code, such as `nop` over the instruction that takes health away, and puts the original bytes back; removing the overlay restores every patch |
| `memory_signature` | A byte pattern unique in its module, with `??` over the offsets that change when the game is rebuilt: `memory_pattern` finds the code again after an update |

The example menu then shows an **AI Advanced Features** button, which opens a window provided by the library, `uih::DrawAIAdvancedFeatures()`:

| Part of the window | What it does |
| --- | --- |
| **Chat** | Talk to Claude from the game while `remote/uih_chat.py` runs: see [Talk to Claude from the game](#talk-to-claude-from-the-game) |
| **Activity** | The assistant's commands and their answers, newest first |
| Switches | Pause the assistant's input (keys, clicks, typing, mouse) or its memory tools, without rebuilding; and the speed of the game, to resume a game the assistant paused |
| **Watch list** | The values watched by the assistant or the player, updated live, with a checkbox to freeze each one, a value to write, and a form to add one |
| **Pointers** | A pointer chain followed as you type it, and a pointer scan whose results go to the watch list in one click |
| **What writes** | The instruction watch on an address, with each instruction, decoded, its hit count and the bytes before it |
| **Patches** | The code the assistant patched, with a button that restores each patch, or all of them |
| **Engine** | Unreal Engine games: their classes, structs and enums with a filter, or with **Functions** and **Properties** the members of every class whose name holds it, one click opening its class with the function ready to call or the property among the live values of its first object; a class's parents, properties with offsets, enum values and functions with their signatures; its live objects; the values of an object's properties, updated live and written on Enter; a form that calls a function on the game thread; and the **Dump C++ SDK** button, with the path of the header and a button that copies it |

### Tools of your own

Your code can give the assistant tools of its own, next to the built-in ones: it knows the game, where the assistant only sees pixels and bytes.

`uih::RegisterTool()` takes a name, a description, the [JSON Schema](https://json-schema.org/) of the arguments and a function. The function receives the arguments as JSON and returns the text the assistant reads. It runs in the frame of the overlay, on the thread of the render callback, so it can use the game's objects as your menu does:

```cpp
uih::RegisterTool("teleport", "Moves the player to x, y, z in world units, and returns where the player ended up.",
                  R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]})",
                  [](const std::string& arguments) {
                      Vector3 to{std::stof(uih::ToolArgument(arguments, "x")), std::stof(uih::ToolArgument(arguments, "y")),
                                 std::stof(uih::ToolArgument(arguments, "z"))};
                      game::LocalPlayer()->SetPosition(to);    // your game's code
                      return "The player is at " + game::LocalPlayer()->Position().ToString();
                  });
```

- `uih::ToolArgument()` reads one argument without a JSON library.
- An exception thrown by the function reaches the assistant as an error.
- The example registers `example_menu`, which reads and sets the slider and the checkbox of its menu.
- The tools need `remoteControl` only. The assistant sees those registered when its session starts: in Claude Code, run `/mcp` to reconnect after registering more. `python remote/uih_mcp.py check` lists them.

### Good to know

<details>
<summary>Security</summary>

- **Any program you run can use the pipe or the socket**, so the remote control is off by default. Other machines cannot, nor, on Linux, other users. Turn it on for single-player games only: anti-cheat systems detect injected DLLs.
- With `remoteMemory`, any program you run can also read and write the game's memory through the pipe or the socket; on Linux, that bypasses Yama's ptrace restrictions for programs of your user.
- One game per pipe or socket name: a second game started with the same name logs that the remote control is unavailable.
- `unloadKey` stops the remote control along with the overlay.

</details>

<details>
<summary>Keys, clicks and focus</summary>

- **While you type to the assistant, the game does not have the focus.** Keys and clicks then reach it as window messages, or X11 events on Linux, which games that read the keyboard or mouse state directly (DirectInput, raw input) do not see, and neither does the overlay menu on Linux. With the focus, a click moves the player's cursor, except at a point another window covers, where it goes as a window message. Click the game window after sending your request, or let the assistant play on a second screen.
- While the overlay menu is open, the game receives no keys: the assistant can close it with `insert`.
- The assistant plays turn by turn, a screenshot and a few keys at a time: fine for menus, puzzles and slow games, not for reflexes.
- On Linux, keys and clicks go through the X server's XTEST extension; without it, they are sent as events to the window. Native Wayland games get them as Wayland events, handed to the listeners of their pointer and keyboard the next time they read their events: the compositor's pointer does not move, and a game that reads no events, such as one stopped by `game_speed 0`, gets them once it runs again (the command answers an error after one second).
- Typed text reaches games that read text input, as chat boxes do; games that read the keys themselves see only Enter and Tab, so use `press_key` for them. On Linux, a character missing from the keyboard layout is mapped to a free keycode while it is typed, which the X server's other applications see too.
- Screenshots of HDR back buffers, with more than 10 bits per channel, fail.

</details>

<details>
<summary>Memory and code</summary>

- A value scan also finds copies of the value: change it in the game and narrow the scan until one address is left.
- `memory_what_writes` sets debug register 3 of every thread of the game on Windows: a debugger attached to the game, or a game that uses that register, sees the change. On Linux, it asks the kernel for a hardware breakpoint on each thread (perf events), which unprivileged processes get while `kernel.perf_event_paranoid` is 2 or lower. Ubuntu sets 4, so run `sudo sysctl kernel.perf_event_paranoid=2` once per boot. Threads started later are watched within half a second.
- `memory_patch` changes code that the game's threads may be running. On Windows, they are suspended while the bytes change, and the patch waits for none of them to be inside those bytes; on Linux, they keep running, and a patch of up to 8 bytes within an aligned 8-byte word is written at once. A thread that returns into the middle of patched bytes, from a call they hold, still runs a cut instruction. Code generated at run time, by a JIT, may be generated again over a patch.
- `memory_disassemble` and `memory_signature` decode x86 and x64 code only: on ARM64 Linux, they answer that the processor is not supported, and `memory_patch` does not check where the instructions end.

</details>

<details>
<summary>Unity and Unreal Engine</summary>

- `engine_instances` lists the objects of the exact class: ask for each subclass by its name. Destroyed Unity objects are left out. An object is recognized by its header and checked against its fields, so for a class with no reference or boolean field, and not a Unity object, a freed object whose memory was not reused yet can still be listed.
- On Unreal Engine, the library finds the name table and the object array by their contents and checks every offset it uses against objects all games have. It was tested on layouts built from Epic's engine headers (5.8 and 4.22) and on a shipped Unreal Engine 4.27 game; a game whose engine was modified, or whose object array or names are encrypted, gets an error. It reads properties of every type and writes numbers, enums, bools, names, structs and object references, not strings, arrays or maps.
- Unreal functions are called through `UObject::ProcessEvent`, found as the virtual function that tests `FUNC_Native` in the function's flags, and checked by a call of `KismetMathLibrary::Add_IntInt` before the first one: when other virtual functions test the same flag, the first that returns the right sum is kept. Finding it decodes the game's x86, x64 or ARM64 code. Arguments are numbers, bools, enums, names, strings, structs and objects; delegates, arrays, maps and sets cannot be passed, and a fault in the game's code fails the call without stopping the game or the overlay, on Linux and with MSVC builds on Windows.
- `engine_call` waits for the game's main thread: on Windows, the thread of its window, which runs the call in its next message; on Linux, the thread reading its X events. A game whose main thread is stuck, such as during a loading screen, answers after 10 seconds with an error. On Unity, structs cannot be passed as arguments.
- The C++ SDK maps the properties it knows the C++ type of: numbers, bools (bitfields as `BitRef`), enums, names, strings, objects, classes, weak pointers, arrays and structs. Delegates, texts, maps, sets and the like are opaque bytes of their size. By default it holds the game's own packages and the engine types they use; `all` writes every package, several megabytes, which a compiler takes some seconds to read.
- Only one copy of the hook runs in a process: a second DLL built from this library, injected beside the first, does not start until the first is unloaded, since both would hook the same functions.

</details>

To write your own client instead of `remote/uih_mcp.py`, see the [protocol](remote/PROTOCOL.md): one text command per line on the pipe or socket.

## API

Everything is in `include/uih.h`, in the `uih` namespace.

| Function | Description |
| --- | --- |
| `Start(config)` | Installs the hooks on a worker thread and returns. Safe to call from `DllMain` or a library constructor. |
| `Stop()` | Removes the overlay, releases the GPU resources on the rendering thread and removes the hooks. Blocks; do not call it from `DllMain` or a hook. `Start()` can be called again afterwards. |
| `ActiveBackend()`, `BackendName(backend)` | The API the overlay is drawn with, `Backend::None` until the first frame. |
| `IsMenuOpen()`, `SetMenuOpen(open)` | Menu state, also toggled by `menuKey`. |
| `AIAdvancedFeaturesEnabled()`, `DrawAIAdvancedFeatures(open)` | Whether the hook runs with `remoteControl` and `aiAdvancedFeatures`; draws the [AI Advanced Features](#advanced-features-and-the-ai-advanced-features-window) window from the render callback, and clears `*open` when it is closed. |
| `CreateTexture(rgba, width, height)` | Image from RGBA pixels, uploaded by the active backend on the next frame and again after a backend switch. Any thread. Returns null once `kMaxTextures` (256) images exist. |
| `DestroyTexture(texture)` | Releases an image on the next frame. |
| `RegisterTool(name, description, parameters, call)`, `UnregisterTool(name)`, `ToolArgument(arguments, key)` | A [tool of your own](#tools-of-your-own) for the assistant, run in the frame of the overlay; false when the name (1 to 64 letters, digits, `_` or `-`) or the JSON Schema is not valid. Any thread, before or after `Start()`. |
| `CaptureFrame(pixels, width, height, timeoutMs)` | Copies the game's next frame, without the overlay, as RGBA pixels. Waits for that frame on any thread except the render callback's; returns false on timeout, or with a back buffer of more than 10 bits per channel (HDR). |

<details>
<summary>The <code>Config</code> fields</summary>

| `Config` field | Default | Description |
| --- | --- | --- |
| `render` | `nullptr` | Called every frame, between `ImGui::NewFrame()` and `ImGui::Render()` |
| `setup` | `nullptr` | Called when the ImGui context is created, and again if the game switches devices or windows |
| `backend` | `None` | Restricts the hook to one API; `None` takes the first that presents |
| `menuKey`, `unloadKey` | `INSERT`, `F11` | Virtual-key codes on Windows, X11 keysyms on Linux |
| `menuButtons` | `GamepadL3 \| GamepadR3` | Controller buttons held together to toggle the menu, `0` to disable; first XInput controller, Windows only |
| `scale` | `0` | Interface size; `0` follows the DPI of the game window |
| `module` | `nullptr` | Windows: module freed by `unloadKey`. With `nullptr`, `unloadKey` removes the overlay and the menu key or buttons bring it back, as on Linux |
| `debugLog` | `false` | `OutputDebugString` on Windows, stderr on Linux |
| `remoteControl` | `false` | Serves the [remote control](#remote-control-by-an-ai-assistant) on a named pipe for local processes (Windows), or a Unix socket for the user's processes (Linux) |
| `remotePipe` | `"universal-imgui-hook"` | Name of that pipe, `\\.\pipe\<remotePipe>`, or socket, `$XDG_RUNTIME_DIR/<remotePipe>` (`/tmp/<remotePipe>-<uid>` without that variable); copied by `Start()` |
| `remoteMemory` | `false` | With `remoteControl`: the memory tools, which read, write and scan the game's memory for the assistant |
| `aiAdvancedFeatures` | `false` | With `remoteControl`: the advanced tools and the AI Advanced Features window; with `remoteMemory` too, the advanced memory tools |

</details>

### From C and other languages

`include/uih_c.h` has the same functions for C, and for languages that call C, such as Rust, Zig or Python's `ctypes`. They are compiled into the same library, so it is still one DLL or `.so`. Draw your menu from the render callback with Dear ImGui's C bindings, such as [cimgui](https://github.com/cimgui/cimgui):

```c
#include "uih_c.h"

static void render(void) { /* your menu, with cimgui */ }

void start_overlay(void) {
    uih_config config;
    uih_config_init(&config);  // the defaults of uih::Config
    config.render = render;
    uih_start(&config);
}
```

`uih_capture_frame()` hands the pixels to a callback instead of filling a `std::vector`, and `uih_create_texture()` returns the `ImTextureData*` as a `void*`. No exception leaves these functions.

[Tools of your own](#tools-of-your-own) work the same way. The callback answers through `uih_tool_answer()`, or through `uih_tool_fail()` for an error. `uih_tool_argument()` copies a value into your buffer and returns its length, as `snprintf()` does. There is nothing to free:

```c
static void teleport(const char* arguments, uih_tool_result* result, void* context) {
    char x[32];
    uih_tool_argument(arguments, "x", x, sizeof(x));
    if (!move_player(atof(x)))
        uih_tool_fail(result, "the player cannot move there");
    else
        uih_tool_answer(result, "moved");
}

uih_register_tool("teleport", "Moves the player to x.",
                  "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"}},\"required\":[\"x\"]}",
                  teleport, NULL);
```

## How it works

```mermaid
flowchart LR
    A["The game presents a frame"] --> B["The hooked present function<br/>(Present, vkQueuePresentKHR, *SwapBuffers)"]
    B --> C["Your render callback<br/>draws the ImGui menu"]
    C --> D["The overlay draws on<br/>the game's back buffer"]
    D --> E["The real present"]
```

`Start()` hooks the present function of each graphics API (`Present`, `vkQueuePresentKHR`, `*SwapBuffers`), found through dummy objects with MinHook on Windows and by symbol interposition on Linux. In each present, the overlay runs your `render` callback and draws the ImGui data on the game's back buffer. While the menu is open, the game's input functions report nothing, and faults in the overlay are caught so they remove the overlay instead of crashing the game.

The details, from hook points and input blocking to teardown, faults, frame capture and the remote control server, are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

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

<details>
<summary><b>MinGW-w64 on Windows</b></summary>

From an [MSYS2](https://www.msys2.org/) UCRT64 shell, or with a [winlibs](https://winlibs.com/) toolchain in `PATH`:

```sh
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake --preset mingw-x64 && cmake --build --preset mingw-x64
```

</details>

<details>
<summary><b>Windows DLL from Linux or macOS</b></summary>

With the `mingw-x64` and `mingw-x86` presets and the toolchain files in `cmake/`:

| System | Packages |
| --- | --- |
| Debian, Ubuntu | `cmake ninja-build g++-mingw-w64-x86-64 g++-mingw-w64-i686` |
| Fedora | `cmake ninja-build mingw64-gcc-c++ mingw32-gcc-c++` |
| Arch | `cmake ninja mingw-w64-gcc` |
| macOS (Homebrew) | `cmake ninja mingw-w64` |

</details>

<details>
<summary><b>Linux library</b></summary>

With the `linux` preset:

| Distribution | Packages |
| --- | --- |
| Debian, Ubuntu | `cmake ninja-build g++ pkg-config libxcb1-dev` |
| Fedora | `cmake ninja-build gcc-c++ pkgconf-pkg-config libxcb-devel libstdc++-static` |
| Arch | `base-devel cmake ninja libxcb` |

At run time, it only needs `libxcb` and glibc, but a glibc at least as recent as the build machine's. To share the library or use it in the Steam Linux Runtime, build it on an old distribution or take the prebuilt one. Set `CC=clang CXX=clang++` before configuring to use Clang.

</details>

<details>
<summary><b>CMake options and minimum versions</b></summary>

Options are passed at configure time, such as `-DUIH_BUILD_TESTS=ON`:

| Option | Default | Effect |
| --- | --- | --- |
| `UIH_BUILD_EXAMPLE` | `ON` at top level | The example DLL or shared object |
| `UIH_BUILD_TESTS` | `OFF` | `uih_host` and the CTest suite |
| `UIH_STATIC_RUNTIME` | `ON` at top level | Static C and C++ runtimes |
| `UIH_SANITIZE` | `OFF` | Linux with GCC: AddressSanitizer and UndefinedBehaviorSanitizer in every target, for the tests |

**Minimum versions**: CMake 3.21, Visual Studio 2022, mingw-w64 11 with GCC 13, GCC 13 or Clang 18 on Linux; Windows 10 at run time. The mingw-w64 headers of Ubuntu 22.04 are too old for D3D12.

</details>

## Tests

`-DUIH_BUILD_TESTS=ON` builds `uih_host`, which renders a solid color with one API and checks the overlay, and registers its scenarios with CTest:

```sh
cmake --preset msvc-x64 -DUIH_BUILD_TESTS=ON && cmake --build --preset msvc-x64
ctest --test-dir build/msvc-x64 -C Release
```

> [!NOTE]
> Some tests press real keys and move the mouse. They are skipped when the host window does not have the focus, so leave the machine alone while they run.

- [tests/README.md](tests/README.md) describes every scenario, how to run the tests without a GPU or under the validation and debug layers, the Linux tests, the real-game tests and what the CI checks.
- [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) lists the games, graphics APIs and toolchains the CI tests on every commit. It also gives the cost of the overlay, measured with `uih_host --bench`: at most 0.07 ms per frame on an RTX 4070 SUPER, whatever the API, with the menu open or closed.

## Contributing

Bug reports, fixes and features are welcome:
- [CONTRIBUTING.md](CONTRIBUTING.md) gives the principles and rules of the project.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) maps where the code is and how it works.
- Report security problems privately, as [SECURITY.md](SECURITY.md) explains.

## Limitations

<details>
<summary><b>Windows</b></summary>

- `CaptureFrame()` only copies 8-bit and 10-bit back buffers: HDR output's floating-point back buffers are not captured, and the frame of an HDR10 swap chain comes with its PQ values as they are, which look washed out.
- On an HDR10 swap chain, the overlay draws its colors with the PQ curve when the game set the color space after the hook started, or when the game's window is on a display in HDR mode. A game injected late on an SDR display, whose HDR10 output Windows converts, shows the overlay with wrong colors until it sets the color space again, as when HDR is turned off and on in its settings.
- A Vulkan swap chain created before the hook has no overlay until the game recreates it. Its presents report `VK_SUBOPTIMAL_KHR`, to which games respond by recreating it.
- Native ARM64 Windows games are not supported: MinHook only handles x86 and x64.
- `game_speed` slows down the clocks the game reads through Windows. A game that times itself with the processor's counter alone (`rdtsc`), which is rare, keeps running in real time.
- MinGW-w64 builds of the DLL survive C++ exceptions in the overlay, but not access violations, since MinGW-w64 has no structured exception handling. The released DLLs are built with MSVC.

</details>

<details>
<summary><b>Linux</b></summary>

- Input is read from the X server, or, for native Wayland games (SDL with `SDL_VIDEODRIVER=wayland`, GLFW, Godot with `--display-driver wayland`, Vulkan or EGL on a `wl_surface`), from the listeners of their `wl_pointer` and `wl_keyboard`. The wheel scrolls the menu in games that read the mouse buttons from the X server, and in Wayland games. Input methods are not handled, and a pointer the game has grabbed stays grabbed while the menu is open.
- On Wayland, a pointer the game locked (`zwp_locked_pointer_v1`) stays locked while the menu is open: its relative motion moves the menu's cursor instead, which the compositor does not draw. Keys are read with the game's keymap through libxkbcommon, opened at run time, or with the US layout when it is not installed. Listeners set with `wl_proxy_add_dispatcher`, and those of a protocol version newer than the library knows, are not read, nor input that a game reads on an event queue of its own from inside its presents. The menu is drawn at a scale of 1 on Wayland.
- 32-bit games, OpenGL ES, and games that link their GL or Vulkan loader statically are not supported.
- `game_speed` needs the library preloaded, since it stands for `clock_gettime()`. A paused game that sleeps until a time it computed from its own clock may spin a core until it runs again.

</details>

The limits of the assistant's tools are in [Good to know](#good-to-know).

## Credits

This project stands on the work of the open source community, and thanks everyone behind it:

- **The libraries it is built with**: [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut and its contributors, the interface this whole project exists to draw; [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu, which places every hook on Windows; [Zydis](https://github.com/zyantific/zydis) by zyantific, the disassembler of the memory tools; the [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) of the Khronos Group; [stb_image](https://github.com/nothings/stb) by Sean Barrett, for the example's picture.
- **What the CI runs on**: [Mesa](https://www.mesa3d.org/)'s llvmpipe and lavapipe, and [mesa-dist-win](https://github.com/pal1000/mesa-dist-win) by pal1000, which let every graphics API render without a GPU; [DDNet](https://ddnet.org/) and [Godot](https://godotengine.org/), open source games that the CI plays on every commit; and [Wine](https://www.winehq.org/), on which it runs the Windows build as a Linux or Steam Deck player does.
- **The projects that explored the same problem**: [kiero](https://github.com/Rebzzel/kiero) by Rebzzel, where this project started, and [UniversalHookX](https://github.com/bruhmoment21/UniversalHookX), [hudhook](https://github.com/veeenu/hudhook) (in Rust) and [HydraHook](https://github.com/nefarius/HydraHook) (for DirectX): have a look at them too, one may fit your project better.
- **The [Model Context Protocol](https://modelcontextprotocol.io)**, an open standard that lets any assistant use the remote control.

| Contributor | What |
| --- | --- |
| [Sh0ckFR](https://github.com/Sh0ckFR) | Author and maintainer |
| [Revan600](https://github.com/Revan600) | ImGui and input hooks |
| [Rebzzel](https://github.com/Rebzzel) | The kiero library, the original base of the hooks |
| [primemb](https://github.com/primemb) | Dear ImGui and kiero updates, input hook improvements |
| [SirMestre](https://github.com/SirMestre) | Fix of the buffer count type |

Thanks as well to everyone who reported a bug, tested a game or starred the repository.

## Support

This project is free and MIT-licensed. It is, as far as we know, the only ImGui hook that covers every major graphics API on both Windows and Linux, tests itself in real games on every commit, and lets an AI assistant play the game from inside it. Rebuilding it would take roughly 125 days of senior C++ work, about €81,000 at French freelance rates: some 18,000 lines, a third of them tests.

If it saved you time, consider [sponsoring](https://github.com/sponsors/Sh0ckFR) or starring the repository. ⭐

## License

This project is licensed under the MIT License; see the [LICENSE](LICENSE) file for details.
