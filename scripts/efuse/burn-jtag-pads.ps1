# Burn JTAG pad eFuses (GPIO39-42)
# Affects: GPIO39, GPIO40, GPIO41, GPIO42
# PERMANENT: Cannot be undone.

param([string]$Port = "COM3")

$ErrorActionPreference = "Stop"
$EspEfuse = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\espefuse.py"

Write-Host "=== Burn eFuses: JTAG Pads (GPIO39-42) ==="
Write-Host "Port: $Port"
Write-Host ""
Write-Host "This script configures JTAG pad eFuses on GPIO39-42."
Write-Host "Disabling pad JTAG frees these pins for general use."
Write-Host ""

# --- SOFT_DIS_JTAG ---
Write-Host "SOFT_DIS_JTAG: Software-disable JTAG."
Write-Host "  Can be re-enabled at runtime via HMAC if configured."
Write-Host "  Less permanent than HARD_DIS_JTAG - good for development."
$ans = Read-Host "  Burn SOFT_DIS_JTAG? [y/N]"
$SOFT_DIS = $ans -match '^[Yy]'

# --- HARD_DIS_JTAG ---
Write-Host ""
Write-Host "HARD_DIS_JTAG: Permanently disable pad JTAG."
Write-Host "  Cannot be re-enabled. You will lose JTAG debug on GPIO39-42."
Write-Host "  Recommended for production devices."
$ans = Read-Host "  Burn HARD_DIS_JTAG? [y/N]"
$HARD_DIS = $ans -match '^[Yy]'

# --- Summary ---
Write-Host ""
Write-Host "=== Summary ==="
Write-Host "  SOFT_DIS_JTAG: $(if($SOFT_DIS){'BURN'}else{'skip'})"
Write-Host "  HARD_DIS_JTAG: $(if($HARD_DIS){'BURN'}else{'skip'})"
Write-Host ""
Write-Host "WARNING: These eFuse burns are PERMANENT and cannot be undone!"
$confirm = Read-Host "Type 'BURN' to confirm"
if ($confirm -ne "BURN") { Write-Host "Aborted."; exit 1 }

if ($SOFT_DIS) {
    Write-Host "Burning SOFT_DIS_JTAG..."
    python $EspEfuse --port $Port burn_efuse SOFT_DIS_JTAG 1 --do-not-confirm
}
if ($HARD_DIS) {
    Write-Host "Burning HARD_DIS_JTAG..."
    python $EspEfuse --port $Port burn_efuse HARD_DIS_JTAG 1 --do-not-confirm
}

Write-Host "Done."
