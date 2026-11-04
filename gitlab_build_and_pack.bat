@echo off
REM ============================================
REM GitLab Build and Pack Script
REM ============================================
REM 1. Build from GitLab repos (no GitHub dependency)
REM 2. Pack with NSIS (gitlab_installer.nsi)
REM 3. Create portable ZIP (免安装包)
REM Usage: gitlab_build_and_pack.bat -v "version" [--skip-build] [--deps-only] [--slicer-only] [--clean-build] [--skip-sign] [--build-only] [--sign-only] [--pack-only]
REM Example: gitlab_build_and_pack.bat -v "2.3.5"
REM          gitlab_build_and_pack.bat -v "2.3.5" --skip-build
REM          gitlab_build_and_pack.bat -v "2.3.5" --deps-only
REM ============================================

chcp 65001 >nul
setlocal enabledelayedexpansion

REM Ensure Windows-native Strawberry Perl is before msys Perl (required for OpenSSL)
if exist "C:\Strawberry\perl\bin\perl.exe" set "PATH=C:\Strawberry\perl\bin;%PATH%"

REM Parse arguments
set "version="
set "branch="
set "skip_build=0"
set "build_target="
set "clean_build=0"
set "skip_sign=0"
set "run_phase="

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="-v" (
    set "version=%~2"
    shift
    shift
    goto :parse_args
)
if /i "%~1"=="-b" (
    set "branch=%~2"
    shift
    shift
    goto :parse_args
)
if /i "%~1"=="--skip-build" (
    set "skip_build=1"
    shift
    goto :parse_args
)
if /i "%~1"=="--deps-only" (
    set "build_target=deps"
    shift
    goto :parse_args
)
if /i "%~1"=="--slicer-only" (
    set "build_target=slicer"
    shift
    goto :parse_args
)
if /i "%~1"=="--clean-build" (
    set "clean_build=1"
    shift
    goto :parse_args
)
if /i "%~1"=="--skip-sign" (
    set "skip_sign=1"
    shift
    goto :parse_args
)
if /i "%~1"=="--build-only" (
    set "run_phase=build-only"
    shift
    goto :parse_args
)
if /i "%~1"=="--sign-only" (
    set "run_phase=sign-only"
    shift
    goto :parse_args
)
if /i "%~1"=="--pack-only" (
    set "run_phase=pack-only"
    shift
    goto :parse_args
)
shift
goto :parse_args

:args_done
REM Strip quotes
if defined version set "version=!version:"=!"

REM Check required args
if "%version%"=="" (
    echo Error: Missing -v version
    echo Usage: %~nx0 -v "version" [--skip-build] [--deps-only] [--slicer-only] [--clean-build]
    exit /b 1
)

echo ============================================
echo GitLab Build and Pack: Version %version%
echo ============================================

REM ============================================
REM Path configuration
REM ============================================
set "CURRENT_DIR=%~dp0"
set "BSL=X\ "
set "BSL=!BSL:~1,1!"
if not "!CURRENT_DIR!"=="" if "!CURRENT_DIR:~-1!"=="!BSL!" set "CURRENT_DIR=!CURRENT_DIR:~0,-1!"

REM GitLab repo root (all repos are here)
if not defined GITLAB_REPO_ROOT set "GITLAB_REPO_ROOT=E:\gitlab_pro"
set "ORCA_ROOT=%GITLAB_REPO_ROOT%\OrcaSlicer"

REM Build script (from GitLab repo)
set "BUILD_SCRIPT=%ORCA_ROOT%\gitlab_build_release_vs2022.bat"

REM NSIS script (from GitLab repo)
set "NSIS_SCRIPT=%ORCA_ROOT%\gitlab_installer.nsi"

REM Build output after install step
set "SLICER_BUILD_DIR=%ORCA_ROOT%\gitlab_build"
set "INSTALL_DIR=%SLICER_BUILD_DIR%\Snapmaker_Orca"

REM Output directory for artifacts
set "BUILD_DIR_NAME=build_%version%"
set "BUILD_DIR_NAME=!BUILD_DIR_NAME:/=_!"
set "BUILD_DIR_NAME=!BUILD_DIR_NAME:\=_!"
set "BUILD_DIR_NAME=!BUILD_DIR_NAME:.=!"
set "BUILD_DIR=%CURRENT_DIR%\!BUILD_DIR_NAME!"

