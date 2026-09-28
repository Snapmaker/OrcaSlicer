# Build UltraNet - EdgeSlicer's private, clean-room Bambu network plug-in - for Windows (MSVC, x64)
# and stage the binaries the Windows package ships (ULTRANET_BIN_DIR, see CMakeLists.txt's install
# rules and build_release_vs2022.bat).
#
#   pwsh scripts/build_ultranet_windows.ps1 <ultranet_checkout> <out_dir>
#
# The Windows counterpart of scripts/build_ultranet_posix.sh, with the same rules because the
# UltraNet source is private and this repository and its CI logs are public:
#   * compiler output goes to a log file under ULTRANET_LOG_DIR and is never printed - on a failure
#     only the file names and line numbers of the errors are shown, never the message or the text;
#   * the build tree is deleted when the script ends (the checkout is the caller's to delete);
#   * <out_dir> receives bambu_networking.dll, BambuSource.dll and the ultranet.txt marker only.
#
# UltraNet is built standalone (its own top-level CMake project, -DEDGESLICER_SRC_DIR pointing at
# this checkout for the plug-in ABI header and the paho sources) with the same MSVC toolset as the
# app in the same job, against the OpenSSL of the app's own deps prefix (static libs).
#
# Environment (all optional):
#   EDGESLICER_SRC        this checkout (default: the repository this script lives in)
#   ULTRANET_DEPS_PREFIX  the deps prefix (default <src>/deps/build/OrcaSlicer_dep/usr/local)
#   ULTRANET_GENERATOR    CMake generator (default "Visual Studio 17 2022", -A x64 is added for VS)
#   EDGESLICER_VERSION    written into the marker (default: read from common_func.hpp)
#   ULTRANET_LOG_DIR      where the build logs go (default: a temp dir, removed on exit)
param(
    [Parameter(Mandatory = $true)][string]$UltraNetSrc,
    [Parameter(Mandatory = $true)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Say([string]$msg) { Write-Host "[ultranet] $msg" }

$UltraNetSrc = (Resolve-Path -LiteralPath $UltraNetSrc).Path
$here = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$src = if ($env:EDGESLICER_SRC) { $env:EDGESLICER_SRC } else { $here }
$src = (Resolve-Path -LiteralPath $src).Path
$prefix = if ($env:ULTRANET_DEPS_PREFIX) { $env:ULTRANET_DEPS_PREFIX } else { Join-Path $src 'deps/build/OrcaSlicer_dep/usr/local' }
$generator = if ($env:ULTRANET_GENERATOR) { $env:ULTRANET_GENERATOR } else { 'Visual Studio 17 2022' }

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("ultranet_build_" + [System.Guid]::NewGuid().ToString('N').Substring(0, 10))
$ownLogs = -not $env:ULTRANET_LOG_DIR
$logDir = if ($ownLogs) { Join-Path $work 'logs' } else { $env:ULTRANET_LOG_DIR }
$build = Join-Path $work 'b_win'
New-Item -ItemType Directory -Force -Path $logDir, $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

# Only the file name and line of each error, never the source text or the compiler's message.
function Report-Failure([string]$log, [string]$what) {
    Say "$what FAILED (full log kept out of the CI output)"
    $lines = Get-Content -LiteralPath $log -ErrorAction SilentlyContinue
    if (-not $lines) { $lines = @() }
    $errs = @($lines | Where-Object { $_ -match '(fatal )?error [A-Z]+[0-9]+|: error:|CMake Error' })
    Say "error lines: $($errs.Count); locations:"
    $locs = foreach ($l in $errs) {
        if ($l -match '([A-Za-z0-9_.\-]+\.(cpp|hpp|h|c))\(([0-9]+)') { "$($matches[1]):$($matches[3])" }
        elseif ($l -match '([A-Za-z0-9_.\-]+\.(cpp|hpp|h|c)):([0-9]+)') { "$($matches[1]):$($matches[3])" }
        elseif ($l -match '(LNK[0-9]+|C[0-9]{4})') { "($($matches[1]))" }
    }
    $locs | Sort-Object -Unique | Select-Object -First 40 | ForEach-Object { Write-Host "[ultranet]   $_" }
    $lines | Where-Object { $_ -match 'CMake Error' } | ForEach-Object { ($_ -replace ' at .*$', '') } |
        Select-Object -First 5 | ForEach-Object { Write-Host "[ultranet]   $_" }
}

# Native tools write to stderr; run them through cmd so nothing reaches this console.
function Invoke-Logged([string]$log, [string[]]$argv) {
    $quoted = ($argv | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    & cmd.exe /d /c "$quoted >> `"$log`" 2>&1"
    return $LASTEXITCODE
}

$ok = $false
try {
    if (-not (Test-Path -LiteralPath (Join-Path $UltraNetSrc 'CMakeLists.txt'))) { throw "no UltraNet CMakeLists.txt in the checkout" }
    if (-not (Test-Path -LiteralPath (Join-Path $prefix 'lib/libcrypto.lib'))) { throw "deps prefix has no OpenSSL: $prefix" }

    $unSha = 'unknown'
    try { $unSha = (& git -C $UltraNetSrc rev-parse --short=10 HEAD 2>$null).Trim() } catch { }
    if (-not $unSha) { $unSha = 'unknown' }
    $version = $env:EDGESLICER_VERSION
    if (-not $version) {
        $hdr = Get-Content -Raw -LiteralPath (Join-Path $src 'src/common_func/common_func.hpp')
        if ($hdr -match '#define\s+Snapmaker_VERSION\s+"([^"]+)"') { $version = $matches[1] } else { $version = 'unknown' }
    }

    foreach ($f in 'bambu_networking.dll', 'BambuSource.dll', 'ultranet.txt') {
        Remove-Item -LiteralPath (Join-Path $OutDir $f) -Force -ErrorAction SilentlyContinue
    }

    $log = Join-Path $logDir 'ultranet_win.log'
    Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
    $prefixFwd = $prefix -replace '\\', '/'
    $cfg = @('cmake', '-S', $UltraNetSrc, '-B', $build, '-G', $generator)
    if ($generator -like 'Visual Studio*') { $cfg += @('-A', 'x64') } else { $cfg += @('-DCMAKE_BUILD_TYPE=Release') }
    $cfg += @(
        "-DEDGESLICER_SRC_DIR=$($src -replace '\\', '/')",
        "-DCMAKE_PREFIX_PATH=$prefixFwd",
        "-DOPENSSL_ROOT_DIR=$prefixFwd",
        '-DCMAKE_DISABLE_FIND_PACKAGE_Boost=ON',
        '-DULTRANET_TESTS=ON',
        '-DULTRANET_QUIET_DIAGNOSTICS=ON'
    )
    Say "building against $prefix ($generator)"
    if ((Invoke-Logged $log $cfg) -ne 0) { Report-Failure $log 'configure'; throw 'configure failed' }

    $bld = @('cmake', '--build', $build, '--config', 'Release', '--target', 'bambu_networking', 'BambuSource', 'loadtest')
    if ($generator -like 'Visual Studio*') { $bld += @('--', '/m', '/nologo', '/v:m', '/nodeReuse:false') }
    if ((Invoke-Logged $log $bld) -ne 0) { Report-Failure $log 'build'; throw 'build failed' }

    $bin = if (Test-Path -LiteralPath (Join-Path $build 'Release/bambu_networking.dll')) { Join-Path $build 'Release' } else { $build }
    $net = Join-Path $bin 'bambu_networking.dll'
    $srcDll = Join-Path $bin 'BambuSource.dll'
    $loadtest = Join-Path $bin 'loadtest.exe'
    foreach ($f in $net, $srcDll, $loadtest) {
        if (-not (Test-Path -LiteralPath $f)) { throw "the build produced no $(Split-Path -Leaf $f)" }
    }

    # The module must load through LoadLibrary and answer the version handshake.
    $ltLog = Join-Path $logDir 'loadtest_win.log'
    & cmd.exe /d /c "`"$loadtest`" `"$net`" > `"$ltLog`" 2>&1"
    $lt = Get-Content -LiteralPath $ltLog
    if ($LASTEXITCODE -ne 0 -or -not ($lt -match '^OK')) {
        Say 'loadtest FAILED:'; $lt | Select-Object -First 20 | ForEach-Object { Write-Host "[ultranet]   $_" }
        throw 'loadtest failed'
    }
    Say "loadtest: $(($lt | Where-Object { $_ -match 'get_version' }) -join ' ')"

    Copy-Item -LiteralPath $net, $srcDll -Destination $OutDir -Force

    # Same first line as scripts/build_ultranet_posix.sh and the VM ship pipeline; the host only
    # checks that the marker is there.
    $marker = @(
        "UltraNet network plugin (EdgeSlicer). This marker tells the host the plugin in this folder is EdgeSlicer's own build, not the Bambu CDN download.",
        ("version: ultranet@{0}  built {1}  for EdgeSlicer {2} (windows)" -f $unSha, (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd'), $version)
    )
    [System.IO.File]::WriteAllText((Join-Path $OutDir 'ultranet.txt'), (($marker -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))

    Get-ChildItem -LiteralPath $OutDir -File | Sort-Object Name | ForEach-Object {
        Say ("staged {0}  {1} bytes  sha256={2}" -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower())
    }
    Say "ultranet@$unSha staged in $OutDir"
    $ok = $true
}
finally {
    # The build tree holds object files compiled from the private source: never leave it behind.
    Remove-Item -LiteralPath $build -Recurse -Force -ErrorAction SilentlyContinue
    if ($ownLogs) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    elseif (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
if (-not $ok) { exit 1 }
