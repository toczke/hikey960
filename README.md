# HiKey960 - Modern Mainline Kernel Port (Armbian)

The HiKey960 (Kirin 960) is a powerful ARM64 board that was officially abandoned on the ancient **Linux 4.19** kernel. Attempting to run a modern, mainline-based kernel (Linux 5.x/6.x) typically results in a bricked bootloader, unbootable partitions, or broken hardware support.

This repository documents the **complete, end-to-end process** for compiling a modern Armbian system with a mainline edge kernel, working around the critical flaws in Hisilicon's EDK2 (UEFI) implementation, and successfully flashing the board so it auto-boots reliably.

```text
                ..                     user@hikey960
            `:]x**j-,'                 -------------
       .,+t***********z\<"             OS: Armbian 26.8.3 bookworm aarch64
       ?******************;            Host: HiKey960
      '*n` .'`^,;;,^`'. ,cc.           Kernel: Linux 7.1.10-edge-arm64
      -<.                .[l           Uptime: 35 mins
     //     ^^      ^^    \\           Packages: 440 (dpkg)
     !^         ^^         ":          Shell: bash 5.2.15
    'tt}`     !~]rj_     ")t/.         Terminal: /dev/pts/0
    Itttt?'   ~~]rr]   `{tttt,         CPU: hi3660 (4+4) @ 2.36 GHz
    \tttttt!""I_]r("""~tttttt1         Memory: 645.18 MiB / 3.79 GiB (17%)
  '_tttttttttttt)ftttttttttttti.       Swap: 0 B / 1.89 GiB (0%)
 \*ztttttttttttttttttttttttttf**[      Disk (/): 7.13 GiB / 28.12 GiB (25%) - ext4
l**c)tttttttttttttttttttttttt(z**,     Disk (/mnt/storage): 618.92 GiB / 3.58 TiB (17%) - ext4
.z*x.`tttttttttttttttttttttttt.`u*n    Disk (/tmp): 48.00 KiB / 1.86 GiB (0%) - ext4
>`   (tttttttttttttttttttttt]   "I     Disk (/var/log): 1.95 MiB / 46.84 MiB (4%) - ext4
     ,tttttttttttttttttttttt`          Local IP (wlan0): 192.168.x.x/24
     ./tttttfttttttttfttttt(           Locale: en_US.UTF-8
      'I)))(\()(tt))|\()({;'           
        .~~~~~~~|)~~~~~~~<                                     
        '[)))))1|()))))))?                                     
          ",,,"    ",,,^
```


## Current Project State
We have successfully ported the HiKey960 to a modern headless server environment.
* **Operating System:** Armbian (Debian 13 "Trixie")
* **Kernel:** Mainline Linux 7.1.x (Edge branch)

**Hardware Support & Fixes:**

| Component | Status | Configuration / Notes |
| :--- | :--- | :--- |
| **Storage** (32GB UFS 2.0) | Working | Restored stock partition table (EFI at LBA 73984) and compiled UFS/EXT4 drivers built-in. |
| **Wi-Fi** (TI WL1837) | Working | Requires `firmware-ti-connectivity` package. Operates natively via `wlcore` drivers. |
| **Bluetooth** (TI WL1837) | Working | Requires `firmware-ti-connectivity`. Interfaces via UART (`hci0`) and requires manual baud rate initialization using `hciattach`. |
| **USB Ports** (3.0 / Type-C) | Working | DTB patch required. Forced `vcc3v3_hub` to `regulator-always-on` to bypass a mainline kernel power bug. |
| **Expansion** (M.2 PCIe Gen2) | Working | Kernel pre-configured with `igc`/`igb`/`e1000e` and `ahci`. Supports networking or SATA adapters (e.g., ASM1166). |
| **Processor** (Kirin 960 4GB) | Working | SMP and CPU frequency scaling operate natively without modifications. |
| **40-Pin LS Header** | Working | UART, I2C, SPI, GPIO supported. `spidev` nodes require DTB patch. **Strictly 1.8V logic.** |
| **60-Pin HS Header** | Unsupported | MIPI CSI/DSI lanes inactive due to missing ISP blobs and disabled DRM. |
| **Graphics** (Mali G71 MP8) | Disabled | `CONFIG_DRM_PANFROST` intentionally unset to ensure stability and prevent SError kernel panics on headless servers. |

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
