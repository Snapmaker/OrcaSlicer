; [1] PACK_SOURCE_DIR = compile-time only (e.g. .\gitlab_build\Snapmaker_Orca). [2] INSTALL_DIR_RUNTIME = runtime install dir (default .\ = $EXEDIR).
Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "WinMessages.nsh"

; Two names are in play. PRODUCT_NAME is the registration name (underscore):
; ARP key, vendor key, window title and default Start Menu group. Users only ever
; see PRODUCT_DISPLAY_NAME, which stays the space form in prompts and shortcuts.
!define PRODUCT_NAME "Snapmaker_Orca"
!define PRODUCT_DISPLAY_NAME "Snapmaker Orca"
!define PRODUCT_PUBLISHER "Snapmaker"
!define PRODUCT_WEB_SITE "https://github.com/Snapmaker/OrcaSlicer"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"
!define PRODUCT_INSTALL_KEY "Software\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}"
; Registration names written by older GitLab packages (space form).
!define PRODUCT_LEGACY_NAME "Snapmaker Orca"
!define PRODUCT_LEGACY_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_LEGACY_NAME}"
!define PRODUCT_LEGACY_INSTALL_KEY "Software\${PRODUCT_PUBLISHER}\${PRODUCT_LEGACY_NAME}"

!ifndef VERSION
    !searchparse /noerrors /file "version.inc" `set(Snapmaker_VERSION "` VERSION `")`
!endif

!ifndef SOURCE_DIR
    !define SOURCE_DIR ".\gitlab_build\Snapmaker_Orca"
!endif
!define PACK_SOURCE_DIR "${SOURCE_DIR}"

; 64-bit app: use PROGRAMFILES64 so default path is C:\Program Files\Snapmaker_Orca, not (x86)
!define INSTALL_DIR_RUNTIME "$PROGRAMFILES64\Snapmaker_Orca"
InstallDir "${INSTALL_DIR_RUNTIME}"

!ifndef OUTPUT_FILE
    !define OUTPUT_FILE "Snapmaker_Orca_Windows_Installer_V${VERSION}.exe"
!endif

; License page: show LICENSE.txt from repo root (same dir as this .nsi)
!ifndef LICENSE_FILE
    !define LICENSE_FILE ".\LICENSE.txt"
!endif

RequestExecutionLevel admin

; No /SOLID to avoid "Internal compiler error #12345: error mmapping datablock"
SetCompressor lzma

; Hide the default "Nullsoft Install System vX.XX" brand text. The single
; space matters: an empty string restores the default text.
BrandingText " "

; Installer and uninstaller icon: set by build script via /DICON_FILE=path (e.g. Snapmaker_Orca.ico or snapmaker.ico)
!ifdef ICON_FILE
    !define MUI_ICON "${ICON_FILE}"
    !define MUI_UNICON "${ICON_FILE}"
!else
    !define MUI_ICON ".\resources\images\Snapmaker_Orca.ico"
    !define MUI_UNICON ".\resources\images\Snapmaker_Orca.ico"
!endif

Var StartMenuFolder
; "1" once a previous installation is detected, before the old
; uninstaller removes the registry keys it would be detected by.
Var SnapmakerIsUpgrade
Var SnapmakerFinTitle
Var SnapmakerFinText
Var SnapmakerFinBitmap
Var SnapmakerFinBitmapCtl
Var SnapmakerFinFont
Var SnapmakerFinCheckbox
Var SnapmakerFinCheckboxText
Var SnapmakerFinTextCtl

; No MUI_WELCOMEPAGE_TITLE/TEXT, the page falls back to the MUI default text
; (localized, and the title follows the Name directive).
!insertmacro MUI_PAGE_WELCOME

; License page without the mandatory checkbox.
!ifdef LICENSE_FILE
    !insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!endif

!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder in which to install ${PRODUCT_DISPLAY_NAME}."
!insertmacro MUI_PAGE_DIRECTORY

; The Start Menu page owns both the folder name and the "create
; shortcuts" choice; the desktop shortcut is created in the same block, so it can
; no longer be selected on its own. No components page.
!define MUI_STARTMENUPAGE_REGISTRY_ROOT "HKLM"
!define MUI_STARTMENUPAGE_REGISTRY_KEY "${PRODUCT_INSTALL_KEY}"
!define MUI_STARTMENUPAGE_REGISTRY_VALUENAME "Start Menu Folder"
!define MUI_STARTMENUPAGE_DEFAULTFOLDER "${PRODUCT_NAME}"
!insertmacro MUI_PAGE_STARTMENU Application $StartMenuFolder

!insertmacro MUI_PAGE_INSTFILES

; No "run application" option and no website link on the finish page.
!insertmacro MUI_PAGE_FINISH

; The uninstaller's finish page carries the "delete personal data" option
; (unchecked by default). It is registered after the file removal page, the way
; the CPack installer registers its finish page.
!insertmacro MUI_UNPAGE_INSTFILES
UninstPage custom un.SnapmakerUninstallFinishCreate un.SnapmakerUninstallFinishLeave

; Installer languages: English first (= default) and SimpChinese, the two
; languages our own prompts exist in. Inserting the full 64-language CPack set
; only localized MUI's built-in buttons while every custom prompt stayed
; English, so the narrower set keeps mixed-locale installs consistent.
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

; Prompts in Chinese and English, wording taken from the CPack installer.
; Prompts keep the space-form product name; the underscore name never reaches a
; prompt. The texts branch on $LANGUAGE at runtime instead of using LangString,
; so every language other than SimpChinese gets the English text.
!ifndef LANG_SIMPCHINESE
!define LANG_SIMPCHINESE 2052
!endif

