"""MCP server that lets an AI assistant, such as Claude Code, see and play a game.

The game runs the library with config.remoteControl set in example/config.cpp,
which serves the commands of src/remote.cpp on a local named pipe on Windows,
and on a Unix socket in $XDG_RUNTIME_DIR on Linux. This script speaks MCP on
stdin/stdout and forwards each tool call there. Python 3.8+, standard library
only.

    python uih_mcp.py [--pipe NAME]         MCP server, started by the assistant
    python uih_mcp.py check [--pipe NAME]   checks that a game with the library answers
    python uih_mcp.py inject <exe|pid> DLL  loads the DLL into a running game (Windows)

NAME is config.remotePipe, universal-imgui-hook by default.
"""
import base64
import ctypes
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

PIPE_NAME = "universal-imgui-hook"
PROTOCOLS = ("2025-06-18", "2025-03-26", "2024-11-05")

TOOLS = [
    {
        "name": "game_info",
        "description": "Graphics API of the game, size of its window, whether it has the keyboard focus and whether the overlay menu is open.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "screenshot",
        "description": "Captures the current frame of the game, without the overlay (so without the text shown by show_text). "
        "The game keeps running between calls.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "max_size": {"type": "integer", "description": "Largest width or height of the image, 1280 by default.", "minimum": 64, "maximum": 2560}
            },
        },
    },
    {
        "name": "show_text",
        "description": "Shows a message to the player at the top of the game screen, in place of the previous one. "
        "Use it to answer, guide or comment in the game. Plain text; \\n starts a new line.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string"},
                "seconds": {"type": "number", "description": "How long it stays, 10 by default; 0 removes the message.", "minimum": 0, "maximum": 3600},
            },
            "required": ["text"],
        },
    },
    {
        "name": "press_key",
        "description": "Holds keys of the game for a duration, then releases them, and returns once they are released. Hold a "
        "direction longer to move further; join keys with + to hold them together, such as right+space for a running jump. "
        "Keys: a letter, a digit, f1 to f12, space, enter, escape, tab, backspace, up, down, left, right, shift, ctrl, alt, "
        "insert, delete, home, end, pageup, pagedown. insert opens the overlay menu and f11 removes the overlay.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "One key, or up to 4 joined with +."},
                "duration_ms": {"type": "integer", "description": "150 by default.", "minimum": 30, "maximum": 10000},
            },
            "required": ["key"],
        },
    },
    {
        "name": "click",
        "description": "Clicks in the game at a point of the last screenshot, in its pixels, and returns once the button is released. "
        "Before any screenshot, x and y are pixels of the game window. Clicks reach the overlay menu too when the game has the focus.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer", "minimum": 0},
                "y": {"type": "integer", "minimum": 0},
                "button": {"type": "string", "enum": ["left", "right", "middle"], "description": "left by default."},
                "duration_ms": {"type": "integer", "description": "How long the button is held, 100 by default.", "minimum": 30, "maximum": 10000},
            },
            "required": ["x", "y"],
        },
    },
]

# Size of the last screenshot, to turn its pixels into pixels of the window.
last_screenshot = None

MEMORY_TYPES = {"i8": "<b", "i16": "<h", "i32": "<i", "i64": "<q", "f32": "<f", "f64": "<d"}
MEMORY_NOTE = " Needs config.remoteMemory = true in example/config.cpp."
ADDRESS = {"type": "string", "description": "Hexadecimal, such as 0x7ff6a1b2c3d4, or module+offset, such as game.exe+1a2b3c."}

TOOLS += [
    {
        "name": "memory_modules",
        "description": "Lists the modules loaded in the game (executable and libraries) with their base address and size." + MEMORY_NOTE,
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "memory_read",
        "description": "Reads memory of the game: hexadecimal dump, and the first bytes as integers, floats and a pointer." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {"address": ADDRESS, "size": {"type": "integer", "description": "64 by default.", "minimum": 1, "maximum": 4096}},
            "required": ["address"],
        },
    },
    {
        "name": "memory_write",
        "description": "Writes a value or bytes into the memory of the game, code included. To change code, memory_patch keeps the "
        "original bytes to restore." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": ADDRESS,
                "type": {"type": "string", "enum": list(MEMORY_TYPES) + ["bytes"]},
                "value": {"type": "string", "description": "A number of the type, or for bytes hexadecimal such as 90 90 90."},
            },
            "required": ["address", "type", "value"],
        },
    },
    {
        "name": "memory_find",
        "description": "Starts a value scan of the writable memory of the game, as Cheat Engine does: finds every address holding the "
        "value. Then change the value in the game (spend money, take damage) and narrow the scan with memory_next until a few "
        "addresses are left. Floats match when they round to the value given, to as many decimals as it has." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {"type": {"type": "string", "enum": list(MEMORY_TYPES)}, "value": {"type": "string"}},
            "required": ["type", "value"],
        },
    },
    {
        "name": "memory_next",
        "description": "Narrows the last memory_find to the addresses that now hold a value, or whose value changed, did not change, "
        "increased or decreased since the previous scan." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {"condition": {"type": "string", "description": "A value, or changed, unchanged, increased, decreased."}},
            "required": ["condition"],
        },
    },
    {
        "name": "memory_results",
        "description": "Addresses left by the last scan, with their current value and module+offset when they are in a module." + MEMORY_NOTE,
        "inputSchema": {"type": "object", "properties": {"count": {"type": "integer", "description": "20 by default.", "minimum": 1, "maximum": 1000}}},
    },
    {
        "name": "memory_pattern",
        "description": "Finds a byte pattern in a module, such as an instruction to patch: hexadecimal bytes separated by spaces, ?? "
        "for any byte. Up to 1000 addresses." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "Such as 48 8B 05 ?? ?? ?? ?? 89 41 10."},
                "module": {"type": "string", "description": "Module name from memory_modules; the executable by default."},
            },
            "required": ["pattern"],
        },
    },
]

