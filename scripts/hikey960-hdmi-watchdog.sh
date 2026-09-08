#!/bin/bash
# HiKey960 HDMI Hotplug Watchdog v1.1
#
# Background: HiKey960's ADV7533 has NO interrupt line wired to Kirin 960.
# When HPD goes low (cable unplug / TV off), ADV7533 hardware kills the TMDS
# transmitter (reg 0xd6 bit4 -> 0). Since the kernel never gets an IRQ, it
# cannot re-arm the transmitter. This daemon polls and re-arms automatically.
#
# To revert: systemctl disable --now hikey960-hdmi-watchdog && rm /usr/local/bin/hikey960-hdmi-watchdog.sh

I2C_BUS=1
ADV_MAIN=0x39

POLL_INTERVAL=1  # seconds between checks
RESTORE_DELAY=2  # seconds to wait after HPD rises before re-arming (give TV time)

log() {
    echo "[$(date '+%H:%M:%S')] hikey960-hdmi-watchdog: $*" | tee -a /var/log/hikey960-hdmi-watchdog.log
}

i2c_get() {
    i2cget -f -y "$I2C_BUS" "$1" "$2" 2>/dev/null
}

i2c_set() {
    i2cset -f -y "$I2C_BUS" "$1" "$2" "$3" 2>/dev/null
}

hex_to_dec() {
    printf '%d' "$1"
}

rearm_hdmi() {
    log "Re-arming HDMI transmitter (TMDS was off or HDCP active, HPD is HIGH)"
    i2c_set $ADV_MAIN 0xd6 0x50  # TMDS ON + HPD override
    i2c_set $ADV_MAIN 0xaf 0x02  # HDMI mode, HDCP OFF (bit1=HDMI, bits2,4=HDCP off)
    i2c_set $ADV_MAIN 0x16 0x20  # RGB 4:4:4
    i2c_set $ADV_MAIN 0x44 0x10  # AVI InfoFrame enable
    local d6=$(i2c_get $ADV_MAIN 0xd6)
    local af=$(i2c_get $ADV_MAIN 0xaf)
    log "Re-armed: 0xd6=$d6 0xaf=$af"
}

log "Watchdog v1.1 started (poll=${POLL_INTERVAL}s)"

HPD_WAS_LOW=0

while true; do
    STATUS=$(i2c_get $ADV_MAIN 0x42)
    TMDS=$(i2c_get $ADV_MAIN 0xd6)
    HDCP=$(i2c_get $ADV_MAIN 0xaf)

    # Convert hex strings to decimal for bit math
    STATUS_DEC=$(hex_to_dec "$STATUS")
    TMDS_DEC=$(hex_to_dec "$TMDS")
    HDCP_DEC=$(hex_to_dec "$HDCP")

    # 0x42 bit7 = HPD (monitor connected)
    HPD_HIGH=$(( (STATUS_DEC & 0x80) != 0 ))
    # 0xd6 bit4 = TMDS transmitter enable
    TMDS_ON=$(( (TMDS_DEC & 0x10) != 0 ))
    # 0xaf bits [4,2] = HDCP frame/stream encryption (bit1 is HDMI-mode selector, NOT HDCP)
    HDCP_ACTIVE=$(( (HDCP_DEC & 0x14) != 0 ))

    if [ "$HPD_HIGH" = "1" ]; then
        if [ "$HPD_WAS_LOW" = "1" ]; then
            log "HPD rose again, waiting ${RESTORE_DELAY}s before re-arm..."
            sleep $RESTORE_DELAY
            rearm_hdmi
            HPD_WAS_LOW=0
        elif [ "$TMDS_ON" = "0" ] || [ "$HDCP_ACTIVE" = "1" ]; then
            rearm_hdmi
        fi
    else
        if [ "$HPD_WAS_LOW" = "0" ]; then
            log "HPD went LOW (TV off / cable unplugged) — waiting for reconnect"
        fi
        HPD_WAS_LOW=1
    fi

    sleep $POLL_INTERVAL
done