; Resolves the install directory of a previous installation into $R0,
; empty when nothing is registered. Sources, in order: ARP InstallLocation, ARP
; UninstallString without its "\Uninstall.exe" tail, the vendor key default value
; (what the CPack installer writes) and the vendor key InstallPath. Both
; registration names are tried; there is no $INSTDIR fallback here - the uninstaller's delete target adds one.
; $R1..$R4 are scratch. Reads use the 32-bit view, the view this installer writes
; those keys in.
!macro SNAPMAKER_RESOLVE_INSTALL_DIR _PREFIX
    SetRegView 32
    StrCpy $R0 ""
    ReadRegStr $R0 HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation"
    StrCmp $R0 "" 0 ${_PREFIX}_found
    ReadRegStr $R0 HKLM "${PRODUCT_LEGACY_UNINST_KEY}" "InstallLocation"
    StrCmp $R0 "" 0 ${_PREFIX}_found
    ReadRegStr $R1 HKLM "${PRODUCT_UNINST_KEY}" "UninstallString"
    StrCmp $R1 "" 0 ${_PREFIX}_parse
    ReadRegStr $R1 HKLM "${PRODUCT_LEGACY_UNINST_KEY}" "UninstallString"
    StrCmp $R1 "" ${_PREFIX}_vendor ${_PREFIX}_parse
    ${_PREFIX}_parse:
        StrCpy $R2 $R1
        StrCpy $R3 $R2 1 0
        StrCmp $R3 "$\"" 0 ${_PREFIX}_parse_trailing_quote
        StrCpy $R2 $R2 "" 1
    ${_PREFIX}_parse_trailing_quote:
        StrCpy $R3 $R2 1 -1
        StrCmp $R3 "$\"" 0 ${_PREFIX}_parse_tail
        StrCpy $R2 $R2 -1
    ${_PREFIX}_parse_tail:
        StrLen $R3 "\Uninstall.exe"
        StrCpy $R4 $R2 $R3 -$R3
        StrCmp $R4 "\Uninstall.exe" 0 ${_PREFIX}_vendor
        StrCpy $R0 $R2 -$R3
        StrCmp $R0 "" 0 ${_PREFIX}_found
    ${_PREFIX}_vendor:
        ReadRegStr $R0 HKLM "${PRODUCT_INSTALL_KEY}" ""
        StrCmp $R0 "" 0 ${_PREFIX}_found
        ReadRegStr $R0 HKLM "${PRODUCT_LEGACY_INSTALL_KEY}" ""
        StrCmp $R0 "" 0 ${_PREFIX}_found
        ReadRegStr $R0 HKLM "${PRODUCT_INSTALL_KEY}" "InstallPath"
        StrCmp $R0 "" 0 ${_PREFIX}_found
        ReadRegStr $R0 HKLM "${PRODUCT_LEGACY_INSTALL_KEY}" "InstallPath"
    ${_PREFIX}_found:
!macroend

Name "${PRODUCT_NAME} V${VERSION}"
OutFile "${OUTPUT_FILE}"

