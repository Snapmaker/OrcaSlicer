; ==============================================================================
; Snapmaker Orca - GitLab Build Installer Script
; ==============================================================================
; Packages build artifacts from gitlab_build\Snapmaker_Orca into a Windows
; installer. Designed for GitLab CI pipelines but also works for local builds.
;
; Usage:
;   makensis gitlab_installer.nsi
;   makensis /DVERSION=2.3.5 /DSOURCE_DIR=.\gitlab_build\Snapmaker_Orca gitlab_installer.nsi
;   makensis /DVERSION=2.3.5 /DSOURCE_DIR=.\custom\path /DOUTPUT_FILE=MyInstaller.exe gitlab_installer.nsi
;
; Silent install (CI):
;   Snapmaker_Orca_GitLab_V2.3.5.exe /S
;   Snapmaker_Orca_GitLab_V2.3.5.exe /S /D=C:\MyCustomPath
; ==============================================================================

; --- Includes ---
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"

; --- Product Info ---
!define PRODUCT_NAME "Snapmaker Orca"
!define PRODUCT_PUBLISHER "Snapmaker"
!define PRODUCT_WEB_SITE "https://github.com/Snapmaker/OrcaSlicer"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"
!define PRODUCT_INSTALL_KEY "Software\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}"

; --- Defaults (override via /D on makensis command line) ---
!ifndef VERSION
    !define VERSION "2.3.5"
!endif

; SOURCE_DIR: where the compiled build output lives (e.g. gitlab_build\Snapmaker_Orca)
!ifndef SOURCE_DIR
    !define SOURCE_DIR ".\gitlab_build\Snapmaker_Orca"
!endif
!define PACK_SOURCE_DIR "${SOURCE_DIR}"

; OUTPUT_FILE: name of the generated installer
!ifndef OUTPUT_FILE
    !define OUTPUT_FILE "Snapmaker_Orca_GitLab_Windows_V${VERSION}.exe"
!endif

; INSTALL_DIR_RUNTIME: default install path at runtime
!define INSTALL_DIR_RUNTIME "$PROGRAMFILES64\Snapmaker_Orca"
InstallDir "${INSTALL_DIR_RUNTIME}"

; LICENSE_FILE: license to show during install
!ifndef LICENSE_FILE
    !define LICENSE_FILE ".\LICENSE.txt"
!endif

; --- Compiler Settings ---
RequestExecutionLevel admin
SetCompressor lzma
; No /SOLID to avoid "Internal compiler error #12345: error mmapping datablock"
; /SOLID is omitted intentionally for large builds

; --- Installer Version Info (embedded in EXE) ---
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey "Comments" "Snapmaker Orca - GitLab Build - Open source slicer for FDM printers"
VIAddVersionKey "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Copyright (C) ${PRODUCT_PUBLISHER}"
VIAddVersionKey "FileDescription" "${PRODUCT_NAME} ${VERSION} Installer (GitLab)"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "InternalName" "${PRODUCT_NAME}"
VIAddVersionKey "LegalTrademarks" ""
VIAddVersionKey "OriginalFilename" "${OUTPUT_FILE}"

; --- Icon ---
!ifdef ICON_FILE
    !define MUI_ICON "${ICON_FILE}"
    !define MUI_UNICON "${ICON_FILE}"
!else
    !define MUI_ICON ".\resources\images\Snapmaker_Orca.ico"
    !define MUI_UNICON ".\resources\images\Snapmaker_Orca.ico"
!endif

; --- Branding (header image, optional) ---
!ifdef WELCOME_BMP
    !define MUI_WELCOMEFINISHPAGE_BITMAP "${WELCOME_BMP}"
!endif

; --- Pages ---
; Welcome
!define MUI_WELCOMEPAGE_TITLE "Welcome to ${PRODUCT_NAME} Setup"
!define MUI_WELCOMEPAGE_TEXT "This wizard will guide you through the installation of ${PRODUCT_NAME} ${VERSION} (GitLab Build).$\r$\n$\r$\nIt is recommended that you close all other applications before starting Setup.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME

; License
!ifdef LICENSE_FILE
    !define MUI_LICENSEPAGE_CHECKBOX
    !insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!endif

