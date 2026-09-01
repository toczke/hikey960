#!/bin/bash
# Flash Armbian to HiKey960
# Custom kernel 6.18.4 with UFS built-in
# Prerequisites: fastboot installed, board in fastboot mode (DIP switch 3=ON)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== HiKey960 Armbian Flasher ==="
echo "Custom kernel 6.18.4 (UFS_HISI=y)"
echo ""

# Check fastboot
if ! fastboot devices 2>/dev/null | grep -q fastboot; then
    echo "ERROR: No fastboot device found."
    echo ""
    echo "Options:"
    echo "  1. Set DIP switch 3=ON, power cycle"
    echo "  2. Or use recovery: DIP 1=ON 2=ON 3=OFF + hikey_idt"
    exit 1
fi

echo "Found device:"
fastboot devices
echo ""

echo "=== 1/5: Flash partition table ==="
sudo fastboot flash ptable "$SCRIPT_DIR/../uefi/prm_ptable.img"

echo "=== 2/5: Flash l-loader ==="
sudo fastboot flash fastboot "$SCRIPT_DIR/../uefi/l-loader.bin"

echo "=== 3/5: Flash UEFI firmware ==="
sudo fastboot flash fip "$SCRIPT_DIR/../uefi/fip.bin"

echo "=== 4/5: Flash boot partition ==="
sudo fastboot flash boot "$SCRIPT_DIR/../uefi/boot-linaro-stretch-developer-hikey-20200720-48.img"

echo "=== 5/5: Flash system (Armbian rootfs, ~3.3GB) ==="
sudo fastboot flash system "$SCRIPT_DIR/../armbian_system.img"

echo ""
echo "=== Flash complete! ==="
echo ""
echo "Next steps:"
echo "  1. Power off (WiFi plug off)"
echo "  2. Set DIP switch 3 to OFF (normal boot)"
echo "  3. Power on (WiFi plug on)"
echo "  4. Wait ~90 seconds"
echo "  5. SSH: ssh root@192.168.0.109 (no password)"
echo ""
