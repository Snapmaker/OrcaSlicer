@REM ============================================================
@REM  gitlab_build_win.bat -- GitLab Internal Build Script (Windows)
@REM
@REM  All source code comes from GitLab. No GitHub dependency at build time.
@REM
@REM  GitLab repos:
@REM    1. snapmaker_orca/orca (main)             -- this repo
@REM    2. snapmaker_orca/snapmaker_orca_vendor   -- deps build system + DL_CACHE
@REM    3. snapmaker_orca/Orca-deps-wxWidgets     -- wxWidgets fork
@REM    4. snapmaker_orca/sentry-native           -- Sentry SDK (crashpad backend)
@REM
@REM  Directory layout (after script runs):
@REM    %ORCA_ROOT%/
@REM    |-- gitlab_build_win.bat
@REM    |-- gitlab_vendor/                -- cloned vendor repo
@REM    |   `-- gitlab_deps/
@REM    |       `-- build/                -- deps build output
@REM    |-- Orca-deps-wxWidgets/          -- cloned wxWidgets (source)
@REM    |-- sentry-native/                -- cloned sentry (source + crashpad)
@REM    |-- gitlab_build/                 -- slicer build output
@REM    |-- gitlab_deps_src/              -- xcopy from vendor/gitlab_deps_src
@REM    `-- deps_src/                     -- original repo version (untouched)
@REM
@REM  Usage:
@REM    gitlab_build_win.bat              -- build deps + slicer
@REM    gitlab_build_win.bat deps         -- build deps only
@REM    gitlab_build_win.bat slicer       -- build slicer only
@REM    gitlab_build_win.bat debug        -- debug build
@REM    gitlab_build_win.bat debuginfo    -- RelWithDebInfo build
@REM
@REM  Environment variables (optional):
@REM    GITLAB_BASE_URL  -- GitLab host (default: gitlab.s.com)
@REM    GITLAB_GROUP     -- GitLab group (default: snapmaker_orca)
@REM ============================================================
@echo off
setlocal ENABLEDELAYEDEXPANSION

REM Ensure Windows-native Strawberry Perl is before msys Perl (required for OpenSSL)
if exist "C:\Strawberry\perl\bin\perl.exe" set "PATH=C:\Strawberry\perl\bin;%PATH%"

REM ============================================================
REM  1. Configure GitLab repository URLs
REM ============================================================
if not defined GITLAB_BASE_URL  set "GITLAB_BASE_URL=gitlab.s.com"
if not defined GITLAB_GROUP     set "GITLAB_GROUP=snapmaker_orca"

set "GITLAB_VENDOR=http://%GITLAB_BASE_URL%/%GITLAB_GROUP%/snapmaker_orca_vendor.git"
set "GITLAB_WXWIDGETS=git@%GITLAB_BASE_URL%:%GITLAB_GROUP%/Orca-deps-wxWidgets.git"
set "GITLAB_SENTRY=http://%GITLAB_BASE_URL%/%GITLAB_GROUP%/sentry-native.git"

REM Script location = orca main repo root
set "ORCA_ROOT=%CD%"

REM All GitLab repos are cloned into ORCA_ROOT
set "VENDOR_DIR=%ORCA_ROOT%\gitlab_vendor"
set "WXWIDGETS_DIR=%ORCA_ROOT%\Orca-deps-wxWidgets"
set "SENTRY_DIR=%ORCA_ROOT%\sentry-native"

REM Build directories
set "DEPS_DIR=%VENDOR_DIR%\gitlab_deps"

echo.
echo ============================================================
echo   GitLab Internal Build -- Snapmaker Orca
echo ============================================================
echo   ORCA_ROOT    : %ORCA_ROOT%
echo   VENDOR       : %VENDOR_DIR%
echo   WXWIDGETS    : %WXWIDGETS_DIR%
echo   SENTRY       : %SENTRY_DIR%
echo   DEPS DIR     : %DEPS_DIR%
echo ============================================================
echo.

REM ============================================================
REM  2. Clone / pull all GitLab sub-repositories
REM ============================================================
echo [1/6] Syncing sub-repositories...

REM --- gitlab_vendor ---
if exist "%VENDOR_DIR%\.git" (
    echo   vendor: fetching latest...
    pushd "%VENDOR_DIR%"
    git fetch origin
    git reset --hard FETCH_HEAD
    popd
) else (
    REM Remove placeholder dir (e.g. only contains .gitkeep) so git clone can proceed
    if exist "%VENDOR_DIR%\" rmdir /s /q "%VENDOR_DIR%"
    echo   vendor: cloning %GITLAB_VENDOR% ...
    git clone "%GITLAB_VENDOR%" "%VENDOR_DIR%"
)
if errorlevel 1 (
    echo   [ERROR] Failed to sync vendor repo
    goto :restore_exit
)

REM --- Orca-deps-wxWidgets ---
if exist "%WXWIDGETS_DIR%\.git" (
    echo   wxWidgets: pulling latest...
    pushd "%WXWIDGETS_DIR%"
    git pull --ff-only
    popd
) else (
    if exist "%WXWIDGETS_DIR%\" rmdir /s /q "%WXWIDGETS_DIR%"
    echo   wxWidgets: cloning %GITLAB_WXWIDGETS% ...
    git clone "%GITLAB_WXWIDGETS%" "%WXWIDGETS_DIR%"
)
if errorlevel 1 (
    echo   [ERROR] Failed to sync wxWidgets repo
    goto :restore_exit
)

REM --- sentry-native ---
if exist "%SENTRY_DIR%\.git" (
    echo   sentry: pulling latest...
    pushd "%SENTRY_DIR%"
    git pull --ff-only
    popd
) else (
    if exist "%SENTRY_DIR%\" rmdir /s /q "%SENTRY_DIR%"
    echo   sentry: cloning %GITLAB_SENTRY% ...
    git clone "%GITLAB_SENTRY%" "%SENTRY_DIR%"
)
if errorlevel 1 (
    echo   [ERROR] Failed to sync sentry repo
    goto :restore_exit
)

REM --- sentry-native submodule (crashpad) ---
echo   sentry: initializing submodule (crashpad)...
pushd "%SENTRY_DIR%"
git submodule update --init --recursive
if not errorlevel 1 goto :submodule_ok
echo   [ERROR] Sentry crashpad submodule init failed.
echo   The sentry-native mirror on GitLab must include all submodules.
echo   Run this on the GitLab mirror:
echo     git submodule update --init --recursive
echo   Then re-mirror to GitLab.
popd
goto :restore_exit
:submodule_ok
popd

echo   All repos synced successfully.
echo.

REM ============================================================
REM  3. Patch vendor cmake files to use local source dirs
REM     Instead of letting ExternalProject git-clone from GitHub,
REM     we point it to the already-cloned local repos.
REM ============================================================
echo [2/6] Patching cmake files for local sources...

REM Convert Windows paths to cmake-friendly forward slashes
set "WX_SOURCE_DIR=%WXWIDGETS_DIR:\=/%"
set "SENTRY_SOURCE_DIR=%SENTRY_DIR:\=/%"

REM Patch wxWidgets.cmake and Sentry.cmake via gitlab_patch_cmake.ps1
REM Replaces GIT_REPOSITORY (GitHub) -> SOURCE_DIR (local clone from GitLab)
powershell -NoProfile -ExecutionPolicy Bypass -File "%ORCA_ROOT%\gitlab_patch_cmake.ps1" "%DEPS_DIR%" "%WX_SOURCE_DIR%" "%SENTRY_SOURCE_DIR%"
if errorlevel 1 (
    echo   [ERROR] Failed to patch cmake files
    goto :restore_exit
)
echo.

REM ============================================================
REM  4. Prepare gitlab_deps_src from vendor
REM     The slicer build uses deps_src/ via add_subdirectory().
REM     We copy the vendor's version to gitlab_deps_src/ and
REM     pass -DDEPS_SRC_DIR=gitlab_deps_src to CMake.
REM     The original deps_src/ is NOT touched -- no swap, no restore.
REM ============================================================
echo [3/6] Preparing vendored library sources...

if exist "%VENDOR_DIR%\gitlab_deps_src\" (
    REM Remove stale copy from previous run
    if exist "%ORCA_ROOT%\gitlab_deps_src\" (
        echo   Removing stale gitlab_deps_src/...
        rmdir /s /q "%ORCA_ROOT%\gitlab_deps_src"
    )

    REM Copy vendor version to root as gitlab_deps_src/ (name matches vendor)
    echo   Copying vendor/gitlab_deps_src/ to gitlab_deps_src/...
    xcopy /E /I /Q "%VENDOR_DIR%\gitlab_deps_src" "%ORCA_ROOT%\gitlab_deps_src"
    if errorlevel 1 (
        echo   [ERROR] Failed to copy gitlab_deps_src from vendor
        goto :restore_exit
    )
) else (
    echo   [WARNING] Vendor repo has no gitlab_deps_src/, using original deps_src/
)

echo   deps_src ready.
echo.

REM ============================================================
REM  5. Verify local repos are usable
REM ============================================================
echo [4/6] Verifying local repos...

if exist "%WXWIDGETS_DIR%\CMakeLists.txt" (
    echo   wxWidgets: CMakeLists.txt found - OK
) else (
    echo   [ERROR] wxWidgets: CMakeLists.txt NOT found at %WXWIDGETS_DIR%
    goto :restore_exit
)

if exist "%SENTRY_DIR%\CMakeLists.txt" (
    echo   Sentry:   CMakeLists.txt found - OK
) else (
    echo   [ERROR] Sentry: CMakeLists.txt NOT found at %SENTRY_DIR%
    goto :restore_exit
)

if exist "%DEPS_DIR%\CMakeLists.txt" (
    echo   Deps:     CMakeLists.txt found - OK
) else (
    echo   [ERROR] Deps: CMakeLists.txt NOT found at %DEPS_DIR%
    goto :restore_exit
)

echo   All repos verified.
echo.

REM ============================================================
REM  6. Build
REM ============================================================
echo [5/6] Starting build...
echo.

set "debug=OFF"
set "debuginfo=OFF"
if "%1"=="debug"      set "debug=ON"
if "%2"=="debug"      set "debug=ON"
if "%1"=="debuginfo"  set "debuginfo=ON"
if "%2"=="debuginfo"  set "debuginfo=ON"

if "%debug%"=="ON" (
    set "build_type=Debug"
    set "deps_build_dir=build-dbg"
    set "slicer_build_dir=gitlab_build-dbg"
) else (
    if "%debuginfo%"=="ON" (
        set "build_type=RelWithDebInfo"
        set "deps_build_dir=build-dbginfo"
        set "slicer_build_dir=gitlab_build-dbginfo"
    ) else (
        set "build_type=Release"
        set "deps_build_dir=build"
        set "slicer_build_dir=gitlab_build"
    )
)

echo   Build type: %build_type%
echo.

REM Signing (optional)
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

REM ============================================================
REM  Build dependencies (deps)
REM
REM  Runs cmake from:  %DEPS_DIR%/%deps_build_dir%/
REM  Reads cmake from: %DEPS_DIR%/CMakeLists.txt
REM
REM  wxWidgets and Sentry use SOURCE_DIR (patched above) so
REM  ExternalProject skips git-clone and builds from local repos.
REM
REM  Other 24 deps (Boost, OpenCV, etc.) use DL_CACHE for offline build.
REM  All deps install to: %DEPS% -> usr/local/
REM ============================================================
if "%1"=="slicer" goto :slicer

echo [deps] Building dependencies...
echo   Source:  %DEPS_DIR%
echo   Build:   %DEPS_DIR%\%deps_build_dir%
echo.

pushd "%DEPS_DIR%"
if not exist "%deps_build_dir%" mkdir "%deps_build_dir%"
pushd "%deps_build_dir%"
set "DEPS=%CD%/OrcaSlicer_dep"

cmake ../ -G "Visual Studio 17 2022" -A x64 ^
    -DDESTDIR="%DEPS%" ^
    -DCMAKE_BUILD_TYPE=%build_type% ^
    -DDEP_DEBUG=%debug% ^
    -DORCA_INCLUDE_DEBUG_INFO=%debuginfo%

if errorlevel 1 (
    echo   [ERROR] CMake configure failed for deps
    popd & popd
    goto :restore_exit
)

cmake --build . --config %build_type% --target deps -- -m
if errorlevel 1 (
    echo   [ERROR] Deps build failed
    popd & popd
    goto :restore_exit
)
popd
popd

echo   Deps build complete.
echo.

if "%1"=="deps" goto :done

REM ============================================================
REM  Build slicer
REM
REM  Main CMakeLists.txt uses find_package() / find_library() to
REM  locate wxWidgets, Sentry, and all other deps via:
REM    CMAKE_PREFIX_PATH = %DEPS%/usr/local
REM
REM  gitlab_deps_src/ is used via -DDEPS_SRC_DIR=gitlab_deps_src
REM  (copied from vendor in step [3/6], original deps_src/ untouched).
REM ============================================================
:slicer
echo [slicer] Building Snapmaker Orca...
echo   Build:   %ORCA_ROOT%\%slicer_build_dir%
echo.

if "%1"=="slicer" (
    REM slicer-only mode: DEPS from prior deps build
    if not defined DEPS set "DEPS=%DEPS_DIR%\%deps_build_dir%\OrcaSlicer_dep"
)

pushd "%ORCA_ROOT%"
if not exist "%slicer_build_dir%" mkdir "%slicer_build_dir%"
pushd "%slicer_build_dir%"

cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DBBL_RELEASE_TO_PUBLIC=1 ^
    -DORCA_TOOLS=ON ^
    %SIG_FLAG% ^
    -DDEPS_SRC_DIR=gitlab_deps_src ^
    -DCMAKE_PREFIX_PATH="%DEPS%/usr/local" ^
    -DCMAKE_INSTALL_PREFIX="./Snapmaker_Orca" ^
    -DCMAKE_BUILD_TYPE=%build_type% ^
    -DWIN10SDK_PATH="%WindowsSdkDir%Include\%WindowsSDKVersion%\"

if errorlevel 1 (
    echo   [ERROR] CMake configure failed for slicer
    popd & popd
    goto :restore_exit
)

cmake --build . --config %build_type% --target ALL_BUILD -- -m
if errorlevel 1 (
    echo   [ERROR] Slicer build failed
    popd & popd
    goto :restore_exit
)

popd
call scripts\run_gettext.bat
pushd "%slicer_build_dir%"
cmake --build . --target install --config %build_type%
popd
popd

echo   Slicer build complete.

REM ============================================================
REM  Done
REM ============================================================
:done
echo.
echo ============================================================
echo   Build completed successfully!
echo.
echo   Type:     %build_type%
echo   Deps:     %DEPS_DIR%\%deps_build_dir%
echo   Slicer:   %ORCA_ROOT%\%slicer_build_dir%
echo   Install:  %ORCA_ROOT%\%slicer_build_dir%\Snapmaker_Orca\
echo ============================================================
set "EC=0"
goto :restore_exit

REM ============================================================
REM  Restore and exit
REM
REM  Restore vendor cmake files so vendor repo stays clean.
REM  No need to restore deps_src -- the original was never touched.
REM ============================================================
:restore_exit
if not defined EC set "EC=1"

REM Restore vendor cmake files (undo SOURCE_DIR patches)
if exist "%VENDOR_DIR%\.git" (
    pushd "%VENDOR_DIR%"
    git checkout -- gitlab_deps/wxWidgets/wxWidgets.cmake 2>nul
    git checkout -- gitlab_deps/Sentry/Sentry.cmake 2>nul
    git checkout -- gitlab_deps/OpenVDB/OpenVDB.cmake 2>nul
    popd
)

exit /b %EC%