ADVANCED_NOTE = " Needs config.aiAdvancedFeatures = true in example/config.cpp."
POINT = {"type": "integer", "minimum": 0}

TOOLS += [
    {
        "name": "type_text",
        "description": "Types text in the game, as a keyboard would, for chat boxes, names and text fields: any Unicode character; "
        "a new line presses Enter and a tab presses Tab. Games that read the keys directly rather than text see only Enter and Tab: "
        "use press_key for them." + ADVANCED_NOTE,
        "inputSchema": {"type": "object", "properties": {"text": {"type": "string", "description": "Up to 2000 characters."}}, "required": ["text"]},
    },
    {
        "name": "mouse_move",
        "description": "Moves the mouse cursor to a point of the last screenshot, in its pixels, without clicking: to hover a "
        "button, show a tooltip, or aim." + ADVANCED_NOTE,
        "inputSchema": {"type": "object", "properties": {"x": POINT, "y": POINT}, "required": ["x", "y"]},
    },
    {
        "name": "drag",
        "description": "Presses a mouse button at a point of the last screenshot, moves to another point while holding it, and "
        "releases it there: drag and drop items, sliders, selection boxes, camera rotation." + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": POINT,
                "y": POINT,
                "to_x": POINT,
                "to_y": POINT,
                "button": {"type": "string", "enum": ["left", "right", "middle"], "description": "left by default."},
                "duration_ms": {"type": "integer", "description": "Duration of the move, 300 by default.", "minimum": 50, "maximum": 10000},
            },
            "required": ["x", "y", "to_x", "to_y"],
        },
    },
    {
        "name": "wait_for_change",
        "description": "Waits until the game screen changes, such as after a loading screen, an animation or the answer of a menu, "
        "instead of taking screenshots in a loop. Compares each frame with the one at the start of the call." + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "timeout_ms": {"type": "integer", "description": "5000 by default.", "minimum": 100, "maximum": 30000},
                "threshold_percent": {"type": "number", "description": "Share of the pixels that must change, 1 by default; raise it "
                                      "for games with an animated background.", "minimum": 0.01, "maximum": 100},
                "screenshot": {"type": "boolean", "description": "Also returns a screenshot once the wait ends, false by default."},
            },
        },
    },
    {
        "name": "game_speed",
        "description": "Sets the speed of the game's clocks: 0 pauses the game while you think, 0.25 slows down a fast fight, 1 is real "
        "time. While the game is paused, press_key, click, type_text, mouse_move, drag and wait_for_change run it for their duration, "
        "then pause it again; advance_ms runs it for that long, then pauses it. The overlay and the screenshots keep working. Games "
        "that time themselves with the processor's counter alone, rare, do not slow down. Without arguments, says the speed." + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "speed": {"type": "number", "description": "0 pauses, 1 is real time.", "minimum": 0, "maximum": 8},
                "advance_ms": {"type": "integer", "description": "Runs the game for these milliseconds, then pauses it.", "minimum": 1,
                               "maximum": 10000},
            },
        },
    },
    {
        "name": "memory_pointer",
        "description": "Follows a pointer chain and shows each pointer read and the address it ends at. Brackets read the pointer "
        "stored at an address: [[game.exe+1a2b]+10]+8. Every memory tool takes these addresses too." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {"type": "object", "properties": {"address": ADDRESS}, "required": ["address"]},
    },
    {
        "name": "memory_pointer_scan",
        "description": "Finds the pointers to an address found with memory_find, those stored in a module first: [game.exe+offset]+delta "
        "finds the value again after the game restarts or reloads, where its own address changes. For a deeper chain, scan "
        "again for the pointers to a pointer that is not in a module." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": ADDRESS,
                "max_offset": {"type": "string", "description": "How far before the address the pointers may point, as the start of "
                               "the object holding the value, in hexadecimal: 1000 by default."},
                "count": {"type": "integer", "description": "30 by default.", "minimum": 1, "maximum": 1000},
            },
            "required": ["address"],
        },
    },
    {
        "name": "memory_watch",
        "description": "Watch list of values, shown live to the player in the AI Advanced Features window of the overlay: add a value "
        "with a type and a label, list the values with their current value, remove one, or clear the list. Freeze them with "
        "memory_freeze." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["add", "list", "remove", "clear"]},
                "address": ADDRESS,
                "type": {"type": "string", "enum": list(MEMORY_TYPES), "description": "For add, i32 by default."},
                "label": {"type": "string", "description": "For add, such as health."},
                "id": {"type": "integer", "description": "For remove: the id given by add or list."},
            },
            "required": ["action"],
        },
    },
    {
        "name": "memory_freeze",
        "description": "Keeps a value of the watch list at a value, written again every frame, as infinite health or money; or "
        "unfreezes it." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "integer", "description": "Id of the value in memory_watch."},
                "value": {"type": "string", "description": "The value to keep; its current value by default."},
                "enabled": {"type": "boolean", "description": "false unfreezes it; true by default."},
            },
            "required": ["id"],
        },
    },
    {
        "name": "memory_what_writes",
        "description": "Finds the instructions of the game that write a value, as the data breakpoint of a debugger does: start on "
        "an address, make the value change in the game, then get the results: each instruction, decoded, with its address and its "
        "hits. Read the code around it with memory_disassemble, and patch it with memory_patch to stop the change. On Linux, needs "
        "kernel.perf_event_paranoid at 2 or lower." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["start", "results", "stop"]},
                "address": ADDRESS,
                "size": {"type": "integer", "enum": [1, 2, 4, 8], "description": "Bytes watched, 4 by default; the address must be a multiple of it."},
                "access": {"type": "boolean", "description": "Also the instructions that read the value; false by default."},
            },
            "required": ["action"],
        },
    },
]

