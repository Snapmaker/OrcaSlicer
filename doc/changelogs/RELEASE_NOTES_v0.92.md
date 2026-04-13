# Snapmaker Orca Full Spectrum v0.92 (Pre-release)

Mixed-filament stability and workflow release.
Based on Snapmaker Orca v2.2.4.

## Highlights

### Gradient Behavior Is Now Gradual
- Removed the `Mixed filament layer cycle` setting.
- Reworked non-height-weighted gradient math to use gradual integer cadence by ratio (minority side anchors to 1 layer, majority side scales progressively).
- This prevents abrupt jumps in layer cadence and makes transitions more predictable.

### Mixed Filament Indexing And Mapping Fixes
- Fixed mixed-filament row labeling so IDs stay compact after deletions in the Mixed Filaments panel.
- Fixed regeneration order so auto-generated/precomputed mixes are appended at the end of existing mixed rows instead of being inserted at the front.
- Fixed paint/index remapping when adding physical or virtual filaments so existing painted mixed regions keep their intended virtual filament mapping.
- Fixed edge case where the first mixed filament could be temporarily replaced by a newly added physical filament.

### Automatic Mixed Filaments Are Editable
- Automatic mixed rows can now be expanded and edited through the gradient controls.
- Edited auto-origin rows are treated as user-managed entries.
- Deleting an edited auto-origin row now keeps that auto pair tombstoned (it will not be regenerated immediately and overwrite user intent).

### Gradient Preview Color Fidelity
- Updated gradient preview blending to use the FilamentMixer library path for color reproduction.
- Preview swatches now better match expected mixed output.

### Selective Expansion/Contraction Fix
- Updated selective mixed-zone contraction so positive values trim only the outside-facing (visible) boundary band.
- Internal/shared boundaries are no longer symmetrically contracted.
- Outward expansion behavior remains unchanged.

## Also Included (v0.91 Carry-Forward Fixes)
- Windows startup/log path handling for non-ASCII usernames (UTF-8-safe path resolution).
- Mixed-filament swatch desync/gray-regression fixes in preview rendering.

## Internal/Config Changes
- Removed `mixed_filament_cycle_layers` from config/UI wiring.
- Extended mixed filament serialized metadata with auto-origin tracking to preserve delete/regenerate intent across reloads.
- Added diagnostic logging around mixed-filament remap and selective-indentation application paths.

## Important Notes
- Experimental build with limited testing.
- Some projects may show different gradient cadence behavior due to the removal of explicit layer-cycle control.
- Use at your own risk.

## Credits
- FilamentMixer color blending integration is powered by FilamentMixer by [justinh-rahb](https://github.com/justinh-rahb).
- Library: [https://github.com/justinh-rahb/filament-mixer](https://github.com/justinh-rahb/filament-mixer)
