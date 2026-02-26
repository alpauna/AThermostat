#!/bin/bash
# Configure WiFi and MQTT credentials on the device
# Usage:
#   ./scripts/configure.sh          — prompts for IP and updates via HTTPS API
#   ./scripts/configure.sh --local  — write config.txt for LittleFS

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Prompt helpers ---
prompt() {
    local var="$1" label="$2" default="$3"
    local input
    if [ -n "$default" ]; then
        read -rp "$label [$default]: " input
        eval "$var=\"${input:-$default}\""
    else
        read -rp "$label: " input
        eval "$var=\"$input\""
    fi
}

prompt_secret() {
    local var="$1" label="$2"
    local input
    read -rsp "$label: " input
    echo
    eval "$var=\"$input\""
}

# Validate system name: alphanumeric + spaces only, max 20 chars, non-empty
validate_system_name() {
    local raw="$1"
    # Strip characters that aren't alphanumeric or space
    local cleaned
    cleaned=$(echo "$raw" | sed 's/[^A-Za-z0-9 ]//g')
    # Truncate to 20 chars
    cleaned="${cleaned:0:20}"
    # Trim leading/trailing spaces
    cleaned=$(echo "$cleaned" | sed 's/^ *//;s/ *$//')
    if [ -z "$cleaned" ]; then
        echo "Error: System Name must contain at least one alphanumeric character" >&2
        exit 1
    fi
    if [ "$cleaned" != "$raw" ]; then
        echo "Note: System Name sanitized to: $cleaned" >&2
    fi
    echo "$cleaned"
}

# --- Local config.txt generation ---
write_local_config() {
    echo "=== Generate config.txt for LittleFS ==="
    echo "Passwords will be stored in plaintext and encrypted on first device boot."
    echo

    prompt SYS_NAME "System Name (max 20 chars, alphanumeric+spaces)" "AThermostat"
    SYS_NAME=$(validate_system_name "$SYS_NAME")
    prompt MQTT_PREFIX "MQTT Topic Prefix" "thermostat"
    prompt MQTT_TEMP_TOPIC "HA Temperature Topic" "homeassistant/sensor/average_home_temperature/state"
    prompt WIFI_SSID "WiFi SSID" ""
    prompt_secret WIFI_PW "WiFi Password"
    prompt MQTT_HOST "MQTT Host" "192.168.1.1"
    prompt MQTT_PORT "MQTT Port" "1883"
    prompt MQTT_USER "MQTT User" "debian"
    prompt_secret MQTT_PW "MQTT Password"

    OUT="$SCRIPT_DIR/../data/config.txt"

    cat > "$OUT" << JSONEOF
{
  "project": "AThermostat",
  "created": "$(date '+%b %d %Y %H:%M:%S')",
  "description": "Thermostat controller for Goodman furnace + heatpump.",
  "system": {
    "name": "$SYS_NAME",
    "mqttPrefix": "$MQTT_PREFIX"
  },
  "wifi": {
    "ssid": "$WIFI_SSID",
    "password": "$WIFI_PW",
    "apFallbackSeconds": 600
  },
  "mqtt": {
    "user": "$MQTT_USER",
    "password": "$MQTT_PW",
    "host": "$MQTT_HOST",
    "port": $MQTT_PORT,
    "tempTopic": "$MQTT_TEMP_TOPIC"
  },
  "logging": {
    "maxLogSize": 524288,
    "maxOldLogCount": 3
  },
  "thermostat": {
    "mode": 0,
    "heatSetpoint": 68.0,
    "coolSetpoint": 76.0,
    "forceFurnace": false,
    "forceNoHP": false
  },
  "admin": {
    "password": ""
  }
}
JSONEOF

    echo
    echo "Written to: $OUT"
    echo "Flash LittleFS to upload this file to the device."
    exit 0
}

# --- Network config via HTTPS API ---
if [ "$1" = "--local" ]; then
    write_local_config
fi

read -rp "Device IP: " DEVICE_IP
if [ -z "$DEVICE_IP" ]; then
    echo "Error: IP address required"
    exit 1
fi

