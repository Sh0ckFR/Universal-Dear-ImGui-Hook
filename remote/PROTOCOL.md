# Remote control protocol

This is the protocol of the [remote control](../README.md#remote-control-by-an-ai-assistant), for writing your own client instead of `remote/uih_mcp.py`.

`uih_mcp.py` speaks MCP with the assistant over standard input and output, and forwards each tool call as one line to the library. The library serves the named pipe `\\.\pipe\<remotePipe>` (Windows) or the Unix socket `$XDG_RUNTIME_DIR/<remotePipe>` (Linux) on its own thread: see `src/remote.cpp`, with the transport and the input in `src/windows/remote.cpp` and `src/linux/remote.cpp`.

## Commands

Each command is one line; each answer starts with `ok` or `error <reason>`. Some answers are followed by binary data or text lines, whose size the `ok` line gives.

| Command | Answer |
| --- | --- |
| `info` | `ok <API> <width>x<height> focused=0\|1 menu=0\|1 memory=0\|1 advanced=0\|1 speed=<speed>` |
| `screenshot <max size>` | `ok <width> <height>`, then the RGB pixels, top row first |
| `text <seconds> <message>` | `ok`; `\n` in the message starts a new line, `0` seconds removes the message |
| `key <keys> <milliseconds>` | `ok <how the keys were sent>`, once they are released |
| `click <x> <y> [left\|right\|middle] [milliseconds]` | `ok <how the click was sent>`, once the button is released; coordinates are pixels of the game window's client area |
| `modules` | `ok <bytes>`, then lines `<base> <size> <name>` |
| `read <address> <size>` | `ok <size>`, then the bytes, at most 65536 |
| `write <address> <hex bytes>` | `ok` |
| `find <type> <value>` | `ok <matches>`, or `ok <matches> kept <count>` beyond 2097152; starts a new scan |
| `next <value>\|changed\|unchanged\|increased\|decreased` | `ok <matches>`, narrowing the last scan |
| `results <count>` | `ok <bytes>`, then lines `<address> <value> <module+offset>` |
| `pattern <bytes> [module]` | `ok <bytes>`, then lines `<address> <module+offset>`, at most 1000 |
| `type <text>` | `ok <how the text was sent>`; `\n` presses Enter, `\t` Tab, `\\` types a backslash |
| `move <x> <y>` | `ok <how the cursor moved>`, after it rested there for 50 ms |
| `drag <x0> <y0> <x1> <y1> [left\|right\|middle] [milliseconds]` | `ok <how the drag was sent>`, once the button is released |
| `wait <milliseconds> [percent]` | `ok changed\|unchanged <milliseconds> <percent of the pixels that changed>` |
| `time [speed]` | `ok <speed>`: the speed of the game's clocks, set first when given, from `0` (paused) to `8`, `1` being real time |
| `time advance <milliseconds>` | `ok 0`, once the game ran in real time for 1 to 10000 milliseconds, after which it is paused |
| `pointer <address>` | `ok <bytes>`, then one line per pointer read and `address <address> <module+offset>` |
| `pointers <address> [max offset]` | `ok <bytes>`, then lines `<location> <module+offset> +<offset>`, those in a module first, at most 1000 |
| `watch add <type> <address> [label]` | `ok <id>` |
| `watch list` | `ok <bytes>`, then lines `<id> <type> <address> <resolved> <value> frozen=0\|1 <label>` |
| `watch remove <id>`, `watch clear` | `ok` |
| `freeze <id> [value]`, `unfreeze <id>` | `ok`; `freeze` keeps the current value by default |
| `writes start <address> [1\|2\|4\|8] [write\|access]`, `writes stop` | `ok` |
| `writes results` | `ok <bytes>`, then lines `<next instruction> <module+offset> <hits> <16 bytes before it>` |
| `tools` | `ok <bytes>`, then a JSON array of the tools the game registered with `uih::RegisterTool()`: `{"name", "description", "inputSchema"}` |
| `tool <name> [JSON object]` | `ok <bytes>`, then the text the tool returned; the arguments, `{}` by default, are one line of JSON |
| `chat poll` | `ok <bytes>`, then what the player did in the chat since the last poll: lines `message <text>`, `reset` (new conversation) or `cancel` (stop the answer) |
| `chat say <text>`, `chat note <text>` | `ok`: an answer from Claude, also shown at the top of the screen while the chat is hidden, or a line from the bridge |
| `chat status [text]` | `ok`: what Claude is doing, empty once it has answered |

## Details

- **Escapes.** The `chat` texts escape a new line as `\n`, a tab as `\t` and a backslash as `\\`.
- **Tools of the game.** `tools` and `tool` answer with `remoteControl` alone. The tool runs in the next frame of the overlay; when none comes within 10 seconds, or the tool takes longer, the command answers `error`. An exception thrown by the tool answers `error <its message>`.
- **Speed of the game.** `time` slows down or pauses the clocks the game's code reads (`src/timing.cpp`): `QueryPerformanceCounter`, `GetTickCount`, `GetTickCount64` and `timeGetTime` on Windows, `clock_gettime` with the monotonic clocks on Linux, where the library must be preloaded or `time` answers `error`. While the game is paused, `key`, `click`, `type`, `move`, `drag` and `wait` run it in real time until they answer, then pause it again. `Stop()` sets the game back to real time.
- **Which commands answer.** The memory commands, `modules` to `pattern` (`src/memory.cpp`), answer only with `remoteMemory`. The commands from `type` on answer only with `aiAdvancedFeatures` (`src/remote.cpp`, `src/advanced.cpp`), and those from `pointer` to `writes results` need `remoteMemory` as well. Otherwise they answer `error`.
- **Types and values.** Types are `i8`, `i16`, `i32`, `i64`, `f32` and `f64`. Floating-point values match when they round to the given value, to as many decimals as it has.
- **Addresses** are hexadecimal or `module+offset`, such as `game.exe+1a2b3c`. With `aiAdvancedFeatures`, they can also be pointer chains: `[game.exe+1a2b3c]+10` is the address 0x10 bytes after the one stored at `game.exe+1a2b3c`.
- **Screenshots** come from `uih::CaptureFrame()`. Keys and clicks go through `SendInput` on Windows and XTEST on Linux when the game has the focus, and as window messages or X11 events otherwise.
