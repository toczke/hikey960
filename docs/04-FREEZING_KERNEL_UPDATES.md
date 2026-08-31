# 04. Freezing Kernel Updates (CRITICAL)

Once you have successfully booted your HiKey960 and verified that everything works (UFS, USB, PCIe/Network, etc.), **you must immediately lock the kernel and bootloader packages.**

Because we are running on the `edge` branch (Linux 7.1+) with custom Device Tree (DTB) patches and specific bootloader configurations, a standard `apt upgrade` will pull the latest automated nightly/weekly build from the Armbian servers. 

This will overwrite your patched DTB (breaking USB), potentially overwrite your GRUB configuration, and can easily cause kernel panics or an unbootable system.

## 1. Locking Packages via APT
Open a terminal on the HiKey960 and run the following commands as root to mark the kernel, headers, firmware, and board-support packages as "held back":

```bash
sudo apt-mark hold armbian-bsp-cli-hikey960-edge-grub
sudo apt-mark hold armbian-firmware-full
sudo apt-mark hold linux-dtb-edge-arm64
sudo apt-mark hold linux-headers-edge-arm64
sudo apt-mark hold linux-image-edge-arm64
```

## 2. Verifying the Hold
To confirm that APT will ignore these packages during an upgrade, run:

```bash
apt-mark showhold
```
You should see all five packages listed.

## 3. Alternative: Using `armbian-config`
Armbian provides a text-based UI tool called `armbian-config` which can also freeze the kernel.
1. Run `sudo armbian-config`
2. Go to **System** -> **Freeze** (Freeze and unfreeze kernel and BSP upgrades).
3. Select it to disable kernel upgrades.

*(Note: Doing this manually via `apt-mark` as shown in Step 1 is preferred because you have exact control over which packages are held).*

## 4. Safe Upgrades
From now on, running `sudo apt update && sudo apt upgrade` is perfectly safe. It will update userland applications (like curl, docker, python) and Debian security patches, but it will skip the core kernel/DTB packages, ensuring your HiKey960 remains stable and functional.