Section "Main program" SecMain
    SectionIn RO

    ; The process check and the already-installed prompt run at the head of
    ; the install section - after the pages, when the user clicks Install - and
    ; no longer from .onInit. That ordering is also what makes the directory prefill in
    ; .onInit correct: nothing uninstalls the old version before it is read.
    Call EnsureSnapmakerNotRunning

    ; Identify an existing installation by either registration name and
    ; offer uninstall / overwrite / cancel. A missing uninstaller no longer blocks
    ; the install; it only skips the removal.
    Call FindExistingUninstaller
    StrCmp $SnapmakerIsUpgrade "1" 0 snap_no_existing_install
    Call ReadInstalledVersion
    StrCmp $R7 "" snap_prompt_without_version snap_prompt_with_version

    snap_prompt_with_version:
        StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snap_prompt_with_version_en
        MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION "已安装 Snapmaker Orca V$R7。$\n$\n是否先卸载旧版本，再安装新版本？" /SD IDYES IDYES snap_uninstall_old IDNO snap_no_existing_install
        Goto snap_prompt_abort
    snap_prompt_with_version_en:
        MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION "Snapmaker Orca V$R7 is already installed.$\n$\nDo you want to uninstall the old version before installing the new one?" /SD IDYES IDYES snap_uninstall_old IDNO snap_no_existing_install
    snap_prompt_abort:
        Abort
    snap_prompt_without_version:
        StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snap_prompt_without_version_en
        MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION "已安装 Snapmaker Orca。$\n$\n是否先卸载旧版本，再安装新版本？" /SD IDYES IDYES snap_uninstall_old IDNO snap_no_existing_install
        Goto snap_prompt_abort_without_version
    snap_prompt_without_version_en:
        MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION "Snapmaker Orca is already installed.$\n$\nDo you want to uninstall the old version before installing the new one?" /SD IDYES IDYES snap_uninstall_old IDNO snap_no_existing_install
    snap_prompt_abort_without_version:
        Abort

    snap_uninstall_old:
        StrCmp $R9 "" snap_uninstall_skipped
        IfFileExists "$R9" 0 snap_uninstall_skipped
        ClearErrors
        StrLen $R1 "\Uninstall.exe"
        StrCpy $R2 $R9 -$R1
    snap_uninstall_trim_slash:
        StrCpy $R1 $R2 1 -1
        StrCmp $R1 "\" snap_uninstall_do_trim snap_uninstall_slash_done
    snap_uninstall_do_trim:
        StrCpy $R2 $R2 -1
        Goto snap_uninstall_trim_slash
    snap_uninstall_slash_done:
        ; /S keeps the upgrade path from re-asking for confirmation.
        ExecWait '"$R9" /S _?=$R2' $R3
        IfErrors snap_uninstall_failed
        IntCmp $R3 0 snap_uninstall_ok snap_uninstall_failed snap_uninstall_failed
    snap_uninstall_ok:
        ; The old uninstaller's self-delete may be deferred or fail; remove it
        ; again so Uninstall.exe never survives an upgrade into the new install.
        Delete "$R9"
        ClearErrors
        Goto snap_no_existing_install
    snap_uninstall_failed:
        StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snap_uninstall_failed_en
        MessageBox MB_OK|MB_ICONSTOP "卸载失败。$\n卸载程序路径：$R9$\n工作目录：$R2$\n退出码：$R3"
        Goto snap_uninstall_failed_done
    snap_uninstall_failed_en:
        MessageBox MB_OK|MB_ICONSTOP "Uninstall failed.$\nUninstaller path: $R9$\nWorking dir: $R2$\nExit code: $R3"
    snap_uninstall_failed_done:
        Abort
    snap_uninstall_skipped:
        ; Uninstaller missing, overwrite in place instead of blocking.
        StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snap_uninstall_skipped_en
        DetailPrint "未找到卸载程序，跳过旧版本移除并直接覆盖安装。"
        Goto snap_uninstall_skipped_done
    snap_uninstall_skipped_en:
        DetailPrint "Uninstaller not found, skipping old-version removal and overwriting."
    snap_uninstall_skipped_done:
    snap_no_existing_install:

    ; An upgrade clears the agreed cache directories so the freshly installed
    ; assets are unpacked instead of stale ones being reused. The context is set
    ; explicitly because $APPDATA/$LOCALAPPDATA follow it, and the shortcut code
    ; below expects the all-users context it restores here.
    StrCmp $SnapmakerIsUpgrade "1" 0 snap_upgrade_cleanup_done
    StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snap_upgrade_cleanup_en
    DetailPrint "正在清理升级残留数据..."
    Goto snap_upgrade_cleanup_start
    snap_upgrade_cleanup_en:
    DetailPrint "Cleaning up upgrade data..."
    snap_upgrade_cleanup_start:
    SetShellVarContext current
    RMDir /r "$APPDATA\${PRODUCT_NAME}\system"
    RMDir /r "$APPDATA\${PRODUCT_NAME}\ota"
    RMDir /r "$APPDATA\${PRODUCT_NAME}\web\flutter_web"
    SetShellVarContext all
    snap_upgrade_cleanup_done:

    SetRegView 32
    SetOutPath "$INSTDIR"

    DetailPrint "Installing ${PRODUCT_DISPLAY_NAME}..."
    DetailPrint "Target dir: $INSTDIR"

    DetailPrint "Copying files..."

    ; PACK_SOURCE_DIR = compile time only. At runtime this File extracts from embedded payload to $INSTDIR.
    File /r /x "*.pdb" /x "*.ilk" /x "*.exp" /x "*.lib" /x "*.obj" /x "*.idb" /x "*.tlog" /x "*.h" /x "*.hpp" /x "*.c" /x "*.cpp" /x "*.cxx" /x "*.cc" /x "*.vcxproj" /x "*.vcxproj.filters" /x "*.sln" /x "*.cmake" /x "*.py" /x "*.vcxproj.user" /x "CMakeFiles" /x "RelWithDebInfo" /x "Debug" /x "MinSizeRel" /x ".vs" /x "vcpkg_installed" /x "*.dir" "${PACK_SOURCE_DIR}\*.*"

    DetailPrint "Creating uninstaller..."
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; Registration name, and the vendor key gets the default value the CPack
    ; installer writes (it is what the directory prefill and the uninstall delete target read back).
    ; ARP DisplayVersion uses the first three version fields.
    Call GetDisplayVersion
    DetailPrint "Writing registry..."
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    ; Quoted, the way the CPack installer writes it (the default path contains
    ; spaces).
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\snapmaker-orca.exe"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "$R6"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "HelpLink" "${PRODUCT_WEB_SITE}"
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoRepair" 1

    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "" "$INSTDIR"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "Version" "${VERSION}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "InstallPath" "$INSTDIR"

    ; URL protocols (same as macOS CFBundleURLSchemes): snapmaker-orca:// and Snapmaker_Orca://
    DetailPrint "Registering URL protocols (snapmaker-orca, Snapmaker_Orca)..."
    SetRegView 64
    WriteRegStr HKLM "Software\Classes\snapmaker-orca" "" "URL:Snapmaker Orca"
    WriteRegStr HKLM "Software\Classes\snapmaker-orca" "URL Protocol" ""
    WriteRegStr HKLM "Software\Classes\snapmaker-orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'
    WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "" "URL:Snapmaker Orca"
    WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "URL Protocol" ""
    WriteRegStr HKLM "Software\Classes\Snapmaker_Orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'
    SetRegView 32

    ; Shortcuts are created only when the Start Menu page left the
    ; checkbox checked, and the folder name comes from that page.
    !insertmacro MUI_STARTMENU_WRITE_BEGIN Application
        DetailPrint "Creating shortcuts..."
        SetShellVarContext current
        CreateShortcut "$DESKTOP\${PRODUCT_DISPLAY_NAME}.lnk" "$INSTDIR\snapmaker-orca.exe" "" "$INSTDIR\snapmaker-orca.exe" 0
        SetShellVarContext all
        CreateDirectory "$SMPROGRAMS\$StartMenuFolder"
        CreateShortcut "$SMPROGRAMS\$StartMenuFolder\${PRODUCT_DISPLAY_NAME}.lnk" "$INSTDIR\snapmaker-orca.exe" "" "$INSTDIR\snapmaker-orca.exe" 0
        CreateShortcut "$SMPROGRAMS\$StartMenuFolder\Uninstall.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0
    !insertmacro MUI_STARTMENU_WRITE_END

    DetailPrint "Installation complete!"
