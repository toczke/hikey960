#!/bin/bash
# HiKey960 HDMI clean shutdown hook
# Called by systemd at reboot/halt before kernel resets peripherals.
# Gracefully disables TMDS transmitter so the TV doesn't latch an error.
# (https://www.freedesktop.org/software/systemd/man/systemd-shutdown.html)

# Action passed as $1: "reboot", "poweroff", "halt", or "kexec"
ACTION="$1"

logger -t hikey960-hdmi-shutdown "Shutdown hook called: action=$ACTION"

# Gracefully power down ADV7533 TMDS transmitter
# This prevents the Philips TV from latching "Format not supported"
# when the kernel DPE/DSI stops generating the pixel clock.
i2cset -f -y 1 0x39 0xd6 0x40 2>/dev/null || true  # TMDS OFF
i2cset -f -y 1 0x39 0x41 0x50 2>/dev/null || true  # ADV7533 power down (safe to do NOW — at shutdown)

logger -t hikey960-hdmi-shutdown "ADV7533 TMDS cleanly powered down"
exit 0
