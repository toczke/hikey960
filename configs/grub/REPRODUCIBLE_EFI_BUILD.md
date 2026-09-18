# HiKey960 Reproducible GRUB 2.06 EFI Modernization Recipe

## Problem Description & Root Cause
The Linaro/Hisilicon EDK2 firmware on HiKey960 loads `\EFI\BOOT\GRUBAA64.EFI` from the internal UFS ESP partition (`/dev/sdd7`, LBA 73984).
The legacy image installed an outdated **GRUB 2.02~beta3 (2017 Debian 9)** binary.
When Armbian/Debian 12 was installed on the rootfs (`/dev/sdd10`), it brought GRUB 2.06 modules (`/boot/grub/arm64-efi/*.mod`).
Because `/dev/sdd7` has a 512-byte FAT sector size while the UFS controller has a 4096-byte hardware sector size, modern Linux kernel 7.x FAT driver refused to mount `/boot/efi` on `/dev/sdd7` (`FAT-fs: logical sector size too small for device`).
Consequently:
1. `dpkg` package upgrades never updated `GRUBAA64.EFI` on `/dev/sdd7`.
2. When the kernel stalled or rebooted uncleanly, the ext4 filesystem (`sdd10`) had modern ext4 features (`metadata_csum`, 64bit, `needs_recovery`) that GRUB 2.02 failed to read (`error: not an ext2 filesystem`).
3. When GRUB 2.02 dropped to rescue shell and attempted to dynamically load modules from `/boot/grub/arm64-efi/`, it crashed with `error: symbol 'grub_calloc' not found` (`grub_calloc` was introduced in GRUB 2.04).

## Solution: Standalone Self-Contained GRUB 2.06
Using `grub-mkstandalone`, we build a unified ARM64 EFI binary that:
1. Embeds core filesystem modules (`part_gpt`, `ext2`, `fat`, `search`, `search_fs_uuid`, `search_label`, `normal`, `linux`, `test`) in the core executable.
2. Contains an embedded memdisk configuration (`configs/grub/standalone_grub.cfg`) that searches for rootfs by UUID (`ebba5088-7c86-426d-8cd2-9899c9d50b36`) or label (`rootfs`).
3. Safely chainloads `($root)/boot/grub/grub.cfg` while guarding against recursion (`[ "$root" != "memdisk" ]`).
4. Provides fallback direct-boot menu entries in the memdisk itself in case the rootfs configuration is temporarily unreadable.

## Reproducible Build & Deployment Steps

1. **Build the Standalone Binary**:
   ```bash
   ./scripts/build_standalone_grub.sh /tmp/GRUBAA64.EFI
   ```

2. **Update the FAT ESP (`/dev/sdd7`) via `mtools` image pipeline**:
   Because Linux 7.x FAT driver cannot mount 512-byte sector FAT directly on 4K block devices, `sdd7` is updated safely using an image file with `mtools`:
   ```bash
   # Dump ESP partition to image
   dd if=/dev/sdd7 of=/tmp/sdd7.img bs=1M
   
   # Backup original binaries
   mmove -i /tmp/sdd7.img ::EFI/BOOT/GRUBAA64.EFI ::EFI/BOOT/GRUBOLD.EFI || true
   mmove -i /tmp/sdd7.img ::EFI/BOOT/BOOTAA64.EFI ::EFI/BOOT/BOOTOLD.EFI || true
   
   # Copy modernized GRUB 2.06 standalone image
   mcopy -o -i /tmp/sdd7.img /tmp/GRUBAA64.EFI ::EFI/BOOT/GRUBAA64.EFI
   mcopy -o -i /tmp/sdd7.img /tmp/GRUBAA64.EFI ::EFI/BOOT/BOOTAA64.EFI
   
   # Verify FAT filesystem integrity
   fsck.vfat -v -n /tmp/sdd7.img
   
   # Flash back to partition and sync
   dd if=/tmp/sdd7.img of=/dev/sdd7 bs=1M status=progress
   sync
   fsck.vfat -v -n /dev/sdd7
   ```

3. **Verify UEFI Boot Entries**:
   ```bash
   efibootmgr -v
   ```
   Confirm `Boot0002* Grub` points to `\EFI\BOOT\GRUBAA64.EFI` on `HD(7,GPT,d3340696-9b95-4c64-8df6-e6d4548fba41,0x12100,0x4000)`.
