#!/bin/sh
# Runs a game with the library preloaded and drives the overlay as a player
# does: INSERT opens the menu, F11 removes it, and the game must keep running.
# Needs an X server, xdotool and ImageMagick.
# Usage: tests/game.sh Universal-ImGui-Hook.so OpenGL|Vulkan screenshot.png game [arguments]
set -eu

library=$(realpath "$1")
backend=$2
screenshot=$3
shift 3
log=$(mktemp)

UIH_DEBUG=1 LD_PRELOAD="$library" SDL_AUDIODRIVER=dummy "$@" >"$log" 2>&1 &
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
        kill -0 $pid 2>/dev/null || fail "the game exited while waiting for '$1'"
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
kill -0 $pid 2>/dev/null || fail "the game did not survive the removal of the overlay"

grep "\[uih\]" "$log"
echo "PASS: $backend"