read -rsp "Admin password (blank if none set): " ADMIN_PW
echo

BASE_URL="https://$DEVICE_IP"
CURL_OPTS="-sk"
AUTH_OPTS=""
if [ -n "$ADMIN_PW" ]; then
    AUTH_OPTS="-u admin:$ADMIN_PW"
fi

# Fetch current config
echo "Fetching current config from $DEVICE_IP..."
CURRENT=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/api/config/load" 2>/dev/null)
if [ -z "$CURRENT" ]; then
    BASE_URL="http://$DEVICE_IP"
    CURRENT=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/api/config/load" 2>/dev/null)
fi
if [ -z "$CURRENT" ]; then
    echo "Error: Could not reach device at $DEVICE_IP"
    exit 1
fi

# Parse current values for defaults
CUR_SYS_NAME=$(echo "$CURRENT" | grep -o '"system_name":"[^"]*"' | cut -d'"' -f4)
CUR_MQTT_PREFIX=$(echo "$CURRENT" | grep -o '"mqtt_prefix":"[^"]*"' | cut -d'"' -f4)
CUR_SSID=$(echo "$CURRENT" | grep -o '"wifi_ssid":"[^"]*"' | cut -d'"' -f4)
CUR_MQTT_HOST=$(echo "$CURRENT" | grep -o '"mqtt_host":"[^"]*"' | cut -d'"' -f4)
CUR_MQTT_PORT=$(echo "$CURRENT" | grep -o '"mqtt_port":[0-9]*' | cut -d: -f2)
CUR_MQTT_USER=$(echo "$CURRENT" | grep -o '"mqtt_user":"[^"]*"' | cut -d'"' -f4)
CUR_MQTT_TEMP=$(echo "$CURRENT" | grep -o '"mqtt_temp_topic":"[^"]*"' | cut -d'"' -f4)

echo
echo "=== Configure Device ==="
echo "Leave blank to keep current value. Passwords always required for changes."
echo

prompt SYS_NAME "System Name (max 20 chars, alphanumeric+spaces)" "${CUR_SYS_NAME:-AThermostat}"
SYS_NAME=$(validate_system_name "$SYS_NAME")
prompt MQTT_PREFIX "MQTT Topic Prefix" "${CUR_MQTT_PREFIX:-thermostat}"
prompt MQTT_TEMP_TOPIC "HA Temperature Topic" "${CUR_MQTT_TEMP:-homeassistant/sensor/average_home_temperature/state}"
prompt WIFI_SSID "WiFi SSID" "$CUR_SSID"

WIFI_PW=""
read -rsp "WiFi Password (blank=no change): " WIFI_PW
echo

prompt MQTT_HOST "MQTT Host" "$CUR_MQTT_HOST"
prompt MQTT_PORT "MQTT Port" "$CUR_MQTT_PORT"
prompt MQTT_USER "MQTT User" "$CUR_MQTT_USER"

MQTT_PW=""
read -rsp "MQTT Password (blank=no change): " MQTT_PW
echo

# Build JSON payload
JSON="{"
JSON+="\"system_name\":\"$SYS_NAME\""
JSON+=",\"mqtt_prefix\":\"$MQTT_PREFIX\""
JSON+=",\"mqtt_temp_topic\":\"$MQTT_TEMP_TOPIC\""
JSON+=",\"wifi_ssid\":\"$WIFI_SSID\""
if [ -n "$WIFI_PW" ]; then
    JSON+=",\"wifi_password\":\"$WIFI_PW\""
fi
JSON+=",\"mqtt_host\":\"$MQTT_HOST\""
JSON+=",\"mqtt_port\":$MQTT_PORT"
JSON+=",\"mqtt_user\":\"$MQTT_USER\""
if [ -n "$MQTT_PW" ]; then
    JSON+=",\"mqtt_password\":\"$MQTT_PW\""
fi
JSON+="}"

echo
echo "Saving configuration..."
RESP=$(curl $CURL_OPTS $AUTH_OPTS \
    -X POST "$BASE_URL/api/config/save" \
    -H "Content-Type: application/json" \
    -d "$JSON")

echo "Response: $RESP"
