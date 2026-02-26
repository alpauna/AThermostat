#!/bin/bash
# Upload cert.pem and key.pem to device LittleFS via FTP
# Usage: ./upload-certs.sh
# Run generate-cert.sh first to create the certificate files.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."
CERT_FILE="$PROJECT_DIR/cert.pem"
KEY_FILE="$PROJECT_DIR/key.pem"
FTP_USER="admin"
FTP_PASS=""

if [ ! -f "$CERT_FILE" ] || [ ! -f "$KEY_FILE" ]; then
    echo "Error: cert.pem and/or key.pem not found in project root."
    echo "Run ./scripts/generate-cert.sh first."
    exit 1
fi

read -rp "Device IP: " DEVICE_IP
if [ -z "$DEVICE_IP" ]; then
    echo "Error: IP address required"
    exit 1
fi

read -rsp "Admin password (blank if none set): " ADMIN_PW
echo

# Try HTTPS first, fall back to HTTP
BASE_URL="https://$DEVICE_IP"
CURL_OPTS="-sk"
AUTH_OPTS=""
if [ -n "$ADMIN_PW" ]; then
    AUTH_OPTS="-u admin:$ADMIN_PW"
fi

# Verify device is reachable
echo "Checking device at $DEVICE_IP..."
HEAP=$(curl $CURL_OPTS --connect-timeout 5 $AUTH_OPTS "$BASE_URL/heap" 2>/dev/null) || true
if [ -z "$HEAP" ]; then
    BASE_URL="http://$DEVICE_IP"
    HEAP=$(curl $CURL_OPTS --connect-timeout 5 $AUTH_OPTS "$BASE_URL/heap" 2>/dev/null) || true
fi
if [ -z "$HEAP" ]; then
    echo "Error: Could not reach device at $DEVICE_IP"
    exit 1
fi
echo "Device online."

# Enable FTP for 10 minutes
echo "Enabling FTP for 10 minutes..."
RESP=$(curl $CURL_OPTS $AUTH_OPTS \
    -X POST "$BASE_URL/ftp" \
    -H "Content-Type: application/json" \
    -d '{"duration":10}')

if echo "$RESP" | grep -q '"error"'; then
    echo "Error: $RESP"
    exit 1
fi
echo "FTP enabled."

# Get FTP password from status endpoint
FTP_STATUS=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/ftp" 2>/dev/null)
FTP_PASS=$(echo "$FTP_STATUS" | grep -o '"password":"[^"]*"' | cut -d'"' -f4)
if [ -z "$FTP_PASS" ]; then
    FTP_PASS="admin"
fi
echo "FTP password: $FTP_PASS"

sleep 2

# Upload cert.pem
echo "Uploading cert.pem..."
curl -s -T "$CERT_FILE" \
    --user "$FTP_USER:$FTP_PASS" \
    "ftp://$DEVICE_IP/cert.pem"

CERT_SIZE=$(stat -c%s "$CERT_FILE" 2>/dev/null || stat -f%z "$CERT_FILE" 2>/dev/null)
echo "Uploaded cert.pem ($CERT_SIZE bytes)"

# Upload key.pem
echo "Uploading key.pem..."
curl -s -T "$KEY_FILE" \
    --user "$FTP_USER:$FTP_PASS" \
    "ftp://$DEVICE_IP/key.pem"

KEY_SIZE=$(stat -c%s "$KEY_FILE" 2>/dev/null || stat -f%z "$KEY_FILE" 2>/dev/null)
echo "Uploaded key.pem ($KEY_SIZE bytes)"

# Disable FTP
curl $CURL_OPTS $AUTH_OPTS \
    -X POST "$BASE_URL/ftp" \
    -H "Content-Type: application/json" \
    -d '{"duration":0}' >/dev/null 2>&1 || true

echo
echo "Certificates uploaded. Reboot the device to enable HTTPS."
read -rp "Reboot now? [y/N] " REBOOT
if [ "$REBOOT" = "y" ] || [ "$REBOOT" = "Y" ]; then
    curl $CURL_OPTS $AUTH_OPTS -X POST "$BASE_URL/reboot" >/dev/null 2>&1 || true
    echo "Reboot initiated."
fi