SectionEnd

Section "Uninstall"

    ; Confirmation at the head of the uninstall section, Chinese and
    ; English, skipped for silent runs. "No" must Quit, not Abort: Abort inside a
    ; section only aborts the section and leaves the wizard stuck on the progress
    ; page instead of closing the window.
    IfSilent snap_confirm_done
    StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snap_confirm_en
    MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON2 "确定要彻底卸载 Snapmaker Orca V${VERSION} 及其全部组件吗？" IDYES snap_confirm_done
    Quit
    snap_confirm_en:
    MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON2 "Are you sure you want to completely uninstall Snapmaker Orca V${VERSION} and all of its components?" IDYES snap_confirm_done
    Quit
    snap_confirm_done:

    ; Advance to the finish page as soon as the removal finishes.
    SetAutoClose true

    DetailPrint "Uninstalling ${PRODUCT_DISPLAY_NAME}..."

    DetailPrint "Checking for running processes..."
    nsExec::ExecToLog 'taskkill /F /IM snapmaker-orca.exe /T'
    Pop $0
    nsExec::ExecToLog 'taskkill /F /IM Snapmaker_Orca.exe /T'
    Pop $0
    Sleep 500

    DetailPrint "Removing desktop shortcut..."
    SetShellVarContext current
    Delete "$DESKTOP\${PRODUCT_DISPLAY_NAME}.lnk"
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
    SetShellVarContext all

    ; Remove the shortcuts inside the folder recorded on the Start Menu
    ; page and prune the folders they leave empty - the leaf folder is only
    ; removed while empty, so a custom folder holding anything else survives - then
    ; the two product group names this installer and older packages create.
    DetailPrint "Removing start menu shortcut..."
    SetRegView 32
    !insertmacro MUI_STARTMENU_GETFOLDER Application $R5
    StrCmp $R5 "" snap_start_menu_loop_done
    Delete "$SMPROGRAMS\$R5\${PRODUCT_DISPLAY_NAME}.lnk"
    Delete "$SMPROGRAMS\$R5\Uninstall.lnk"
    StrCpy $R6 "$SMPROGRAMS\$R5"
    StrCpy $R7 0
    snap_start_menu_parent_loop:
        IntOp $R7 $R7 + 1
        StrCmp $R7 8 snap_start_menu_loop_done
        ClearErrors
        RMDir "$R6"
        IfErrors snap_start_menu_loop_done
        GetFullPathName $R6 "$R6\.."
        StrCmp "$R6" "$SMPROGRAMS" snap_start_menu_loop_done snap_start_menu_parent_loop
    snap_start_menu_loop_done:
        RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"
        RMDir /r "$SMPROGRAMS\${PRODUCT_LEGACY_NAME}"

    ; Remove the install directory taken from the registry, falling back
    ; to $INSTDIR the way the CPack installer does. An empty path is never passed
    ; to a delete command, and NSIS cannot delete its own working directory.
    DetailPrint "Removing install directory..."
    !insertmacro SNAPMAKER_RESOLVE_INSTALL_DIR snap_install_dir
    StrCmp $R0 "" 0 snap_install_dir_resolved
    StrCpy $R0 "$INSTDIR"
    snap_install_dir_resolved:
    StrCmp $R0 "" 0 snap_install_dir_delete
    StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snap_install_dir_unknown_en
    MessageBox MB_OK|MB_ICONEXCLAMATION "无法确定安装目录，已跳过删除。请手动删除该目录。"
    Goto snap_install_dir_done
    snap_install_dir_unknown_en:
    MessageBox MB_OK|MB_ICONEXCLAMATION "The installation directory could not be determined, so it was left in place. Please delete it manually."
    Goto snap_install_dir_done
    snap_install_dir_delete:
        SetOutPath "$TEMP"
        Delete "$R0\Uninstall.exe"
        RMDir /r /REBOOTOK "$R0"
        RMDir "$R0"
    snap_install_dir_done:

    ; URL protocol keys under both hives. The app writes the per-user copy
    ; at runtime, the installer only writes the machine-wide one.
    DetailPrint "Removing registry entries..."
    SetRegView 64
    DeleteRegKey HKLM "Software\Classes\snapmaker-orca"
    DeleteRegKey HKLM "Software\Classes\Snapmaker_Orca"
    DeleteRegKey HKCU "Software\Classes\snapmaker-orca"
    DeleteRegKey HKCU "Software\Classes\Snapmaker_Orca"

    ; ARP and vendor keys for both registration names, then the
    ; vendor parent key when it is left empty.
    SetRegView 32
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_LEGACY_UNINST_KEY}"
    DeleteRegKey HKCU "${PRODUCT_UNINST_KEY}"
    DeleteRegKey HKCU "${PRODUCT_LEGACY_UNINST_KEY}"
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}"
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_LEGACY_INSTALL_KEY}"
    DeleteRegKey HKCU "${PRODUCT_INSTALL_KEY}"
    DeleteRegKey HKCU "${PRODUCT_LEGACY_INSTALL_KEY}"
    DeleteRegKey /ifempty ${PRODUCT_UNINST_ROOT_KEY} "Software\${PRODUCT_PUBLISHER}"
    DeleteRegKey /ifempty HKCU "Software\${PRODUCT_PUBLISHER}"

    DetailPrint "Uninstall complete!"
