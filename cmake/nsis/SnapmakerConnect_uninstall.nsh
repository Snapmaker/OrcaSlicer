; Wait for the connect CLI to release its executable after the installer has
; force-terminated it. Exact installed paths are checked for compatibility.
Push $R0
StrCpy $R0 40

SnapmakerConnectUninstall_wait:
  Delete "$INSTDIR\resources\snapmaker_connection_windows_x64.exe"
  Delete "$INSTDIR\resources\snapmaker_connection_windows_arm64.exe"
  Delete "$INSTDIR\resources\snapmaker_control_windows_x64.exe"
  Delete "$INSTDIR\resources\snapmaker_control_windows_arm64.exe"
  IfFileExists "$INSTDIR\resources\snapmaker_connection_windows_x64.exe" SnapmakerConnectUninstall_retry
  IfFileExists "$INSTDIR\resources\snapmaker_connection_windows_arm64.exe" SnapmakerConnectUninstall_retry
  IfFileExists "$INSTDIR\resources\snapmaker_control_windows_x64.exe" SnapmakerConnectUninstall_retry
  IfFileExists "$INSTDIR\resources\snapmaker_control_windows_arm64.exe" SnapmakerConnectUninstall_retry
  Goto SnapmakerConnectUninstall_done

SnapmakerConnectUninstall_retry:
  Sleep 1000
  IntOp $R0 $R0 - 1
  IntCmp $R0 0 SnapmakerConnectUninstall_timeout SnapmakerConnectUninstall_timeout SnapmakerConnectUninstall_wait

SnapmakerConnectUninstall_timeout:
  IfSilent SnapmakerConnectUninstall_silent SnapmakerConnectUninstall_failed
SnapmakerConnectUninstall_silent:
  SetErrorLevel 8
  Abort
SnapmakerConnectUninstall_failed:
  MessageBox MB_OK|MB_ICONSTOP "The Snapmaker Connect executable is still locked.$\nMake sure Snapmaker Orca is closed, check antivirus or indexing software, then run the uninstaller again."
  Abort

SnapmakerConnectUninstall_done:
ClearErrors
Pop $R0