; Components (Main program / Network plugins / Profiles)
!define MUI_COMPONENTSPAGE_TEXT_TOP "Select the components you want to install."
!define MUI_COMPONENTSPAGE_TEXT_COMPLIST "Select components:"
!insertmacro MUI_PAGE_COMPONENTS

; Directory
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder in which to install ${PRODUCT_NAME}."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Destination folder:"
!insertmacro MUI_PAGE_DIRECTORY

; InstFiles
!insertmacro MUI_PAGE_INSTFILES

; Finish
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Run ${PRODUCT_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION "LaunchApp"
!define MUI_FINISHPAGE_LINK "Visit ${PRODUCT_NAME} website"
!define MUI_FINISHPAGE_LINK_LOCATION "${PRODUCT_WEB_SITE}"
!insertmacro MUI_PAGE_FINISH

; Uninstall
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; --- Languages ---
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

; --- Installer Attributes ---
Name "${PRODUCT_NAME} ${VERSION}"
OutFile "${OUTPUT_FILE}"

; ==============================================================================
; Installer Sections
; ==============================================================================

; --- Section Group "Core" (hidden group, always installed) ---
Section "!Main program" SecMain
    SectionIn RO
    SectionInstType 1 2 3

    Call EnsureSnapmakerNotRunning

    ; -- Install core files --
    SetOutPath "$INSTDIR"

    DetailPrint "Installing ${PRODUCT_NAME} ${VERSION}..."
    DetailPrint "Source: ${PACK_SOURCE_DIR}"
    DetailPrint "Target: $INSTDIR"

    DetailPrint "Copying files (this may take a minute)..."

    ; --------------------------------------------------------------------------
    ; File extraction with comprehensive exclusion list.
    ; Excludes: debug symbols, build artifacts, dev headers/libs, temp files,
    ;           CMake junk, version control, Python cache, CI artifacts.
    ; --------------------------------------------------------------------------
    File /r \
        /x "*.pdb" \
        /x "*.ilk" \
        /x "*.exp" \
        /x "*.lib" \
        /x "*.obj" \
        /x "*.idb" \
        /x "*.tlog" \
        /x "*.ipch" \
        /x "*.iobj" \
        /x "*.ipdb" \
        /x "*.suo" \
        /x "*.sdf" \
        /x "*.opensdf" \
        /x "*.user" \
        /x "*.cache" \
        /x "*.recipe" \
        /x "*.log" \
        /x "*.h" \
        /x "*.hpp" \
        /x "*.hxx" \
        /x "*.c" \
        /x "*.cpp" \
        /x "*.cxx" \
        /x "*.cc" \
        /x "*.vcxproj" \
        /x "*.vcxproj.filters" \
        /x "*.vcxproj.user" \
        /x "*.sln" \
        /x "*.cmake" \
        /x "*.py" \
        /x "*.pyc" \
        /x "*.pyo" \
        /x "*.md" \
        /x "*.bat" \
        /x "*.sh" \
        /x "*.ps1" \
        /x "CMakeFiles" \
        /x "CMakeCache.txt" \
        /x "RelWithDebInfo" \
        /x "Debug" \
        /x "MinSizeRel" \
        /x "Release" \
        /x ".vs" \
        /x "vcpkg_installed" \
        /x "*.dir" \
        /x "__pycache__" \
        /x ".git" \
        /x ".gitignore" \
        /x ".gitattributes" \
        /x "include\*" \
        /x "lib\*" \
        /x "deps_src\*" \
        /x "src\*" \
        /x "x64\*" \
        "${PACK_SOURCE_DIR}\*.*"

    ; -- Verify critical files exist --
    ${If} ${FileExists} "$INSTDIR\snapmaker-orca.exe"
        DetailPrint "  snapmaker-orca.exe - OK"
    ${Else}
        MessageBox MB_OK|MB_ICONSTOP \
            "Installation failed: snapmaker-orca.exe was not found in the package.$\r$\n$\r$\n\
            Source directory: ${PACK_SOURCE_DIR}$\r$\n\
            The installer may be corrupted or the build is incomplete."
        Abort
    ${EndIf}

    ${If} ${FileExists} "$INSTDIR\Snapmaker_Orca.dll"
        DetailPrint "  Snapmaker_Orca.dll - OK"
    ${Else}
        DetailPrint "  WARNING: Snapmaker_Orca.dll not found"
    ${EndIf}

    ; -- Create uninstaller --
    DetailPrint "Creating uninstaller..."
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; -- Registry: Add/Remove Programs --
    DetailPrint "Writing registry..."

    StrCpy $0 "$INSTDIR"
    ${GetSize} "$INSTDIR" "/M=Snapmaker_Orca.dll" $1 $2 $3
    ; Fallback size estimate if GetSize fails
    ${If} $1 == 0
        StrCpy $1 500000  ; ~500 MB fallback
    ${EndIf}

    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\snapmaker-orca.exe"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "HelpLink" "${PRODUCT_WEB_SITE}"
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoRepair" 1
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "EstimatedSize" $1

    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "Version" "${VERSION}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "InstallPath" "$INSTDIR"

    ; -- URL Protocols --
    DetailPrint "Registering URL protocols (snapmaker-orca, Snapmaker_Orca)..."
    SetRegView 64
    WriteRegStr HKLM "Software\Classes\snapmaker-orca" "" "URL:Snapmaker Orca"
    WriteRegStr HKLM "Software\Classes\snapmaker-orca" "URL Protocol" ""
    WriteRegStr HKLM "Software\Classes\snapmaker-orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'
    WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "" "URL:Snapmaker Orca"
    WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "URL Protocol" ""
    WriteRegStr HKLM "Software\Classes\Snapmaker_Orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'
    SetRegView 32

    DetailPrint "Core installation complete!"
SectionEnd

; --- Optional: Network Plugins (sentry, crashpad, etc.) ---
Section /o "Network Plugins" SecNetwork
    DetailPrint "Network plugin support files already included in core install."
    DetailPrint "  - sentry.dll (crash reporting)"
    DetailPrint "  - crashpad_handler.exe (crash diagnostics)"
    DetailPrint "  - WebView2Loader.dll (web view support)"
    ; These files are already bundled in the core install, so this section
    ; is informational. Add future network plugin files here if needed.
SectionEnd

; --- Desktop Shortcut ---
Section "Desktop shortcut" SecDesktop
    DetailPrint "Creating desktop shortcut..."
    SetShellVarContext current
    CreateShortcut "$DESKTOP\Snapmaker Orca.lnk" \
        "$INSTDIR\snapmaker-orca.exe" "" "$INSTDIR\snapmaker-orca.exe" 0
    SetShellVarContext all
SectionEnd

; --- Start Menu Shortcuts ---
Section "Start menu shortcut" SecStartMenu
    DetailPrint "Creating start menu shortcuts..."
    CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\Snapmaker Orca.lnk" \
        "$INSTDIR\snapmaker-orca.exe" "" "$INSTDIR\snapmaker-orca.exe" 0
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" \
        "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0
SectionEnd

; --- Install Type Definitions ---
; Full (1): Core + Network + Desktop + StartMenu
; Typical (2): Core + Desktop + StartMenu
; Minimal (3): Core only
InstType "Full"
InstType "Typical"
InstType "Minimal"

; ==============================================================================
; Section Descriptions
; ==============================================================================
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} \
        "Install ${PRODUCT_NAME} core files (required).$\r$\nIncludes the main application, all DLLs, resources, profiles, and i18n files."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecNetwork} \
        "Network and crash-reporting plugins.$\r$\nIncludes sentry.dll, crashpad_handler.exe, and WebView2Loader.dll."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} \
        "Create a shortcut to ${PRODUCT_NAME} on the desktop."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} \
        "Create a Start Menu folder with ${PRODUCT_NAME} and Uninstall shortcuts."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ==============================================================================
