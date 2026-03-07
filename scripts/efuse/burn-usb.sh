#!/bin/bash
# Burn USB-related eFuses (GPIO19/20)
# Affects: GPIO19 (USB D-), GPIO20 (USB D+)
# PERMANENT: Cannot be undone.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuses: USB Configuration (GPIO19/20) ==="
echo "Port: $PORT"
echo
echo "This script configures USB-related eFuses on GPIO19/20."
echo "Disabling USB-JTAG and USB Serial JTAG frees these pins"
echo "for general use (e.g. HX710 pressure sensors)."
echo

# --- DIS_USB_JTAG ---
echo "DIS_USB_JTAG: Disable USB-JTAG debug function on GPIO19/20."
echo "  Recommended if using GPIO19/20 for other purposes."
read -rp "  Burn DIS_USB_JTAG? [Y/n]: " ans
DIS_USB_JTAG=1
if [[ "$ans" =~ ^[Nn] ]]; then DIS_USB_JTAG=0; fi

# --- DIS_USB_SERIAL_JTAG ---
echo
echo "DIS_USB_SERIAL_JTAG: Disable USB serial JTAG on GPIO19/20."
echo "  Recommended if using GPIO19/20 for other purposes."
read -rp "  Burn DIS_USB_SERIAL_JTAG? [Y/n]: " ans
DIS_USB_SERIAL_JTAG=1
if [[ "$ans" =~ ^[Nn] ]]; then DIS_USB_SERIAL_JTAG=0; fi

# --- DIS_USB ---
echo
echo "DIS_USB: Disable USB OTG function entirely."
echo "  Only needed if you want to fully disable USB OTG peripheral."
read -rp "  Burn DIS_USB? [y/N]: " ans
DIS_USB=0
if [[ "$ans" =~ ^[Yy] ]]; then DIS_USB=1; fi

# --- USB_EXCHG_PINS ---
echo
echo "USB_EXCHG_PINS: Swap USB D- (GPIO19) and D+ (GPIO20) assignments."
echo "  Only needed for boards with reversed USB wiring."
read -rp "  Burn USB_EXCHG_PINS? [y/N]: " ans
USB_EXCHG=0
if [[ "$ans" =~ ^[Yy] ]]; then USB_EXCHG=1; fi

# --- Summary ---
echo
echo "=== Summary ==="
echo "  DIS_USB_JTAG:        $([ $DIS_USB_JTAG -eq 1 ] && echo 'BURN' || echo 'skip')"
echo "  DIS_USB_SERIAL_JTAG: $([ $DIS_USB_SERIAL_JTAG -eq 1 ] && echo 'BURN' || echo 'skip')"
echo "  DIS_USB:             $([ $DIS_USB -eq 1 ] && echo 'BURN' || echo 'skip')"
echo "  USB_EXCHG_PINS:      $([ $USB_EXCHG -eq 1 ] && echo 'BURN' || echo 'skip')"
echo
echo "WARNING: These eFuse burns are PERMANENT and cannot be undone!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

if [ $DIS_USB_JTAG -eq 1 ]; then
    echo "Burning DIS_USB_JTAG..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse DIS_USB_JTAG 1 --do-not-confirm
fi
if [ $DIS_USB_SERIAL_JTAG -eq 1 ]; then
    echo "Burning DIS_USB_SERIAL_JTAG..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse DIS_USB_SERIAL_JTAG 1 --do-not-confirm
fi
if [ $DIS_USB -eq 1 ]; then
    echo "Burning DIS_USB..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse DIS_USB 1 --do-not-confirm
fi
if [ $USB_EXCHG -eq 1 ]; then
    echo "Burning USB_EXCHG_PINS..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse USB_EXCHG_PINS 1 --do-not-confirm
fi

echo "Done."
