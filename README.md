# HiKey960 - Modern Mainline Kernel Port (Armbian)

The HiKey960 (Kirin 960) is a powerful ARM64 board that was officially abandoned on the ancient **Linux 4.19** kernel. Attempting to run a modern, mainline-based kernel (Linux 5.x/6.x) typically results in a bricked bootloader, unbootable partitions, or broken hardware support.

This repository documents the **complete, end-to-end process** for compiling a modern Armbian system with a mainline edge kernel, working around the critical flaws in Hisilicon's EDK2 (UEFI) implementation, and successfully flashing the board so it auto-boots reliably.

## Current Project State
We have successfully ported the HiKey960 to a modern headless server environment.
* **Operating System:** Armbian (Debian 13 "Trixie")
* **Kernel:** Mainline Linux 7.1.x (Edge branch)

**Hardware Support & Fixes:**
* **Storage (32GB UFS 2.0):** **Working.** Booting directly from the internal UFS storage. Achieved by restoring the stock partition table (keeping the EFI partition at LBA 73984) and compiling UFS/EXT4 drivers built-in.
* **Wireless (TI WL1837 Wi-Fi & Bluetooth):** **Working.** The integrated 802.11ac dual-band Wi-Fi operates natively out of the box (`wlan0` via `wl18xx`/`wlcore` drivers). Bluetooth is recognized via UART (`hci0`), but depending on the specific Armbian firmware package, it may require manual baud rate initialization using `hciattach`.
* **USB Ports (2x USB 3.0, 1x Type-C):** **Working.** A bug in the mainline kernel disables the power to the Microchip USB hub. Fixed via a custom Device Tree (DTB) patch that forces `vcc3v3_hub` to `regulator-always-on`.
* **Expansion (M.2 Key M PCIe Gen2):** **Working.** Kernel configured with `igc`, `igb`, and `e1000e` modules to support 2.5GbE network adapters (e.g., Intel I225-V) in the M.2 slot, freeing up USB ports.
* **Processor (Kirin 960 4xA73 + 4xA53, 3GB LPDDR4):** **Working.** SMP and CPU frequency scaling are operational.
* **40-Pin Low Speed Expansion Connector:** **Working (Full Support).** Standard buses (UART, I2C, SPI) and GPIOs are supported. A custom Device Tree Blob (DTB) patch is required to expose `spidev` nodes to user-space. Logic levels are strictly 1.8V.
* **60-Pin High Speed Expansion Connector:** **Disabled/Unsupported.** Contains MIPI DSI (Display) and MIPI CSI (Camera) interfaces. Since this is a headless build (DRM disabled) and mainline camera support for the Kirin ISP is essentially non-existent, these high-speed multimedia lanes are inactive.
* **Graphics (Mali G71 MP8 GPU):** **Disabled.** Deliberately disabled (`CONFIG_DRM_PANFROST` unset) to ensure 100% stability. Mainline Panfrost/Kirin DRM drivers can cause kernel panics without proper Android blobs. Since this board is used as a headless server, the GPU is unnecessary.


## The Challenge
Nobody does this because the Hisilicon firmware is fundamentally broken in several ways:
1.  **EDK2 NVRAM Hardcoding:** The stock UEFI bootloader ("Grub" entry) ignores standard EFI partition UUIDs. It hardcodes the EFI System Partition (ESP) to **Partition Index 7** and **LBA 73984**. Custom partition tables (like Armbian's default `maxroot`) shift this LBA, breaking auto-boot completely.
2.  **Sparse Image Parser Bug:** The HiKey960's `fastboot` implementation crashes (`Unsupported Chunk Type: 0xFFFF`) when flashing large, modern rootfs images.
3.  **Kernel Fragility:** Modern mainline kernels often break compatibility with the closed-source Wi-Fi/Bluetooth binaries or bootloader chain.

## Documentation Workflow

Please read the documentation in the following order to successfully build and flash your board:

1.  [01-BUILDING_ARMBIAN.md](docs/01-BUILDING_ARMBIAN.md) - How to compile the modern Armbian image from source.
2.  [02-EDK2_AND_FLASHING.md](docs/02-EDK2_AND_FLASHING.md) - **CRITICAL:** How to handle the partition table bug and flash the image chunk-by-chunk.
3.  [03-POST_FLASH_STABILIZATION.md](docs/03-POST_FLASH_STABILIZATION.md) - Fixing `fstab` and getting the board to boot reliably.
4.  [04-FREEZING_KERNEL_UPDATES.md](docs/04-FREEZING_KERNEL_UPDATES.md) - **CRITICAL:** Locking kernel packages via `apt-mark` to prevent automated updates from overwriting our DTB fixes and bricking the system.
5.  [05-GPIO_EXPANSION_HEADER.md](docs/05-GPIO_EXPANSION_HEADER.md) - Hardware specifications, 1.8V logic limits, full 40-pin layout, and SPI/PWM device tree configuration.
6.  [06-HS_EXPANSION_HEADER.md](docs/06-HS_EXPANSION_HEADER.md) - Details on the 60-pin HS connector, MIPI CSI/DSI limitations, and ISP hardware blockers on mainline Linux.
7.  [07-MULTIMEDIA_AND_GPU.md](docs/07-MULTIMEDIA_AND_GPU.md) - Why the HDMI port and Mali-G71 GPU are intentionally disabled for server stability.
8.  [08-BOARD_SWITCHES.md](docs/08-BOARD_SWITCHES.md) - Hardware DIP switch configurations for Normal Boot, Fastboot, and Brick Recovery.

## Assets in this Repository
*   `firmware/prm_ptable.img` - The stock Linaro partition table. **This is the holy grail** for fixing the EDK2 auto-boot bug. You *must* use this partition table to keep the ESP at LBA 73984.