; Uninstaller Section
; ==============================================================================
Section "Uninstall"

    DetailPrint "Uninstalling ${PRODUCT_NAME} ${VERSION}..."

    ; -- Terminate running process --
    DetailPrint "Terminating running processes..."
    nsExec::ExecToLog 'taskkill /F /IM snapmaker-orca.exe /T'
    Sleep 1000
    nsExec::ExecToLog 'taskkill /F /IM crashpad_handler.exe /T'
    Sleep 500

    ; -- Remove shortcuts --
    DetailPrint "Removing shortcuts..."
    SetShellVarContext current
    Delete "$DESKTOP\Snapmaker Orca.lnk"
    SetShellVarContext all
    Delete "$DESKTOP\Snapmaker Orca.lnk"
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
    RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"

    ; -- Remove install directory --
    DetailPrint "Removing installed files..."
    RMDir /r /REBOOTOK "$INSTDIR"
    ; Clean up empty dir
    RMDir "$INSTDIR"

    ; -- Remove registry entries --
    DetailPrint "Removing registry entries..."
    SetRegView 64
    DeleteRegKey HKLM "Software\Classes\snapmaker-orca"
    DeleteRegKey HKLM "Software\Classes\Snapmaker_Orca"
    SetRegView 32
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}"
    DeleteRegKey HKCU "${PRODUCT_INSTALL_KEY}"

    DetailPrint "Uninstall complete!"