CLASS = {"type": "string", "description": "Unity: Namespace.Name, such as Game.Player, or Name alone when no other class has it; "
         "Outer/Inner for a nested class; Assembly-CSharp:Game.Player to pick the assembly. Unreal: the name without its prefix, such as "
         "PlayerCharacter_C or Pawn; /Script/Engine:Actor to pick the package."}
OBJECT = {"type": "string", "description": "Address of an object, from engine_instances or a field that refers to one."}

TOOLS += [
    {
        "name": "engine_info",
        "description": "Tells which engine runs the game, for the engine_ tools: Unity with Mono or IL2CPP, or Unreal Engine 4 or 5, "
        "and its version. The engine tools see the game's objects by their classes and fields, where memory_ tools see bytes." + MEMORY_NOTE,
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "engine_classes",
        "description": "Without arguments, lists the assemblies of the game's scripts with their number of classes; the game's own "
        "code is usually in Assembly-CSharp on Unity, and in /Script/<game> and the /Game/ packages of its blueprints on Unreal. With a "
        "filter or an assembly, lists the classes (and on Unreal the structs and enums) whose name contains the filter, case-insensitive, "
        "at most 1000." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "filter": {"type": "string", "description": "Part of the class name, such as player or inventory."},
                "assembly": {"type": "string", "description": "Only the classes of this assembly, such as Assembly-CSharp, or of this "
                             "Unreal package, such as /Script/Engine."},
            },
        },
    },
    {
        "name": "engine_class",
        "description": "Describes a class: its parents, its fields with their type and offset in the object (or static, const), "
        "inherited fields included, and its methods with their parameters. On Unreal: its properties and its functions." + MEMORY_NOTE,
        "inputSchema": {"type": "object", "properties": {"class": CLASS}, "required": ["class"]},
    },
    {
        "name": "engine_instances",
        "description": "Finds the live objects of a class, such as every Game.Player, by scanning the game's memory for their header "
        "and checking each against the layout of the class: the addresses for engine_field and engine_call. On Unity, objects of "
        "subclasses are listed under their own class, and destroyed objects are left out. On Unreal, the objects come from the engine's "
        "object array, those of subclasses included, with their class and name; default objects are left out." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {"class": CLASS, "count": {"type": "integer", "description": "100 by default.", "minimum": 1, "maximum": 1000}},
            "required": ["class"],
        },
    },
    {
        "name": "engine_field",
        "description": "Reads a field of an object, or a static field without an object, with its type: numbers, booleans, strings, "
        "enums as numbers, structs such as Vector3 with their fields, references as the address and class of the object. With a "
        "value, writes it first. Reach the fields of a struct with a dot: position.x. Unreal properties belong to objects: give one; "
        "the library writes numbers, enums (by value or name), bools, object references and structs, as a JSON object of their "
        "fields, there." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "class": CLASS,
                "field": {"type": "string", "description": "Such as health, or position.x."},
                "object": OBJECT,
                "value": {"type": ["string", "number", "boolean", "null", "object"], "description": "A number, true or false, a "
                          "string, null, the address of an object for a reference, or a JSON object for an Unreal struct."},
            },
            "required": ["class", "field"],
        },
    },
    {
        "name": "engine_call",
        "description": "Calls a method of the game's scripts, on its main thread, such as Player.Heal(50) or GameManager.LoadLevel(2), "
        "and returns its result, or the exception it threw. Risky: the method runs as the game would run it, so a wrong call can break "
        "the game's state. Arguments are numbers, booleans, strings, null or object addresses. On Unity, structs cannot be passed. On "
        "Unreal, the function runs through UObject::ProcessEvent: a struct is a JSON object of its fields, such as {\"X\": 1, \"Y\": 2, "
        "\"Z\": 3}, an enum its value or name, and the out parameters come back after the return value; a static function, such as "
        "those of KismetMathLibrary, takes no object." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "class": CLASS,
                "method": {"type": "string", "description": "Its name, such as Heal; a property getter is get_Name. Unreal names may "
                           "have spaces, such as Get Health Ratio."},
                "object": {**OBJECT, "description": "For a method that is not static: the object it is called on."},
                "arguments": {"type": "array", "description": "In the order of the parameters; the method is picked by its name and "
                              "number of parameters.", "items": {}},
            },
            "required": ["class", "method"],
        },
    },
]

TOOLS += [
    {
        "name": "engine_dump",
        "description": "Unreal Engine games: writes a C++ header of the game's classes, structs, enums and functions, an SDK for a "
        "DLL of the player's own, such as a trainer, injected into the game: it includes the header, calls uesdk::WaitForInit(), reads "
        "the properties through accessors such as player->Health() and calls the functions through wrappers, on the game thread with "
        "uesdk::GameThread::Run(). Returns the full path of the file, in the temporary directory, and what it holds. The header finds "
        "the engine again at each launch and checks it is the same build of the game; dump it again after an update." + MEMORY_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "scope": {"type": "string", "description": "game (the default): the game's own packages, /Script/<game> and the /Game/ "
                          "blueprints, with the engine types they use; all: every package; or packages separated by commas, such as "
                          "/Script/Engine,/Game."},
            },
        },
    },
    {
        "name": "memory_disassemble",
        "description": "Disassembles the code of the game at an address: each x86 or x64 instruction in Intel syntax with its address, "
        "module+offset and bytes, the exported function it starts when known, and the module+offset and symbol of the addresses it "
        "branches to or reads. before also shows the instructions that lead to the address, as for one found by memory_what_writes. "
        "Not on ARM64." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": ADDRESS,
                "count": {"type": "integer", "description": "Instructions from the address on, 20 by default.", "minimum": 1, "maximum": 200},
                "before": {"type": "integer", "description": "Instructions before the address, 0 by default.", "minimum": 0, "maximum": 50},
            },
            "required": ["address"],
        },
    },
    {
        "name": "memory_patch",
        "description": "Changes the code of the game: writes bytes over its instructions, such as 90 (nop) over an instruction that "
        "takes health away, and keeps the original bytes, which memory_unpatch puts back, as removing the overlay does. Shows the "
        "new instructions. Refuses data, the code of the overlay and its hooks, and another patch. On Windows, waits for a moment "
        "when no thread of the game runs the bytes being changed." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": ADDRESS,
                "bytes": {"type": "string", "description": "Hexadecimal, up to 256 bytes, such as 90 90 90 or b8 01 00 00 00 c3."},
            },
            "required": ["address", "bytes"],
        },
    },
    {
        "name": "memory_unpatch",
        "description": "Puts back the original bytes of a patch of memory_patch, or of all of them. Without arguments, lists the "
        "patches." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "integer", "description": "The patch to restore, given by memory_patch."},
                "all": {"type": "boolean", "description": "Restores every patch."},
            },
        },
    },
    {
        "name": "memory_signature",
        "description": "Makes a signature of the code at an address: a byte pattern found once in its module, with ?? over the "
        "offsets and addresses that change when the game is rebuilt or loaded elsewhere. memory_pattern finds the code again after "
        "a restart or an update of the game, where its module+offset changes. x86 and x64." + MEMORY_NOTE + ADVANCED_NOTE,
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": ADDRESS,
                "length": {"type": "integer", "description": "Bytes the pattern covers at least; it grows until it is unique.",
                           "minimum": 1, "maximum": 128},
            },
            "required": ["address"],
        },
    },
]

