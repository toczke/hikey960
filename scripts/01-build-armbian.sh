#!/usr/bin/env bash
set -euo pipefail

WORKSPACE_DIR="$(cd .. && pwd)"
REPO="$WORKSPACE_DIR/armbian-build"
STATE="$WORKSPACE_DIR/flawless-build-state"
LOG="$STATE/build.log"
STATUS="$STATE/status.txt"
CONFIG_FILE="userpatches/linux-uefi-arm64-edge.config"

mkdir -p "$STATE"
exec > >(tee -a "$LOG") 2>&1
printf '%s [FLAWLESS] PREPARE\n' "$(date -Is)" > "$STATUS"

cd "$REPO"
mkdir -p userpatches
# Wyczyść poprzednie ustawienia
cp config/kernel/linux-uefi-arm64-edge.config "$CONFIG_FILE"

set_symbol() {
    local symbol="$1"
    local value="$2"
    sed -i "/^CONFIG_${symbol}=/d" "$CONFIG_FILE"
    sed -i "/^# CONFIG_${symbol} is not set/d" "$CONFIG_FILE"
    if [ "$value" == "n" ]; then
        printf '# CONFIG_%s is not set\n' "$symbol" >> "$CONFIG_FILE"
    else
        printf 'CONFIG_%s=%s\n' "$symbol" "$value" >> "$CONFIG_FILE"
    fi
}

echo "1. UFS (Storage) - OBLIGATORYJNE =y, inaczej kernel panic VFS"
for sym in SCSI BLK_DEV_SD SCSI_UFSHCD SCSI_UFSHCD_PLATFORM SCSI_UFS_HISI EXT4_FS; do set_symbol "$sym" "y"; done

echo "2. Clocks & PMIC - OBLIGATORYJNE =y"
for sym in COMMON_CLK_HI3660 COMMON_RESET_HI3660 RESET_CONTROLLER MFD_HI6421_PMIC REGULATOR REGULATOR_HI6421V530 REGULATOR_FIXED_VOLTAGE; do set_symbol "$sym" "y"; done

echo "3. USB (Dual-Role) - OBLIGATORYJNE =y, kasujemy gryzące się tryby host/gadget"
for sym in USB USB_XHCI_HCD USB_XHCI_PLATFORM USB_DWC3 USB_DWC3_DUAL_ROLE HISI_HIKEY_USB PHY_HI3660_USB USB_ROLE_SWITCH TYPEC TYPEC_TCPM TYPEC_TCPCI TYPEC_RT1711H EXTCON EXTCON_USB_GPIO; do set_symbol "$sym" "y"; done
set_symbol "USB_DWC3_HOST" "n"
set_symbol "USB_DWC3_GADGET" "n"

echo "4. Networking & UART (bez tego płytka jest 'martwa' komunikacyjnie)"
set_symbol "IGB" "m"
set_symbol "IGC" "m"
set_symbol "E1000E" "m"
set_symbol "IKHEADERS" "n"
for sym in SERIAL_AMBA_PL011 SERIAL_AMBA_PL011_CONSOLE DEVTMPFS DEVTMPFS_MOUNT EFI EFI_STUB EFI_PARTITION USB_NET_DRIVERS USB_USBNET USB_RTL8152; do set_symbol "$sym" "y"; done

echo "5. WiFi & Bluetooth - moduły, ale włączone, upewnijmy się że działają"
set_symbol "MAC80211" "y"
set_symbol "CFG80211" "y"
set_symbol "WL18XX" "m"
set_symbol "WLCORE_SDIO" "m"
set_symbol "BT_HCIUART" "m"

echo "6. CRITICAL FIX: ZABIJAMY PANFROST (Mali-G71 lockup) i Naprawiamy BT DMA w DTB"
# Dodajemy patcha w postaci skryptu hooka użytkownika Armbiana, który dynamicznie usuwa dmas z UART4
# i dodaje status = "disabled" do węzła GPU, aby wyeliminować twardy lockup.
cat << 'EOF' > userpatches/lib.config
post_patch_kernel() {
    local dts="arch/arm64/boot/dts/hisilicon/hi3660-hikey960.dts"
    if [ -f "$dts" ]; then
        echo "Aplikowanie poprawek dla HiKey960 w pliku DTS..."
        # Usunięcie dmas z węzła serial@fdf01000 (UART4) by naprawić błędy BT
        sed -i '/serial@fdf01000 {/,/bluetooth {/ s/dmas = .*/\/\* usunięto dmas dla BT \*\//' "$dts"
        # Wyłączenie GPU Panfrost
        echo -e "\n&gpu {\n\tstatus = \"disabled\";\n};\n" >> "$dts"
    fi
}
EOF

cp "$CONFIG_FILE" "$STATE/requested-kernel.config"

printf '%s [FLAWLESS] BUILDING\n' "$(date -Is)" > "$STATUS"

set +e
export KERNELBRANCH="tag:v7.1.10"

# Fix Docker DNS resolution
export DOCKER_EXTRA_ARGS=( "--dns=8.8.8.8" "--dns=1.1.1.1" )

PREFER_DOCKER=yes ./compile.sh build \
    BOARD=hikey960 \
    RELEASE=bookworm \
    BUILD_DESKTOP=no \
    BUILD_MINIMAL=yes \
    KERNEL_CONFIGURE=no \
    KERNEL_BTF=no \
    BRANCH=edge
result=$?
set -e

if [ "$result" -eq 0 ]; then
    printf '%s [FLAWLESS] SUCCESS\n' "$(date -Is)" > "$STATUS"
else
    printf '%s [FLAWLESS] FAILED exit=%s\n' "$(date -Is)" "$result" > "$STATUS"
fi

find output -maxdepth 4 -type f -printf '%s %TY-%Tm-%TdT%TH:%TM:%TS %p\n' 2>/dev/null | sort > "$STATE/output-files.txt"
exit "$result"
