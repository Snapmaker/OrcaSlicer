# Patch vendor cmake files to use local source dirs
# Called by gitlab_build_release_vs2022.bat
# Usage: powershell -File gitlab_patch_cmake.ps1 <depsDir> <wxSourceDir> <sentrySourceDir>

param(
    [Parameter(Mandatory = $true)][string]$DepsDir,
    [Parameter(Mandatory = $true)][string]$WxSourceDir,
    [Parameter(Mandatory = $true)][string]$SentrySourceDir
)

$ErrorActionPreference = 'Stop'

function Write-CMakeFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Content
    )
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

$wxFile = Join-Path $DepsDir 'wxWidgets\wxWidgets.cmake'
$sentryFile = Join-Path $DepsDir 'Sentry\Sentry.cmake'
$openvdbFile = Join-Path $DepsDir 'OpenVDB\OpenVDB.cmake'

foreach ($f in @($wxFile, $sentryFile, $openvdbFile)) {
    if (-not (Test-Path -LiteralPath $f)) {
        throw "Missing cmake file: $f"
    }
}

# wxWidgets: GIT_REPOSITORY -> SOURCE_DIR, remove GIT_SHALLOW
$c = Get-Content -LiteralPath $wxFile -Raw
$c = $c -replace 'GIT_REPOSITORY "https://github\.com/SoftFever/Orca-deps-wxWidgets"', ('SOURCE_DIR "' + $WxSourceDir + '"')
$c = $c -replace '\r?\n\s*GIT_SHALLOW ON', ''
Write-CMakeFile -Path $wxFile -Content $c
Write-Host "  wxWidgets -> SOURCE_DIR"

# Sentry: GIT_REPOSITORY -> SOURCE_DIR, remove GIT_TAG + GIT_SHALLOW
$c = Get-Content -LiteralPath $sentryFile -Raw
$c = $c -replace 'GIT_REPOSITORY\s+https://github\.com/getsentry/sentry-native\.git', ('SOURCE_DIR "' + $SentrySourceDir + '"')
$c = $c -replace '\r?\n\s*GIT_TAG\s+[\d.]+', ''
$c = $c -replace '\r?\n\s*GIT_SHALLOW\s+ON', ''
Write-CMakeFile -Path $sentryFile -Content $c
Write-Host "  Sentry    -> SOURCE_DIR"

# OpenVDB: disable vdb_print tool (links tbb12.lib which causes LNK1104)
$c = Get-Content -LiteralPath $openvdbFile -Raw
$c = $c -replace '-DOPENVDB_BUILD_VDB_PRINT=ON', '-DOPENVDB_BUILD_VDB_PRINT=OFF'
Write-CMakeFile -Path $openvdbFile -Content $c
Write-Host "  OpenVDB   -> VDB_PRINT=OFF"
