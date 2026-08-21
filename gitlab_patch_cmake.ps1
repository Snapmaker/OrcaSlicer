# Patch vendor cmake files to use local source dirs
# Called by gitlab_build_win.bat
# Usage: powershell -File gitlab_patch_cmake.ps1 <depsDir> <wxSourceDir> <sentrySourceDir>

param(
    [string]$DepsDir,
    [string]$WxSourceDir,
    [string]$SentrySourceDir
)

$wxFile = Join-Path $DepsDir 'wxWidgets\wxWidgets.cmake'
$sentryFile = Join-Path $DepsDir 'Sentry\Sentry.cmake'
$openvdbFile = Join-Path $DepsDir 'OpenVDB\OpenVDB.cmake'

# wxWidgets: GIT_REPOSITORY -> SOURCE_DIR, remove GIT_SHALLOW
$c = Get-Content $wxFile -Raw
$c = $c -replace 'GIT_REPOSITORY "https://github\.com/SoftFever/Orca-deps-wxWidgets"', ('SOURCE_DIR "' + $WxSourceDir + '"')
$c = $c -replace '\r?\n\s*GIT_SHALLOW ON', ''
Set-Content $wxFile -Value $c -NoNewline
Write-Host "  wxWidgets -> SOURCE_DIR"

# Sentry: GIT_REPOSITORY -> SOURCE_DIR, remove GIT_TAG + GIT_SHALLOW
$c = Get-Content $sentryFile -Raw
$c = $c -replace 'GIT_REPOSITORY\s+https://github\.com/getsentry/sentry-native\.git', ('SOURCE_DIR "' + $SentrySourceDir + '"')
$c = $c -replace '\r?\n\s*GIT_TAG\s+[\d.]+', ''
$c = $c -replace '\r?\n\s*GIT_SHALLOW\s+ON', ''
Set-Content $sentryFile -Value $c -NoNewline
Write-Host "  Sentry    -> SOURCE_DIR"

# OpenVDB: disable vdb_print tool (links tbb12.lib which causes LNK1104)
$c = Get-Content $openvdbFile -Raw
$c = $c -replace '-DOPENVDB_BUILD_VDB_PRINT=ON', '-DOPENVDB_BUILD_VDB_PRINT=OFF'
Set-Content $openvdbFile -Value $c -NoNewline
Write-Host "  OpenVDB   -> VDB_PRINT=OFF"
