# 03. Post-Flash Stabilization

After flashing, the OS needs a few manual tweaks to boot correctly and remain stable. Modern mainline kernels on the HiKey960 are prone to regressions, so we must lock the system state once it's working.

## 1. Fixing `/etc/fstab` (Before First Boot)
Because we flashed the stock partition table, the OS partition number for the EFI System Partition (ESP) has likely changed from what the Armbian image expects (e.g., from `/dev/sdd6` to `/dev/sdd7`). 

If `/etc/fstab` points to the wrong block device, the OS will hang during boot trying to mount `/boot/efi`.

**How to fix:**
You can either mount the `.img` locally on your PC before flashing, OR boot into the UEFI shell, boot Linux manually once, and edit the file.
Change the `/boot/efi` line in `/etc/fstab` to use the stock ESP's `PARTUUID` instead of a hardcoded block device.

**Change this:**
```text
/dev/sdd6  /boot/efi  vfat  defaults  0  2
```
**To this:**
```text
PARTUUID=d3340696-9b95-4c64-8df6-e6d4548fba41  /boot/efi  vfat  defaults  0  2
```
*(The UUID `d3340696-9b95-4c64-8df6-e6d4548fba41` is a constant in the stock Linaro `prm_ptable.img`).*

> **⚠️ NOTE on `/boot/efi` mounting:** The stock FAT32 EFI partition uses a 512-byte sector size, but the native UFS memory uses 4K sectors. Modern Linux kernels may refuse to mount it, throwing `FAT-fs: logical sector size too small for device`. This is usually harmless for a headless server (as the EDK2 bootloader still reads it just fine at boot), but it means `apt upgrade` cannot update GRUB. Freezing kernel updates (Step 04) prevents this from causing apt failures.

## 2. Booting
1. Power off the board.
2. Turn **Switch 3 OFF**. (Leave Switch 1 ON).
3. Power on. The board should automatically boot directly into Armbian without dropping into the UEFI shell.

## 3. Next Steps
Move on to the final and most important step: **[04-FREEZING_KERNEL_UPDATES.md](04-FREEZING_KERNEL_UPDATES.md)** to lock your system state and prevent catastrophic regressions during `apt upgrade`.
