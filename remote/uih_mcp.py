"""MCP server that lets an AI assistant, such as Claude Code, see and play a game.

The game runs the library with config.remoteControl set in example/config.cpp,
which serves the commands of src/remote.cpp on a local named pipe on Windows,
and on a Unix socket in $XDG_RUNTIME_DIR on Linux. This script speaks MCP on
stdin/stdout and forwards each tool call there. Python 3.8+, standard library
only.

    python uih_mcp.py [--pipe NAME]         MCP server, started by the assistant
    python uih_mcp.py inject <exe|pid> DLL  loads the DLL into a running game (Windows)

NAME is config.remotePipe, universal-imgui-hook by default.
"""
import base64
import ctypes
import json
import os
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
]

# The font of the overlay covers Latin-1: typographic signs outside it become their ASCII forms.
TYPOGRAPHY = str.maketrans({"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ", "\u202f": " "})


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
        pipe = client.makefile("rwb", buffering=0)
        client.close()  # The file keeps the connection.
        return pipe
    path = "\\\\.\\pipe\\" + PIPE_NAME
    for attempt in range(20):
        try:
            return open(path, "r+b", buffering=0)
        except FileNotFoundError:
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
        return [{"type": "text", "text": f"Graphics API: {backend}, window {size}, keyboard focus: {focused[8:] == '1'}, overlay menu open: {menu[5:] == '1'}"}]
    if name == "screenshot":
        size = int(args.get("max_size", 1280))
        text, rgb = request(f"screenshot {size}", payload=lambda answer: int(answer.split()[1]) * int(answer.split()[2]) * 3)
        width, height = (int(value) for value in text.split()[1:3])
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
    raise GameError(f"Unknown tool {name}.")


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
            reply(message_id, {"tools": TOOLS})
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


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "inject":
        if sys.platform != "win32":
            sys.exit("On Linux, start the game with LD_PRELOAD=/path/to/Universal-ImGui-Hook.so instead")
        inject(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 1:
        serve()
    elif len(sys.argv) == 3 and sys.argv[1] == "--pipe" and sys.argv[2] and not any(c in sys.argv[2] for c in "\\/"):
        PIPE_NAME = sys.argv[2]
        serve()
    else:
        sys.exit(__doc__)