# The font of the overlay covers Latin-1: typographic signs outside it become their ASCII forms.
TYPOGRAPHY = str.maketrans({"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ", "\u202f": " "})


BUILTIN_TOOLS = {tool["name"] for tool in TOOLS}
# As uih::RegisterTool() accepts them.
TOOL_NAME = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


class GameError(Exception):
    pass


def connect():
    """Opens the pipe or the socket the game serves, as a binary file."""
    if sys.platform != "win32":
        # As src/linux/remote.cpp names it.
        runtime = os.environ.get("XDG_RUNTIME_DIR")
        path = os.path.join(runtime, PIPE_NAME) if runtime else f"/tmp/{PIPE_NAME}-{os.getuid()}"
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.connect(path)
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            raise GameError(f"No game is under remote control on {path}: start the game with LD_PRELOAD set to the library "
                            "built with config.remoteControl = true in example/config.cpp. The socket opens with the first "
                            "frame of the overlay. The unload key stops the remote control: press the menu key in the game "
                            "to bring it back.")
        except OSError as error:
            client.close()
            raise GameError(f"Cannot connect to {path}: {error.strerror or error}. The game must run as the same user.")
        pipe = client.makefile("rwb", buffering=0)
        client.close()  # The file keeps the connection.
        return pipe
    path = "\\\\.\\pipe\\" + PIPE_NAME
    for attempt in range(20):
        try:
            return open(path, "r+b", buffering=0)
        except FileNotFoundError:
            # Between two clients, the game creates the pipe again.
            if attempt < 10:
                time.sleep(0.1)
                continue
            raise GameError(f"No game is under remote control on {path}: start the game, then load the DLL built with "
                            "config.remoteControl = true in example/config.cpp (python uih_mcp.py inject <game.exe> <dll>). "
                            "The unload key stops the remote control: inject the DLL again, or press the menu key in the game "
                            "when the DLL stays loaded, as dinput8.dll.")
        except OSError:
            # The previous client is still disconnecting.
            time.sleep(0.1)
    raise GameError("The game does not accept the connection.")


def request(line, payload=None):
    """Sends one command to the game; returns its answer, and the bytes that follow when `payload` gives their count."""
    with connect() as pipe:
        pipe.write(line.encode("utf-8") + b"\n")
        answer = bytearray()
        while not answer.endswith(b"\n"):
            byte = pipe.read(1)
            if not byte:
                raise GameError("The game closed the connection.")
            answer += byte
        text = answer.decode("utf-8").strip()
        if text.startswith("error "):
            raise GameError(text[6:])
        data = b""
        if payload:
            size = payload(text)
            while len(data) < size:
                chunk = pipe.read(size - len(data))
                if not chunk:
                    raise GameError("The game closed the connection during the screenshot.")
                data += chunk
        return text, data


