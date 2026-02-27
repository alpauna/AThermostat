#!/bin/bash
# Burn STRAP_JTAG_SEL eFuse — enables JTAG source selection via GPIO3 strapping
# PERMANENT: Cannot be undone.
# When burned, GPIO3 at boot selects JTAG source: LOW=pad JTAG, HIGH=USB JTAG
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: STRAP_JTAG_SEL ==="
echo "This will PERMANENTLY enable JTAG source selection via GPIO3 strapping."
echo "GPIO3 at boot: LOW=pad JTAG, HIGH=USB JTAG"
echo "Port: $PORT"
echo
echo "WARNING: This is irreversible!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse STRAP_JTAG_SEL 1 --do-not-confirm
echo "Done. STRAP_JTAG_SEL has been burned."
