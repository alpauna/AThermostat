#!/bin/bash
# Burn DIS_DOWNLOAD_MODE eFuse — disables UART download boot mode
# PERMANENT: Cannot be undone. You will NOT be able to flash via serial after this.
# Only use if OTA updates are fully working and reliable.
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Burn eFuse: DIS_DOWNLOAD_MODE ==="
echo "This will PERMANENTLY disable UART download/flash mode."
echo "Port: $PORT"
echo
echo "!!! DANGER: After burning, you can ONLY update firmware via OTA. !!!"
echo "!!! If OTA breaks, the device becomes unrecoverable.            !!!"
echo
echo "WARNING: This is irreversible!"
read -rp "Type 'BURN' to confirm: " confirm
if [ "$confirm" != "BURN" ]; then
    echo "Aborted."
    exit 1
fi

python3 "$ESPEFUSE" --port "$PORT" burn_efuse DIS_DOWNLOAD_MODE 1 --do-not-confirm
echo "Done. DIS_DOWNLOAD_MODE has been burned. Serial flashing is now disabled."