def png(width, height, rgb):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    stride = width * 3
    rows = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def call_tool(name, args):
    if name == "game_info":
        text, _ = request("info")
        backend, size, focused, menu = text.split()[1:5]
        memory = "memory=1" in text.split()
        advanced = "advanced=1" in text.split()
        speed = next((float(word[6:]) for word in text.split() if word.startswith("speed=")), 1.0)
        paced = "" if speed == 1 else ", game paused" if speed == 0 else f", game speed: {speed:g}"
        return [{"type": "text", "text": f"Graphics API: {backend}, window {size}, keyboard focus: {focused[8:] == '1'}, overlay menu open: "
                 f"{menu[5:] == '1'}, memory tools: {'on' if memory else 'off'}, advanced tools: {'on' if advanced else 'off'}{paced}"}]
    if name == "screenshot":
        size = int(args.get("max_size", 1280))
        text, rgb = request(f"screenshot {size}", payload=lambda answer: int(answer.split()[1]) * int(answer.split()[2]) * 3)
        width, height = (int(value) for value in text.split()[1:3])
        global last_screenshot
        last_screenshot = (width, height)
        image = base64.b64encode(png(width, height, rgb)).decode("ascii")
        return [{"type": "image", "data": image, "mimeType": "image/png"}, {"type": "text", "text": f"{width}x{height}"}]
    if name == "show_text":
        message = str(args.get("text", "")).translate(TYPOGRAPHY).replace("\r", "").replace("\n", "\\n")
        message = message.encode("latin-1", "replace").decode("latin-1")
        request(f"text {float(args.get('seconds', 10))} {message}")
        return [{"type": "text", "text": "Shown in the game."}]
    if name == "press_key":
        key = str(args.get("key", "")).strip()
        if not key or " " in key:
            raise GameError("Give keys without spaces, such as space or right+space.")
        text, _ = request(f"key {key} {int(args.get('duration_ms', 150))}")
        return [{"type": "text", "text": f"Pressed {key}: {text[3:]}."}]
    if name == "click":
        (x, y), = window_points([(args.get("x", 0), args.get("y", 0))])
        button = str(args.get("button", "left"))
        text, _ = request(f"click {x} {y} {button} {int(args.get('duration_ms', 100))}")
        return [{"type": "text", "text": f"Clicked {button} at {x},{y} in the window: {text[3:]}."}]
    if name == "type_text":
        text = str(args.get("text", ""))
        if not text:
            raise GameError("Give the text to type.")
        escaped = text.replace("\\", "\\\\").replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\\n").replace("\t", "\\t")
        answer, _ = request(f"type {escaped}")
        return [{"type": "text", "text": answer[3:4].upper() + answer[4:] + "."}]
    if name == "mouse_move":
        (x, y), = window_points([(args.get("x", 0), args.get("y", 0))])
        answer, _ = request(f"move {x} {y}")
        return [{"type": "text", "text": f"Moved to {x},{y} in the window: {answer[3:]}."}]
    if name == "drag":
        (x, y), (to_x, to_y) = window_points([(args.get("x", 0), args.get("y", 0)), (args.get("to_x", 0), args.get("to_y", 0))])
        button = str(args.get("button", "left"))
        answer, _ = request(f"drag {x} {y} {to_x} {to_y} {button} {int(args.get('duration_ms', 300))}")
        return [{"type": "text", "text": f"Dragged {button} from {x},{y} to {to_x},{to_y} in the window: {answer[3:]}."}]
    if name == "wait_for_change":
        timeout = int(args.get("timeout_ms", 5000))
        answer, _ = request(f"wait {timeout} {float(args.get('threshold_percent', 1))}")
        words = answer.split()
        if words[1] == "changed":
            message = f"The screen changed after {words[2]} ms ({words[3]}% of the pixels)."
        else:
            message = f"No change within {words[2]} ms: at most {words[3]}% of the pixels changed."
        content = [{"type": "text", "text": message}]
        if args.get("screenshot"):
            content += call_tool("screenshot", {})
        return content
    if name == "game_speed":
        if "advance_ms" in args:
            request(f"time advance {int(args['advance_ms'])}")
            return [{"type": "text", "text": f"The game ran for {int(args['advance_ms'])} ms and is paused."}]
        answer, _ = request(f"time {float(args['speed'])}" if "speed" in args else "time")
        speed = float(answer.split()[1])
        state = "paused" if speed == 0 else "in real time" if speed == 1 else f"at {speed:g} times real time"
        return [{"type": "text", "text": f"The game runs {state}."}]
    if name.startswith("memory_"):
        return [{"type": "text", "text": memory_tool(name, args)}]
    if name.startswith("engine_"):
        return [{"type": "text", "text": engine_tool(name, args)}]
    if name not in BUILTIN_TOOLS and TOOL_NAME.match(name or ""):
        # A tool of the game, uih::RegisterTool(): the arguments go as JSON on one line.
        _, data = request(f"tool {name} {json.dumps(args, ensure_ascii=False)}", payload=text_payload)
        return [{"type": "text", "text": data.decode("utf-8", "replace")}]
    raise GameError(f"Unknown tool {name}.")


def game_tools():
    """The tools the game registered with uih::RegisterTool(); none when no game answers."""
    try:
        _, data = request("tools", payload=text_payload)
        tools = json.loads(data.decode("utf-8"))
    except (GameError, ValueError):
        return []
    return [tool for tool in tools if isinstance(tool, dict) and TOOL_NAME.match(str(tool.get("name", "")))
            and tool["name"] not in BUILTIN_TOOLS and isinstance(tool.get("inputSchema"), dict)]


def text_payload(answer):
    return int(answer.split()[1])


