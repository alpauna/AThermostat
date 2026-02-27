#!/bin/bash
# Burn HARD_DIS_JTAG eFuse — permanently disables pad JTAG on GPIO39-42
# PERMANENT: Cannot be undone.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: HARD_DIS_JTAG ==="
echo "This will PERMANENTLY disable pad JTAG on GPIO39-42."
echo "Port: $PORT"
echo
echo "WARNING: This is irreversible! You will lose JTAG debugging capability."
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse HARD_DIS_JTAG 1 --do-not-confirm
echo "Done. HARD_DIS_JTAG has been burned."
