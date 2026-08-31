# HiKey960 - Modern Mainline Kernel Port (Armbian)

The HiKey960 (Kirin 960) is a powerful ARM64 board that was officially abandoned on the ancient **Linux 4.19** kernel. Attempting to run a modern, mainline-based kernel (Linux 5.x/6.x) typically results in a bricked bootloader, unbootable partitions, or broken hardware support.

This repository documents the **complete, end-to-end process** for compiling a modern Armbian system with a mainline edge kernel, working around the critical flaws in Hisilicon's EDK2 (UEFI) implementation, and successfully flashing the board so it auto-boots reliably.

## The Challenge
Nobody does this because the Hisilicon firmware is fundamentally broken in several ways:
1.  **EDK2 NVRAM Hardcoding:** The stock UEFI bootloader ("Grub" entry) ignores standard EFI partition UUIDs. It hardcodes the EFI System Partition (ESP) to **Partition Index 7** and **LBA 73984**. Custom partition tables (like Armbian's default `maxroot`) shift this LBA, breaking auto-boot completely.
2.  **Sparse Image Parser Bug:** The HiKey960's `fastboot` implementation crashes (`Unsupported Chunk Type: 0xFFFF`) when flashing large, modern rootfs images.
3.  **Kernel Fragility:** Modern mainline kernels often break compatibility with the closed-source Wi-Fi/Bluetooth binaries or bootloader chain.

## Documentation Workflow

Please read the documentation in the following order to successfully build and flash your board:

1.  [01-BUILDING_ARMBIAN.md](docs/01-BUILDING_ARMBIAN.md) - How to compile the modern Armbian image from source.
2.  [02-EDK2_AND_FLASHING.md](docs/02-EDK2_AND_FLASHING.md) - **CRITICAL:** How to handle the partition table bug and flash the image chunk-by-chunk.
3.  [03-POST_FLASH_STABILIZATION.md](docs/03-POST_FLASH_STABILIZATION.md) - Fixing `fstab` and freezing the kernel packages to prevent future bricks.

## Assets in this Repository
*   `firmware/prm_ptable.img` - The stock Linaro partition table. **This is the holy grail** for fixing the EDK2 auto-boot bug. You *must* use this partition table to keep the ESP at LBA 73984.