SectionEnd

; ARP DisplayVersion is the first three version fields recomposed, the way
; the CPack side derives it from the version regex, so "2.4.0-rc1" reports "2.4.0"
; and "2.4.0.1" reports "2.4.0". Result in $R6.
Function GetDisplayVersion
    StrCpy $R2 "${VERSION}"
    StrCpy $R6 ""
    StrCpy $R4 0
    StrCpy $R5 0
    snap_display_version_scan:
        StrCpy $R3 $R2 1 $R4
        StrCmp $R3 "" snap_display_version_done
        StrCmp $R3 "." snap_display_version_dot
        ; Anything that is not a digit ends the numeric prefix.
        StrCpy $R1 "0123456789"
        StrCpy $R0 0
    snap_display_version_digit:
        StrCpy $R7 $R1 1 $R0
        StrCmp $R7 "" snap_display_version_done
        StrCmp $R3 $R7 snap_display_version_keep_digit
        IntOp $R0 $R0 + 1
        Goto snap_display_version_digit
    snap_display_version_keep_digit:
        StrCpy $R6 "$R6$R3"
        IntOp $R4 $R4 + 1
        Goto snap_display_version_scan
    snap_display_version_dot:
        ; A separator counts only between fields, and only for the first three.
        StrCmp $R6 "" snap_display_version_done
        StrCmp $R5 2 snap_display_version_done
        IntOp $R5 $R5 + 1
        StrCpy $R6 "$R6."
        IntOp $R4 $R4 + 1
        Goto snap_display_version_scan
    snap_display_version_done:
        ; Never report an empty version for a string the recomposition rejects.
        StrCmp $R6 "" 0 snap_display_version_end
        StrCpy $R6 "${VERSION}"
    snap_display_version_end:
FunctionEnd

; Prevent overwriting locked DLLs when snapmaker-orca (or legacy Snapmaker_Orca.exe) is still running.
Function EnsureSnapmakerNotRunning
    snapmaker_check_loop:
        nsExec::Exec 'cmd.exe /c tasklist /FI "IMAGENAME eq snapmaker-orca.exe" 2>nul | find /i "snapmaker-orca.exe" >nul'
        Pop $0
        IntCmp $0 0 snapmaker_in_use snapmaker_try_legacy snapmaker_try_legacy
    snapmaker_try_legacy:
        nsExec::Exec 'cmd.exe /c tasklist /FI "IMAGENAME eq Snapmaker_Orca.exe" 2>nul | find /i "Snapmaker_Orca.exe" >nul'
        Pop $0
        IntCmp $0 0 snapmaker_in_use snapmaker_idle snapmaker_idle
    snapmaker_in_use:
        IfSilent snapmaker_silent snapmaker_prompt
    snapmaker_silent:
        SetErrorLevel 7
        Quit
    snapmaker_prompt:
        StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 snapmaker_prompt_en
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Snapmaker Orca 仍在运行。$\n请关闭程序后点击重试继续，或点击取消退出安装程序。" IDRETRY snapmaker_check_loop
        Goto snapmaker_prompt_done
    snapmaker_prompt_en:
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Snapmaker Orca is still running.$\nClose the program, then click Retry to continue, or Cancel to exit the installer." IDRETRY snapmaker_check_loop
    snapmaker_prompt_done:
        Abort
    snapmaker_idle:
FunctionEnd

; Locates an existing installation, setting $R9 to its Uninstall.exe (empty
; when there is none) and $SnapmakerIsUpgrade to "1" as soon as any trace of an
; installation is found - an ARP entry under either registration name, an
; Uninstall.exe in the target directory, or an application executable there. The
; flag drives both the already-installed prompt and the upgrade cache cleanup, and it is set here because
; the old uninstaller deletes the registry keys it would otherwise be read from.
Function FindExistingUninstaller
    StrCpy $R9 ""
    StrCpy $SnapmakerIsUpgrade "0"
    StrCpy $R8 ""
    ReadRegStr $R8 HKLM "${PRODUCT_UNINST_KEY}" "UninstallString"
    StrCmp $R8 "" 0 find_uninst_have_str
    ReadRegStr $R8 HKCU "${PRODUCT_UNINST_KEY}" "UninstallString"
    StrCmp $R8 "" 0 find_uninst_have_str
    ReadRegStr $R8 HKLM "${PRODUCT_LEGACY_UNINST_KEY}" "UninstallString"
    StrCmp $R8 "" 0 find_uninst_have_str
    ReadRegStr $R8 HKCU "${PRODUCT_LEGACY_UNINST_KEY}" "UninstallString"
    StrCmp $R8 "" 0 find_uninst_have_str
    IfFileExists "$INSTDIR\Uninstall.exe" 0 find_uninst_check_app
    StrCpy $R8 "$INSTDIR\Uninstall.exe"
    Goto find_uninst_have_str
    find_uninst_check_app:
        IfFileExists "$INSTDIR\snapmaker-orca.exe" find_uninst_have_existing find_uninst_check_legacy
    find_uninst_check_legacy:
        IfFileExists "$INSTDIR\Snapmaker_Orca.exe" find_uninst_have_existing find_uninst_done
    find_uninst_have_existing:
        ; Application found without a registered uninstaller: still an upgrade, and
        ; the removal step is skipped because $R9 stays empty.
        StrCpy $SnapmakerIsUpgrade "1"
        Goto find_uninst_done
    find_uninst_have_str:
        StrCpy $SnapmakerIsUpgrade "1"
        StrCpy $0 $R8 1 0
        StrCmp $0 "$\"" 0 find_uninst_check_last
        StrCpy $R8 $R8 "" 1
    find_uninst_check_last:
        StrCpy $0 $R8 1 -1
        StrCmp $0 "$\"" 0 find_uninst_exists
        StrCpy $R8 $R8 -1
    find_uninst_exists:
        IfFileExists "$R8" 0 find_uninst_done
        StrCpy $R9 $R8
    find_uninst_done:
