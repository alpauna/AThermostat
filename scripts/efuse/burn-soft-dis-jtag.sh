#!/bin/bash
# Burn SOFT_DIS_JTAG eFuse — software-disables JTAG (can be re-enabled via HMAC)
# PERMANENT: The eFuse bit itself cannot be unburned, but JTAG can be
# re-enabled at runtime if HMAC-based JTAG re-enable is configured.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: SOFT_DIS_JTAG ==="
echo "This will software-disable JTAG. Can be re-enabled via HMAC if configured."
echo "Port: $PORT"
echo
echo "WARNING: This eFuse burn is irreversible!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse SOFT_DIS_JTAG 1 --do-not-confirm
echo "Done. SOFT_DIS_JTAG has been burned."
