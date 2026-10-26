; Remove both managed rules and legacy user-created Block rules scoped to the
; installed CLI paths. Filtering by program avoids removing unrelated rules.
nsExec::ExecToLog 'netsh advfirewall firewall delete rule name=all program="$INSTDIR\resources\snapmaker_control_windows_x64.exe"'
Pop $0
nsExec::ExecToLog 'netsh advfirewall firewall delete rule name=all program="$INSTDIR\resources\snapmaker_control_windows_arm64.exe"'
Pop $0
