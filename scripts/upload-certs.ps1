# Upload cert.pem and key.pem to device LittleFS via FTP
# Usage: .\scripts\upload-certs.ps1
# Run generate-cert.ps1 first to create the certificate files.

$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $PSScriptRoot
$CertFile = Join-Path $ProjectDir "cert.pem"
$KeyFile = Join-Path $ProjectDir "key.pem"
$FtpUser = "admin"

if (!(Test-Path $CertFile) -or !(Test-Path $KeyFile)) {
    Write-Error "cert.pem and/or key.pem not found in project root. Run generate-cert.ps1 first."
    exit 1
}

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrEmpty($DeviceIP)) {
    Write-Error "IP address required"; exit 1
}

$secure = Read-Host "Admin password (blank if none set)" -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
$AdminPW = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)

$BaseURL = "https://$DeviceIP"
$CurlBase = @("-sk")
if ($AdminPW) { $CurlBase += @("-u", "admin:$AdminPW") }

# Verify device is reachable
Write-Host "Checking device at $DeviceIP..."
$heap = & curl @CurlBase --connect-timeout 5 "$BaseURL/heap" 2>$null
if ([string]::IsNullOrEmpty($heap)) {
    $BaseURL = "http://$DeviceIP"
    $heap = & curl @CurlBase --connect-timeout 5 "$BaseURL/heap" 2>$null
}
if ([string]::IsNullOrEmpty($heap)) {
    Write-Error "Could not reach device at $DeviceIP"; exit 1
}
Write-Host "Device online."

# Enable FTP for 10 minutes
Write-Host "Enabling FTP for 10 minutes..."
$resp = & curl @CurlBase -X POST "$BaseURL/ftp" -H "Content-Type: application/json" -d '{"duration":10}' 2>$null
if ($resp -match '"error"') {
    Write-Error "Error: $resp"; exit 1
}
Write-Host "FTP enabled."

# Get FTP password
$ftpStatus = & curl @CurlBase "$BaseURL/ftp" 2>$null | ConvertFrom-Json
$FtpPass = if ($ftpStatus.password) { $ftpStatus.password } else { "admin" }
Write-Host "FTP password: $FtpPass"

Start-Sleep -Seconds 2

# Upload cert.pem
Write-Host "Uploading cert.pem..."
& curl -s -T $CertFile --user "${FtpUser}:${FtpPass}" "ftp://${DeviceIP}/cert.pem"
$certSize = (Get-Item $CertFile).Length
Write-Host "Uploaded cert.pem ($certSize bytes)"

# Upload key.pem
Write-Host "Uploading key.pem..."
& curl -s -T $KeyFile --user "${FtpUser}:${FtpPass}" "ftp://${DeviceIP}/key.pem"
$keySize = (Get-Item $KeyFile).Length
Write-Host "Uploaded key.pem ($keySize bytes)"

# Disable FTP
try {
    & curl @CurlBase -X POST "$BaseURL/ftp" -H "Content-Type: application/json" -d '{"duration":0}' 2>$null | Out-Null
} catch { }

Write-Host ""
Write-Host "Certificates uploaded. Reboot the device to enable HTTPS."
$reboot = Read-Host "Reboot now? [y/N]"
if ($reboot -eq "y" -or $reboot -eq "Y") {
    try { & curl @CurlBase -X POST "$BaseURL/reboot" 2>$null | Out-Null } catch { }
    Write-Host "Reboot initiated."
}
