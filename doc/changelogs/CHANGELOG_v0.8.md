## v0.8 Pre-release

EXPERIMENTAL BUILD - LIMITED TESTING
Based on Snapmaker Orca v2.2.4

v0.8 full spectrum pre-release focuses on filament mixer color accuracy and Linux AppImage packaging.

This has had limited testing on the Snapmaker U1 via the community.

### What's New in v0.8

#### Filament Mixer Color Blending (v0.8)
- Replaced the legacy RYB blend path with FilamentMixer.
- Updated mixed-filament preview/display color computation to use FilamentMixer interpolation.
- Improved mixed-filament color consistency for generated combinations.
- Retained legacy RYB conversion helpers in code as reference.

#### Linux AppImage Packaging (new in v0.8)
- Added Linux release artifact: `Snapmaker_Orca_Linux_V2.2.4.AppImage`.
- AppImage is now the primary Linux release artifact for end users.
- Current Linux build target is `x86_64`.
- Current runtime baseline is glibc `2.38+`.
- `Snapmaker_Orca.tar` remains available as an advanced/manual fallback.

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
1. Linux AppImage behavior across all desktop environments/distributions.
2. Mixed-filament visual color matching across different filament brands/materials.

### Known Issues
- Older Linux distributions may fail to run this AppImage due to glibc mismatch.
- Some systems may require `libfuse2` for AppImage execution.
- On-screen color blend preview may not exactly match physical print results.

### Credits
- FilamentMixer color blending integration is powered by the FilamentMixer library by [justinh-rahb](https://github.com/justinh-rahb).
- Library repository: [https://github.com/justinh-rahb/filament-mixer](https://github.com/justinh-rahb/filament-mixer).
