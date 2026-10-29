"""Runs a game with tests/remote_preload.cpp preloaded and plays it as an AI
assistant does, through remote/uih_mcp.py: description of the game, screenshot,
message, INSERT to open the menu, then F11 to remove the overlay. Needs an X
server, xdotool and ImageMagick.

    python3 tests/game_mcp.py libuih_remote_preload.so OpenGL|Vulkan screenshot.png game [arguments]

The screenshot of the MCP server is saved as screenshot.png, and the screen with
the message as screenshot-text.png.
"""
import base64
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

MCP_SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "remote", "uih_mcp.py")


class Failure(Exception):
    pass


class Game:
    def __init__(self, library, command):
        self.log = tempfile.NamedTemporaryFile("w+", suffix=".log")
        environment = dict(os.environ, LD_PRELOAD=os.path.realpath(library), UIH_DEBUG="1", SDL_AUDIODRIVER="dummy")
        self.process = subprocess.Popen(command, env=environment, stdout=self.log, stderr=subprocess.STDOUT)

    def text(self):
        with open(self.log.name, errors="replace") as log:
            return log.read()

    def wait_for(self, line):
        for _ in range(120):
            if f"[uih] {line}" in self.text():
                return
            if self.process.poll() is not None:
                raise Failure(f"the game exited while waiting for '{line}'")
            time.sleep(0.5)
        raise Failure(f"timed out waiting for '{line}'")


class Assistant:
    """The MCP client side, as Claude Code drives remote/uih_mcp.py."""

    def __init__(self, pipe):
        self.process = subprocess.Popen([sys.executable, MCP_SERVER, "--pipe", pipe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.id = 0

    def send(self, message):
        self.process.stdin.write(json.dumps(message).encode("utf-8") + b"\n")
        self.process.stdin.flush()

    def request(self, method, params):
        self.id += 1
        self.send({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params})
        line = self.process.stdout.readline()
        if not line:
            raise Failure(f"the MCP server exited during {method}")
        answer = json.loads(line)
        if answer.get("id") != self.id or "result" not in answer:
            raise Failure(f"wrong answer to {method}: {answer}")
        return answer["result"]

    def call(self, tool, arguments=None, error=False):
        result = self.request("tools/call", {"name": tool, "arguments": arguments or {}})
        text = " ".join(item["text"] for item in result["content"] if item["type"] == "text")
        print(f"{tool} {json.dumps(arguments or {})}: {text}")
        if bool(result.get("isError")) != error:
            raise Failure(f"{tool} {'succeeded' if error else 'failed'}: {text}")
        return result["content"], text

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            self.process.kill()


def pixels(image):
    """Width, height and RGB rows of the PNG that remote/uih_mcp.py writes."""
    width, height = struct.unpack(">II", image[16:24])
    data, offset = b"", 8
    while offset < len(image):
        size, kind = struct.unpack(">I4s", image[offset:offset + 8])
        if kind == b"IDAT":
            data += image[offset + 8:offset + 8 + size]
        offset += 12 + size
    return width, height, zlib.decompress(data)


def play(game, assistant, backend, screenshot):
    info = assistant.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                            "clientInfo": {"name": "game_mcp.py", "version": "1"}})
    if info["serverInfo"]["name"] != "universal-imgui-hook":
        raise Failure(f"unexpected server {info['serverInfo']}")
    assistant.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    tools = sorted(tool["name"] for tool in assistant.request("tools/list", {})["tools"])
    if tools != ["game_info", "press_key", "screenshot", "show_text"]:
        raise Failure(f"unexpected tools {tools}")

    _, text = assistant.call("game_info")
    if not text.startswith(f"Graphics API: {backend}, window ") or "keyboard focus: True" not in text or "menu open: False" not in text:
        raise Failure("the game is not described as it runs, with the pointer over its window")

    content, text = assistant.call("screenshot", {"max_size": 640})
    image = base64.b64decode(next(item["data"] for item in content if item["type"] == "image"))
    with open(screenshot, "wb") as file:
        file.write(image)
    width, height, rows = pixels(image)
    if text != f"{width}x{height}" or max(width, height) != 640 or len(rows) != height * (width * 3 + 1):
        raise Failure(f"the screenshot is {width}x{height} with {len(rows)} bytes, described as {text}")
    if not any(rows[y * (width * 3 + 1) + 1:(y + 1) * (width * 3 + 1)].strip(b"\0") for y in range(height)):
        raise Failure("the screenshot is black")

    assistant.call("show_text", {"text": "Hello from the MCP server", "seconds": 60})
    time.sleep(1)
    subprocess.run(["import", "-window", "root", os.path.splitext(screenshot)[0] + "-text.png"], check=True)

    _, text = assistant.call("press_key", {"key": "insert", "duration_ms": 1000})
    if not text.startswith("Pressed insert: sent as keyboard input"):
        raise Failure("the key was not sent as keyboard input to the focused game")
    game.wait_for("Menu opened")
    _, text = assistant.call("game_info")
    if "menu open: True" not in text:
        raise Failure("the menu opened by the assistant is described as closed")

    # The unload key ends the remote control with the overlay, while the
    # assistant holds it: the answer may be lost.
    result = assistant.request("tools/call", {"name": "press_key", "arguments": {"key": "f11", "duration_ms": 1000}})
    print("press_key f11: " + " ".join(item["text"] for item in result["content"]))
    game.wait_for("Stopped")
    time.sleep(1)
    _, text = assistant.call("game_info", error=True)
    if not text.startswith("No game is under remote control"):
        raise Failure("the remote control still answers once the overlay is removed")
    if game.process.poll() is not None:
        raise Failure("the game did not survive the removal of the overlay")


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    library, backend, screenshot = sys.argv[1:4]
    game = Game(library, sys.argv[4:])
    assistant = None
    try:
        game.wait_for(f"{backend} backend active")
        game.wait_for("Remote control on")
        subprocess.run(["xdotool", "mousemove", "300", "300"], check=True)
        time.sleep(2)
        # As tests/remote_preload.cpp names the socket.
        assistant = Assistant(f"uih-host-{game.process.pid}")
        play(game, assistant, backend, screenshot)
    except Failure as failure:
        print(f"FAIL: {failure}")
        print(game.text())
        sys.exit(1)
    except Exception:
        print(game.text())
        raise
    finally:
        if assistant:
            assistant.close()
        game.process.kill()
    print("\n".join(line for line in game.text().splitlines() if "[uih]" in line))
    print(f"PASS: {backend} through the MCP server")


if __name__ == "__main__":
    main()