if defined SHORT_BUILD_PATH (
    set "BUILD_DIR_NAME_SHORT=build_!version!"
    set "BUILD_DIR_NAME_SHORT=!BUILD_DIR_NAME_SHORT:/=_!"
    set "BUILD_DIR_NAME_SHORT=!BUILD_DIR_NAME_SHORT:\=_!"
    set "BUILD_DIR_NAME_SHORT=!BUILD_DIR_NAME_SHORT:.=!"
    set "BUILD_DIR=!SHORT_BUILD_PATH!!BUILD_DIR_NAME_SHORT!"
    echo [Info] Short path: !BUILD_DIR!
)

echo.
echo ============================================
echo Configuration:
echo   Version:      %version%
echo   GitLab Root:  %GITLAB_REPO_ROOT%
echo   Orca Root:    %ORCA_ROOT%
echo   Build Script: %BUILD_SCRIPT%
echo   NSIS Script:  %NSIS_SCRIPT%
echo   Install Dir:  %INSTALL_DIR%
echo   Output Dir:   %BUILD_DIR%
echo ============================================
echo.

REM ============================================
REM Signing configuration (Windows Authenticode via SafeNet eToken)
REM   - Signs with the code-signing cert found in the user's cert store.
REM   - Key is on the SafeNet eToken; PIN is cached by the SafeNet client.
REM     If a PIN prompt appears, enter the value of SIGN_PSW.
REM   - Update SIGN_SHA1 after a certificate renewal.
REM ============================================
REM Dynamically locate the latest signtool.exe (avoid hardcoding SDK version)
set "SIGNTOOL="
for /f "delims=" %%i in ('dir /b /s /on "C:\Program Files (x86)\Windows Kits\10\bin\x64\signtool.exe" 2^>nul') do set "SIGNTOOL=%%i"
set "SIGN_SHA1=6690289daa949025dae57afbed00d353faf8cd91"
set "SIGN_TS=http://timestamp.digicert.com"

REM ============================================
REM Verify paths
REM ============================================
if not exist "%ORCA_ROOT%" (
    echo [Error] OrcaSlicer repo not found: %ORCA_ROOT%
    echo Check GITLAB_REPO_ROOT or clone the repo to E:\gitlab_pro\OrcaSlicer
    exit /b 1
)
if not exist "%BUILD_SCRIPT%" (
    echo [Error] Build script not found: %BUILD_SCRIPT%
    exit /b 1
)
if not exist "%NSIS_SCRIPT%" (
    echo [Error] NSIS script not found: %NSIS_SCRIPT%
    exit /b 1
)

REM ============================================
REM Find NSIS
REM ============================================
REM Find NSIS dir and cd into it (avoids path-with-parens issues)
set "NSIS_EXE="
if exist "C:\Program Files (x86)\NSIS\Bin\makensis.exe" set NSIS_EXE="C:\Program Files (x86)\NSIS\Bin\makensis.exe"
if not defined NSIS_EXE if exist "C:\Program Files\NSIS\Bin\makensis.exe" set NSIS_EXE="C:\Program Files\NSIS\Bin\makensis.exe"
if not defined NSIS_EXE if exist "C:\Program Files (x86)\NSIS\makensis.exe" set NSIS_EXE="C:\Program Files (x86)\NSIS\makensis.exe"
if not defined NSIS_EXE if exist "C:\Program Files\NSIS\makensis.exe" set NSIS_EXE="C:\Program Files\NSIS\makensis.exe"
if not defined NSIS_EXE (
    echo [Error] NSIS not found. Install NSIS or set NSIS_PATH.
    exit /b 1
)

REM ============================================
REM Phase dispatch (for split Jenkins stages)
REM ============================================
if "%run_phase%"=="sign-only" goto :skip_build
if "%run_phase%"=="pack-only" goto :pack_only

REM ============================================
REM Build (from GitLab repos)
REM ============================================
if "%skip_build%"=="1" goto :skip_build

echo.
echo ============================================
echo Step 1: Build from GitLab repos
echo ============================================

REM Call the GitLab build script from its own directory
pushd "%ORCA_ROOT%"

