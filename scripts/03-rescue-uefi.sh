#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$SCRIPT_DIR/../uefi"
echo "Injecting RAM fastboot..."
sudo "$SCRIPT_DIR/../uefi/hikey_idt" -c config -p /dev/ttyUSB0
sleep 5

echo "1/4 Flashing middleman fastboot..."
sudo fastboot flash ptable "$SCRIPT_DIR/../uefi/prm_ptable.img"
sudo fastboot flash fastboot "$SCRIPT_DIR/../uefi/hisi-fastboot.img"

echo "2/4 Rebooting into middleman fastboot..."
sudo fastboot reboot-bootloader
sleep 7

echo "3/4 Flashing NEW expanded ptable..."
sudo fastboot flash ptable "$SCRIPT_DIR/../uefi/prm_ptable_expanded.img"
echo "Rebooting to apply NEW ptable..."
sudo fastboot reboot-bootloader
sleep 7

echo "4/4 Flashing UEFI bootloaders..."
sudo fastboot flash fastboot "$SCRIPT_DIR/../uefi/l-loader.bin"
sudo fastboot flash fip "$SCRIPT_DIR/../uefi/fip.bin"
sudo fastboot flash nvme "$SCRIPT_DIR/../uefi/hisi-nvme.img"
sudo fastboot flash fw_lpm3 "$SCRIPT_DIR/../uefi/hisi-lpm3.img"
sudo fastboot flash trustfirmware "$SCRIPT_DIR/../uefi/hisi-bl31.bin"

echo "SUCCESS! Ultimate flash complete!"
