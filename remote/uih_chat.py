#!/usr/bin/env python3
"""Lets the player talk to Claude from the Chat tab of the AI Advanced Features
window of the overlay. Each message of the player runs Claude Code headless
(claude -p) with the tools of uih_mcp.py, in one conversation, and what
Claude writes goes back to the chat as it writes it: Claude looks at the game,
plays and changes it while the player watches.

Needs Claude Code (claude on the PATH, signed in) and a game running the
library built with config.remoteControl and config.aiAdvancedFeatures; with
config.remoteMemory too, Claude also gets the memory tools. Start it next to
the game and leave it running:

    python uih_chat.py [--pipe NAME] [--model MODEL] [--claude PATH]

In the window, New conversation makes Claude forget what was said, and Stop
ends the answer in progress. Claude only gets the tools of the game, no
shell or files.
"""
import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uih_mcp  # noqa: E402

POLL_SECONDS = 0.25
SERVER = "uih-game"
TOOL_PREFIX = f"mcp__{SERVER}__"
SYSTEM_PROMPT = """You are Claude, inside a video game the player is playing, through the tools of the uih-game server.
The player talks to you from a chat window drawn over the game, and reads there everything you write, as you write it.
- Answer in the language of the player, briefly, in plain text: no Markdown tables or headings, the chat shows raw text.
- Before acting, look at the game with screenshot; after acting, check the result with another screenshot.
- Say in one short line what you are about to do before a long task, then report what happened.
- The overlay menu is open while the player chats with you, and the game then sees no keys or clicks: close it with
  press_key insert before playing, and tell the player you did. The player opens it again with Insert.
- You only have the tools of the game: no shell, no files."""


