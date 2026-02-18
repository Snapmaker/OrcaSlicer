## v0.9 Pre-release

EXPERIMENTAL BUILD - LIMITED TESTING
Based on Snapmaker Orca v2.2.4

v0.9 full spectrum pre-release focuses on mixed-filament workflow UX and stability.

### What's New in v0.9

#### Mixed Filaments UI/UX Overhaul (v0.9)
- Replaced the blocking mixed-filament popup with an inline expand/retract editor.
- Added row hover feedback and click-anywhere row activation for custom mixed filaments.
- Added dynamic mixed-filament panel resizing with an expansion cap sized for up to two expanded rows.
- Simplified editor flow to `Simple` mode only (mode selector removed from the inline editor).
- Moved gradient selector inline with filament selectors for faster editing.
- Reworked filament selectors into compact color swatch pickers with `F1/F2/...` labels.
- Simplified pattern editing:
  - removed insert dropdown/button flow,
  - kept direct filament buttons for appending pattern tokens.
- Moved preview labels into the preview bar itself (`Preview` + ratio overlay), with outlined text for readability.

#### Mixed Filaments Behavior and Reliability (v0.9)
- Automatic mixed filaments are now read-only in the inline settings editor.
- Automatic mixed filaments can now be deleted from the UI.
- Added persistent deleted-state storage for mixed filament rows so deleted auto rows stay deleted after refresh/restart.
- Excluded deleted rows from enabled/displayed virtual filament mapping.
- Improved change propagation so mixed-filament edits correctly mark config/project dirty states.
- Addressed mixed-filament editor collapse/refresh stability issues.

#### Local Z Dithering and Prime Tower Handling (v0.9)
- Enhanced Local-Z phase-b tool change handling when wipe/prime tower is enabled.
- Added an unplanned Local-Z tool-change path in wipe tower integration to emit proper tool-change/wipe G-code outside the preplanned per-layer sequence.
- Enabled Local-Z phase-b execution with wipe tower active (still blocked when wiping overrides are active).
- Restored pre-pass extruder state after Local-Z phase-b so subsequent wipe/prime tower planning remains synchronized.
- Improved Local-Z + wipe tower diagnostics/fallback logging for troubleshooting.

#### Dark Mode and Visual Consistency (v0.9)
- Added dark-mode-aware styling to mixed-filament rows and inline editor controls.
- Fixed dark-mode text contrast for mixed-filament row labels and controls.
- Updated gradient/preview border handling for better dark-mode contrast.

### Installation

#### Windows
1. Download `Snapmaker_Orca.zip`.
2. Extract to a folder.
3. Run the executable.

#### macOS
1. Download the macOS build (`arm64` for Apple Silicon or `x86_64` for Intel).
2. If the release asset is a `.zip`, unzip it first.
3. Open the `.dmg`.
4. Drag `Snapmaker_Orca.app` into `Applications`.
5. Launch the app from `Applications`.

#### Linux (AppImage)
1. Download `Snapmaker_Orca_Linux_V2.2.4.AppImage`.
2. Run `chmod +x Snapmaker_Orca_Linux_V2.2.4.AppImage`.
3. Run `./Snapmaker_Orca_Linux_V2.2.4.AppImage`.

### Warning
- Use at your own risk.
- May produce incorrect G-code in edge cases.
- Mixed-filament behavior is still experimental in some scenarios.
- This release has had limited real-printer validation.

### Features Not Yet Fully Tested
1. Mixed-filament editor behavior on all platform/theme combinations (Windows/macOS/Linux).
2. Dark-mode visual consistency across all desktop environments.
3. Mixed-filament preview/readability with all localization strings and scaling factors.

### Known Issues
- On-screen color blend preview may not exactly match physical print results.
- Some UI spacing/alignment may vary by OS and system font rendering.
- Older Linux distributions may fail to run this AppImage due to glibc mismatch.

### Credits
- FilamentMixer color blending integration is powered by the FilamentMixer library by [justinh-rahb](https://github.com/justinh-rahb).
- Library repository: [https://github.com/justinh-rahb/filament-mixer](https://github.com/justinh-rahb/filament-mixer).
