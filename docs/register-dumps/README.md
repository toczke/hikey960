# ADV7533 Register Dumps

This directory contains full I2C register dumps captured from the running HiKey960 board.

## How to capture a dump

Run on the board (SSH or serial console), with HDMI cable connected and display active:

```bash
DATE=$(date +%F)
DUMP=~/adv7533-live-${DATE}.txt

echo "# ADV7533 I2C Register Dump - ${DATE}" > "${DUMP}"
echo "# Board: HiKey960 / Kirin 960" >> "${DUMP}"
echo "# Chip ID: i2cget -f -y 1 0x3c 0x00 → 0x75, 0x01 → 0x33 (ADV7533 confirmed)" >> "${DUMP}"
echo "" >> "${DUMP}"

for page in 0x39 0x3c; do
    echo "=== PAGE ${page} ===" >> "${DUMP}"
    for r in $(seq 0 255); do
        printf '0x%02x: ' $r >> "${DUMP}"
        i2cget -f -y 1 ${page} $(printf '0x%02x' $r) >> "${DUMP}" 2>/dev/null || echo "ERR" >> "${DUMP}"
    done
    echo "" >> "${DUMP}"
done

# Specifically read the unresolved registers:
echo "=== KEY REGISTERS ===" >> "${DUMP}"
echo -n "0x3c 0x1c (lane count, UNRESOLVED): " >> "${DUMP}"
i2cget -f -y 1 0x3c 0x1c >> "${DUMP}" 2>/dev/null || echo "ERR" >> "${DUMP}"
echo -n "0x39 0x9e (TMDS/PLL lock status): " >> "${DUMP}"
i2cget -f -y 1 0x39 0x9e >> "${DUMP}" 2>/dev/null || echo "ERR" >> "${DUMP}"
echo -n "0x3c 0x00 (chip ID high, expect 0x75): " >> "${DUMP}"
i2cget -f -y 1 0x3c 0x00 >> "${DUMP}" 2>/dev/null || echo "ERR" >> "${DUMP}"
echo -n "0x3c 0x01 (chip ID low, expect 0x33): " >> "${DUMP}"
i2cget -f -y 1 0x3c 0x01 >> "${DUMP}" 2>/dev/null || echo "ERR" >> "${DUMP}"

echo "Dump written to: ${DUMP}"
```

Then copy `adv7533-live-<date>.txt` to this directory and commit.

## Required analysis after capture

1. Read `0x3c 0x1c` — resolve the lane-count register conflict in `docs/09-HDMI_HARDWARE_REGISTERS.md §3`.
2. Read `0x39 0x9e` — document live PLL/TMDS lock status.
3. Confirm `0x3c 0x00=0x75` and `0x3c 0x01=0x33` — chip ID (ADV7533).
4. Update `docs/07-MULTIMEDIA_AND_GPU.md §1.7` and `docs/09-HDMI_HARDWARE_REGISTERS.md §3` with resolved values.

## Dumps in this directory

- [`adv7533-live-2026-09-12.txt`](adv7533-live-2026-09-12.txt): Captured on 2026-09-12 with physical HDMI connected and Weston running on card1.
  - Chip ID: `0x3c 0x00=0x75`, `0x3c 0x01=0x33` (ADV7533 confirmed)
  - Lane count: `0x3c 0x1c=0x40` (`lanes << 4 = 4 << 4 = 0x40`)
  - PLL/TMDS lock: `0x39 0x9e=0x14` (Bit 4 TMDS clock detected, Bit 2 PLL locked)
  - HDMI status: `0x39 0x42=0xf0` (HPD high, state ready)
