#!/bin/bash
# Generate self-signed ECC P-256 certificate for ESP32 HTTPS server
# Usage: ./generate-cert.sh [system-name]
# Output files are written to the project root directory.
# Upload cert.pem and key.pem to device LittleFS via FTP after generation.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."

CN="${1:-AThermostat}"

openssl req -x509 \
    -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes \
    -keyout "$PROJECT_DIR/key.pem" \
    -out "$PROJECT_DIR/cert.pem" \
    -days 3650 \
    -subj "/CN=$CN"

echo "Generated $PROJECT_DIR/cert.pem and $PROJECT_DIR/key.pem (CN=$CN)"
echo "Upload both files to device LittleFS root via FTP (use upload-certs.sh)."
