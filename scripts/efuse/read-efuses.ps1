# Read all eFuse values from the ESP32-S3

param([string]$Port = "COM3")

$EspEfuse = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\espefuse.py"

Write-Host "=== Reading eFuses from $Port ==="
python $EspEfuse --port $Port summary