if "%clean_build%"=="1" (
    echo [Clean Build] Removing previous build directories...
    if exist "%ORCA_ROOT%\gitlab_build" (
        echo   Removing %ORCA_ROOT%\gitlab_build...
        rmdir /s /q "%ORCA_ROOT%\gitlab_build" 2>nul
    )
    if exist "%ORCA_ROOT%\gitlab_vendor\gitlab_deps\build" (
        echo   Removing deps build cache...
        rmdir /s /q "%ORCA_ROOT%\gitlab_vendor\gitlab_deps\build" 2>nul
    )
    echo.
)

if "%build_target%"=="" (
    echo Building deps + slicer from GitLab repos...
    echo Command: "%BUILD_SCRIPT%"
    echo.
    call "%BUILD_SCRIPT%"
) else if "%build_target%"=="deps" (
    echo Building deps only from GitLab repos...
    echo Command: "%BUILD_SCRIPT%" deps
    echo.
    call "%BUILD_SCRIPT%" deps
) else if "%build_target%"=="slicer" (
    echo Building slicer only from GitLab repos...
    echo Command: "%BUILD_SCRIPT%" slicer
    echo.
    call "%BUILD_SCRIPT%" slicer
)

set BUILD_ERROR=!ERRORLEVEL!
popd

if !BUILD_ERROR! neq 0 (
    echo [Error] Build failed with exit code: !BUILD_ERROR!
    exit /b 1
)
echo [OK] Build completed.
goto :after_build

:skip_build
echo.
echo ============================================
echo Step 1: Build SKIPPED (--skip-build)
echo ============================================

:after_build

REM If deps-only mode, stop here
if "%build_target%"=="deps" (
    echo.
    echo ============================================
    echo [OK] Deps build done. Skipping pack.
    echo ============================================
    exit /b 0
)

REM ============================================
REM Verify build output
REM ============================================
echo.
echo ============================================
echo Step 2: Verify build output
echo ============================================

set "BUILD_EXE="
if exist "%INSTALL_DIR%\snapmaker-orca.exe" (
    set "BUILD_EXE=%INSTALL_DIR%\snapmaker-orca.exe"
    echo [OK] Found: !BUILD_EXE!
) else (
    echo [Error] snapmaker-orca.exe not found in %INSTALL_DIR%
    echo Build may have failed or install step was not run.
    exit /b 1
)

REM ============================================
REM Step 2.5: Sign binaries (Authenticode)
REM   Recursively signs every .exe/.dll under the install dir, but skips
REM   resources\ (the Connect CLI keeps its staged signature, and the rest
REM   of that tree is not PE). New folders are picked up
REM   without editing this script. Files are grouped into batches so one
REM   signtool process signs many files (avoids a SafeNet PIN prompt per
REM   file). A failed signature is reported and does not stop packaging.
REM ============================================
if "%skip_sign%"=="1" goto :after_sign
if "%run_phase%"=="build-only" goto :after_sign
:sign_binaries_now
if not exist "!SIGNTOOL!" (
    echo [Error] signtool.exe not found: !SIGNTOOL!
    exit /b 1
)

set SIGN_ERROR=0
set "SIGN_FILES="
set "SIGN_COUNT=0"
set "SKIP_ROOT=%INSTALL_DIR%\resources\"
for %%E in (exe dll) do (
    for /r "%INSTALL_DIR%" %%F in (*.%%E) do (
        set "SIGN_PATH=%%~fF"
        if /i not "!SIGN_PATH:%SKIP_ROOT%=!"=="!SIGN_PATH!" (
            echo   Skip "!SIGN_PATH!"
        ) else (
            set "SIGN_FILES=!SIGN_FILES! "%%F""
            set /a SIGN_COUNT+=1
            if !SIGN_COUNT! geq 50 call :flush_sign_batch
        )
    )
)
call :flush_sign_batch
if !SIGN_ERROR! neq 0 (
    echo [Warn] One or more binaries failed to sign. Continuing.
) else (
    echo [OK] Binaries signed.
)

:after_sign
if "%run_phase%"=="build-only" exit /b 0
if "%run_phase%"=="sign-only" exit /b 0

REM ============================================
REM Step 3: NSIS installer
REM ============================================
:pack_only
echo.
echo ============================================
echo Step 3: Pack installer with NSIS
echo ============================================

