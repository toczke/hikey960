# UEFI Bootloader (EDK2) and System Flashing

The HiKey960 relies on a fragile implementation of Hisilicon's EDK2 (UEFI). The bootloader strictly expects the EFI System Partition (ESP) to exist exactly at **LBA 73984** on the internal UFS. Standard Linux image writing (like `dd`) alters this layout, rendering the board unbootable.

We utilize the `fastboot` protocol to flash partitions individually, retaining the factory layout.

## Normal System Flashing (Fastboot)

Once you have your compiled Armbian image, use the provided `scripts/02-flash-rootfs.sh` script.

### 1. Enter Fastboot Mode
Set the physical DIP switches on the board to Fastboot Mode:
*   Switch 1: `OFF`
*   Switch 2: `ON`
*   Switch 3: `OFF`
Connect a USB-C cable to your PC and power the board.

### 2. Run the Flash Sequence
The script explicitly flashes the stock partition table (`prm_ptable.img`), bootloaders, and finally the massive rootfs in sparse chunks to bypass hardware buffer limits.
```bash
./scripts/02-flash-rootfs.sh
```

## Ultimate Rescue (Serial Recovery)

If the bootloader is completely corrupted (the board doesn't show up in `fastboot devices`), you must perform an ultimate rescue via the IDT serial protocol.

### 1. Enter Recovery Mode
*   Switch 1: `OFF`
*   Switch 2: `OFF`
*   Switch 3: `OFF`

### 2. Run the Recovery Sequence
Execute the `scripts/03-rescue-uefi.sh` script. This will use the `hikey_idt` tool to inject a temporary bootloader directly into RAM via the serial port (`/dev/ttyUSB0`), reboot the board into an intermediate fastboot state, and definitively rewrite the partition table (`prm_ptable_expanded.img`) and EDK2 UEFI firmwares from scratch.
```bash
./scripts/03-rescue-uefi.sh
```
After a successful rescue, return the switches to Auto-Boot (`ON`, `OFF`, `OFF`) and power cycle.
