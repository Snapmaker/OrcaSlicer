; Remove URL protocol keys installed by SnapmakerURLProtocols_install.nsh
SetRegView 64
DeleteRegKey HKLM "Software\Classes\snapmaker-orca"
DeleteRegKey HKLM "Software\Classes\Snapmaker_Orca"
DeleteRegKey HKCU "Software\Classes\snapmaker-orca"
DeleteRegKey HKCU "Software\Classes\Snapmaker_Orca"
SetRegView 32