FunctionEnd

; Sets $R7 to the already-installed version from ARP DisplayVersion / Version.
Function ReadInstalledVersion
    StrCpy $R7 ""
    SetRegView 64
    Call ReadInstalledVersionFromView
    StrCmp $R7 "" 0 read_ver_strip
    SetRegView 32
    Call ReadInstalledVersionFromView
    read_ver_strip:
        SetRegView 32
        StrCmp $R7 "" read_ver_done
        StrCpy $0 $R7 1
        StrCmp $0 "V" read_ver_strip_one
        StrCmp $0 "v" read_ver_strip_one read_ver_done
    read_ver_strip_one:
        StrCpy $R7 $R7 "" 1
    read_ver_done:
FunctionEnd

Function ReadInstalledVersionFromView
    ReadRegStr $R7 HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion"
    StrCmp $R7 "" 0 read_ver_view_done
    ReadRegStr $R7 HKCU "${PRODUCT_UNINST_KEY}" "DisplayVersion"
    StrCmp $R7 "" 0 read_ver_view_done
    ReadRegStr $R7 HKLM "${PRODUCT_LEGACY_UNINST_KEY}" "DisplayVersion"
    StrCmp $R7 "" 0 read_ver_view_done
    ReadRegStr $R7 HKCU "${PRODUCT_LEGACY_UNINST_KEY}" "DisplayVersion"
    StrCmp $R7 "" 0 read_ver_view_done
    ReadRegStr $R7 HKLM "${PRODUCT_INSTALL_KEY}" "Version"
    StrCmp $R7 "" 0 read_ver_view_done
    ReadRegStr $R7 HKCU "${PRODUCT_INSTALL_KEY}" "Version"
    StrCmp $R7 "" 0 read_ver_view_done
    ReadRegStr $R7 HKLM "${PRODUCT_LEGACY_INSTALL_KEY}" "Version"
    StrCmp $R7 "" 0 read_ver_view_done
    ReadRegStr $R7 HKCU "${PRODUCT_LEGACY_INSTALL_KEY}" "Version"
    read_ver_view_done:
FunctionEnd

; Prefill the directory page from the previous installation, unless the
; user already overrode $INSTDIR on the command line (/D=).
Function .onInit
    StrCmp "$INSTDIR" "${INSTALL_DIR_RUNTIME}" 0 snap_on_init_done
    !insertmacro SNAPMAKER_RESOLVE_INSTALL_DIR snap_on_init
    StrCmp $R0 "" snap_on_init_done
    StrCpy $INSTDIR $R0
    snap_on_init_done:
FunctionEnd

; The uninstall finish page, styled like the welcome page the way the CPack
; installer's SnapmakerUninstallFinish.nsh does it: hide the MUI header strip,
; reuse MUI's own welcome-page banner/font assets, and create the "delete
; personal data" checkbox UNCHECKED so a casual Finish click keeps user data.
; The two tiers run from the leave callback, so a silent (/S) upgrade - no pages,
; no callbacks - never touches user data.
; ---- Layout constants: the only values to hand-tune. ----
; Vertical geometry is in dialog units (u suffix applied at use sites) and
; derived at compile time with !define /math. Horizontal geometry is in pixels
; and derived at run time from the measured wizard client width, so the
; banner:text width ratio holds at any DPI or window size.
!define SF_TOP 10            ; top of the title+text block
!define SF_TITLE_H 34        ; title height (reserves two wrapped lines)
!define SF_TEXT_H 16         ; text label height (one line)
!define SF_CHECK_H 16        ; user-data checkbox height (one line)
!define SF_CHECK_BAND_NUM 3  ; checkbox floats NUM/DEN of the way into the free
!define SF_CHECK_BAND_DEN 8  ; band below the text block (3/8 = reference design)
!define SF_GAP_V 4           ; vertical gap between title and text label
!define SF_MARGIN_X 16       ; right page margin (text box to right edge), px
!define SF_GAP_X 24          ; gap between banner and text box, in pixels
!define SF_BITMAP_PART 1     ; banner share of the usable width (ratio numerator)
!define SF_TITLE_PART 2      ; text share of the usable width (ratio numerator)
!define SF_BTN_RESERVE 40    ; bottom button row height kept out of the content
                             ; area, in pixels (buttons are ~23px at 96 DPI and
                             ; scale with DPI; raise this for high-DPI overlap)
