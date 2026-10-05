; Remove URL protocol keys installed by SnapmakerURLProtocols_install.nsh
SetRegView 64
; Remove only our own schemes -- never the official app's snapmaker-orca:// /
; Snapmaker_Orca:// keys. ultraone and snapmaker-ultra were ours under the older
; names, so they go too.
DeleteRegKey HKLM "Software\Classes\edgeslicer"
DeleteRegKey HKLM "Software\Classes\ultraone"
DeleteRegKey HKLM "Software\Classes\snapmaker-ultra"
SetRegView 32

; Drop the inbound firewall rule the installer pre-created for us.
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer"'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer LAN discovery"'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer FlashForge discovery"'
Pop $0
; Both the UDP and the TCP WebRTC rules share one name, so one delete removes them.
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer WebRTC video"'
Pop $0
