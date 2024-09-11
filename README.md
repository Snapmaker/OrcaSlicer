[![Build all](https://github.com/SoftFever/Snapmaker_Orca/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/SoftFever/Snapmaker_Orca/actions/workflows/build_all.yml)
# Snapmaker Orca     
Snapmaker Orca is an open source slicer for FDM printers.   

![discord-mark-blue](https://github.com/SoftFever/Snapmaker_Orca/assets/103989404/b97d5ffc-072d-4d0a-bbda-e67ef373876f) Join community: [Snapmaker_Orca Official Discord Server](https://discord.gg/P4VE9UY9gJ)   

# Main features
- Auto calibrations for all printers
- Sandwich(inner-outer-inner) mode - an improved version of the `External perimeters first` mode
- [Precise wall](https://github.com/SoftFever/Snapmaker_Orca/wiki/Precise-wall)
- Polyholes conversion support [SuperSlicer Wiki: Polyholes](https://github.com/supermerill/SuperSlicer/wiki/Polyholes)
- Klipper support
- More granular controls
- More features can be found in [change notes](https://github.com/SoftFever/Snapmaker_Orca/releases/)  


# Download

### Stable Release
📥 **[Download the Latest Stable Release](https://github.com/SoftFever/Snapmaker_Orca/releases/latest)**  
Visit our GitHub Releases page for the latest stable version of Snapmaker Orca, recommended for most users.

### Nightly Builds
🌙 **[Download the Latest Nightly Build](https://github.com/SoftFever/Snapmaker_Orca/releases/tag/nightly-builds)**  
Explore the latest developments in Snapmaker Orca with our nightly builds. Feedback on these versions is highly appreciated.


# How to install
**Windows**: 
1.  Download the installer for your preferred version from the [releases page](https://github.com/SoftFever/Snapmaker_Orca/releases).
    - *For convenience there is also a portable build available.*
    - *If you have troubles to run the build, you might need to install following runtimes:*
      - [MicrosoftEdgeWebView2RuntimeInstallerX64](https://github.com/SoftFever/Snapmaker_Orca/releases/download/v1.0.10-sf2/MicrosoftEdgeWebView2RuntimeInstallerX64.exe)
          - [Details of this runtime](https://aka.ms/webview2)
          - [Alternative Download Link Hosted by Microsoft](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
      - [vcredist2019_x64](https://github.com/SoftFever/Snapmaker_Orca/releases/download/v1.0.10-sf2/vcredist2019_x64.exe)
          -  [Alternative Download Link Hosted by Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe)
          -  This file may already be available on your computer if you've installed visual studio.  Check the following location: `%VCINSTALLDIR%Redist\MSVC\v142`

**Mac**:
1. Download the DMG for your computer: `arm64` version for Apple Silicon and `x86_64` for Intel CPU.  
2. Drag Snapmaker_Orca.app to Application folder. 
3. *If you want to run a build from a PR, you also need following instructions below*  
    <details quarantine>
    - Option 1 (You only need to do this once. After that the app can be opened normally.):
      - Step 1: Hold _cmd_ and right click the app, from the context menu choose **Open**.
      - Step 2: A warning window will pop up, click _Open_  
      
    - Option 2:  
      Execute this command in terminal: `xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app`
      ```console
          softfever@mac:~$ xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app
      ```
    - Option 3:  
        - Step 1: open the app, a warning window will pop up  
            ![image](./SoftFever_doc/mac_cant_open.png)  
        - Step 2: in `System Settings` -> `Privacy & Security`, click `Open Anyway`:  
            ![image](./SoftFever_doc/mac_security_setting.png)  
    </details>
    
**Linux(Ubuntu)**:
 1. If you run into trouble to execute it, try this command in terminal:  
    `chmod +x /path_to_appimage/Snapmaker_Orca_Linux.AppImage`
    
# How to compile
- Windows 64-bit  
  - Tools needed: Visual Studio 2019, Cmake, git, git-lfs, Strawberry Perl.
      - You will require cmake version 3.14 or later, which is available [on their website](https://cmake.org/download/).
      - Strawberry Perl is [available on their github repository](https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/).
  - Run `build_release.bat` in `x64 Native Tools Command Prompt for VS 2019`
  - Note: Don't forget to run `git lfs pull` after cloning the repository to download tools on Windows

- Mac 64-bit  
  - Tools needed: Xcode, Cmake, git, gettext, libtool, automake, autoconf, texinfo
      - You can install most of them by running `brew install cmake gettext libtool automake autoconf texinfo`
  - run `build_release_macos.sh`
  - To build and debug in XCode:
      - run `XCode.app`
      - open ``build_`arch`/Snapmaker_Orca.xcodeproj``
      - menu bar: Product => Scheme => Snapmaker_Orca
      - menu bar: Product => Scheme => Edit Scheme...
          - Run => Info tab => Build Configuration: `RelWithDebInfo`
          - Run => Options tab => Document Versions: uncheck `Allow debugging when browsing versions`
      - menu bar: Product => Run

- Ubuntu 
  - Dependencies **Will be auto installed with the shell script**: `libmspack-dev libgstreamerd-3-dev libsecret-1-dev libwebkit2gtk-4.0-dev libosmesa6-dev libssl-dev libcurl4-openssl-dev eglexternalplatform-dev libudev-dev libdbus-1-dev extra-cmake-modules libgtk2.0-dev libglew-dev libudev-dev libdbus-1-dev cmake git texinfo`
  - run 'sudo ./BuildLinux.sh -u'
  - run './BuildLinux.sh -dsir'


# Note: 
If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.
```
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Supports
**Snapmaker Orca** is an open-source project, and I'm deeply grateful to all my sponsors and backers.   
Their generous support enables me to purchase filaments and other essential 3D printing materials for the project.   
Thank you! :)

### Sponsors:  
<table>
<tr>
<td>
<a href="https://peopoly.net/">
    <img src="SoftFever_doc\sponsor_logos\peopoly-standard-logo.png" alt="Peopoly" width="96" height="">
</a>
</td> 
<td>
<a href="https://qidi3d.com/">
    <img src="SoftFever_doc\sponsor_logos\QIDI.png" alt="QIDI" width="96" height="">
</a>
</td>
<td>
<a href="https://phrozen3d.com/">
    <img src="SoftFever_doc\sponsor_logos\Phrozen_Logo圓_.png" alt="Phrozen Technology" width="96" height="">
</a>
</td>
<td>
<a href="https://bigtree-tech.com/">
    <img src="SoftFever_doc\sponsor_logos\BigTreeTech.png" alt="BIGTREE TECH" width="96" height="">
</a>
</td>
</tr>
</table>

### Backers:  
**Ko-fi supporters**: [Backers list](https://github.com/user-attachments/files/16147016/Supporters_638561417699952499.csv)

## Support me  
<a href="https://github.com/sponsors/SoftFever"><img src="https://img.shields.io/static/v1?label=Sponsor&message=%E2%9D%A4&logo=GitHub&color=%23fe8e86" width="130"></a>

<a href="https://ko-fi.com/G2G5IP3CP"><img src="https://ko-fi.com/img/githubbutton_sm.svg" width="200"></a>

[![PayPal](https://img.shields.io/badge/PayPal-00457C?style=for-the-badge&logo=paypal&logoColor=white)](https://paypal.me/softfever3d)


## Some background
Snapmaker_Orca is originally forked from Bambu Studio, it was previously known as BambuStudio-SoftFever.

Bambu Studio is forked from [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. 
Snapmaker Orca incorporates a lot of features from SuperSlicer by @supermerill
Snapmaker Orca's logo is designed by community member Justin Levine(@freejstnalxndr)  


# License
Snapmaker Orca is licensed under the GNU Affero General Public License, version 3. Snapmaker Orca is based on Bambu Studio by BambuLab.

Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

Snapmaker Orca includes a pressure advance calibration pattern test adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.

The bambu networking plugin is based on non-free libraries from Bambulab. It is optional to the Snapmaker Orca and provides extended functionalities for Bambulab printer users.

