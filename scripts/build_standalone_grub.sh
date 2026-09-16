#!/usr/bin/env bash
#
# build_standalone_grub.sh - Build modern self-contained GRUB 2.06 EFI image for HiKey960
#
# Generates GRUBAA64.EFI with pre-loaded ext2/fat/gpt/search modules and embedded
# memdisk config. This replaces the outdated 2017 GRUB 2.02 binary on the HiKey960 ESP (/dev/sdd7).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_EFI="${1:-/tmp/GRUBAA64.EFI}"
CFG_FILE="${REPO_ROOT}/configs/grub/standalone_grub.cfg"

if [ ! -f "${CFG_FILE}" ]; then
    echo "Error: Configuration file ${CFG_FILE} not found!" >&2
    exit 1
fi

echo "Building standalone GRUB 2.06 EFI image..."
grub-mkstandalone \
    --compress=xz \
    -O arm64-efi \
    -o "${OUTPUT_EFI}" \
    -d /usr/lib/grub/arm64-efi \
    --modules="part_gpt ext2 fat search search_fs_uuid search_label normal linux test" \
    boot/grub/grub.cfg="${CFG_FILE}"

echo "Successfully built ${OUTPUT_EFI} ($(stat -c%s "${OUTPUT_EFI}") bytes)"
