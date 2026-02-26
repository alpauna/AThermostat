# Restore config.txt to device LittleFS from a local backup
# Usage:
#   .\scripts\restore-config.ps1                              — pick from available backups
#   .\scripts\restore-config.ps1 -File backups\config-latest.txt  — restore a specific file

param(
    [string]$File = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BackupDir = Join-Path (Split-Path -Parent $ScriptDir) "backups"

$FtpUser = "admin"
$FtpPass = ""

# --- Determine which file to restore ---

$ConfigFile = ""
if ($File -ne "") {
    if (-not (Test-Path $File)) {
        Write-Host "Error: File not found: $File"
        exit 1
    }
    $ConfigFile = (Resolve-Path $File).Path
} else {
    if (-not (Test-Path $BackupDir)) {
        Write-Host "Error: No backups directory found. Run backup-config.ps1 first."
        exit 1
    }

    Write-Host "Available backups:"
    Write-Host ""

    $backupFiles = @()

    # Latest copy
    $latestFile = Join-Path $BackupDir "config-latest.txt"
    if (Test-Path $latestFile) {
        $size = (Get-Item $latestFile).Length
        $backupFiles += $latestFile
        Write-Host ("  [{0}] config-latest.txt ({1} bytes)" -f $backupFiles.Count, $size)
    }

    # Timestamped backups (newest first)
    $dirs = Get-ChildItem -Path $BackupDir -Directory | Where-Object { $_.Name -match '^\d{8}-\d{6}$' } | Sort-Object Name -Descending
    foreach ($dir in $dirs) {
        $cfg = Join-Path $dir.FullName "config.txt"
        if (Test-Path $cfg) {
            $size = (Get-Item $cfg).Length
            $backupFiles += $cfg
            Write-Host ("  [{0}] {1}\config.txt ({2} bytes)" -f $backupFiles.Count, $dir.Name, $size)
        }
    }

    if ($backupFiles.Count -eq 0) {
        Write-Host "  (none found)"
        Write-Host ""
        Write-Host "Run backup-config.ps1 first, or specify a file with -File."
        exit 1
    }

    Write-Host ""
    $choice = Read-Host "Select backup to restore [1-$($backupFiles.Count)]"
    $idx = 0
    if (-not [int]::TryParse($choice, [ref]$idx) -or $idx -lt 1 -or $idx -gt $backupFiles.Count) {
        Write-Host "Invalid selection."
        exit 1
    }
    $ConfigFile = $backupFiles[$idx - 1]
}

$fileSize = (Get-Item $ConfigFile).Length
Write-Host ""
Write-Host "Restoring: $ConfigFile ($fileSize bytes)"

# Validate JSON
try {
    Get-Content $ConfigFile -Raw | ConvertFrom-Json | Out-Null
} catch {
    Write-Host "Warning: File does not appear to be valid JSON."
    $cont = Read-Host "Continue anyway? [y/N]"
    if ($cont -ne "y" -and $cont -ne "Y") {
        Write-Host "Cancelled."
        exit 0
    }
}

# --- Connect to device ---

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrWhiteSpace($DeviceIP)) {
    Write-Host "Error: IP address required"
    exit 1
}

$AdminPwSecure = Read-Host "Admin password (blank if none set)" -AsSecureString
$AdminPw = [System.Runtime.InteropServices.Marshal]::PtrToStringAuto(
    [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($AdminPwSecure))

$BaseUrl = "https://$DeviceIP"
$CurlBase = @("-sk")
if ($AdminPw -ne "") { $CurlBase += @("-u", "admin:$AdminPw") }

# Verify device is reachable
Write-Host "Checking device at $DeviceIP..."
$heap = & curl @CurlBase --connect-timeout 5 "$BaseUrl/heap" 2>$null
if ([string]::IsNullOrEmpty($heap)) {
    $BaseUrl = "http://$DeviceIP"
    $heap = & curl @CurlBase --connect-timeout 5 "$BaseUrl/heap" 2>$null
}
if ([string]::IsNullOrEmpty($heap)) {
    Write-Host "Error: Could not reach device at $DeviceIP"
    exit 1
}
Write-Host "Device online."

# Enable FTP for 10 minutes
Write-Host "Enabling FTP for 10 minutes..."
$resp = & curl @CurlBase -X POST "$BaseUrl/ftp" -H "Content-Type: application/json" -d '{"duration":10}' 2>$null
if ($resp -match '"error"') {
    Write-Host "Error: $resp"
    exit 1
}
Write-Host "FTP enabled."

# Get FTP password
$ftpStatus = & curl @CurlBase "$BaseUrl/ftp" 2>$null | ConvertFrom-Json
$FtpPass = if ($ftpStatus.password) { $ftpStatus.password } else { "admin" }
Write-Host "FTP password: $FtpPass"

Start-Sleep -Seconds 2

# Upload config.txt via FTP
Write-Host "Uploading config.txt to device..."
& curl -s -T $ConfigFile --user "${FtpUser}:${FtpPass}" "ftp://${DeviceIP}/config.txt"

# Verify upload
Write-Host "Verifying upload..."
try {
    $tempFile = [System.IO.Path]::GetTempFileName()
    & curl -s -o $tempFile --user "${FtpUser}:${FtpPass}" "ftp://${DeviceIP}/config.txt" 2>$null
    $remoteSize = (Get-Item $tempFile).Length
    Remove-Item $tempFile -Force
    if ($remoteSize -eq $fileSize) {
        Write-Host "Verified: $remoteSize bytes on device matches local file."
    } else {
        Write-Host "Warning: Size mismatch - local $fileSize bytes, device $remoteSize bytes"
    }
} catch {
    Write-Host "Warning: Could not verify upload"
}

# Disable FTP
try {
    & curl @CurlBase -X POST "$BaseUrl/ftp" -H "Content-Type: application/json" -d '{"duration":0}' 2>$null | Out-Null
} catch { }

Write-Host ""
Write-Host "Config restored. Reboot the device to load the new configuration."
$reboot = Read-Host "Reboot now? [y/N]"
if ($reboot -eq "y" -or $reboot -eq "Y") {
    try { & curl @CurlBase -X POST "$BaseUrl/reboot" 2>$null | Out-Null } catch { }
    Write-Host "Reboot initiated."
}