SectionEnd

; ==============================================================================
; Functions
; ==============================================================================

; Launch the application after install
Function LaunchApp
    ExecShell "open" "$INSTDIR\snapmaker-orca.exe"
FunctionEnd

; Prevent overwriting locked DLLs when snapmaker-orca is still running.
; Also checks for the legacy Snapmaker_Orca.exe process name.
Function EnsureSnapmakerNotRunning
    snapmaker_check_loop:
        ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq snapmaker-orca.exe" 2>nul | find /i "snapmaker-orca.exe" >nul' $0
        IntCmp $0 0 snapmaker_in_use snapmaker_try_legacy snapmaker_try_legacy

    snapmaker_try_legacy:
        ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq Snapmaker_Orca.exe" 2>nul | find /i "Snapmaker_Orca.exe" >nul' $0
        IntCmp $0 0 snapmaker_in_use snapmaker_try_crashpad snapmaker_try_crashpad

    snapmaker_try_crashpad:
        ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq crashpad_handler.exe" 2>nul | find /i "crashpad_handler.exe" >nul' $0
        IntCmp $0 0 snapmaker_in_use snapmaker_idle snapmaker_idle

    snapmaker_in_use:
        IfSilent snapmaker_silent snapmaker_prompt

    snapmaker_silent:
        ; In silent mode (CI), automatically terminate and retry
        DetailPrint "Silent mode: attempting to terminate running processes..."
        nsExec::ExecToLog 'taskkill /F /IM snapmaker-orca.exe /T'
        nsExec::ExecToLog 'taskkill /F /IM Snapmaker_Orca.exe /T'
        nsExec::ExecToLog 'taskkill /F /IM crashpad_handler.exe /T'
        Sleep 1500
        Goto snapmaker_check_loop

    snapmaker_prompt:
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
            "Snapmaker Orca is still running.$\r$\n$\r$\n\
            Please close the application (snapmaker-orca.exe) before continuing.$\r$\n$\r$\n\
            Click Retry after closing, or Cancel to exit the installer." \
            IDRETRY snapmaker_check_loop
        Abort

    snapmaker_idle:
FunctionEnd

; --- .onInit: Runs at installer startup ---
Function .onInit

    ; -- Check if already running --
    Call EnsureSnapmakerNotRunning

    ; -- Check for previous installation --
    ReadRegStr $R0 ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString"
    StrCmp $R0 "" done

    ; Previous version detected - offer to uninstall
    IfSilent silent_uninst
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
        "${PRODUCT_NAME} is already installed.$\r$\n$\r$\nClick OK to uninstall the old version, or Cancel to abort." \
        IDOK uninst
    Abort
    Goto done

    silent_uninst:
        ; In silent/CI mode, auto-uninstall the old version
        DetailPrint "Silent mode: uninstalling previous version..."
        Goto uninst

    uninst:
        ClearErrors
        ExecWait '"$R0" _?=$INSTDIR'
        IfErrors 0 done
        ; If uninstaller failed, try forceful cleanup
        DetailPrint "Previous uninstaller failed, attempting cleanup..."
        RMDir /r /REBOOTOK "$INSTDIR"

    done:
FunctionEnd

; --- .onInstSuccess: Runs after successful installation ---
Function .onInstSuccess
    ; Log success for CI pipelines
    DetailPrint "Installation of ${PRODUCT_NAME} ${VERSION} completed successfully."
FunctionEnd
