#!/bin/bash
# Read all eFuse values from the ESP32-S3
set -e

PORT="${1:-/dev/ttyUSB0}"
ESPEFUSE="$HOME/.platformio/packages/tool-esptoolpy/espefuse.py"

echo "=== Reading eFuses from $PORT ==="
python3 "$ESPEFUSE" --port "$PORT" summary
