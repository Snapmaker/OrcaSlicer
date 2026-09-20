; Wait for the shared connect CLI's built-in no-client self-exit before CPack
; removes generated files. Only exact installed paths are checked; no process
; is enumerated or terminated by name.
Push $R0
StrCpy $R0 40

SnapmakerConnectUninstall_wait:
  Delete "$INSTDIR\resources\snapmaker_connection_windows_x64.exe"
  Delete "$INSTDIR\resources\snapmaker_connection_windows_arm64.exe"
  IfFileExists "$INSTDIR\resources\snapmaker_connection_windows_x64.exe" SnapmakerConnectUninstall_retry
  IfFileExists "$INSTDIR\resources\snapmaker_connection_windows_arm64.exe" SnapmakerConnectUninstall_retry
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
  MessageBox MB_OK|MB_ICONSTOP "Snapmaker Connect is still shutting down.$\nClose Snapmaker Orca, wait up to 40 seconds, then run the uninstaller again."
  Abort

SnapmakerConnectUninstall_done:
ClearErrors
Pop $R0
