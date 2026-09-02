# Kernel Compilation and Armbian Preparation for HiKey960

Our methodology avoids compiling the entire Armbian OS from scratch (which is time-consuming and often breaks). Instead, we utilize official Armbian rootfs images and compile only our rock-solid, custom-tailored Kernel and Device Tree Blobs (DTB).

## 1. Preparing the Armbian Rootfs

Instead of building the OS, download a nightly community image (e.g., `minimal` or `gnome_desktop`) from the [Armbian HiKey960 Releases](https://armbian.com/boards/hikey960). Once downloaded, extract the pure `ext4` rootfs partition using our automated extraction script:

```bash
# This mounts the image, extracts the ext4 partition, and creates rootfs.img
./scripts/04-prepare-armbian-img.sh /path/to/Armbian_community_26.11.0-trunk...img.xz
```
The resulting `rootfs.img` is immediately ready to be flashed via fastboot.

---

## 2. Compiling the Kernel & DTB

We use the mainline Linux kernel (e.g., v7.1-edge) to ensure all modern drivers are available. You have two options for compilation:

### Method A: GitHub Actions (Recommended)
Our repository contains a fully automated CI/CD pipeline (`.github/workflows/kernel-build.yml`). 
Pushing code to the `main` branch or clicking **Run workflow** in the GitHub Actions tab will automatically cross-compile the kernel on cloud servers and upload `hikey960-dtbs` and `hikey960-kernel-image` as downloadable artifacts.

### Method B: Local Cross-Compilation
To compile the kernel locally on an Ubuntu/Debian x86_64 host, install the required cross-compilation toolchain:

```bash
sudo apt update
sudo apt install -y gcc-aarch64-linux-gnu build-essential bc bison flex libssl-dev make device-tree-compiler
```

Clone the upstream Linux kernel tree:
```bash
git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux-src
cd linux-src
```

#### Step 2.1: Configuration
Generate the default configuration for the ARM64 architecture:
```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
```

**⚠️ CRITICAL KERNEL FLAGS:**
Before proceeding, you *must* edit the generated `.config` file to ensure the system can boot from the internal 32GB UFS storage and operate stably. Standard generic configs often build critical storage drivers as external modules (`=m`), which causes a boot loop because the kernel cannot read the UFS to load the module.

Ensure the following flags are strictly set to built-in (`=y`) or disabled (`# is not set`):

*   **Internal Storage (UFS) and Filesystem:**
    ```ini
    CONFIG_SCSI_UFSHCD=y
    CONFIG_SCSI_UFS_HISI=y
    CONFIG_EXT4_FS=y
    ```
    *(If these are `=m`, the HiKey960 will kernel panic at boot with `VFS: Cannot open root device`)*

*   **Graphics / GPU (Stability Fix):**
    ```ini
    # CONFIG_DRM_PANFROST is not set
    ```
    *(Disables the Mali G71 open-source driver to prevent SError hardware interrupts and hard lockups on headless servers)*

*   **Networking & Expansion (Optional but recommended):**
    ```ini
    CONFIG_SATA_AHCI=m
    CONFIG_IGC=m
    CONFIG_IGB=m
    ```
    *(Ensures support for M.2 ASM1166 SATA controllers and Intel 2.5GbE network cards)*

#### Step 2.2: Build the Kernel Core
Compile the uncompressed kernel executable (`Image.gz`). This takes 15-40 minutes depending on your CPU:
```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image.gz
```

#### Step 2.3: Build Device Tree Blobs (DTB)
Before compiling the hardware description files, it's highly recommended to apply a patch to the HiKey960 device tree to fix the native Bluetooth driver (`hci_ti`) timeouts. Delete the `dmas` and `max-speed` properties from the `uart4` node:
```bash
sed -i '/&uart4 {/a \\t/delete-property/ dmas;\n\t/delete-property/ max-speed;' arch/arm64/boot/dts/hisilicon/hi3660-hikey960.dts
```
Then, compile the DTBs:
```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) dtbs
```

#### Output Locations
Once compilation finishes successfully, your generated assets will be located at:
* **Kernel:** `arch/arm64/boot/Image.gz`
* **DTB:** `arch/arm64/boot/dts/hisilicon/hi3660-hikey960.dtb`

These files can now be moved to the UEFI/boot partitions as detailed in `02-EDK2_AND_FLASHING.md`.
