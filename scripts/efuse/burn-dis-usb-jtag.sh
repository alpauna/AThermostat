#!/bin/bash
# Burn DIS_USB_JTAG eFuse — disables USB-JTAG debug on GPIO19/20
# PERMANENT: Cannot be undone. Frees GPIO19/20 from USB-JTAG function.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: DIS_USB_JTAG ==="
echo "This will PERMANENTLY disable USB-JTAG debug on GPIO19/20."
echo "Port: $PORT"
echo
echo "WARNING: This is irreversible!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse DIS_USB_JTAG 1 --do-not-confirm
echo "Done. DIS_USB_JTAG has been burned."
