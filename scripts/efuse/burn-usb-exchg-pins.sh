#!/bin/bash
# Burn USB_EXCHG_PINS eFuse — swaps USB D- (GPIO19) and D+ (GPIO20)
# PERMANENT: Cannot be undone.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: USB_EXCHG_PINS ==="
echo "This will PERMANENTLY swap USB D- and D+ pin assignments (GPIO19 <-> GPIO20)."
echo "Port: $PORT"
echo
echo "WARNING: This is irreversible!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse USB_EXCHG_PINS 1 --do-not-confirm
echo "Done. USB_EXCHG_PINS has been burned."
