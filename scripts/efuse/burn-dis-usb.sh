#!/bin/bash
# Burn DIS_USB eFuse — disables USB OTG function
# PERMANENT: Cannot be undone.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: DIS_USB ==="
echo "This will PERMANENTLY disable USB OTG function."
echo "Port: $PORT"
echo
echo "WARNING: This is irreversible!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse DIS_USB 1 --do-not-confirm
echo "Done. DIS_USB has been burned."