def window_points(points):
    """Points of the last screenshot, in pixels of the game window."""
    points = [(int(x), int(y)) for x, y in points]
    if not last_screenshot:
        return points
    info, _ = request("info")
    width, height = (int(value) for value in info.split()[2].split("x"))
    return [(min(width - 1, x * width // last_screenshot[0]), min(height - 1, y * height // last_screenshot[1])) for x, y in points]


def advanced_memory_tool(name, args):
    if name == "memory_pointer":
        _, data = request(f"pointer {str(args.get('address', '')).strip()}", payload=text_payload)
        return data.decode("utf-8", "replace").strip()
    if name == "memory_pointer_scan":
        address = str(args.get("address", "")).strip()
        max_offset = str(args.get("max_offset", "1000")).strip().lower()
        max_offset = max_offset[2:] if max_offset.startswith("0x") else max_offset
        _, data = request(f"pointers {address} {max_offset or '1000'}", payload=text_payload)
        lines = []
        for row in data.decode("utf-8", "replace").splitlines()[:max(1, min(int(args.get("count", 30)), 1000))]:
            location, where, delta = row.split()
            delta = "" if delta == "+0x0" else delta
            lines.append(f"[{where}]{delta}  (stored at {location}, in a module: kept after a restart)" if where != "-" else
                         f"[{location}]{delta}  (stored on the heap or a stack: moves after a restart)")
        return "\n".join(lines) if lines else "No pointer to this address."
    if name == "memory_watch":
        action = str(args.get("action", ""))
        if action == "add":
            label = " ".join(str(args.get("label", "")).split())
            answer, _ = request(f"watch add {args.get('type', 'i32')} {str(args.get('address', '')).strip()} {label}".rstrip())
            return f"Added as value {answer.split()[1]}; the player sees it in the AI Advanced Features window."
        if action == "remove":
            request(f"watch remove {int(args.get('id', 0))}")
            return "Removed."
        if action == "clear":
            request("watch clear")
            return "The watch list is empty."
        _, data = request("watch list", payload=text_payload)
        lines = []
        for row in data.decode("utf-8", "replace").splitlines():
            parts = row.split(" ", 6)
            label = f" {parts[6]}" if len(parts) > 6 else ""
            lines.append(f"{parts[0]}:{label} {parts[1]} at {parts[2]} ({parts[3]}) = {parts[4]}" + (" (frozen)" if parts[5] == "frozen=1" else ""))
        return "\n".join(lines) if lines else "The watch list is empty."
    if name == "memory_freeze":
        value_id = int(args.get("id", 0))
        if args.get("enabled", True) is False:
            request(f"unfreeze {value_id}")
            return f"Value {value_id} is no longer frozen."
        value = str(args.get("value", "")).strip()
        request(f"freeze {value_id} {value}".rstrip())
        return f"Value {value_id} is frozen" + (f" at {value}." if value else " at its current value.")
    action = str(args.get("action", ""))
    if action == "start":
        address = str(args.get("address", "")).strip()
        request(f"writes start {address} {int(args.get('size', 4))} {'access' if args.get('access') else 'write'}")
        return f"Watching {address}. Make the value change in the game, then call memory_what_writes with action results."
    if action == "stop":
        request("writes stop")
        return "Stopped watching."
    _, data = request("writes results", payload=text_payload)
    lines = []
    for row in data.decode("utf-8", "replace").splitlines():
        row, _, instruction = row.partition(" ; ")
        parts = row.split(" ", 3)
        if instruction:
            address, _, text = instruction.partition(" ")
            lines.append(f"{text} at {address}, ending before {parts[1]} ({parts[0]}): {parts[2]} hits")
        else:
            lines.append(f"Instruction ending before {parts[1]} ({parts[0]}): {parts[2]} hits. Bytes before: {parts[3] if len(parts) > 3 else '?'}")
    if not lines:
        return "No instruction hit yet: make the value change in the game."
    return "\n".join(lines) + "\nmemory_disassemble with before=10 shows the code that leads to an instruction."


def disassembly(text, mark=None):
    """The lines of the disasm command, laid out for reading; `mark` points at one address."""
    lines = []
    for row in text.splitlines():
        parts = row.split(" ", 4)
        if len(parts) < 5:
            continue
        address, where, symbol, code, instruction = parts
        if symbol != "-" and "+" not in symbol:
            lines.append(f"{symbol}:")
        code = " ".join(code[i:i + 2] for i in range(0, len(code), 2))
        prefix = "> " if mark is not None and int(address, 16) == mark else "  "
        lines.append(f"{prefix}{address}  {where:<28} {code:<30} {instruction}")
    return "\n".join(lines)


def code_tool(name, args):
    address = str(args.get("address", "")).strip()
    if name == "memory_disassemble":
        count = max(1, min(int(args.get("count", 20)), 200))
        before = max(0, min(int(args.get("before", 0)), 50))
        answer, data = request(f"disasm {address} {count} {before}", payload=text_payload)
        # The address given, resolved, follows the size of the text.
        mark = int(answer.split()[2], 16) if before and len(answer.split()) > 2 else None
        text = data.decode("utf-8", "replace")
        return disassembly(text, mark) or "No instruction."
    if name == "memory_patch":
        data = bytes.fromhex(str(args.get("bytes", "")).replace(" ", ""))
        if not data:
            raise GameError("Give the new bytes in hexadecimal, such as 90 90.")
        answer, _ = request(f"patch {address} {data.hex(' ')}")
        words = answer.split()
        message = f"Patch {words[1]}: {len(data)} bytes written at {address}; memory_unpatch with id {words[1]} puts the original back."
        if len(words) >= 5 and words[2] == "cuts":
            message += (f" Warning: the patch ends inside the instruction at {words[3]}, whose last bytes, up to {words[4]}, are "
                        "left and run as other instructions: pad the patch with 90 (nop) up to there, unless that is the intent.")
        try:
            _, listing = request(f"disasm {address} {max(2, min(len(data), 20))}", payload=text_payload)
            message += "\nThe code there now:\n" + disassembly(listing.decode("utf-8", "replace"))
        except GameError:
            pass  # Not on ARM64.
        return message
    if name == "memory_unpatch":
        if args.get("all"):
            answer, _ = request("unpatch all")
            return f"Restored the original bytes of {answer.split()[1]} patches."
        if "id" in args:
            answer, _ = request(f"unpatch {int(args['id'])}")
            if answer.endswith("changed"):
                return f"The game changed the bytes of patch {int(args['id'])} since: they were left as they are, and the patch forgotten."
            return f"Patch {int(args['id'])} restored: the original bytes are back."
        _, data = request("patches", payload=text_payload)
        lines = []
        for row in data.decode("utf-8", "replace").splitlines():
            patch_id, at, where, original, patched = row.split(" ")
            lines.append(f"{patch_id}: {at} ({where}), {original} replaced with {patched}")
        return "\n".join(lines) if lines else "No patch."
    length = args.get("length")
    _, data = request(f"signature {address} {int(length)}" if length else f"signature {address}", payload=text_payload)
    module, _, pattern = data.decode("utf-8", "replace").strip().partition(" ")
    return (f"{pattern}\nFound once in {module}, at {address}. After an update or a restart, memory_pattern with this pattern and "
            f"module {module} finds the code again.")


def memory_tool(name, args):
    if name in ("memory_pointer", "memory_pointer_scan", "memory_watch", "memory_freeze", "memory_what_writes"):
        return advanced_memory_tool(name, args)
    if name in ("memory_disassemble", "memory_patch", "memory_unpatch", "memory_signature"):
        return code_tool(name, args)
    if name == "memory_modules":
        _, data = request("modules", payload=text_payload)
        return data.decode("utf-8", "replace") or "No module."
    if name == "memory_read":
        address = str(args.get("address", "")).strip()
        size = max(1, min(int(args.get("size", 64)), 4096))
        _, data = request(f"read {address} {size}", payload=text_payload)
        lines = []
        for offset in range(0, len(data), 16):
            row = data[offset:offset + 16]
            ascii_text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
            lines.append(f"+{offset:04x}  {row.hex(' '):<47}  {ascii_text}")
        views = [f"{kind} {struct.unpack(code, data[:struct.calcsize(code)])[0]:g}" if kind[0] == "f" else
                 f"{kind} {struct.unpack(code, data[:struct.calcsize(code)])[0]}"
                 for kind, code in MEMORY_TYPES.items() if len(data) >= struct.calcsize(code)]
        if len(data) >= 8:
            views.append(f"pointer 0x{struct.unpack('<Q', data[:8])[0]:x}")
        return "\n".join(lines) + "\nAt the address: " + ", ".join(views)
    if name == "memory_write":
        address = str(args.get("address", "")).strip()
        kind = str(args.get("type", ""))
        value = str(args.get("value", "")).strip()
        if kind == "bytes":
            data = bytes.fromhex(value)
        elif kind in MEMORY_TYPES:
            number = float(value) if kind[0] == "f" else int(value, 0)
            code = MEMORY_TYPES[kind]
            if kind[0] == "i" and number >= 1 << (struct.calcsize(code) * 8 - 1):
                code = code.upper()  # Unsigned.
            data = struct.pack(code, number)
        else:
            raise GameError("The type is i8, i16, i32, i64, f32, f64 or bytes.")
        if not data:
            raise GameError("Give at least one byte.")
        request(f"write {address} {data.hex(' ')}")
        return f"Wrote {len(data)} bytes at {address}."
    if name == "memory_find":
        text, _ = request(f"find {args.get('type', '')} {str(args.get('value', '')).strip()}")
        words = text.split()
        if len(words) >= 4:
            return f"{words[1]} matches; the first {words[3]} are kept. Narrow them with memory_next."
        return f"{words[1]} matches." + (" Narrow them with memory_next, or list them with memory_results." if words[1] != "0" else "")
    if name == "memory_next":
        text, _ = request(f"next {str(args.get('condition', '')).strip()}")
        return f"{text.split()[1]} matches left."
    if name == "memory_results":
        _, data = request(f"results {int(args.get('count', 20))}", payload=text_payload)
        return data.decode("utf-8", "replace") or "No match."
    pattern = " ".join(str(args.get("pattern", "")).split())
    module = str(args.get("module", "")).strip()
    _, data = request(f"pattern {pattern} {module}".strip(), payload=text_payload)
    return data.decode("utf-8", "replace") or "Not found."


def engine_word(value, what):
    """One word of an engine command: a name with spaces, such as an Unreal blueprint's, goes between double quotes."""
    word = str(value or "").strip()
    if not word:
        raise GameError(f"Give the {what}.")
    if any(c.isspace() for c in word):
        if '"' in word or "\n" in word:
            raise GameError(f"The {what} cannot hold double quotes or line breaks.")
        return f'"{word}"'
    return word


def engine_value(value):
    """A value for engine set: a string the game should store goes quoted; numbers, booleans, null and addresses as they are."""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False)
    raw = str(value).strip()
    if raw.lower() in ("true", "false", "null") or re.fullmatch(r"[-+]?(0x[0-9a-fA-F]+|[0-9]*\.?[0-9]+([eE][-+]?[0-9]+)?)", raw):
        return raw
    return json.dumps(str(value), ensure_ascii=False)


def engine_tool(name, args):
    if name == "engine_info":
        answer, _ = request("engine info")
        words = answer.split()
        if words[1] == "none":
            return ("The game runs no engine whose objects the library knows: the engine tools see Unity games, with Mono or IL2CPP, "
                    "and Unreal Engine 4 and 5 games.")
        if words[1] == "unreal":
            version = words[3] if words[3] != "-" else words[2][2:]
            return (f"Unreal Engine {version} ({words[4]}). The engine's classes are in packages such as /Script/Engine, the game's own "
                    "in /Script/<game> and, for its blueprints, under /Game/: list them with engine_classes.")
        version = f" {words[3]}" if words[3] != "-" else ""
        return (f"{words[1].capitalize()}{version} with {'IL2CPP' if words[2] == 'il2cpp' else 'Mono'} ({words[4]}). "
                "The game's scripts are usually in the assembly Assembly-CSharp: list its classes with engine_classes.")
    if name == "engine_dump":
        scope = str(args.get("scope", "") or "").strip()
        _, data = request("engine dump" + (f" {engine_word(scope, 'scope')}" if scope else ""), payload=text_payload)
        lines = data.decode("utf-8", "replace").splitlines()
        if not lines:
            raise GameError("The library wrote no SDK.")
        return (f"The SDK is in {lines[0]}\n" + "\n".join(lines[1:]) +
                "\nInclude it in the DLL's code (C++17), call uesdk::WaitForInit() on a thread of the DLL, then call the game's "
                "functions inside uesdk::GameThread::Run(), and uesdk::Shutdown() before the DLL unloads.")
    if name == "engine_classes":
        filter_text = str(args.get("filter", "") or "").strip()
        assembly = str(args.get("assembly", "") or "").strip()
        if not filter_text and not assembly:
            _, data = request("engine assemblies", payload=text_payload)
            return "Assemblies and their number of classes:\n" + data.decode("utf-8", "replace")
        command = f"engine classes {engine_word(filter_text or '*', 'filter')}"
        if assembly:
            command += f" {engine_word(assembly, 'assembly')}"
        _, data = request(command, payload=text_payload)
        return data.decode("utf-8", "replace") or "No class matches."
    class_name = engine_word(args.get("class"), "class")
    if name == "engine_class":
        _, data = request(f"engine class {class_name}", payload=text_payload)
        return data.decode("utf-8", "replace")
    if name == "engine_instances":
        count = max(1, min(int(args.get("count", 100)), 1000))
        _, data = request(f"engine instances {class_name} {count}", payload=text_payload)
        return data.decode("utf-8", "replace") or f"No live object of {class_name}."
    obj = str(args.get("object", "") or "").strip()
    target = engine_word(obj, "object address") if obj else "-"
    if name == "engine_field":
        field = engine_word(args.get("field"), "field")
        if "value" in args:
            _, data = request(f"engine set {class_name} {field} {target} {engine_value(args['value'])}", payload=text_payload)
            return "Written; it reads back: " + data.decode("utf-8", "replace")
        _, data = request(f"engine get {class_name} {field}" + (f" {target}" if obj else ""), payload=text_payload)
        return data.decode("utf-8", "replace")
    if name != "engine_call":
        raise GameError(f"Unknown tool {name}.")
    method = engine_word(args.get("method"), "method")
    arguments = args.get("arguments") or []
    if not isinstance(arguments, list):
        raise GameError("Give the arguments as an array.")
    _, data = request(f"engine call {class_name} {method} {target} {json.dumps(arguments, ensure_ascii=False)}", payload=text_payload)
    return "Returned " + data.decode("utf-8", "replace")


def reply(message_id, result=None, error=None):
    message = {"jsonrpc": "2.0", "id": message_id}
    if error:
        message["error"] = error
    else:
        message["result"] = result
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def serve():
    sys.stdout.reconfigure(encoding="utf-8")
    for line in sys.stdin.buffer:
        try:
            message = json.loads(line)
        except ValueError:
            continue
        method = message.get("method")
        message_id = message.get("id")
        if message_id is None:
            continue  # Notifications, such as notifications/initialized.
        params = message.get("params") or {}
        if method == "initialize":
            version = params.get("protocolVersion")
            reply(message_id, {
                "protocolVersion": version if version in PROTOCOLS else PROTOCOLS[0],
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "universal-imgui-hook", "version": "1.0.0"},
                "instructions": "Tools to see and play the game the player is running. Take a screenshot before acting, "
                                "and again after acting to check the result: the game keeps running between calls.",
            })
        elif method == "ping":
            reply(message_id, {})
        elif method == "tools/list":
            reply(message_id, {"tools": TOOLS + game_tools()})
        elif method == "tools/call":
            try:
                content = call_tool(params.get("name"), params.get("arguments") or {})
                reply(message_id, {"content": content})
            except GameError as error:
                reply(message_id, {"content": [{"type": "text", "text": str(error)}], "isError": True})
            except Exception as error:  # A bug here must not end the session.
                reply(message_id, {"content": [{"type": "text", "text": f"Internal error: {error!r}"}], "isError": True})
        else:
            reply(message_id, error={"code": -32601, "message": f"Unknown method {method}"})


def inject(target, dll):
    from ctypes import wintypes

    if target.isdigit():
        pid = int(target)
    else:
        rows = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {target}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
        pids = [int(row.split('","')[1]) for row in rows.splitlines() if row.startswith('"')]
        if not pids:
            sys.exit(f"{target} is not running")
        pid = pids[0]
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.VirtualAllocEx.restype = ctypes.c_void_p
    kernel32.VirtualAllocEx.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD)
    kernel32.WriteProcessMemory.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE
    kernel32.GetProcAddress.restype = ctypes.c_void_p
    kernel32.GetProcAddress.argtypes = (wintypes.HMODULE, ctypes.c_char_p)
    kernel32.CreateRemoteThread.restype = wintypes.HANDLE
    kernel32.CreateRemoteThread.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p)
    kernel32.GetExitCodeThread.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)

    import os
    path = os.path.abspath(dll)
    if not os.path.isfile(path):
        sys.exit(f"{path} does not exist")
    process = kernel32.OpenProcess(0x1FFFFF, False, pid)
    if not process:
        sys.exit(f"Cannot open process {pid} (error {ctypes.get_last_error()})")
    data = ctypes.create_unicode_buffer(path)
    size = ctypes.sizeof(data)
    remote = kernel32.VirtualAllocEx(process, None, size, 0x3000, 0x04)
    kernel32.WriteProcessMemory(process, remote, data, size, None)
    load_library = kernel32.GetProcAddress(kernel32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    thread = kernel32.CreateRemoteThread(process, None, 0, load_library, remote, 0, None)
    if not thread:
        sys.exit(f"Cannot start the loading thread (error {ctypes.get_last_error()})")
    kernel32.WaitForSingleObject(thread, 10000)
    code = wintypes.DWORD()
    kernel32.GetExitCodeThread(thread, ctypes.byref(code))
    kernel32.CloseHandle(thread)
    kernel32.CloseHandle(process)
    if not code.value:
        sys.exit("The game did not load the DLL (another architecture, or a missing dependency)")
    print(f"Loaded {path} into process {pid}")


def check():
    try:
        text = call_tool("game_info", {})[0]["text"]
    except GameError as error:
        sys.exit(f"No answer on {PIPE_NAME}: {error}")
    names = ", ".join(tool["name"] for tool in game_tools())
    print(f"The game answers on {PIPE_NAME}. {text}, tools of the game: {names or 'none'}.")


def valid_pipe(name):
    return bool(name) and not any(c in name for c in "\\/")


if __name__ == "__main__":
    if len(sys.argv) in (2, 4) and sys.argv[1] == "check" and (len(sys.argv) == 2 or (sys.argv[2] == "--pipe" and valid_pipe(sys.argv[3]))):
        if len(sys.argv) == 4:
            PIPE_NAME = sys.argv[3]
        check()
    elif len(sys.argv) == 4 and sys.argv[1] == "inject":
        if sys.platform != "win32":
            sys.exit("On Linux, start the game with LD_PRELOAD=/path/to/Universal-ImGui-Hook.so instead")
        inject(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 1:
        serve()
    elif len(sys.argv) == 3 and sys.argv[1] == "--pipe" and valid_pipe(sys.argv[2]):
        PIPE_NAME = sys.argv[2]
        serve()
    else:
        sys.exit(__doc__)
