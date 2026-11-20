# FilamentMixer endpoint correction

The green-to-white pink fringe came from the polynomial predictor, not from the gradient stop handles. At 95% white, the original predictor returned RGB (244, 233, 233). The corrected predictor returns (244, 255, 238), then approaches pure white smoothly.

## Investigation

The imported model fits each RGB output channel independently. Its endpoint branches return the exact input colors, but its polynomial is not constrained to approach those colors as the ratio approaches zero or one. Clamping channels to the display gamut does not fix that discontinuity.

An audit of 4,096 random RGB pairs (NumPy default_rng seed 17092026; first draw all A colors, then all B colors, integers 0 through 255) evaluated both polynomial endpoint limits before integer quantization, after gamut clipping. Of 8,192 endpoint approaches:

- 6,476 missed the corresponding input by more than two levels in at least one channel.
- 518 missed by more than ten levels.
- The median maximum-channel error was 3.75 levels; the largest was 44.39.

The largest error occurred approaching (253, 9, 26) from (6, 254, 209): the polynomial approached approximately (241, 53, 33) instead. This is a general endpoint defect, not a special case involving white.

Examples approaching white at 99% white:

| Other color | Original RGB | Corrected RGB |
| --- | --- | --- |
| Green (0, 255, 0) | (252, 228, 247) | (253, 255, 252) |
| Red (255, 0, 0) | (237, 255, 255) | (255, 251, 252) |
| Blue (0, 0, 255) | (255, 255, 243) | (250, 252, 255) |
| Black (0, 0, 0) | (229, 252, 253) | (252, 252, 253) |

## Correction and scope

For input colors A and B and raw polynomial P(t), apply:

```text
C(t) = P(t) - (1-t) * (P(0)-A) - t * (P(1)-B)
```

This subtracts a linear interpolation of the endpoint errors before gamut clipping. It guarantees the correct limits for every finite RGB pair while retaining the polynomial's nonlinear curvature. The implementation cancels the constant and linear terms algebraically and evaluates the 36 remaining features with mixing-ratio degree two or higher, instead of evaluating the full polynomial three times. Output channels are rounded to the nearest byte after clipping. Identical input colors return that same color at every ratio.

The correction is in the shared application wrapper, `src/libslic3r/filament_mixer.cpp`. It applies to FilamentMixer-backed gradient sliders, blended/Prepare previews, static mixtures, matching, recommendations, and the floating-point wrappers. The measured KM/K-S backend and imported polynomial coefficients are unchanged. Generic mixed-color predictions, and consequently recipes selected through matching, can change because they now use the corrected curve.

## Remaining limitations

Endpoint correction does not establish physical accuracy throughout a mixture. For example, a 50/50 black-white mix remains blue-tinted: RGB (120, 123, 155), compared with the original (100, 125, 160). The endpoint correction also leaves small order asymmetries in the fitted model; the random-pair audit found approximately one channel level of difference between corrected A/B at 25% and corrected B/A at 75% before clipping and rounding.

The imported model can also change identical colors in the interior of a blend. The explicit same-color guard fixes that exact case, but does not constitute a general correction for nearly identical colors.

Clamping every mixed RGB channel between its two endpoint values would suppress legitimate pigment behavior such as blue and yellow mixing to green. For a wider physical correction, the model needs additional constraints and validation against measured mixtures, or replacement. This patch fixes endpoint continuity and exact same-color identity; it does not claim to repair every interior hue error.

## Validation

- Native gradient widget and blended preview visually checked with the reported green-white-yellow recipe: no pink band at the white stop.
- Endpoint checks cover all 729 ordered pairs on a 27-color RGB grid, plus 4,096 deterministic random pairs, at exact endpoints and ratios 0.00001 / 0.99999. Every near-endpoint result rounds to its corresponding input color.
- Same-color identity checked at five interior ratios for 4,096 random colors.
- The green-white transition has no red/pink dominance and no channel jump exceeding four levels between adjacent one-percent samples.
- Blue-yellow still predicts green; it has not become ordinary RGB interpolation.
- The original model's documented golden example remains pinned separately, verifying that its coefficient table is unchanged.
- Windows application DLL, launcher, and tests build successfully. Relevant regressions: 226 cases, 224 passed and two existing expected failures; 112,983 assertions, including two expected failures.

The audit and unit checks establish software behavior, not printed color accuracy.
