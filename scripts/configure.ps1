# Configure WiFi and MQTT credentials on the device
# Usage:
#   .\scripts\configure.ps1          - prompts for IP and updates via HTTPS API
#   .\scripts\configure.ps1 -Local   - write config.txt for LittleFS
# Requires: curl (built into Windows 10+)

param(
    [switch]$Local
)

$ErrorActionPreference = "Stop"

# --- Helpers ---

function Read-Prompt {
    param([string]$Label, [string]$Default)
    if ($Default) {
        $input = Read-Host "$Label [$Default]"
        if ([string]::IsNullOrEmpty($input)) { return $Default }
        return $input
    } else {
        return Read-Host $Label
    }
}

function Read-Secret {
    param([string]$Label)
    $secure = Read-Host $Label -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

function Validate-SystemName {
    param([string]$Raw)
    # Strip non-alphanumeric/space, truncate to 20, trim
    $cleaned = ($Raw -replace '[^A-Za-z0-9 ]', '')
    if ($cleaned.Length -gt 20) { $cleaned = $cleaned.Substring(0, 20) }
    $cleaned = $cleaned.Trim()
    if ([string]::IsNullOrEmpty($cleaned)) {
        Write-Error "System Name must contain at least one alphanumeric character"
        exit 1
    }
    if ($cleaned -ne $Raw) {
        Write-Host "Note: System Name sanitized to: $cleaned"
    }
    return $cleaned
}

# --- Local config.txt generation ---

if ($Local) {
    Write-Host "=== Generate config.txt for LittleFS ==="
    Write-Host "Passwords will be stored in plaintext and encrypted on first device boot."
    Write-Host ""

    $SysName      = Read-Prompt "System Name (max 20 chars, alphanumeric+spaces)" "AThermostat"
    $SysName      = Validate-SystemName $SysName
    $MqttPrefix   = Read-Prompt "MQTT Topic Prefix" "thermostat"
    $MqttTempTopic = Read-Prompt "HA Temperature Topic" "homeassistant/sensor/average_home_temperature/state"
    $WifiSSID     = Read-Prompt "WiFi SSID"
    $WifiPW       = Read-Secret "WiFi Password"
    $MqttHost     = Read-Prompt "MQTT Host" "192.168.1.1"
    $MqttPort     = Read-Prompt "MQTT Port" "1883"
    $MqttUser     = Read-Prompt "MQTT User" "debian"
    $MqttPW       = Read-Secret "MQTT Password"

    $OutFile = Join-Path $PSScriptRoot "..\data\config.txt"
    $DateTime = Get-Date -Format "MMM dd yyyy HH:mm:ss"

    $config = @{
        project     = "AThermostat"
        created     = $DateTime
        description = "Thermostat controller for Goodman furnace + heatpump."
        system      = @{ name = $SysName; mqttPrefix = $MqttPrefix }
        wifi        = @{ ssid = $WifiSSID; password = $WifiPW; apFallbackSeconds = 600 }
        mqtt        = @{ user = $MqttUser; password = $MqttPW; host = $MqttHost; port = [int]$MqttPort; tempTopic = $MqttTempTopic }
        logging     = @{ maxLogSize = 524288; maxOldLogCount = 3 }
        thermostat  = @{ mode = 0; heatSetpoint = 68.0; coolSetpoint = 76.0; forceFurnace = $false; forceNoHP = $false }
        admin       = @{ password = "" }
    } | ConvertTo-Json -Depth 4

    $config | Set-Content -Path $OutFile -Encoding UTF8
    Write-Host ""
    Write-Host "Written to: $OutFile"
    Write-Host "Flash LittleFS to upload this file to the device."
    exit 0
}

# --- Network config via HTTPS API ---

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrEmpty($DeviceIP)) {
    Write-Error "IP address required"
    exit 1
}

$AdminPW = Read-Secret "Admin password (blank if none set)"

$BaseURL = "https://$DeviceIP"
$CurlBase = @("-sk")
if ($AdminPW) { $CurlBase += @("-u", "admin:$AdminPW") }

# Fetch current config
Write-Host "Fetching current config from $DeviceIP..."
$TempFile = [IO.Path]::GetTempFileName()
& curl @CurlBase "$BaseURL/api/config/load" -o $TempFile 2>$null
if (!(Test-Path $TempFile) -or (Get-Item $TempFile).Length -eq 0) {
    $BaseURL = "http://$DeviceIP"
    & curl @CurlBase "$BaseURL/api/config/load" -o $TempFile 2>$null
}
if (!(Test-Path $TempFile) -or (Get-Item $TempFile).Length -eq 0) {
    Write-Error "Could not reach device at $DeviceIP"
    exit 1
}

try {
    $current = Get-Content $TempFile -Raw | ConvertFrom-Json
} catch {
    Write-Error "Could not reach device at $DeviceIP"
    exit 1
} finally {
    Remove-Item $TempFile -ErrorAction SilentlyContinue
}

$CurSysName      = if ($current.system_name)      { $current.system_name }      else { "AThermostat" }
$CurMqttPrefix   = if ($current.mqtt_prefix)       { $current.mqtt_prefix }       else { "thermostat" }
$CurSSID         = $current.wifi_ssid
$CurMqttHost     = $current.mqtt_host
$CurMqttPort     = $current.mqtt_port
$CurMqttUser     = $current.mqtt_user
$CurMqttTempTopic = $current.mqtt_temp_topic

Write-Host ""
Write-Host "=== Configure Device ==="
Write-Host "Leave blank to keep current value. Passwords always required for changes."
Write-Host ""

$SysName      = Read-Prompt "System Name (max 20 chars, alphanumeric+spaces)" $CurSysName
$SysName      = Validate-SystemName $SysName
$MqttPrefix   = Read-Prompt "MQTT Topic Prefix" $CurMqttPrefix
$MqttTempTopic = Read-Prompt "HA Temperature Topic" $CurMqttTempTopic
$WifiSSID     = Read-Prompt "WiFi SSID" $CurSSID

$WifiPW = Read-Secret "WiFi Password (blank=no change)"

$MqttHost = Read-Prompt "MQTT Host" $CurMqttHost
$MqttPort = Read-Prompt "MQTT Port" $CurMqttPort
$MqttUser = Read-Prompt "MQTT User" $CurMqttUser

$MqttPW = Read-Secret "MQTT Password (blank=no change)"

# Build JSON payload
$payload = @{
    system_name    = $SysName
    mqtt_prefix    = $MqttPrefix
    mqtt_temp_topic = $MqttTempTopic
    wifi_ssid      = $WifiSSID
    mqtt_host      = $MqttHost
    mqtt_port      = [int]$MqttPort
    mqtt_user      = $MqttUser
}
if ($WifiPW) {
    $payload.wifi_password = $WifiPW
}
if ($MqttPW) {
    $payload.mqtt_password = $MqttPW
}

$jsonFile = [IO.Path]::GetTempFileName()
$payload | ConvertTo-Json | Set-Content $jsonFile -Encoding UTF8

Write-Host ""
Write-Host "Saving configuration..."
$resp = & curl @CurlBase -X POST "$BaseURL/api/config/save" -H "Content-Type: application/json" -d "@$jsonFile" 2>$null
Remove-Item $jsonFile -ErrorAction SilentlyContinue

Write-Host "Response: $resp"
