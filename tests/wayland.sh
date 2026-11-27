#!/bin/sh
# Runs a command in Weston, a Wayland compositor, nested in the X server of
# DISPLAY at the top left corner of its screen. The kiosk shell shows the
# newest window on the whole output and gives it the keyboard: keys and clicks
# sent to the X server, with xdotool or XTEST, reach the Wayland application
# through Weston, and the screen of the X server shows what Weston draws.
# The command gets WAYLAND_DISPLAY; run the application with `env -u DISPLAY`
# so that it only sees the compositor. Needs weston and an X server (Xvfb).
# Usage: tests/wayland.sh [--size WIDTHxHEIGHT] command [arguments]
set -eu

size=800x600
if [ "${1:-}" = --size ]; then
    size=$2
    shift 2
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    XDG_RUNTIME_DIR=$(mktemp -d)
    export XDG_RUNTIME_DIR
fi
socket=uih-weston-$$
log=$(mktemp)

weston --backend=x11 --renderer=pixman --shell=kiosk --width="${size%x*}" --height="${size#*x}" \
    --socket="$socket" --idle-time=0 >"$log" 2>&1 &
weston=$!
trap 'kill $weston 2>/dev/null || true' EXIT

for _ in $(seq 100); do
    [ -S "$XDG_RUNTIME_DIR/$socket" ] && break
    if ! kill -0 $weston 2>/dev/null; then
        echo "FAIL: Weston exited"
        cat "$log"
        exit 1
    fi
    sleep 0.1
done
if [ ! -S "$XDG_RUNTIME_DIR/$socket" ]; then
    echo "FAIL: Weston did not open its socket"
    cat "$log"
    exit 1
fi
# Weston gives the keyboard to its clients while its window has the focus of the X server.
if command -v xdotool >/dev/null; then
    timeout 10 xdotool search --sync --onlyvisible --name Weston windowfocus || echo "Weston's window was not found"
fi

status=0
WAYLAND_DISPLAY=$socket "$@" || status=$?
if [ $status -ne 0 ]; then
    echo "Weston's log:"
    cat "$log"
fi
exit $status
