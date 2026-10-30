; Pre-fill the installer directory page with the existing install path so an
; overwrite/upgrade reinstalls into the same folder.
; Hooked through MUI_PAGE_CUSTOMFUNCTION_PRE: the first MUI page macro (Welcome)
; consumes and !undef's the hook, so this runs exactly once, before the directory
; page. $IS_DEFAULT_INSTALLDIR is set by .onInit (1 = the user did NOT pass /D=),
; so a /D= override is preserved. Fresh installs read no UninstallString and are
; left untouched.
!define MUI_PAGE_CUSTOMFUNCTION_PRE SnapmakerDirPre

Function SnapmakerDirPre
  StrCmp "$IS_DEFAULT_INSTALLDIR" "1" 0 SnapmakerDirPre_done
    ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker_Orca" "UninstallString"
    StrCmp $0 "" SnapmakerDirPre_hkcu SnapmakerDirPre_have
  SnapmakerDirPre_hkcu:
    ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker_Orca" "UninstallString"
    StrCmp $0 "" SnapmakerDirPre_hklm_space SnapmakerDirPre_have
  SnapmakerDirPre_hklm_space:
    ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker Orca" "UninstallString"
    StrCmp $0 "" SnapmakerDirPre_hkcu_space SnapmakerDirPre_have
  SnapmakerDirPre_hkcu_space:
    ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker Orca" "UninstallString"
    StrCmp $0 "" SnapmakerDirPre_done SnapmakerDirPre_have
  SnapmakerDirPre_have:
    ; UninstallString is quoted; strip the surrounding quotes.
    StrCpy $1 $0 1 0
    StrCmp $1 "$\"" 0 SnapmakerDirPre_check_last
    StrCpy $0 $0 "" 1
  SnapmakerDirPre_check_last:
    StrCpy $1 $0 1 -1
    StrCmp $1 "$\"" 0 SnapmakerDirPre_strip
    StrCpy $0 $0 -1
  SnapmakerDirPre_strip:
    StrLen $1 "\Uninstall.exe"
    StrCpy $0 $0 -$1
    StrCmp $0 "" SnapmakerDirPre_done
    StrCpy $INSTDIR $0
  SnapmakerDirPre_done:
FunctionEnd
