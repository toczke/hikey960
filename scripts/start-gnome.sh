#!/bin/bash
# HiKey960 GNOME Wayland Launcher

export XDG_RUNTIME_DIR=/tmp/gnome-runtime
mkdir -p "$XDG_RUNTIME_DIR" && chmod 0700 "$XDG_RUNTIME_DIR"

export XDG_SESSION_TYPE=wayland
export GDK_BACKEND=wayland
export CLUTTER_BACKEND=wayland
export MOZ_ENABLE_WAYLAND=1

# Auto-detect optimal display mode
if [ -x /usr/local/bin/hikey960-hdmi-autores.py ]; then
  /usr/local/bin/hikey960-hdmi-autores.py || true
fi

# Stop Weston if running
systemctl stop wayland-desktop.service 2>/dev/null || true
killall -9 weston weston-terminal 2>/dev/null || true
sleep 1

# Launch GNOME Shell native Wayland session
exec dbus-run-session -- gnome-shell --wayland --no-xfce
