#!/bin/bash
# HiKey960 Wayland (Weston) Desktop Launcher

export XDG_RUNTIME_DIR=/tmp/weston-runtime
mkdir -p "$XDG_RUNTIME_DIR" && chmod 0700 "$XDG_RUNTIME_DIR"

# Kill any existing weston instance
killall weston 2>/dev/null || true
sleep 0.5

# Launch Weston Desktop Shell on DRM card1
weston --drm-device=card1 --current-mode --continue-without-input --tty=2 --log=/var/log/weston.log &
WESTON_PID=$!

# Wait for Wayland socket to appear
for i in $(seq 1 20); do
  SOCKET=$(ls $XDG_RUNTIME_DIR/wayland-* 2>/dev/null | grep -v '\.lock' | head -n 1)
  if [ -n "$SOCKET" ]; then
    export WAYLAND_DISPLAY=$(basename "$SOCKET")
    break
  fi
  sleep 0.2
done

# Launch default graphical terminal
if [ -n "$WAYLAND_DISPLAY" ]; then
  weston-terminal &
fi

wait "$WESTON_PID"
