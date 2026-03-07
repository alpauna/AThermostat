#!/bin/bash
# Burn VDD_SPI and power-related eFuses
# Affects: GPIO45 (VDD_SPI strapping), GPIO33-37 (power source)
# PERMANENT: Cannot be undone.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuses: VDD / Power Configuration ==="
echo "Port: $PORT"
echo
echo "This script configures power-related eFuses."
echo "Burning VDD_SPI_FORCE frees GPIO45 as a regular GPIO."
echo

# --- VDD_SPI_FORCE ---
echo "VDD_SPI_FORCE: Force VDD_SPI voltage from eFuse instead of GPIO45 strapping."
echo "  This frees GPIO45 for use as a regular GPIO."
read -rp "  Burn VDD_SPI_FORCE? [Y/n]: " ans
VDD_SPI_FORCE=1
if [[ "$ans" =~ ^[Nn] ]]; then VDD_SPI_FORCE=0; fi

# --- VDD_SPI_XPD ---
echo
echo "VDD_SPI_XPD: Power up the internal VDD_SPI regulator."
echo "  Required if VDD_SPI_FORCE is set. Most boards need this ON."
read -rp "  Burn VDD_SPI_XPD? [Y/n]: " ans
VDD_SPI_XPD=1
if [[ "$ans" =~ ^[Nn] ]]; then VDD_SPI_XPD=0; fi

# --- VDD_SPI_TIEH ---
echo
echo "VDD_SPI_TIEH: Select VDD_SPI voltage level."
echo "  0 = 1.8V, 1 = 3.3V"
echo "  Most ESP32-S3 boards with standard flash use 3.3V."
read -rp "  VDD_SPI voltage [3.3V/1.8V] (default 3.3V): " volt
VDD_SPI_TIEH=1
if [ "$volt" = "1.8V" ] || [ "$volt" = "1.8" ]; then VDD_SPI_TIEH=0; fi

# --- PIN_POWER_SELECTION ---
echo
echo "PIN_POWER_SELECTION: Power source for GPIO33-37."
echo "  0 = VDD_SPI (follows VDD_SPI voltage above)"
echo "  1 = VDD3P3_CPU (always 3.3V from CPU rail)"
echo "  Recommended: 1 (VDD3P3_CPU) for consistent 3.3V on GPIO33-37."
read -rp "  Use VDD3P3_CPU for GPIO33-37? [Y/n]: " ans
PIN_POWER=1
if [[ "$ans" =~ ^[Nn] ]]; then PIN_POWER=0; fi

# --- Summary ---
echo
echo "=== Summary ==="
echo "  VDD_SPI_FORCE:       $([ $VDD_SPI_FORCE -eq 1 ] && echo 'BURN' || echo 'skip')"
echo "  VDD_SPI_XPD:         $([ $VDD_SPI_XPD -eq 1 ] && echo 'BURN' || echo 'skip')"
echo "  VDD_SPI_TIEH:        $([ $VDD_SPI_TIEH -eq 1 ] && echo 'BURN (3.3V)' || echo 'skip (1.8V)')"
echo "  PIN_POWER_SELECTION: $([ $PIN_POWER -eq 1 ] && echo 'BURN (VDD3P3_CPU)' || echo 'skip (VDD_SPI)')"
echo
echo "WARNING: These eFuse burns are PERMANENT and cannot be undone!"
echo "Incorrect voltage settings may damage flash or PSRAM."
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

if [ $VDD_SPI_FORCE -eq 1 ]; then
    echo "Burning VDD_SPI_FORCE..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse VDD_SPI_FORCE 1 --do-not-confirm
fi
if [ $VDD_SPI_XPD -eq 1 ]; then
    echo "Burning VDD_SPI_XPD..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse VDD_SPI_XPD 1 --do-not-confirm
fi
if [ $VDD_SPI_TIEH -eq 1 ]; then
    echo "Burning VDD_SPI_TIEH (3.3V)..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse VDD_SPI_TIEH 1 --do-not-confirm
fi
if [ $PIN_POWER -eq 1 ]; then
    echo "Burning PIN_POWER_SELECTION (VDD3P3_CPU)..."
    python3 "$ESPEFUSE" --port "$PORT" burn_efuse PIN_POWER_SELECTION 1 --do-not-confirm
fi

echo "Done."
