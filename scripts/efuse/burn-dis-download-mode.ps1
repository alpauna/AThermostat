# Burn DIS_DOWNLOAD_MODE eFuse - disables UART download boot mode
# PERMANENT: Cannot be undone. You will NOT be able to flash via serial after this.
# Only use if OTA updates are fully working and reliable.

param([string]$Port = "COM3")

$ErrorActionPreference = "Stop"
$EspEfuse = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\espefuse.py"

Write-Host "=== Burn eFuse: DIS_DOWNLOAD_MODE ==="
Write-Host "This will PERMANENTLY disable UART download/flash mode."
Write-Host "Port: $Port"
Write-Host ""
Write-Host "!!! DANGER: After burning, you can ONLY update firmware via OTA. !!!"
Write-Host "!!! If OTA breaks, the device becomes unrecoverable.            !!!"
Write-Host ""
Write-Host "WARNING: This is irreversible!"
$confirm = Read-Host "Type 'BURN' to confirm"
if ($confirm -ne "BURN") { Write-Host "Aborted."; exit 1 }

python $EspEfuse --port $Port burn_efuse DIS_DOWNLOAD_MODE 1 --do-not-confirm
Write-Host "Done. DIS_DOWNLOAD_MODE has been burned. Serial flashing is now disabled."
