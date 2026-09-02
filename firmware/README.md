# Texas Instruments Firmware

This directory contains the necessary firmware files for the **Texas Instruments WL1837** Wi-Fi and Bluetooth module on the HiKey960.

To install this firmware manually:
1. Copy the `ti-connectivity` folder to your board's `/lib/firmware/` directory:
   ```bash
   sudo cp -r ti-connectivity /lib/firmware/
   ```
2. Make sure the files have correct read permissions.
3. If your kernel is missing the `bluez` or `rfkill` packages, ensure you install them via apt:
   ```bash
   sudo apt-get install bluez rfkill
   ```
4. Reboot the board.
