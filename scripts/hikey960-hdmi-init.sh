#!/bin/bash
# HiKey960 HDMI Console Initialization Service
# v2.0 - fixed: removed destructive 0x41=0x50 power-down, proper HDMI register sequencing

# Wait for I2C and DRM to become ready
for i in $(seq 1 30); do
  if [ -e /dev/i2c-1 ] && [ -e /dev/fb0 ]; then
    break
  fi
  sleep 0.2
done

MODE=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || echo "1280,720")

if [ "$MODE" = "1920,1080" ]; then
  # --- 1080p Mode (1920x1080) ---
  # DPE LDI HRZ_CTRL0: hfp=88 (0x58), hbp(148) + hsw(44) - 1 = 191 (0xbf) -> 0x00bf0058
  busybox devmem 0xe867d000 32 0x00bf0058 2>/dev/null || true

  # DSI Host: hsa=33 (0x21), hbp=111 (0x6f), hline=1650 (0x672)
  busybox devmem 0xe8601048 32 0x00000021 2>/dev/null || true
  busybox devmem 0xe860104c 32 0x0000006f 2>/dev/null || true
  busybox devmem 0xe8601050 32 0x00000672 2>/dev/null || true

  # ADV7533 CEC Timing Generator: 1080p (chip confirmed ADV7533; NOT ADV7535)
  # htotal=2200 (0x898)
  i2cset -f -y 1 0x3c 0x28 0x89 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x29 0x80 2>/dev/null || true
  # hsw=44 (0x02c)
  i2cset -f -y 1 0x3c 0x2a 0x02 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2b 0xc0 2>/dev/null || true
  # hfp=88 (0x058)
  i2cset -f -y 1 0x3c 0x2c 0x05 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2d 0x80 2>/dev/null || true
  # hbp=148 (0x094)
  i2cset -f -y 1 0x3c 0x2e 0x09 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2f 0x40 2>/dev/null || true

  # vtotal=1125 (0x465)
  i2cset -f -y 1 0x3c 0x30 0x46 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x31 0x50 2>/dev/null || true
  # vsw=5
  i2cset -f -y 1 0x3c 0x32 0x00 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x33 0x50 2>/dev/null || true
  # vfp=4
  i2cset -f -y 1 0x3c 0x34 0x00 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x35 0x40 2>/dev/null || true
  # vbp=36 (0x024)
  i2cset -f -y 1 0x3c 0x36 0x02 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x37 0x40 2>/dev/null || true
else
  # --- 720p Mode (1280x720) ---
  busybox devmem 0xe867d000 32 0x00ef0050 2>/dev/null || true
  busybox devmem 0xe8601048 32 0x0000001f 2>/dev/null || true
  busybox devmem 0xe860104c 32 0x00000099 2>/dev/null || true
  busybox devmem 0xe8601050 32 0x000004cb 2>/dev/null || true

  i2cset -f -y 1 0x3c 0x28 0x64 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x29 0x00 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2a 0x02 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2b 0x80 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2c 0x05 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2d 0x00 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2e 0x0c 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x2f 0x80 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x30 0x2e 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x31 0xe0 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x32 0x00 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x33 0x50 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x34 0x00 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x35 0x50 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x36 0x01 2>/dev/null || true
  i2cset -f -y 1 0x3c 0x37 0x40 2>/dev/null || true
fi

# Common ADV7533 CEC/DSI-side settings (chip confirmed ADV7533 by I2C ID: 0x00=0x75, 0x01=0x33)
i2cset -f -y 1 0x3c 0x16 0x18 2>/dev/null || true  # PLL clock divider: 4 lanes
i2cset -f -y 1 0x3c 0x55 0x00 2>/dev/null || true  # CEC clock divider reset
i2cset -f -y 1 0x3c 0x27 0x0b 2>/dev/null || true  # Timing gen bypass (use Kirin DPE timing)

# --- FIX v2.0: do NOT issue 0x41=0x50 (power-down) here ---
# Issuing power-down resets ALL registers incl. 0xaf -> HDCP bits get set, TV rejects stream.
# Instead: only ensure TMDS is ON and HDCP is OFF without touching the power state.
i2cset -f -y 1 0x39 0xd6 0x50 2>/dev/null || true  # TMDS transmitter ON + HPD override
i2cset -f -y 1 0x39 0xaf 0x02 2>/dev/null || true  # HDMI mode, HDCP encryption OFF
i2cset -f -y 1 0x39 0x16 0x20 2>/dev/null || true  # RGB 4:4:4 input format
i2cset -f -y 1 0x39 0x44 0x10 2>/dev/null || true  # Enable AVI InfoFrame packet

# Set LDI to normal display mode (scan fb0)
busybox devmem 0xe867d024 32 0x00002ec1 2>/dev/null || true
busybox devmem 0xe867d028 32 0x00000001 2>/dev/null || true

# Prevent fbcon blanking
echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true
setterm -blank 0 -powerdown 0 > /dev/tty1 2>/dev/null || true

exit 0
