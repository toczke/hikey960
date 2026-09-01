#!/usr/bin/env bash
# Skrypt do ekstrakcji partycji ext4 z oficjalnych obrazów Armbiana (.img.xz)
# Wynikiem jest plik rootfs.img gotowy do wgrania przez fastboot flash userdata

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Użycie: $0 <sciezka_do_pliku_Armbian.img.xz>"
    exit 1
fi

IMAGE_XZ="$1"
IMAGE_RAW="${IMAGE_XZ%.xz}"

echo "[1/4] Rozpakowywanie obrazu (to chwilę potrwa)..."
if [ ! -f "$IMAGE_RAW" ]; then
    xz -d -k -T0 "$IMAGE_XZ"
else
    echo "Plik .img już istnieje, pomijam dekompresję."
fi

echo "[2/4] Mapowanie partycji z pliku obrazu..."
LOOP_DEV=$(sudo losetup -Pf --show "$IMAGE_RAW")
echo "Zamapowano jako $LOOP_DEV"

# Szukamy partycji ext4 (zazwyczaj jest to partycja 1 lub 2)
# Sprawdzamy partycję 1
FS_TYPE=$(sudo blkid -o value -s TYPE "${LOOP_DEV}p1" || true)
TARGET_PART="${LOOP_DEV}p1"

if [ "$FS_TYPE" != "ext4" ]; then
    # Sprawdzamy partycję 2 (jeśli p1 to vfat/boot)
    FS_TYPE=$(sudo blkid -o value -s TYPE "${LOOP_DEV}p2" || true)
    TARGET_PART="${LOOP_DEV}p2"
fi

if [ "$FS_TYPE" != "ext4" ]; then
    echo "BŁĄD: Nie znaleziono partycji ext4 w obrazie!"
    sudo losetup -d "$LOOP_DEV"
    exit 1
fi

echo "[3/4] Wykryto główny system plików na $TARGET_PART. Kopiowanie danych do rootfs.img..."
sudo dd if="$TARGET_PART" of=rootfs.img bs=4M status=progress

echo "[4/4] Sprzątanie..."
sudo losetup -d "$LOOP_DEV"

echo "================================================="
echo "SUKCES! Wygenerowano plik: rootfs.img"
echo "Możesz go teraz sflashować za pomocą:"
echo "sudo fastboot flash userdata rootfs.img"
echo "================================================="
