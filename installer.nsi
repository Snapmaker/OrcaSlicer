; [1] PACK_SOURCE_DIR = compile-time only (e.g. .\build\Snapmaker_Orca). [2] INSTALL_DIR_RUNTIME = runtime install dir (default .\ = $EXEDIR).
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define PRODUCT_NAME "EdgeSlicer"
!define PRODUCT_PUBLISHER "EdgeSlicer"
!define PRODUCT_WEB_SITE "https://github.com/aceRage/Snapmaker-Ultra"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"
!define PRODUCT_INSTALL_KEY "Software\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}"

!ifndef VERSION
    !define VERSION "2.3.6"
!endif

!ifndef SOURCE_DIR
    !define SOURCE_DIR ".\build\Snapmaker_Orca"
!endif
!define PACK_SOURCE_DIR "${SOURCE_DIR}"

; 64-bit app: use PROGRAMFILES64 so default path is C:\Program Files\EdgeSlicer, not (x86)
!define INSTALL_DIR_RUNTIME "$PROGRAMFILES64\EdgeSlicer"
InstallDir "${INSTALL_DIR_RUNTIME}"

!ifndef OUTPUT_FILE
    !define OUTPUT_FILE "EdgeSlicer_Windows_Installer_V${VERSION}.exe"
!endif

; License page: show LICENSE.txt from repo root (same dir as this .nsi)
!ifndef LICENSE_FILE
    !define LICENSE_FILE ".\LICENSE.txt"
!endif

RequestExecutionLevel admin

; No /SOLID to avoid "Internal compiler error #12345: error mmapping datablock"
SetCompressor lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey "Comments" "EdgeSlicer is an open source slicer for FDM printers, for Snapmaker and Bambu Lab machines"
VIAddVersionKey "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Copyright (C) the ${PRODUCT_PUBLISHER} contributors"
VIAddVersionKey "FileDescription" "${PRODUCT_NAME} ${VERSION} Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "InternalName" "${PRODUCT_NAME}"
VIAddVersionKey "LegalTrademarks" ""
VIAddVersionKey "OriginalFilename" "${OUTPUT_FILE}"

; Installer and uninstaller icon: set by build_and_pack.bat via /DICON_FILE=path (e.g. EdgeSlicer.ico or snapmaker.ico)
!ifdef ICON_FILE
    !define MUI_ICON "${ICON_FILE}"
    !define MUI_UNICON "${ICON_FILE}"
!else
    !define MUI_ICON ".\resources\images\EdgeSlicer.ico"
    !define MUI_UNICON ".\resources\images\EdgeSlicer.ico"
!endif

!define MUI_WELCOMEPAGE_TITLE "Welcome to ${PRODUCT_NAME} Setup"
!define MUI_WELCOMEPAGE_TEXT "This wizard will guide you through the installation of ${PRODUCT_NAME} ${VERSION}.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME

!ifdef LICENSE_FILE
    !define MUI_LICENSEPAGE_CHECKBOX
    !insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!endif

!insertmacro MUI_PAGE_COMPONENTS

