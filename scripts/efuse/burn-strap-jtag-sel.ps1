# Burn STRAP_JTAG_SEL eFuse - enables JTAG source selection via GPIO3 strapping
# PERMANENT: Cannot be undone.
# When burned, GPIO3 at boot selects JTAG source: LOW=pad JTAG, HIGH=USB JTAG

param([string]$Port = "COM3")

$ErrorActionPreference = "Stop"
$EspEfuse = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\espefuse.py"

Write-Host "=== Burn eFuse: STRAP_JTAG_SEL ==="
Write-Host "This will PERMANENTLY enable JTAG source selection via GPIO3 strapping."
Write-Host "GPIO3 at boot: LOW=pad JTAG, HIGH=USB JTAG"
Write-Host "Port: $Port"
Write-Host ""
Write-Host "WARNING: This is irreversible!"
$confirm = Read-Host "Type 'BURN' to confirm"
if ($confirm -ne "BURN") { Write-Host "Aborted."; exit 1 }

python $EspEfuse --port $Port burn_efuse STRAP_JTAG_SEL 1 --do-not-confirm
Write-Host "Done. STRAP_JTAG_SEL has been burned."
