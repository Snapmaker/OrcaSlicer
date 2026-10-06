; Machine-wide URL protocol registration (mirrors macOS CFBundleURLSchemes).
; Included by CPack-generated NSIS installer via CPACK_NSIS_INSTALL_SCRIPT.
; On 64-bit Windows, 32-bit NSIS must use the 64-bit registry view so the 64-bit app sees these keys.
SetRegView 64

; Register ONLY our own scheme, so a side-by-side official Snapmaker Orca keeps its
; snapmaker-orca:// / Snapmaker_Orca:// handlers and our uninstall never removes them.
; This is the same scheme the running app registers in HKCU (GUI_App::associate_url);
; the two used to disagree, which was the collision this comment claimed to avoid.
WriteRegStr HKLM "Software\Classes\edgeslicer" "" "URL:EdgeSlicer"
WriteRegStr HKLM "Software\Classes\edgeslicer" "URL Protocol" ""
WriteRegStr HKLM "Software\Classes\edgeslicer\shell\open\command" "" '"$INSTDIR\EdgeSlicer.exe" "%1"'

SetRegView 32

; Windows Firewall: the inbound rule keys on the image path, so the rename would
; otherwise leave the old rule dead and pop a consent dialog on the first LAN listen.
; Pre-create it (idempotent: delete any rule of the same name first) so the phone/LAN
; service on TCP 13640 answers straight away. nsExec so nothing flashes a console.
;
; The range, not just 13640: RemoteHub.cpp's bind() steps to 13641, 13642, ... when something
; else on the PC already holds 13640 (HUB_PORT..HUB_PORT+19, see the loop there), and a rule
; covering only 13640 left the phone with no answer at all once the hub landed on a later port
; ("No hub answered on that link" - a real tester hit this, 2026-09-17). profile=private,domain
; only, same as before: a Public network is deliberately left out (that is the network class
; Windows assigns to a coffee-shop Wi-Fi, an untrusted LAN, etc.), so this rule never opens the
; port to a network the user has not vouched for. The hub page now tells the user how to allow it
; there themselves (RemoteHub.cpp's firewall_query/port_note) if their own Wi-Fi happens to be
; classed Public.
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer"'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="EdgeSlicer" dir=in action=allow program="$INSTDIR\EdgeSlicer.exe" protocol=TCP localport=13640-13659 profile=private,domain enable=yes'
Pop $0
; Bambu LAN discovery: the network plug-in (loaded inside EdgeSlicer.exe) listens for the printers'
; SSDP announcements on UDP 2021 and for M-SEARCH replies on UDP 1990. Without this rule a fresh
; install only discovers printers if the user accepted Windows' first-run prompt for the right
; network profile - the usual reason a P1S/X1/A1 never shows up in the printer list.
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer LAN discovery"'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="EdgeSlicer LAN discovery" dir=in action=allow program="$INSTDIR\EdgeSlicer.exe" protocol=UDP localport=2021,1990 profile=private,domain enable=yes'
Pop $0
; FlashForge LAN search (Flashforge::discover_printers, the printer dialog's Browse and the Device
; tab's Add printer > Search): EdgeSlicer broadcasts to UDP 48899 and the printers answer to UDP
; 18007, the port it listens on. The broadcast goes out freely; the answers are what Windows
; Firewall drops without this rule. Private and Domain only, like the rules above.
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer FlashForge discovery"'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="EdgeSlicer FlashForge discovery" dir=in action=allow program="$INSTDIR\EdgeSlicer.exe" protocol=UDP localport=18007 profile=private,domain enable=yes'
Pop $0
; WebRTC camera video: go2rtc (bundled, $INSTDIR\resources\tools\go2rtc\go2rtc.exe) carries the
; media straight from this PC to the phone rather than through the hub, so unlike everything else
; the hub runs it needs an inbound port. It takes the first free one in 8555-8574 (RemoteHub.cpp,
; free_webrtc_port), hence the range rather than a single port, and both protocols: UDP is the
; media, TCP is go2rtc's ICE-TCP fallback for networks that drop UDP. Program-bound like the rules
; above, so nothing else on the PC gains a port. Without this rule Windows prompts on go2rtc's
; first bind and writes a *block* rule if the prompt is dismissed - which the hub then has to
; detect and explain (RemoteHub.cpp, firewall_query). Pre-creating it is what stops that.
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer WebRTC video"'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="EdgeSlicer WebRTC video" dir=in action=allow program="$INSTDIR\resources\tools\go2rtc\go2rtc.exe" protocol=UDP localport=8555-8574 profile=private,domain enable=yes'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="EdgeSlicer WebRTC video" dir=in action=allow program="$INSTDIR\resources\tools\go2rtc\go2rtc.exe" protocol=TCP localport=8555-8574 profile=private,domain enable=yes'
Pop $0