REM Ensure output directory exists
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM Real product version comes from version.inc; -v/%version% is only a filename label
set "REAL_VER="
for /f "usebackq tokens=2 delims=()" %%A in (`findstr /b /c:"set(Snapmaker_VERSION" "%ORCA_ROOT%\version.inc"`) do set "REAL_VER=%%A"
set "REAL_VER=!REAL_VER:Snapmaker_VERSION =!"
set "REAL_VER=!REAL_VER:"=!"
if not defined REAL_VER set "REAL_VER=%version%"
set "OUTPUT_FILE=Snapmaker_Orca_Windows_Installer_V%REAL_VER%_%version%.exe"
if "%version%"=="" set "OUTPUT_FILE=Snapmaker_Orca_Windows_Installer_V%REAL_VER%.exe"
set "OUTPUT_PATH=%BUILD_DIR%\%OUTPUT_FILE%"

REM Find icon file for NSIS
set "ICON_ARG="
if exist "%ORCA_ROOT%\resources\images\Snapmaker_Orca.ico" (
    set "ICON_FILE=%ORCA_ROOT%\resources\images\Snapmaker_Orca.ico"
) else if exist "%INSTALL_DIR%\resources\images\Snapmaker_Orca.ico" (
    set "ICON_FILE=%INSTALL_DIR%\resources\images\Snapmaker_Orca.ico"
)
if defined ICON_FILE (
    set "ICON_ARG=/DICON_FILE=!ICON_FILE!"
    echo [NSIS] Using icon: !ICON_FILE!
)

REM Find LICENSE file for NSIS
set "LICENSE_FILE=%ORCA_ROOT%\LICENSE.txt"
if not exist "!LICENSE_FILE!" set "LICENSE_FILE="

REM Build the makensis command
echo.
echo [NSIS] Parameters:
echo   SOURCE_DIR   = %INSTALL_DIR%
echo   VERSION      = %version%
echo   OUTPUT_FILE  = %OUTPUT_FILE%
echo.

REM We are already in NSIS dir from earlier pushd; just run makensis.exe
set "NSIS_CMD=!NSIS_EXE! /INPUTCHARSET UTF8 /DVERSION=%version% /DSOURCE_DIR=%INSTALL_DIR% /DOUTPUT_FILE=!OUTPUT_PATH! !ICON_ARG!"

if defined LICENSE_FILE (
    set "NSIS_CMD=!NSIS_CMD! /DLICENSE_FILE=!LICENSE_FILE!"
)

set "NSIS_CMD=!NSIS_CMD! -- "%NSIS_SCRIPT%""

echo [NSIS] Full command:
echo !NSIS_CMD!
echo.

!NSIS_CMD!

set NSIS_ERROR=!ERRORLEVEL!

if !NSIS_ERROR! neq 0 (
    echo [Error] NSIS compile failed, code !NSIS_ERROR!
    if !NSIS_ERROR! equ 2 (
        echo.
        echo [Error 0x2] File not found. Check:
        echo   - NSIS install path - spaces or parentheses issue
        echo   - SOURCE_DIR: %INSTALL_DIR%
        echo   - NSIS script: %NSIS_SCRIPT%
    )
    exit /b 1
)

REM Verify installer was created
set "FOUND_INSTALLER="
if exist "!OUTPUT_PATH!" (
    set "FOUND_INSTALLER=!OUTPUT_PATH!"
) else (
    REM Check if NSIS placed it elsewhere
    for %%f in (
        "%ORCA_ROOT%\%OUTPUT_FILE%"
        "%BUILD_DIR%\%OUTPUT_FILE%"
    ) do (
        if exist "%%f" (
            set "FOUND_INSTALLER=%%f"
            if /i not "%%f"=="!OUTPUT_PATH!" (
                echo Moving installer to output dir...
                move /y "%%f" "!OUTPUT_PATH!" >nul
            )
        )
    )
)

if "!FOUND_INSTALLER!"=="" (
    echo [Error] Installer not created. Check NSIS output above.
    exit /b 1
)

echo [OK] Installer: !OUTPUT_PATH!

REM ============================================
REM Step 3.5: Sign installer
REM ============================================
if "%skip_sign%"=="1" goto :skip_installer_sign
echo Signing installer...
"!SIGNTOOL!" sign /sha1 "%SIGN_SHA1%" /fd SHA256 /tr "%SIGN_TS%" /td SHA256 "!OUTPUT_PATH!"
if errorlevel 1 (
    echo [Error] Failed to sign installer: !OUTPUT_PATH!
    exit /b 1
)
echo [OK] Installer signed.
:skip_installer_sign

