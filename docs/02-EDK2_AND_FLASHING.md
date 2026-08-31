# 02. EDK2 Quirks & Flashing Procedure

This is the most critical step. The HiKey960 will refuse to auto-boot and will crash during standard flashing if you do not follow these steps precisely.

## The EDK2 NVRAM Bug (The "Grub" Entry)
The HiKey960's Hisilicon EDK2 implementation contains a factory NVRAM boot entry labeled `Grub`. This entry is supposed to find the EFI System Partition (ESP) and execute `EFI\BOOT\BOOTAA64.EFI`.

However, the EDK2 firmware **does not search for the ESP by UUID or partition type**. It hardcodes the expected location:
*   **Partition Index:** 7
*   **Start LBA:** 73984 (Offset: ~37.8 MB)

If your partition table alters this geometry (which modern images usually do), EDK2 silently fails the `Grub` boot entry and drops you into the `Android Fastboot` entry or a UEFI Shell prompt, requiring manual keyboard intervention on every boot.

### The Solution
We must flash the **stock Linaro partition table** (`firmware/prm_ptable.img`). This forces the ESP back to Index 7 / LBA 73984.

## The Flashing Steps

### 1. Enter Fastboot Mode
1. Ensure the board is powered off.
2. Set the DIP switches: **Switch 1 (Auto-boot) ON**, **Switch 2 OFF**, **Switch 3 (Fastboot) ON**.
3. Connect USB-C to your PC and power on the board.

### 2. Flash the Stock Partition Table
Flash the stock partition table provided in this repository.
```bash
fastboot flash ptable firmware/prm_ptable.img
```

**CRITICAL STEP:** You must reboot the bootloader immediately so it loads the new partition map into memory.
```bash
fastboot reboot-bootloader
```

### 3. Flash the Armbian Image (Avoiding the Sparse Bug)
Hisilicon's sparse image parser is broken. If you flash a 30GB+ `.img` file directly, `fastboot` will try to send massive sparse chunks, causing the board to crash with `Unsupported Chunk Type: 0xFFFF`.

You must force `fastboot` to split the image into 128MB chunks using the `-S 128M` flag.
```bash
# Assuming your built image is named armbian-image.img
fastboot -S 128M flash system armbian-image.img
```
*(Note: Because we kept the stock ptable, the `system` partition extends to the end of the 32GB UFS, so we won't lose space by flashing the Armbian rootfs here).*

### 4. Prepare for Boot
Do **not** boot the OS just yet. Because we flashed the stock partition table, the block device mappings in the Armbian image's `/etc/fstab` are now wrong. Proceed to Document 03.
