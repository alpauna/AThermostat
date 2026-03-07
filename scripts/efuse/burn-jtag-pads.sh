#!/bin/bash
# Burn JTAG pad eFuses (GPIO39-42)
# Affects: GPIO39, GPIO40, GPIO41, GPIO42
# PERMANENT: Cannot be undone.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuses: JTAG Pads (GPIO39-42) ==="
echo "Port: $PORT"
echo
echo "This script configures JTAG pad eFuses on GPIO39-42."
echo "Disabling pad JTAG frees these pins for general use."
echo

# --- SOFT_DIS_JTAG ---
echo "SOFT_DIS_JTAG: Software-disable JTAG."
echo "  Can be re-enabled at runtime via HMAC if configured."
echo "  Less permanent than HARD_DIS_JTAG - good for development."
read -rp "  Burn SOFT_DIS_JTAG? [y/N]: " ans
SOFT_DIS=0
if [[ "$ans" =~ ^[Yy] ]]; then SOFT_DIS=1; fi

# --- HARD_DIS_JTAG ---
echo
echo "HARD_DIS_JTAG: Permanently disable pad JTAG."
echo "  Cannot be re-enabled. You will lose JTAG debug on GPIO39-42."
echo "  Recommended for production devices."
read -rp "  Burn HARD_DIS_JTAG? [y/N]: " ans
HARD_DIS=0
if [[ "$ans" =~ ^[Yy] ]]; then HARD_DIS=1; fi

# --- Summary ---
echo
echo "=== Summary ==="
echo "  SOFT_DIS_JTAG: $([ $SOFT_DIS -eq 1 ] && echo 'BURN' || echo 'skip')"
echo "  HARD_DIS_JTAG: $([ $HARD_DIS -eq 1 ] && echo 'BURN' || echo 'skip')"
echo
echo "WARNING: These eFuse burns are PERMANENT and cannot be undone!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

if [ $SOFT_DIS -eq 1 ]; then
    echo "Burning SOFT_DIS_JTAG..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse SOFT_DIS_JTAG 1 --do-not-confirm
fi
if [ $HARD_DIS -eq 1 ]; then
    echo "Burning HARD_DIS_JTAG..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse HARD_DIS_JTAG 1 --do-not-confirm
fi

echo "Done."