REM ============================================
REM Step 4: Portable ZIP (免安装包)
REM ============================================
echo.
echo ============================================
echo Step 4: Create portable ZIP (免安装包)
echo ============================================

REM Find 7-Zip (use if+goto to avoid (x86) parens issue)
set "ZIP_TOOL="
if exist "C:\Program Files\7-Zip\7z.exe" set "ZIP_TOOL=C:\Program Files\7-Zip\7z.exe" & goto :zip_found
if exist "C:\Program Files (x86)\7-Zip\7z.exe" set "ZIP_TOOL=C:\Program Files (x86)\7-Zip\7z.exe" & goto :zip_found
if exist "%ORCA_ROOT%\tools\7z.exe" set "ZIP_TOOL=%ORCA_ROOT%\tools\7z.exe" & goto :zip_found
if exist "D:\tools\7z.exe" set "ZIP_TOOL=D:\tools\7z.exe" & goto :zip_found
:zip_found

if "!ZIP_TOOL!"=="" (
    echo [Warn] 7-Zip not found, skipping portable ZIP.
    echo Install 7-Zip or place 7z.exe in %ORCA_ROOT%\tools\
    goto :skip_portable
)

set "PORTABLE_FILE=Snapmaker_Orca_Windows_%REAL_VER%_%version%_portable.zip"
if "%version%"=="" set "PORTABLE_FILE=Snapmaker_Orca_Windows_%REAL_VER%_portable.zip"
set "PORTABLE_PATH=%BUILD_DIR%\%PORTABLE_FILE%"

echo Source: %INSTALL_DIR%
echo Output: %PORTABLE_FILE%

pushd "%SLICER_BUILD_DIR%"

REM Create exclude list
set "EXCLUDE_LIST=%SLICER_BUILD_DIR%\pack_portable_exclude.txt"
echo Snapmaker_Orca\include\* > "!EXCLUDE_LIST!"
echo Snapmaker_Orca\lib\* >> "!EXCLUDE_LIST!"

echo Creating ZIP...
"!ZIP_TOOL!" a -tzip "%PORTABLE_FILE%" "Snapmaker_Orca\*" -xr@"!EXCLUDE_LIST!" -y

set ZIP_ERROR=!ERRORLEVEL!
if exist "!EXCLUDE_LIST!" del "!EXCLUDE_LIST!"
popd

if !ZIP_ERROR! neq 0 (
    echo [Warn] Portable ZIP failed, 7z exit code: !ZIP_ERROR!
) else (
    REM Move ZIP to build output dir if it was created in slicer_build_dir
    if exist "%SLICER_BUILD_DIR%\%PORTABLE_FILE%" (
        if /i not "%SLICER_BUILD_DIR%\%PORTABLE_FILE%"=="!PORTABLE_PATH!" (
            move /y "%SLICER_BUILD_DIR%\%PORTABLE_FILE%" "!PORTABLE_PATH!" >nul 2>&1
        )
    )
    if exist "!PORTABLE_PATH!" (
        echo [OK] Portable: !PORTABLE_PATH!
    ) else (
        echo [Warn] Portable ZIP not found at expected path, check 7z output above.
    )
)

:skip_portable

REM ============================================
REM Done
REM ============================================
echo.
echo ============================================
echo [OK] GitLab Build and Pack Complete!
echo ============================================
echo Version: %version%
echo Output dir: %BUILD_DIR%
echo Output files:
if exist "%OUTPUT_PATH%" (
    echo   [Installer]  %OUTPUT_FILE%
    for %%f in ("%OUTPUT_PATH%") do echo   Size: %%~zf bytes
)
if exist "%PORTABLE_PATH%" (
    echo   [Portable]   %PORTABLE_FILE%
    for %%f in ("%PORTABLE_PATH%") do echo   Size: %%~zf bytes
)
echo ============================================

REM Jenkins/CI: don't pause
if not defined CI if not defined JENKINS_URL pause

exit /b 0

:flush_sign_batch
if not defined SIGN_FILES goto :eof
echo   Signing !SIGN_COUNT! binaries ...
"!SIGNTOOL!" sign /sha1 "%SIGN_SHA1%" /fd SHA256 /tr "%SIGN_TS%" /td SHA256 !SIGN_FILES!
if errorlevel 1 (
    echo   [Warn] One or more binaries in this batch failed to sign.
    set SIGN_ERROR=1
)
set "SIGN_FILES="
set "SIGN_COUNT=0"
goto :eof