; ---- Derived values: computed, do not edit. ----
; NOTE: !define /math takes exactly two operands, so sums are chained through
; intermediate symbols instead of written as a + b + c.
!define /math SF_TITLE_BOTTOM ${SF_TOP} + ${SF_TITLE_H}
!define /math SF_TEXT_Y   ${SF_TITLE_BOTTOM} + ${SF_GAP_V}
!define /math SF_PARTS    ${SF_BITMAP_PART} + ${SF_TITLE_PART}

Function un.SnapmakerUninstallFinishCreate
  StrCmp $LANGUAGE ${LANG_SIMPCHINESE} 0 SnapmakerFin_l10n_en
  StrCpy $SnapmakerFinTitle "Snapmaker Orca V${VERSION} 已成功从本机卸载。"
  StrCpy $SnapmakerFinText "点击【完成】关闭卸载程序"
  StrCpy $SnapmakerFinCheckboxText "删除个人数据"
  SendMessage $mui.Button.Next ${WM_SETTEXT} 0 "STR:完成(&F)"
  Goto SnapmakerFin_l10n_done
SnapmakerFin_l10n_en:
  StrCpy $SnapmakerFinTitle "Snapmaker Orca V${VERSION} has been successfully uninstalled from this computer."
  StrCpy $SnapmakerFinText "Click Finish to close the uninstaller"
  StrCpy $SnapmakerFinCheckboxText "Delete personal data"
  SendMessage $mui.Button.Next ${WM_SETTEXT} 0 "STR:&Finish"
