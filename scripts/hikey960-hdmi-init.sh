#!/bin/bash
# HiKey960 HDMI Console Initialization Service
# Configures Kirin 960 DPE/DSI and ADV7535 for stable 720p60 HDMI console output.

for i in $(seq 1 30); do
  if [ -e /dev/i2c-1 ] && [ -e /dev/fb0 ]; then
    break
  fi
  sleep 0.2
done

# 1. Configure DPE LDI timings for 1600x750 (72 MHz / 60 Hz exact)
# LDI HRZ_CTRL0 = hfp(80) | ((hbp(200) + hsw(40) - 1) << 16) = 0x00ef0050
busybox devmem 0xe867d000 32 0x00ef0050 2>/dev/null || true

# 2. Configure DSI Host timings for 1600x750
# VID_HSA_TIME (0x48) = 31 (0x1f)
busybox devmem 0xe8601048 32 0x0000001f 2>/dev/null || true
# VID_HBP_TIME (0x4c) = 153 (0x99)
busybox devmem 0xe860104c 32 0x00000099 2>/dev/null || true
# VID_HLINE_TIME (0x50) = 1227 (0x4cb)
busybox devmem 0xe8601050 32 0x000004cb 2>/dev/null || true

# 3. Configure ADV7535 bridge
# CEC Timing generator: 1600x750
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

# DSI 4 lanes clock divider = 0x18
i2cset -f -y 1 0x3c 0x16 0x18 2>/dev/null || true

# Disable ADV internal test pattern (pass through video)
i2cset -f -y 1 0x3c 0x55 0x00 2>/dev/null || true

# Disable ADV timing generator for direct DSI sync
i2cset -f -y 1 0x3c 0x27 0x0b 2>/dev/null || true

# Enable TMDS clock driver
i2cset -f -y 1 0x39 0xd6 0x50 2>/dev/null || true

# Power cycle transmitter state machine to lock PLL
i2cset -f -y 1 0x39 0x41 0x50 2>/dev/null || true
sleep 0.1
i2cset -f -y 1 0x39 0x41 0x10 2>/dev/null || true
i2cset -f -y 1 0x39 0xd6 0x50 2>/dev/null || true

# 4. Set LDI to normal display mode (scan fb0)
busybox devmem 0xe867d028 32 0x00000001 2>/dev/null || true

# 5. Prevent fbcon blanking
echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true
setterm -blank 0 -powerdown 0 > /dev/tty1 2>/dev/null || true

exit 0
