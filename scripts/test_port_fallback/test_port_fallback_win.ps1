# Test: verify OrcaSlicer handles port 13619 being occupied (Windows)
# Usage: .\scripts\test_port_fallback\test_port_fallback_win.ps1
#        or specify app path: .\scripts\test_port_fallback\test_port_fallback_win.ps1 "C:\path\to\orca-slicer.exe"

param(
    [string]$AppPath = ""
)

$ErrorActionPreference = "Stop"
$TargetPort = 13619

# Determine project root: go up 2 levels from script location
$ProjectRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

Write-Host "=== Port $TargetPort Occupancy Test (Windows) ===" -ForegroundColor Cyan
Write-Host ""

# 1. Check port status before test
Write-Host "[1/4] Checking port $TargetPort..."
$existing = netstat -ano | Select-String ":$TargetPort "
if ($existing) {
    Write-Host "  WARNING: port $TargetPort is already in use:" -ForegroundColor Yellow
    $existing | ForEach-Object { Write-Host "    $_" }
} else {
    Write-Host "  OK: port $TargetPort is free"
}

# 2. Occupy port on all interfaces to match OrcaSlicer's 0.0.0.0 binding
Write-Host "[2/4] Occupying port $TargetPort..."
try {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $TargetPort)
    $listener.Start()
    Write-Host "  OK: port $TargetPort held by PowerShell listener"
} catch {
    Write-Host "  ERROR: failed to occupy port $TargetPort : $_" -ForegroundColor Red
    exit 1
}

# 3. Launch OrcaSlicer
Write-Host "[3/4] Launching OrcaSlicer..."
$foundPath = ""
if ($AppPath -and (Test-Path $AppPath)) {
    $foundPath = $AppPath
} else {
    $candidates = @(
        "$ProjectRoot\build\Snapmaker_Orca\snapmaker-orca.exe",
        "$ProjectRoot\build\OrcaSlicer_apprel.exe",
        "$ProjectRoot\build\src\Release\orca-slicer.exe"
    )
    $foundPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

if ($foundPath) {
    Start-Process -FilePath $foundPath
    Write-Host "  App launched from: $foundPath"
} else {
    Write-Host "  Could not find orca-slicer.exe automatically." -ForegroundColor Yellow
    Write-Host "  Usage: .\$($MyInvocation.MyCommand.Name) 'C:\path\to\orca-slicer.exe'"
}
Write-Host "  Check OrcaSlicer logs for:"
Write-Host "    'Original port $TargetPort is in use, switching to port XXXX'"

# 4. Wait for user to test, then cleanup
Write-Host ""
Write-Host "[4/4] Press Enter after testing to release port and cleanup..."
Read-Host | Out-Null

$listener.Stop()
Write-Host "  Port $TargetPort released."
Write-Host "=== Test complete ==="
