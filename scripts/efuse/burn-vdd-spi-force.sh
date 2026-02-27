#!/bin/bash
# Burn VDD_SPI_FORCE eFuse — forces VDD_SPI voltage from eFuse, frees GPIO45
# PERMANENT: Cannot be undone. GPIO45 will no longer be a VDD_SPI strapping pin.
#
# You should also burn VDD_SPI_XPD (regulator power up) and VDD_SPI_TIEH (voltage
# select: 0=1.8V, 1=3.3V) to set the desired VDD_SPI voltage.
#
# For most setups with 3.3V flash: burn VDD_SPI_FORCE=1, VDD_SPI_XPD=1, VDD_SPI_TIEH=1
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: VDD_SPI_FORCE ==="
echo "This will PERMANENTLY force VDD_SPI voltage from eFuse settings."
echo "GPIO45 will be freed as a regular GPIO."
echo "Port: $PORT"
echo
echo "Current VDD_SPI eFuses will also be set for 3.3V operation."
echo "  VDD_SPI_FORCE = 1 (force from eFuse)"
echo "  VDD_SPI_XPD   = 1 (power up internal regulator)"
echo "  VDD_SPI_TIEH  = 1 (3.3V)"
echo
echo "WARNING: This is irreversible! Incorrect voltage may damage flash."
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse VDD_SPI_FORCE 1 --do-not-confirm
python3 "$ESPEFUSE" --port "$PORT" burn_efuse VDD_SPI_XPD 1 --do-not-confirm
python3 "$ESPEFUSE" --port "$PORT" burn_efuse VDD_SPI_TIEH 1 --do-not-confirm
echo "Done. VDD_SPI_FORCE, VDD_SPI_XPD, VDD_SPI_TIEH have been burned."
echo "GPIO45 is now a regular GPIO."
