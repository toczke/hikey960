# Physical DIP Switches (Boot Modes)

To successfully develop, flash, and boot the HiKey960, you must configure the physical 3-position DIP switch located near the edge of the board. This is a hardware-level override that dictates the bootloader behavior.

## Switch Layout and Functions

The switches are numbered 1 to 3.

### 1. Auto-Boot Mode (Normal Operation)
This is the standard mode for booting into Armbian / Linux.
*   **Switch 1 (Auto Boot):** `ON` (Closed)
*   **Switch 2 (Boot Select):** `OFF` (Open)
*   **Switch 3 (Extended):** `OFF` (Open)

### 2. Fastboot / Recovery Mode (Flashing)
To flash the `prm_ptable.img`, bootloader, or rootfs via USB-C, you must force the board into UEFI Fastboot mode.
*   **Switch 1 (Auto Boot):** `OFF` (Open)
*   **Switch 2 (Boot Select):** `ON` (Closed)
*   **Switch 3 (Extended):** `OFF` (Open)

*Note: After flashing your partitions in Fastboot mode, you must power down the board, return the switches to the Auto-Boot configuration, and apply power again.*

### 3. USB Recovery (Brick Rescue)
Used only if the UEFI bootloader is completely corrupted and you need to push a new bootloader directly to RAM via Hisilicon's USB serial protocol.
*   **Switch 1 (Auto Boot):** `OFF` (Open)
*   **Switch 2 (Boot Select):** `OFF` (Open)
*   **Switch 3 (Extended):** `OFF` (Open)
