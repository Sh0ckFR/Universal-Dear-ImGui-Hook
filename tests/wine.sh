#!/bin/sh
# Runs a Windows game under Wine with the DLL loaded as dinput8.dll next to
# its executable, as the README tells Wine and Proton players to do, and
# drives the overlay as tests/game.sh does: INSERT opens the menu, F11 removes
# it, and the game must keep running. The menu must show on screen: the
# captures before and after INSERT, and after F11, must differ.
# Needs Wine, an X server, xdotool and ImageMagick.
# Usage: tests/wine.sh Universal-ImGui-Hook.dll OpenGL|D3D11|Vulkan|... capture-prefix game.exe [arguments]
set -eu

library=$(realpath "$1")
backend=$2
captures=$3
game=$(realpath "$4")
shift 4
log=$(mktemp)

# A copy of the DLL named dinput8.dll, loaded by a game that loads
# DirectInput 8; the override makes Wine prefer it to its own dinput8.
cp "$library" "$(dirname "$game")/dinput8.dll"
# +debugstr prints what the DLL writes with OutputDebugString(), +loaddll which
# dinput8.dll Wine loaded.
UIH_DEBUG=1 WINEDLLOVERRIDES="dinput8=n,b" WINEDEBUG=+debugstr,+loaddll wine "$game" "$@" >"$log" 2>&1 &
pid=$!
trap 'wineserver -k 2>/dev/null || true; rm -f "$(dirname "$game")/dinput8.dll"' EXIT

fail() {
    echo "FAIL: $1"
    cat "$log"
    exit 1
}

wait_for() {
    for _ in $(seq 240); do
        grep -q "$1" "$log" && return 0
        kill -0 $pid 2>/dev/null || fail "the game exited while waiting for '$1'"
        sleep 0.5
    done
    fail "timed out waiting for '$1'"
}

# The DLL reads the keys once per frame: a game that renders without GPU
# can take longer than a short key press between two frames.
press_until() {
    xdotool keydown "$1"
    wait_for "$2"
    sleep 0.2
    xdotool keyup "$1"
}

# Pixels whose colors differ by more than 10% between two captures.
changed() {
    compare -metric AE -fuzz 10% "$1" "$2" null: 2>&1 | awk '{ printf "%d", $1 }'
}

wait_for "dinput8\.dll\" at [0-9A-Fa-f]*: native"
wait_for "\[uih\] $backend backend active"
# Without a window manager, the keyboard goes to the window under the pointer.
xdotool mousemove 640 400
sleep 3
import -window root "$captures-before.png"
press_until Insert "\[uih\] Menu opened"
sleep 1.5
import -window root "$captures-menu.png"
# The 2% of a 1024x768 window that tests/game.cpp requires.
threshold=15728
count=$(changed "$captures-before.png" "$captures-menu.png")
echo "$count pixels changed with the menu open"
[ "$count" -ge $threshold ] || fail "the menu is not visible on screen"

press_until F11 "\[uih\] Stopped"
sleep 2
kill -0 $pid 2>/dev/null || fail "the game did not survive the removal of the overlay"
import -window root "$captures-after.png"
count=$(changed "$captures-menu.png" "$captures-after.png")
echo "$count pixels changed once the overlay was removed"
[ "$count" -ge $threshold ] || fail "the menu stays on screen after the overlay was removed"

grep "dinput8\.dll\" at [0-9A-Fa-f]*: native" "$log"
grep "\[uih\]" "$log"
echo "PASS: $backend"
