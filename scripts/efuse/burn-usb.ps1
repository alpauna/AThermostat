# Burn USB-related eFuses (GPIO19/20)
# Affects: GPIO19 (USB D-), GPIO20 (USB D+)
# PERMANENT: Cannot be undone.

param([string]$Port = "COM3")

$ErrorActionPreference = "Stop"
$EspEfuse = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\espefuse.py"

Write-Host "=== Burn eFuses: USB Configuration (GPIO19/20) ==="
Write-Host "Port: $Port"
Write-Host ""
Write-Host "This script configures USB-related eFuses on GPIO19/20."
Write-Host "Disabling USB-JTAG and USB Serial JTAG frees these pins"
Write-Host "for general use (e.g. HX710 pressure sensors)."
Write-Host ""

# --- DIS_USB_JTAG ---
Write-Host "DIS_USB_JTAG: Disable USB-JTAG debug function on GPIO19/20."
Write-Host "  Recommended if using GPIO19/20 for other purposes."
$ans = Read-Host "  Burn DIS_USB_JTAG? [Y/n]"
$DIS_USB_JTAG = $ans -notmatch '^[Nn]'

# --- DIS_USB_SERIAL_JTAG ---
Write-Host ""
Write-Host "DIS_USB_SERIAL_JTAG: Disable USB serial JTAG on GPIO19/20."
Write-Host "  Recommended if using GPIO19/20 for other purposes."
$ans = Read-Host "  Burn DIS_USB_SERIAL_JTAG? [Y/n]"
$DIS_USB_SERIAL_JTAG = $ans -notmatch '^[Nn]'

# --- DIS_USB ---
Write-Host ""
Write-Host "DIS_USB: Disable USB OTG function entirely."
Write-Host "  Only needed if you want to fully disable USB OTG peripheral."
$ans = Read-Host "  Burn DIS_USB? [y/N]"
$DIS_USB = $ans -match '^[Yy]'

# --- USB_EXCHG_PINS ---
Write-Host ""
Write-Host "USB_EXCHG_PINS: Swap USB D- (GPIO19) and D+ (GPIO20) assignments."
Write-Host "  Only needed for boards with reversed USB wiring."
$ans = Read-Host "  Burn USB_EXCHG_PINS? [y/N]"
$USB_EXCHG = $ans -match '^[Yy]'

# --- Summary ---
Write-Host ""
Write-Host "=== Summary ==="
Write-Host "  DIS_USB_JTAG:        $(if($DIS_USB_JTAG){'BURN'}else{'skip'})"
Write-Host "  DIS_USB_SERIAL_JTAG: $(if($DIS_USB_SERIAL_JTAG){'BURN'}else{'skip'})"
Write-Host "  DIS_USB:             $(if($DIS_USB){'BURN'}else{'skip'})"
Write-Host "  USB_EXCHG_PINS:      $(if($USB_EXCHG){'BURN'}else{'skip'})"
Write-Host ""
Write-Host "WARNING: These eFuse burns are PERMANENT and cannot be undone!"
$confirm = Read-Host "Type 'BURN' to confirm"
if ($confirm -ne "BURN") { Write-Host "Aborted."; exit 1 }

if ($DIS_USB_JTAG) {
    Write-Host "Burning DIS_USB_JTAG..."
    python $EspEfuse --port $Port burn_efuse DIS_USB_JTAG 1 --do-not-confirm
}
if ($DIS_USB_SERIAL_JTAG) {
    Write-Host "Burning DIS_USB_SERIAL_JTAG..."
    python $EspEfuse --port $Port burn_efuse DIS_USB_SERIAL_JTAG 1 --do-not-confirm
}
if ($DIS_USB) {
    Write-Host "Burning DIS_USB..."
    python $EspEfuse --port $Port burn_efuse DIS_USB 1 --do-not-confirm
}
if ($USB_EXCHG) {
    Write-Host "Burning USB_EXCHG_PINS..."
    python $EspEfuse --port $Port burn_efuse USB_EXCHG_PINS 1 --do-not-confirm
}

Write-Host "Done."
