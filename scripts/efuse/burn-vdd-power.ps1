# Burn VDD_SPI and power-related eFuses
# Affects: GPIO45 (VDD_SPI strapping), GPIO33-37 (power source)
# PERMANENT: Cannot be undone.

param([string]$Port = "COM3")

$ErrorActionPreference = "Stop"
$EspEfuse = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\espefuse.py"

Write-Host "=== Burn eFuses: VDD / Power Configuration ==="
Write-Host "Port: $Port"
Write-Host ""
Write-Host "This script configures power-related eFuses."
Write-Host "Burning VDD_SPI_FORCE frees GPIO45 as a regular GPIO."
Write-Host ""

# --- VDD_SPI_FORCE ---
Write-Host "VDD_SPI_FORCE: Force VDD_SPI voltage from eFuse instead of GPIO45 strapping."
Write-Host "  This frees GPIO45 for use as a regular GPIO."
$ans = Read-Host "  Burn VDD_SPI_FORCE? [Y/n]"
$VDD_SPI_FORCE = $ans -notmatch '^[Nn]'

# --- VDD_SPI_XPD ---
Write-Host ""
Write-Host "VDD_SPI_XPD: Power up the internal VDD_SPI regulator."
Write-Host "  Required if VDD_SPI_FORCE is set. Most boards need this ON."
$ans = Read-Host "  Burn VDD_SPI_XPD? [Y/n]"
$VDD_SPI_XPD = $ans -notmatch '^[Nn]'

# --- VDD_SPI_TIEH ---
Write-Host ""
Write-Host "VDD_SPI_TIEH: Select VDD_SPI voltage level."
Write-Host "  0 = 1.8V, 1 = 3.3V"
Write-Host "  Most ESP32-S3 boards with standard flash use 3.3V."
$volt = Read-Host "  VDD_SPI voltage [3.3V/1.8V] (default 3.3V)"
$VDD_SPI_TIEH = $volt -ne "1.8V" -and $volt -ne "1.8"

# --- PIN_POWER_SELECTION ---
Write-Host ""
Write-Host "PIN_POWER_SELECTION: Power source for GPIO33-37."
Write-Host "  0 = VDD_SPI (follows VDD_SPI voltage above)"
Write-Host "  1 = VDD3P3_CPU (always 3.3V from CPU rail)"
Write-Host "  Recommended: 1 (VDD3P3_CPU) for consistent 3.3V on GPIO33-37."
$ans = Read-Host "  Use VDD3P3_CPU for GPIO33-37? [Y/n]"
$PIN_POWER = $ans -notmatch '^[Nn]'

# --- Summary ---
Write-Host ""
Write-Host "=== Summary ==="
Write-Host "  VDD_SPI_FORCE:       $(if($VDD_SPI_FORCE){'BURN'}else{'skip'})"
Write-Host "  VDD_SPI_XPD:         $(if($VDD_SPI_XPD){'BURN'}else{'skip'})"
Write-Host "  VDD_SPI_TIEH:        $(if($VDD_SPI_TIEH){'BURN (3.3V)'}else{'skip (1.8V)'})"
Write-Host "  PIN_POWER_SELECTION: $(if($PIN_POWER){'BURN (VDD3P3_CPU)'}else{'skip (VDD_SPI)'})"
Write-Host ""
Write-Host "WARNING: These eFuse burns are PERMANENT and cannot be undone!"
Write-Host "Incorrect voltage settings may damage flash or PSRAM."
$confirm = Read-Host "Type 'BURN' to confirm"
if ($confirm -ne "BURN") { Write-Host "Aborted."; exit 1 }

if ($VDD_SPI_FORCE) {
    Write-Host "Burning VDD_SPI_FORCE..."
    python $EspEfuse --port $Port burn_efuse VDD_SPI_FORCE 1 --do-not-confirm
}
if ($VDD_SPI_XPD) {
    Write-Host "Burning VDD_SPI_XPD..."
    python $EspEfuse --port $Port burn_efuse VDD_SPI_XPD 1 --do-not-confirm
}
if ($VDD_SPI_TIEH) {
    Write-Host "Burning VDD_SPI_TIEH (3.3V)..."
    python $EspEfuse --port $Port burn_efuse VDD_SPI_TIEH 1 --do-not-confirm
}
if ($PIN_POWER) {
    Write-Host "Burning PIN_POWER_SELECTION (VDD3P3_CPU)..."
    python $EspEfuse --port $Port burn_efuse PIN_POWER_SELECTION 1 --do-not-confirm
}

Write-Host "Done."