SnapmakerFin_l10n_done:
  ; Finish is the only way out: disable Back and Cancel.
  EnableWindow $mui.Button.Cancel 0
  EnableWindow $mui.Button.Back 0
  ; Welcome-page look: remove the WHOLE MUI header strip, not just its texts.
  ShowWindow $mui.Header.Background ${SW_HIDE}
  ShowWindow $mui.Line.Standard ${SW_HIDE}
  ShowWindow $mui.Header.Text ${SW_HIDE}
  ShowWindow $mui.Header.SubText ${SW_HIDE}
  ShowWindow $mui.Header.Image ${SW_HIDE}
  ; Expand the content area (1018) to the full wizard client so no empty band
  ; is left where the header strip used to be (what MUI's ioSpecial pages do).
  System::Call '*(i, i, i, i) i .r2'
  System::Call 'user32::GetClientRect(p $HWNDPARENT, p r2)'
  System::Call '*$2(i, i, i .r3, i .r4)'
  System::Free $2
  IntOp $4 $4 - ${SF_BTN_RESERVE}
  GetDlgItem $2 $HWNDPARENT 1018
  System::Call 'user32::SetWindowPos(p $2, p 0, i 0, i 0, i r3, i r4, i 0x4)'
  ; Horizontal layout, derived from the measured client width ($3, pixels):
  ; usable = width - SF_MARGIN_X (right) - SF_GAP_X (banner-to-text), split into
  ; SF_PARTS equal shares with the banner taking SF_BITMAP_PART and the text
  ; box SF_TITLE_PART (1:2). The banner is flush against the left edge, so the
  ; text x is banner width + gap. nsDialogs treats plain numbers as pixels and
  ; "Nu" as dialog units, so controls mix run-time pixel x/w with u y/h.
  IntOp $5 $3 - ${SF_MARGIN_X}
  IntOp $5 $5 - ${SF_GAP_X}
  IntOp $6 $5 / ${SF_PARTS}
  IntOp $7 $6 * ${SF_TITLE_PART}
  IntOp $8 $6 + ${SF_GAP_X}
  ; Welcome-page look: banner fills the page below the button row on the left.
  nsDialogs::Create 1018
  Pop $0
  SetCtlColors $0 "" 0xFFFFFF
  ; Banner: same win.bmp asset MUI_WELCOMEFINISHPAGE stretches on the installer
  ; welcome page -- full-height (client height $4, already minus SF_BTN_RESERVE)
  ; and 1/3 of the client width, flush to the top-left corner.
  ${NSD_CreateBitmap} 0 0 $6 $4 ""
  Pop $SnapmakerFinBitmapCtl
  ${NSD_SetStretchedImage} $SnapmakerFinBitmapCtl "${NSISDIR}\Contrib\Graphics\Wizard\win.bmp" $SnapmakerFinBitmap
  ; Title in welcome-page style: 12pt bold via the built-in CreateFont
  ; instruction, same font face and size/weight as MUI2's own Welcome/Finish
  ; titles. $(^Font) resolves per installer language (MS Shell Dlg -> Segoe UI
  ; for English, SimpChinese via SimpChinese.nlf), so Chinese and English both
  ; render with native glyphs. The instruction takes POINTS ("12" = 12pt =
  ; 16px at 96 DPI), unlike raw gdi32::CreateFont whose height is pixels.
  CreateFont $SnapmakerFinFont "$(^Font)" "12" "700"
  ; The English title is long; SS_EDITCONTROL makes the static control word-wrap
  ; instead of clipping. SF_TITLE_H reserves two lines; SF_TEXT_Y sits below it.
  nsDialogs::CreateControl "STATIC" ${DEFAULT_STYLES}|${SS_NOTIFY}|${SS_EDITCONTROL} ${WS_EX_TRANSPARENT} $8 ${SF_TOP}u $7 ${SF_TITLE_H}u $SnapmakerFinTitle
  Pop $1
  SetCtlColors $1 0x000000 0xFFFFFF
  SendMessage $1 ${WM_SETFONT} $SnapmakerFinFont 1
  ${NSD_CreateLabel} $8 ${SF_TEXT_Y}u $7 ${SF_TEXT_H}u $SnapmakerFinText
  Pop $SnapmakerFinTextCtl
  SetCtlColors $SnapmakerFinTextCtl 0x000000 0xFFFFFF
  SendMessage $SnapmakerFinTextCtl ${WM_SETFONT} $SnapmakerFinFont 1
  ; User-data checkbox, left-aligned with the texts, created UNCHECKED by
  ; default so a casual Finish click keeps user data. Created at y=0; the real
  ; position is set below by measuring the actual text-block bottom.
  ${NSD_CreateCheckBox} $8 0 $7 ${SF_CHECK_H}u $SnapmakerFinCheckboxText
  Pop $SnapmakerFinCheckbox
  SetCtlColors $SnapmakerFinCheckbox 0x000000 0xFFFFFF
  ; Float the checkbox SF_CHECK_BAND_NUM/SF_CHECK_BAND_DEN of the way into the
  ; free band between the text block and the content-area bottom. The band's
  ; two ends live in different coordinate systems (text block in dialog units,
  ; content bottom in measured pixels), so measure the text label's real pixel
  ; rect and place the checkbox relative to it -- correct at any DPI/size.
  ; The label and the checkbox are both SF_*_H tall, so the label height is
  ; reused as the checkbox height. Registers: $4 content height (preserved),
  ; $8 text x, then $2 struct / $5-$7 / $9 scratch.
  IntOp $9 $8 + 0                ; keep text x for SetWindowPos below
  System::Call '*(i, i, i, i) i .r2'
  System::Call 'user32::GetWindowRect(p $SnapmakerFinTextCtl, p r2)'
  System::Call '*$2(i .r5, i .r6, i, i .r7)'   ; label left/top/bottom, screen px
  System::Free $2
  IntOp $8 $7 - $6               ; label height (px) = SF_TEXT_H in u
  System::Call '*(i, i) i .r2'
  System::Call '*$2(i r5, i r6)'
  System::Call 'user32::ScreenToClient(p $HWNDPARENT, p r2)'
  System::Call '*$2(i .r5, i .r6)'              ; label top-left in wizard-client px
  System::Free $2
  IntOp $5 $6 + $8               ; label bottom, wizard-client px
  IntOp $6 $4 - $5               ; free band height (px)
  IntOp $6 $6 - $8               ; minus checkbox height
  IntOp $6 $6 * ${SF_CHECK_BAND_NUM}
  IntOp $6 $6 / ${SF_CHECK_BAND_DEN}
  IntOp $6 $5 + $6               ; final checkbox y (px)
  System::Call 'user32::SetWindowPos(p $SnapmakerFinCheckbox, p 0, i r9, i r6, i 0, i 0, i 0x15)'
  nsDialogs::Show
  ; Clear the bitmap from the control to get back the loaded handle, then free it.
  SendMessage $SnapmakerFinBitmapCtl ${STM_SETIMAGE} 0 0
  Pop $SnapmakerFinBitmap
  ${NSD_FreeImage} $SnapmakerFinBitmap
  ; Free the font created by CreateFont (title + text no longer use it).
  System::Call 'gdi32::DeleteObject(i $SnapmakerFinFont)'
FunctionEnd

; Leave callback of the finish page (registered as the second parameter of
; "UninstPage custom ... ..."): runs after the user clicks Finish, before the
; wizard exits. Silent /S runs show no pages, so this NEVER fires during the
; installer's own upgrade uninstall -- user data survives upgrades by design.
Function un.SnapmakerUninstallFinishLeave
  ${NSD_GetState} $SnapmakerFinCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    Call un.SnapmakerDeleteUserData
  ${Else}
    Call un.SnapmakerDeletePartUserData
  ${EndIf}
FunctionEnd

; Called when "Delete personal data" is checked: remove the whole user data,
; both Local and Roaming. SetShellVarContext current is required because the
; uninstall section leaves the all-users context active (admin installs).
; Called ONLY from the finish page leave callback -- never in silent (/S) runs,
; so upgrades never touch user data.
Function un.SnapmakerDeleteUserData
  SetShellVarContext current
  RMDir /r "$LOCALAPPDATA\Snapmaker_Orca"
  RMDir /r "$APPDATA\Snapmaker_Orca"
FunctionEnd

; Called when "Delete personal data" is NOT checked: partial cleanup -- delete
; everything under Local except the EBWebView folder (WebView2 cache), keeping
; EBWebView and the whole Roaming config directory. Called ONLY from the finish
; page leave callback -- never in silent (/S) runs, so upgrades never touch user data.
Function un.SnapmakerDeletePartUserData
  SetShellVarContext current
  StrCpy $R0 "$LOCALAPPDATA\Snapmaker_Orca"
  IfFileExists "$R0\*.*" 0 SnapmakerDelPart_done
  FindFirst $R1 $R2 "$R0\*.*"
  StrCmp $R2 "" SnapmakerDelPart_close
SnapmakerDelPart_loop:
  StrCmp $R2 "." SnapmakerDelPart_next
  StrCmp $R2 ".." SnapmakerDelPart_next
  StrCmp $R2 "EBWebView" SnapmakerDelPart_next
  Delete "$R0\$R2"
  RMDir /r "$R0\$R2"
SnapmakerDelPart_next:
  FindNext $R1 $R2
  StrCmp $R2 "" SnapmakerDelPart_close
  Goto SnapmakerDelPart_loop
SnapmakerDelPart_close:
  FindClose $R1
SnapmakerDelPart_done:
FunctionEnd
