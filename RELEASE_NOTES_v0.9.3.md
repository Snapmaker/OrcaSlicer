# Snapmaker Orca Full Spectrum v0.9.3 (Pre-release)

Mixed-filament reliability, Local-Z dithering fixes, and fork-safe CI/CD.
Based on Snapmaker Orca v2.2.4.

## Highlights

### Local-Z Dithering Fixes
- Fixed prime tower degradation during Local-Z dithering by directly switching extruders instead of replaying full tool change templates.
- Improved starting component logic for alternating layers so height adjustments are computed from the correct pass sequence.
- Updated tooltip to clearly explain variable layer height blending with concrete examples (e.g. a 66/33 blend at 0.12 mm prints as 0.08 mm + 0.04 mm layers).

### Stable Mixed Filament Identity
- Introduced `stable_id` for mixed filament entries, giving each entry a persistent identity that survives additions, deletions, and reordering.
- Custom GCode tool changes now reference filament IDs instead of extruder IDs, preventing misrouted tool changes when mixed filaments are modified.
- Stable ID allocation and normalization ensures consistent behavior across project save/load cycles.

### Mixed Filament 3MF and GUI Improvements
- Added functions to compute physical filament count and maximum supported filament ID from project configuration.
- GUI now displays total filament counts including mixed filaments in object list and object table.
- Streamlined color assignment and naming for both physical and mixed filaments during initialization.
- Canvas colors now refresh immediately when mixed filaments are modified.

### ObjColorPanel Filament Mapping
- Fixed filament ID assignment during color updates by introducing a mapping mechanism for appended filament IDs.
- Improved filament ID resolution in approximate-match and default-strategy code paths.

### Network Discovery Stability
- Hardened UdpSocket (mDNS/Bonjour) with a socket quarantine mechanism that centralizes error handling for setup, send, and receive operations.
- Added a `cancel` method and `m_socket_usable` flag to prevent operations on broken sockets, fixing crashes on systems where mDNS discovery fails.

## Bug Fixes
- Fixed `MixedFilamentConfigPanel` summary handling to avoid empty-string edge cases in preview data.
- Fixed `instance_shift` type to `Vec3d` for correct object pasting behavior in the Selection class.
- Replaced `wxEmptyString` with `wxString()` across Plater to prevent subtle initialization issues.

## Tests
- Added unit tests for custom GCode tool change handling with mixed filament configurations.
- Added unit tests for stable ID remapping in mixed filament scenarios.

## Important Notes
- Experimental build with limited testing.
- macOS builds from this fork are unsigned and not notarized. macOS Gatekeeper may warn or block the app; use right-click > Open or allow it in System Settings > Privacy & Security.
- Use at your own risk.

## Credits
- FilamentMixer color blending integration is powered by FilamentMixer by [justinh-rahb](https://github.com/justinh-rahb).
- Library: [https://github.com/justinh-rahb/filament-mixer](https://github.com/justinh-rahb/filament-mixer)
