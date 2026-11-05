; The connect CLI uses mDNS on UDP 5353 and transient UDP ports for LAN device
; discovery. Remove any prior user-created Block rule for this program path,
; then add a deterministic allow rule after the executable has been installed.
IfFileExists "$INSTDIR\resources\snapmaker_control_windows_x64.exe" 0 SnapmakerConnectFirewall_arm64
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name=all program="$INSTDIR\resources\snapmaker_control_windows_x64.exe"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Snapmaker Control (x64)" dir=in action=allow program="$INSTDIR\resources\snapmaker_control_windows_x64.exe" enable=yes protocol=udp profile=any'
  Pop $0

SnapmakerConnectFirewall_arm64:
IfFileExists "$INSTDIR\resources\snapmaker_control_windows_arm64.exe" 0 SnapmakerConnectFirewall_done
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name=all program="$INSTDIR\resources\snapmaker_control_windows_arm64.exe"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Snapmaker Control (ARM64)" dir=in action=allow program="$INSTDIR\resources\snapmaker_control_windows_arm64.exe" enable=yes protocol=udp profile=any'
  Pop $0

SnapmakerConnectFirewall_done:
