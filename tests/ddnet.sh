#!/bin/sh
# Needs an X server, xdotool and ImageMagick.
# Usage: tests/ddnet.sh Universal-ImGui-Hook.so path/to/DDNet OpenGL|Vulkan [screenshot.png]
set -eu

library=$(realpath "$1")
game=$(realpath "$2")
backend=$3
screenshot=${4:-ddnet.png}
log=$(mktemp)

UIH_DEBUG=1 LD_PRELOAD="$library" SDL_AUDIODRIVER=dummy "$game" \
    "gfx_backend $backend" "gfx_fullscreen 0" "gfx_screen_width 1024" "gfx_screen_height 768" >"$log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT

fail() {
    echo "FAIL: $1"
    cat "$log"
    exit 1
}

wait_for() {
    for _ in $(seq 120); do
        grep -q "$1" "$log" && return 0
        kill -0 $pid 2>/dev/null || fail "DDNet exited while waiting for '$1'"
        sleep 0.5
    done
    fail "timed out waiting for '$1'"
}

hold_key() {
    xdotool keydown "$1"
    sleep 0.5
    xdotool keyup "$1"
}

wait_for "\[uih\] $backend backend active"
xdotool mousemove 300 300
sleep 2
hold_key Insert
wait_for "\[uih\] Menu opened"
sleep 1
import -window root "$screenshot"
hold_key F11
wait_for "\[uih\] Stopped"
sleep 1
kill -0 $pid 2>/dev/null || fail "DDNet did not survive the removal of the overlay"

grep "\[uih\]" "$log"
echo "PASS: DDNet $backend"
