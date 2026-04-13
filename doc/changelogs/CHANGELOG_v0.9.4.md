# Snapmaker Orca Full Spectrum v0.9.4

Mixed-filament workflow expansion, Local-Z ordering fixes, and release hardening.
Based on Snapmaker Orca v2.2.4.

## Highlights

### Multi-Perimeter Mixed Filament Support
- Added support for multi-perimeter patterns so mixed-filament layouts can preserve more complex perimeter assignments.
- Extended the mixed-filament planning path to carry those patterns through slicing and G-code generation.

### Object Color Mapping And Filament Assignment
- Improved object-color matching with a more predictable `Keep color` path when new colors are appended.
- Simplified appended-color handling so imported object colors map to the intended filament entries more reliably.
- Increased the color picker capacity beyond 16 colors for larger multi-color projects.

### Mixed Filament Workflow Improvements
- Improved mixed-filament UI performance in the object-color workflow.
- Added mixed-filament reordering support so custom entries can be reorganized without recreating them.
- Updated project/config handling so reordered entries stay consistent across the slicing pipeline.

### Local-Z And Tool Ordering
- Added Local-Z wipe tower reserve planning and pass ordering updates to keep tool sequencing aligned with Local-Z execution.
- Introduced `LocalZOrderOptimizer` coverage and integration points to improve pass ordering determinism.

### Stability And Build Reliability
- Added GUI startup profiling hooks to help diagnose initialization cost.
- Hardened Bonjour socket cleanup by dropping unusable sockets more aggressively.
- Added `/FS` to MSVC parallel compile options to reduce Windows build contention.

## Tests

- Added `test_local_z_order_optimizer.cpp`.
- Expanded mixed-filament regression coverage in `test_mixed_filament.cpp`.
- Added triangle selector coverage in `test_triangle_selector.cpp`.

## Important Notes

- Experimental release with limited printer validation.
- macOS builds from this fork are unsigned and not notarized.
- Use at your own risk.
