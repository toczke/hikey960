# HiKey960 - Modern Mainline Kernel Port (Armbian)

The HiKey960 (Kirin 960) is a powerful ARM64 board that was officially abandoned on the ancient **Linux 4.19** kernel. Attempting to run a modern, mainline-based kernel (Linux 5.x/6.x) typically results in a bricked bootloader, unbootable partitions, or broken hardware support.

This repository documents the **complete, end-to-end process** for compiling a modern Armbian system with a mainline edge kernel, working around the critical flaws in Hisilicon's EDK2 (UEFI) implementation, and successfully flashing the board so it auto-boots reliably.

```text
                ..                     user@hikey960
            `:]x**j-,'                 -------------
       .,+t***********z\<"             OS: Armbian 26.8.3 bookworm aarch64
       ?******************;            Host: HiKey960
      '*n` .'`^,;;,^`'. ,cc.           Kernel: Linux 7.1.13+
      -<.                .[l           Uptime: 11 mins
     //     ^^      ^^    \\           Packages: 491 (dpkg)
     !^         ^^         ":          Shell: bash 5.2.15
    'tt}`     !~]rj_     ")t/.         Display (HDMI-A-1): 1920x1080, 60 Hz
    Itttt?'   ~~]rr]   `{tttt,         Terminal: /dev/pts/0 9.2p1 Debian-2+deb12u10
    \tttttt!""I_]r("""~tttttt1         CPU: hi3660 (4+4) @ 2.36 GHz
  '_tttttttttttt)ftttttttttttti.       GPU: Hisilicon hi3660-mali [Integrated]
 \*ztttttttttttttttttttttttttf**[      Memory: 341.53 MiB / 3.79 GiB (9%)
l**c)tttttttttttttttttttttttt(z**,     Swap: Disabled
.z*x.`tttttttttttttttttttttttt.`u*n    Disk (/): 8.13 GiB / 28.12 GiB (29%) - ext4
>`   (tttttttttttttttttttttt]   "I     Local IP (wlan0): 192.168.x.x/24
     ,tttttttttttttttttttttt`          Locale: en_US.UTF-8
     ./tttttfttttttttfttttt(           
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
| **Bluetooth** (TI WL1837) | Working | Requires `bluez`, `rfkill`, and TI firmware. Requires DTB patch removing `dmas` and `max-speed` from UART4 to fix DMA and baud rate timeouts. Initializes automatically natively via `hci_ti`. |
| **USB Ports** (3.0 / Type-C) | Working | DTB patch required. Forced `vcc3v3_hub` to `regulator-always-on` to bypass a mainline kernel power bug. |
| **Expansion** (M.2 PCIe Gen2) | Working | Kernel pre-configured with `igc`/`igb`/`e1000e` and `ahci`. Supports networking or SATA adapters (e.g., ASM1166). |
| **Processor** (Kirin 960 4GB) | Working | SMP and CPU frequency scaling operate natively without modifications. |
| **40-Pin LS Header** | Working | UART, I2C, SPI, GPIO supported. `spidev` nodes require DTB patch. **Strictly 1.8V logic.** |
| **60-Pin HS Header** | Unsupported | MIPI CSI lanes inactive due to missing ISP blobs. |
| **Graphics** (Mali G71 MP8) | Working | `panfrost` driver functional. CMA size reduced to 64MB to prevent boot panics. |
| **Display** (HDMI) | **Hardware Verified** | Ported legacy `kirin960-drm` to Linux 7.1 KMS (`feature/kirin960-drm-rewrite`). Pipeline (DPE, DSI, ADV7535) binds and registers `fb0: kirindrmfb`. Private DPE SMMU TrustZone AXI lockup solved in Build 75; 4-lane DSI bus alignment, VKMS virtual display removal, ADV7535 hardware binding, and empirical physical HDMI picture validation on external monitor achieved in Build 78. |

## GitHub Actions CI
This repository is equipped with a fully automated **GitHub Actions** workflow (`.github/workflows/kernel-build.yml`). 
Whenever a change is pushed to `main`, it will automatically:
1. Clone the latest `linux-7.1.y` stable kernel source from kernel.org.
2. Apply the custom HiKey960 configurations (UFS, PMIC, USB, Panfrost kill).
3. Apply Device Tree (DTB) patches on the fly to fix the UART4 Bluetooth bugs (`dmas` and `max-speed`).
4. Build the `Image.gz` and `.dtb` files.
5. Automatically create a **GitHub Release** with the compiled, production-ready kernel files attached as artifacts for easy downloading.

## Challenges Overcome
Nobody ported this board to modern Linux because the Hisilicon firmware is fundamentally broken in several ways. This repository systematically resolves all of them:
1.  **EDK2 NVRAM Hardcoding:** The stock UEFI bootloader ignores standard EFI partition UUIDs, hardcoding the ESP to **Partition Index 7** and **LBA 73984**. Custom partition tables shift this LBA, breaking auto-boot. **Solution:** We provide and flash the stock `prm_ptable.img` to lock the LBA in place, ensuring reliable auto-booting.
2.  **Sparse Image Parser Bug:** The HiKey960's `fastboot` crashes (`Unsupported Chunk Type: 0xFFFF`) when flashing large, modern rootfs images. **Solution:** Our flashing documentation provides the exact chunk-split workarounds to safely flash modern Armbian images.
3.  **Kernel Fragility & Hardware Regressions:** Mainline kernels broke compatibility with the Wi-Fi/Bluetooth UART bus and entirely dropped the proprietary display drivers. **Solution:** Our CI pipeline applies on-the-fly Device Tree (DTB) patches to fix Bluetooth DMA/baud-rate timeouts, modifies CMA allocation to stabilize the GPU, and injects a manually ported Kirin DRM driver to restore physical HDMI output.

## Documentation Workflow

Please read the documentation in the following order to successfully build and flash your board:

1.  [01-BUILDING_ARMBIAN.md](docs/01-BUILDING_ARMBIAN.md) - How to compile the modern Armbian image from source.
2.  [02-EDK2_AND_FLASHING.md](docs/02-EDK2_AND_FLASHING.md) - **CRITICAL:** How to handle the partition table bug and flash the image chunk-by-chunk.
3.  [03-POST_FLASH_STABILIZATION.md](docs/03-POST_FLASH_STABILIZATION.md) - Fixing `fstab` and getting the board to boot reliably.
4.  [04-FREEZING_KERNEL_UPDATES.md](docs/04-FREEZING_KERNEL_UPDATES.md) - **CRITICAL:** Locking kernel packages via `apt-mark` to prevent automated updates from overwriting our DTB fixes and bricking the system.
5.  [05-GPIO_EXPANSION_HEADER.md](docs/05-GPIO_EXPANSION_HEADER.md) - Hardware specifications, 1.8V logic limits, full 40-pin layout, and SPI/PWM device tree configuration.
6.  [06-HS_EXPANSION_HEADER.md](docs/06-HS_EXPANSION_HEADER.md) - Details on the 60-pin HS connector, MIPI CSI/DSI limitations, and ISP hardware blockers on mainline Linux.
7.  [07-MULTIMEDIA_AND_GPU.md](docs/07-MULTIMEDIA_AND_GPU.md) - How the Mali-G71 GPU and HDMI display pipeline (DPE/DSI) were ported to Linux 7.1.
8.  [08-BOARD_SWITCHES.md](docs/08-BOARD_SWITCHES.md) - Hardware DIP switch configurations for Normal Boot, Fastboot, and Brick Recovery.

## Assets in this Repository
*   `firmware/prm_ptable.img` - The stock Linaro partition table. **This is the holy grail** for fixing the EDK2 auto-boot bug. You *must* use this partition table to keep the ESP at LBA 73984.
