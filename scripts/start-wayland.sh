#!/bin/bash
export XDG_RUNTIME_DIR=/tmp/weston-runtime
mkdir -p "$XDG_RUNTIME_DIR" && chmod 0700 "$XDG_RUNTIME_DIR"

killall weston 2>/dev/null || true
sleep 0.5

/usr/local/bin/hikey960-hdmi-autores.py || true

weston --drm-device=card1 --continue-without-input --tty=2 --log=/var/log/weston.log &
WESTON_PID=$!

for i in $(seq 1 20); do
  SOCKET=$(ls $XDG_RUNTIME_DIR/wayland-* 2>/dev/null | grep -v '\.lock' | head -n 1)
  if [ -n "$SOCKET" ]; then
    export WAYLAND_DISPLAY=$(basename "$SOCKET")
    break
  fi
  sleep 0.2
done

# NOTE: TMDS and HDCP are now managed by hikey960-hdmi-watchdog.service
# This block is kept as a one-shot safety net at startup only
sleep 1
i2cset -f -y 1 0x39 0xd6 0x50 2>/dev/null || true
i2cset -f -y 1 0x39 0xaf 0x02 2>/dev/null || true

if [ -n "$WAYLAND_DISPLAY" ]; then
  weston-terminal &
  # weston-simple-egl removed: GPU demo triangle not needed on production desktop
fi

wait "$WESTON_PID"