class Bridge:
    def __init__(self, args):
        self.args = args
        self.claude = args.claude or shutil.which("claude")
        if not self.claude:
            sys.exit("Claude Code is not on the PATH: install it from https://claude.com/claude-code and sign in, or give --claude")
        # Out of any project: no CLAUDE.md, and the conversations of the chat stay together.
        self.work = os.path.join(tempfile.gettempdir(), "uih-chat")
        os.makedirs(self.work, exist_ok=True)
        self.config = os.path.join(self.work, "mcp.json")
        server = {"type": "stdio", "command": sys.executable, "args": [os.path.join(os.path.dirname(os.path.abspath(__file__)), "uih_mcp.py"),
                                                                       "--pipe", args.pipe]}
        with open(self.config, "w", encoding="utf-8") as file:
            json.dump({"mcpServers": {SERVER: server}}, file)
        self.prompt_file = os.path.join(self.work, "system.txt")
        with open(self.prompt_file, "w", encoding="utf-8") as file:
            file.write(SYSTEM_PROMPT)
        self.session = None
        self.pending = []

    # The game.

    def request(self, line, payload=False):
        """The answer of the game, or None when it does not answer now."""
        try:
            return uih_mcp.request(line, payload=uih_mcp.text_payload if payload else None)
        except uih_mcp.GameError as error:
            if "advanced features are off" in str(error):
                sys.exit(f"The game refuses the chat: {error}")
            return None
        except OSError:
            return None

    def poll(self):
        """What the player did in the chat since the last poll, or None without the game."""
        answer = self.request("chat poll", payload=True)
        if answer is None:
            return None
        return answer[1].decode("utf-8", "replace").splitlines()

    def chat(self, action, text=""):
        text = text.replace("\\", "\\\\").replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\\n").replace("\t", "\\t")
        self.request(f"chat {action} {text}".rstrip())

    def take(self, lines):
        """Keeps the messages; True when the player asked to stop or to start over."""
        stop = False
        for line in lines:
            if line.startswith("message "):
                self.pending.append(unescape(line[8:]))
            elif line == "reset":
                self.session = None
                self.pending.clear()
                stop = True
                print("New conversation.")
            elif line == "cancel":
                stop = True
        return stop

    # Claude Code.

    def run(self, prompt):
        command = [self.claude, "-p", "--output-format", "stream-json", "--verbose", "--mcp-config", self.config, "--strict-mcp-config",
                   "--tools", "", "--allowedTools", f"mcp__{SERVER}", "--append-system-prompt-file", self.prompt_file]
        if self.session:
            command += ["--resume", self.session]
        if self.args.model:
            command += ["--model", self.args.model]
        print(f"Player: {prompt}")
        self.chat("status", "thinking")
        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        process = subprocess.Popen(command, cwd=self.work, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", errors="replace", creationflags=flags)
        process.stdin.write(prompt)
        process.stdin.close()
        events = queue.Queue()
        errors = []
        threading.Thread(target=lambda: [events.put(line) for line in process.stdout] + [events.put(None)], daemon=True).start()
        threading.Thread(target=lambda: errors.extend(process.stderr), daemon=True).start()

        said = False
        stopped = False
        while True:
            try:
                line = events.get(timeout=POLL_SECONDS)
            except queue.Empty:
                lines = self.poll()
                if lines and self.take(lines) and not stopped:
                    stopped = True
                    self.stop(process)
                continue
            if line is None:
                break
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if event.get("session_id") and not stopped:
                self.session = event["session_id"]
            if event.get("type") == "assistant":
                for block in event.get("message", {}).get("content", []):
                    if block.get("type") == "text" and block.get("text", "").strip():
                        print(f"Claude: {block['text']}")
                        self.chat("say", block["text"].strip())
                        said = True
                    elif block.get("type") == "tool_use":
                        self.chat("status", describe(block))
            elif event.get("type") == "result":
                if event.get("is_error"):
                    self.chat("note", f"Claude Code failed: {event.get('result') or event.get('subtype')}")
                    said = True
                elif not said and event.get("result"):
                    self.chat("say", event["result"])
                    said = True
        process.wait()
        self.chat("status")
        if stopped:
            self.chat("note", "Stopped.")
        elif process.returncode and not said:
            details = "".join(errors).strip().splitlines()
            self.chat("note", "Claude Code stopped" + (f": {details[-1]}" if details else f" with code {process.returncode}"))

    @staticmethod
    def stop(process):
        if sys.platform == "win32":
            # The tools of the game run in a process of Claude Code.
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True)
        else:
            process.kill()

    def serve(self):
        print(f"Waiting for the game on {self.args.pipe}...")
        connected = False
        while True:
            lines = self.poll()
            if lines is None:
                if connected:
                    print("The game is gone: waiting for it to come back...")
                    connected = False
                time.sleep(1)
                continue
            if not connected:
                print("Connected: talk to Claude from the Chat tab of the AI Advanced Features window (Insert opens the menu).")
                connected = True
            self.take(lines)
            if self.pending:
                prompt = "\n".join(self.pending)
                self.pending.clear()
                self.run(prompt)
            else:
                time.sleep(POLL_SECONDS)


def unescape(text):
    """The text of a line of chat poll: \\n, \\t and \\\\ as the library escapes them."""
    out = []
    escaped = False
    for char in text:
        if escaped:
            out.append({"n": "\n", "t": "\t"}.get(char, char))
            escaped = False
        elif char == "\\":
            escaped = True
        else:
            out.append(char)
    return "".join(out)


def describe(block):
    """What a tool call does, in a few words."""
    name = block.get("name", "")
    name = name[len(TOOL_PREFIX):] if name.startswith(TOOL_PREFIX) else name
    values = [str(value) for value in (block.get("input") or {}).values() if not isinstance(value, (dict, list))]
    text = " ".join([name.replace("_", " ")] + values)
    return text if len(text) <= 80 else text[:77] + "..."


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pipe", default="universal-imgui-hook", help="config.remotePipe of the game")
    parser.add_argument("--model", help="model of Claude Code, such as sonnet or opus")
    parser.add_argument("--claude", help="path of the claude executable")
    args = parser.parse_args()
    if not args.pipe or any(c in args.pipe for c in "\\/"):
        sys.exit("--pipe takes a name, not a path")
    uih_mcp.PIPE_NAME = args.pipe
    try:
        Bridge(args).serve()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
