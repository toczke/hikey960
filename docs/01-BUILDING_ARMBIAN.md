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
Our repository contains a fully automated CI/CD pipeline:
- [`.github/workflows/kernel-build.yml`](../.github/workflows/kernel-build.yml): Tests compilation on every PR and branch push, uploading test artifacts.
- [`.github/workflows/release.yml`](../.github/workflows/release.yml): Compiles and publishes formal GitHub Public Releases automatically whenever changes are merged into `main` or `master`.

### Method B: Local Cross-Compilation
To compile the kernel locally on an Ubuntu/Debian x86_64 host, install the required cross-compilation toolchain:

```bash
sudo apt update
sudo apt install -y gcc-aarch64-linux-gnu build-essential bc bison flex libssl-dev make device-tree-compiler
```

Clone the upstream Linux kernel tree:
```bash
sudo apt install -y curl tar xz-utils
curl -fSL https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.1.13.tar.xz -o linux-7.1.13.tar.xz
mkdir -p linux-src && tar -xf linux-7.1.13.tar.xz -C linux-src --strip-components=1
cd linux-src
```

Apply the repository patches:
```bash
for p in ../patches/*.patch; do patch -p1 < "$p"; done
```

Inject Kirin DRM and VPU drivers:
```bash
cp -r ../drivers-import/kirin960 drivers/gpu/drm/hisilicon/
echo "source \"drivers/gpu/drm/hisilicon/kirin960/Kconfig\"" >> drivers/gpu/drm/hisilicon/Kconfig
echo "obj-y += kirin960/" >> drivers/gpu/drm/hisilicon/Makefile

cp -r ../drivers-import/vcodec drivers/
sed -i '/endmenu/i source "drivers/vcodec/Kconfig"' drivers/Kconfig
echo "obj-y += vcodec/" >> drivers/Makefile
```

#### Step 2.1: Configuration
Copy the ground-truth defconfig:
```bash
cp ../configs/hikey960-defconfig .config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
```

**⚠️ CRITICAL KERNEL FLAGS:**
Ensure the following flags are strictly set in your `.config`:

*   **Internal Storage (UFS) and Filesystem:**
    ```ini
    CONFIG_SCSI_UFSHCD=y
    CONFIG_SCSI_UFS_HISI=y
    CONFIG_EXT4_FS=y
    ```
    *(If these are `=m`, the HiKey960 will kernel panic at boot with `VFS: Cannot open root device`)*

*   **Graphics / GPU & CMA Memory:**
    ```ini
    CONFIG_DRM_PANFROST=y
    CONFIG_CMA_SIZE_MBYTES=256
    ```
    *(Panfrost is kept ENABLED. `CONFIG_CMA_SIZE_MBYTES=256` allocates 256MB of contiguous physical memory out-of-the-box, matching the required memory pool for 4K UHD and 4× concurrent 1080p VPU hardware video decode and encode pipelines without runtime starvation.)*

*   **Hardware Video Acceleration (VPU — Kirin 960 VDH/VEDU):**
    ```ini
    CONFIG_HI_VCODEC=y
    CONFIG_HI_VCODEC_VDEC_HI3660=y
    CONFIG_HI_VCODEC_VENC_HI3660=y
    ```
    *(Enables `/dev/hi_vdec` and `/dev/hi_venc` for 10/10 VDEC codec decode and 4K/HEVC/concurrent VENC hardware encoding)*

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