!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder in which to install ${PRODUCT_NAME}."
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Run ${PRODUCT_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION "LaunchApp"
!define MUI_FINISHPAGE_LINK "Visit ${PRODUCT_NAME} website"
!define MUI_FINISHPAGE_LINK_LOCATION "${PRODUCT_WEB_SITE}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Name "${PRODUCT_NAME} ${VERSION}"
OutFile "${OUTPUT_FILE}"

Section "Main program" SecMain
    SectionIn RO

    Call EnsureSnapmakerNotRunning

    SetOutPath "$INSTDIR"
    
    DetailPrint "Installing ${PRODUCT_NAME}..."
    DetailPrint "Target dir: $INSTDIR"
    
    DetailPrint "Copying files..."
    
    ; PACK_SOURCE_DIR = compile time only. At runtime this File extracts from embedded payload to $INSTDIR. Exclude include and lib dirs.
    File /r /x "*.pdb" /x "*.ilk" /x "*.exp" /x "*.lib" /x "*.obj" /x "*.idb" /x "*.tlog" /x "*.h" /x "*.hpp" /x "*.c" /x "*.cpp" /x "*.cxx" /x "*.cc" /x "*.vcxproj" /x "*.vcxproj.filters" /x "*.sln" /x "*.cmake" /x "*.py" /x "*.md" /x "*.vcxproj.user" /x "CMakeFiles" /x "RelWithDebInfo" /x "Debug" /x "MinSizeRel" /x ".vs" /x "vcpkg_installed" /x "*.dir" /x "include\*" /x "lib\*" "${PACK_SOURCE_DIR}\*.*"
    
    IfFileExists "$INSTDIR\EdgeSlicer.exe" 0 extract_error
    
    DetailPrint "Creating uninstaller..."
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    
    DetailPrint "Writing registry..."
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\EdgeSlicer.exe"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "HelpLink" "${PRODUCT_WEB_SITE}"
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoRepair" 1

    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "Version" "${VERSION}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "InstallPath" "$INSTDIR"

    ; URL protocol (same as macOS CFBundleURLSchemes): edgeslicer://. Registering the official
    ; app's snapmaker-orca:// and Snapmaker_Orca:// here took its handlers away machine-wide.
    DetailPrint "Registering the edgeslicer:// URL protocol..."
    SetRegView 64
    WriteRegStr HKLM "Software\Classes\edgeslicer" "" "URL:${PRODUCT_NAME}"
    WriteRegStr HKLM "Software\Classes\edgeslicer" "URL Protocol" ""
    WriteRegStr HKLM "Software\Classes\edgeslicer\shell\open\command" "" '"$INSTDIR\EdgeSlicer.exe" "%1"'
    SetRegView 32

    ; Windows Firewall keys its rules on the image path, so the binary rename would leave
    ; the old rule dead and pop a consent dialog the first time the LAN/phone service
    ; listens. Pre-create the inbound rule (delete first, so a reinstall stays idempotent).
    DetailPrint "Adding the Windows Firewall rule for TCP 13640..."
    nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${PRODUCT_NAME}"'
    Pop $0
    nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="${PRODUCT_NAME}" dir=in action=allow program="$INSTDIR\EdgeSlicer.exe" protocol=TCP localport=13640 profile=private,domain enable=yes'
    Pop $0

    ; WebRTC camera video: go2rtc (bundled) carries the media straight from this PC to the phone,
    ; so unlike everything else the hub runs it needs an inbound port - the first free one in
    ; 8555-8574 (RemoteHub.cpp, free_webrtc_port), hence a range, on both UDP (the media) and TCP
    ; (go2rtc's ICE-TCP fallback for networks that drop UDP). Program-bound, like the rule above.
    DetailPrint "Adding the Windows Firewall rule for WebRTC video (UDP/TCP 8555-8574)..."
    nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${PRODUCT_NAME} WebRTC video"'
    Pop $0
    nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="${PRODUCT_NAME} WebRTC video" dir=in action=allow program="$INSTDIR\resources\tools\go2rtc\go2rtc.exe" protocol=UDP localport=8555-8574 profile=private,domain enable=yes'
    Pop $0
    nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="${PRODUCT_NAME} WebRTC video" dir=in action=allow program="$INSTDIR\resources\tools\go2rtc\go2rtc.exe" protocol=TCP localport=8555-8574 profile=private,domain enable=yes'
    Pop $0

    DetailPrint "Installation complete!"
    Goto end_section
    
    extract_error:
        MessageBox MB_OK|MB_ICONSTOP "Installation failed: EdgeSlicer.exe was not found in the package. The installer may be corrupted."
        Abort
    
    end_section:
SectionEnd

Section "Desktop shortcut" SecDesktop
    DetailPrint "Creating desktop shortcut..."
    SetShellVarContext current
    CreateShortcut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\EdgeSlicer.exe" "" "$INSTDIR\EdgeSlicer.exe" 0
    SetShellVarContext all
SectionEnd

Section "Start menu shortcut" SecStartMenu
    DetailPrint "Creating start menu shortcut..."
    CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\EdgeSlicer.exe" "" "$INSTDIR\EdgeSlicer.exe" 0
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "Install ${PRODUCT_NAME} and all required files."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Create a desktop shortcut for ${PRODUCT_NAME}."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Create a start menu shortcut for ${PRODUCT_NAME}."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"

    DetailPrint "Uninstalling ${PRODUCT_NAME}..."
    
    DetailPrint "Checking for running processes..."
    nsExec::ExecToLog 'taskkill /F /IM EdgeSlicer.exe /T'
    ; the name this fork shipped under before the binary rename
    nsExec::ExecToLog 'taskkill /F /IM snapmaker-orca.exe /T'
    Sleep 500
    
    DetailPrint "Removing desktop shortcut..."
    Delete "$DESKTOP\Snapmaker Orca.lnk"  ; the shortcut older builds of this installer left
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
    
    DetailPrint "Removing start menu shortcut..."
    RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"
    
    DetailPrint "Removing install directory..."
    RMDir /r /REBOOTOK "$INSTDIR"
    
    RMDir "$INSTDIR"
    
    DetailPrint "Removing registry entries..."
    SetRegView 64
    DeleteRegKey HKLM "Software\Classes\edgeslicer"
    ; the scheme this fork registered under its previous name
    DeleteRegKey HKLM "Software\Classes\ultraone"
    SetRegView 32

    DetailPrint "Removing the Windows Firewall rule..."
    nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${PRODUCT_NAME}"'
    Pop $0
    ; Both WebRTC rules share one name, so one delete removes them.
    nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${PRODUCT_NAME} WebRTC video"'
    Pop $0

    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}"
    DeleteRegKey HKCU "${PRODUCT_INSTALL_KEY}"
    
    DetailPrint "Uninstall complete!"
SectionEnd

Function LaunchApp
    ExecShell "open" "$INSTDIR\EdgeSlicer.exe"
FunctionEnd

; Prevent overwriting locked DLLs when EdgeSlicer (or a legacy snapmaker-orca.exe /
; Snapmaker_Orca.exe from before the binary rename) is still running.
Function EnsureSnapmakerNotRunning
    snapmaker_check_loop:
        ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq EdgeSlicer.exe" 2>nul | find /i "EdgeSlicer.exe" >nul' $0
        IntCmp $0 0 snapmaker_in_use snapmaker_try_legacy1 snapmaker_try_legacy1
    snapmaker_try_legacy1:
        ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq snapmaker-orca.exe" 2>nul | find /i "snapmaker-orca.exe" >nul' $0
        IntCmp $0 0 snapmaker_in_use snapmaker_try_legacy2 snapmaker_try_legacy2
    snapmaker_try_legacy2:
        ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq Snapmaker_Orca.exe" 2>nul | find /i "Snapmaker_Orca.exe" >nul' $0
        IntCmp $0 0 snapmaker_in_use snapmaker_idle snapmaker_idle
    snapmaker_in_use:
        IfSilent snapmaker_silent snapmaker_prompt
    snapmaker_silent:
        SetErrorLevel 7
        Quit
    snapmaker_prompt:
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${PRODUCT_NAME} is still running (EdgeSlicer.exe).$\r$\nClose the program, then click Retry, or Cancel to exit the installer." IDRETRY snapmaker_check_loop
        Abort
    snapmaker_idle:
FunctionEnd

Function .onInit

    Call EnsureSnapmakerNotRunning

    ReadRegStr $R0 ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString"
    StrCmp $R0 "" done

    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
    "${PRODUCT_NAME} is already installed.$\n$\nClick OK to uninstall the old version, or Cancel to abort." \
    IDOK uninst
    Abort
    
    uninst:
        ClearErrors
        ExecWait '$R0 _?=$INSTDIR'
        
        IfErrors no_remove_uninstaller done
        no_remove_uninstaller:
    
    done:
FunctionEnd
