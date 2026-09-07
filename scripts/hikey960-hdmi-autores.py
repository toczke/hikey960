#!/usr/bin/env python3
"""
HiKey960 HDMI Dynamic Mode Auto-Negotiator.
Detects connected display modes via DRM/KMS, filters by ADV7533 hardware limits
(pixel clock <= 80MHz) and consumer TV compatibility (>= 50Hz), and selects
the highest working resolution.
"""

import sys
import os
import re
import subprocess

WESTON_INI_PATH = "/etc/xdg/weston/weston.ini"
MAX_PIXEL_CLOCK_KHZ = 80000  # ADV7533 hardware silicon limit: 80.0 MHz
MIN_REFRESH_RATE_HZ = 48.0   # Standard TV compatibility (50Hz / 59.94Hz / 60Hz)

def get_connected_modes():
    try:
        out = subprocess.check_output(["modetest", "-M", "kirin", "-c"], stderr=subprocess.DEVNULL).decode("utf-8")
    except Exception as e:
        print(f"[autores] Error querying modetest: {e}", file=sys.stderr)
        return []

    modes = []
    # Pattern matches lines like:
    # #14 1280x720 60.00 1280 1390 1430 1650 720 725 730 750 74250 flags: phsync, pvsync; ...
    for line in out.splitlines():
        m = re.search(r"#\d+\s+(\d+)x(\d+)\s+([\d\.]+)\s+.*?\s+(\d+)\s+flags:\s*(.*?);", line)
        if m:
            w = int(m.group(1))
            h = int(m.group(2))
            fps = float(m.group(3))
            pclk_khz = int(m.group(4))
            flags_and_type = m.group(5)
            is_pref = "preferred" in flags_and_type
            modes.append({
                "w": w,
                "h": h,
                "fps": fps,
                "pclk": pclk_khz,
                "aspect": w / h,
                "preferred": is_pref,
            })
    return modes

def find_best_mode(modes):
    if not modes:
        return {"w": 1280, "h": 720, "fps": 60.0, "pclk": 74250}

    # Filter by hardware limits and TV compatibility
    valid = [
        m for m in modes
        if m["pclk"] <= MAX_PIXEL_CLOCK_KHZ and m["fps"] >= MIN_REFRESH_RATE_HZ
    ]

    if not valid:
        # Fallback to standard 720p60 if no modes passed filter
        return {"w": 1280, "h": 720, "fps": 60.0, "pclk": 74250}

    # Detect preferred aspect ratio from display EDID
    preferred_aspect = 16.0 / 9.0  # default
    for m in modes:
        if m.get("preferred"):
            preferred_aspect = m["aspect"]
            break

    # Prioritize modes matching display aspect ratio, then area, then refresh rate
    def mode_rank(m):
        aspect_diff = abs(m["aspect"] - preferred_aspect)
        matches_aspect = 1 if aspect_diff < 0.05 else 0
        is_60 = 1 if abs(m["fps"] - 60.0) < 1.0 else 0
        area = m["w"] * m["h"]
        return (matches_aspect, area, is_60, m["fps"])

    valid.sort(key=mode_rank, reverse=True)
    return valid[0]

def update_weston_config(best_mode):
    mode_str = f"{best_mode['w']}x{best_mode['h']}@{int(round(best_mode['fps']))}"
    print(f"[autores] Selected optimal mode: {mode_str} (pixel clock: {best_mode['pclk']/1000.0:.2f} MHz)")

    content = f"""[core]
backend=drm-backend.so
shell=desktop-shell.so
idle-time=0

[shell]
locking=false
panel-position=top
background-color=0xff1e385b

[output]
name=HDMI-A-1
mode={mode_str}
"""
    try:
        os.makedirs(os.path.dirname(WESTON_INI_PATH), exist_ok=True)
        with open(WESTON_INI_PATH, "w") as f:
            f.write(content)
        print(f"[autores] Successfully updated {WESTON_INI_PATH} with mode={mode_str}")
    except Exception as e:
        print(f"[autores] Failed to write {WESTON_INI_PATH}: {e}", file=sys.stderr)

    return mode_str

if __name__ == "__main__":
    modes = get_connected_modes()
    best = find_best_mode(modes)
    mode_str = update_weston_config(best)
    print(mode_str)
