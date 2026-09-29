#!/usr/bin/env bash
# spike/verify_chameleon.sh
#
# Chameleon Brim + Support Interface Auto-Match — repeatable regression check for:
#   1. Off-mode ("object" brim_filament_source) byte-identity vs the frozen
#      baseline_clean.gcode, AND determinism of two consecutive off-mode slices.
#   2. nearest_wall-mode sanity (exit 0, M620 count within the switch-count
#      guard bound, brim feature present) AND determinism of two consecutive
#      nearest_wall slices.
#   3. (Part 2) Support Interface Auto-Match on the tshape.stl fixture:
#      manual/off-mode support-region identity vs p2_baseline.gcode (tower off/on,
#      plus a v2.6 LEGACY-KEY check that the old "manual" string still resolves
#      to matching DISABLED), and (v2.4 spec A/D, retargeted for v2.6) a
#      LEGACY-KEY check: spike_p2_overrides.json still literally sets
#      support_interface_filament_source = "nearest_surface" - the enum key
#      v2.6 removed entirely (replacing it with the support_filament_matching
#      checkbox) - proving PrintConfigDef::handle_legacy's rename+remap branch
#      (PrintConfig.cpp, folds together the v2.4 "nearest_surface" alias and
#      the v2.6 enum-to-bool migration) migrates it to
#      support_filament_matching = "1" instead of silently substituting the
#      default (matching disabled). Retargeted from this section's pre-v2.4
#      "nearest_surface-mode sanity" checks - see the section's own comment
#      below for exactly what's asserted and why.
#   4. (Part 2, v2.2 Task 4, spec C8; retargeted for v2.6) a SECOND LEGACY-KEY
#      check: spike_p2_wall_overrides.json still literally sets
#      support_interface_filament_source = "nearest_wall" - also removed by
#      v2.6 - proving the same handle_legacy branch migrates THIS old value to
#      support_filament_matching = "1" too (both old non-manual values land on
#      matching ENABLED). Bounded M620 growth + feature-tag presence (tower
#      off/on) plus a determinism repeat round out the group, same sanity bar
#      as section 3's checks. Checks are prefixed "p2-wall-" to avoid
#      colliding with section 2's unrelated "nearestwall-*" (Part 1
#      brim_filament_source=nearest_wall) checks.
#
# Run from anywhere; the script cd's into its own directory (spike/) first so
# all the relative harness paths below resolve the same way regardless of the
# caller's cwd. Requires a built EdgeSlicer.exe (../build/src/Release/).
#
# Plain bash (Git Bash on Windows is fine) — uses process substitution, so
# `bash verify_chameleon.sh` / `./verify_chameleon.sh`, not `sh verify_chameleon.sh`.

set -u
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

# ---------------------------------------------------------------------------
# Harness fixtures (per spike/FINDINGS.md harness notes)
# ---------------------------------------------------------------------------
EXE="../build/src/Release/EdgeSlicer.exe"
DATADIR="C:/Dev/SnapmakerOrcaSupports/spike/datadir"
MACHINE_PROFILE="../resources/profiles/BBL/machine/Bambu Lab X1 Carbon 0.4 nozzle.json"
FIL_PLA="../resources/profiles/BBL/filament/Bambu PLA Basic @BBL X1C.json"
FIL_PETG="../resources/profiles/BBL/filament/Bambu PETG Basic @BBL X1C.json"
OBJ_OVERRIDES="spike_process_overrides.json"      # implicit brim_filament_source = object
CHA_OVERRIDES="spike_chameleon_overrides.json"    # brim_filament_source = nearest_wall

BASELINE="out/baseline_clean.gcode"
PLATE_OUT="out/plate_1.gcode"   # EdgeSlicer.exe's fixed multi-object-plate output name

OUT_OBJ_1="out/verify_object_1.gcode"
OUT_OBJ_2="out/verify_object_2.gcode"
OUT_CHA_1="out/verify_nearestwall_1.gcode"
OUT_CHA_2="out/verify_nearestwall_2.gcode"

M620_MIN=2
M620_MAX=10

# ---------------------------------------------------------------------------
# Support Interface Auto-Match (Part 2) fixtures
# ---------------------------------------------------------------------------
TSHAPE="tshape.stl"
# v2.6: this fixture's value is now DELIBERATELY the OLD key/value -
# support_interface_filament_source = "manual" - so it doubles as a THIRD
# legacy-key check (section 3a below): the old "manual" string must resolve to
# support_filament_matching = disabled, same as the option's own default.
P2_MANUAL_OVERRIDES="spike_support_overrides.json"
# v2.4 (spec A/D), retargeted v2.6: this fixture's value is DELIBERATELY left as the
# literal old key/string support_interface_filament_source = "nearest_surface" - both
# the enum value (v2.4) and the enum option itself (v2.6, replaced by the
# support_filament_matching checkbox) are gone - so it now serves as one of the two
# legacy-key check fixtures (section 3 below). Do NOT edit its content to the new
# key: the whole point of the section 3 checks is proving the OLD key/string still
# slices correctly via PrintConfigDef::handle_legacy's rename+remap branch.
P2_NEAREST_OVERRIDES="spike_p2_overrides.json"
# v2.2 Task 4 (spec C8), retargeted v2.6: support_interface_filament_source =
# nearest_wall - the OLD key/value, also gone as of v2.6. Same fixture/shape as
# P2_NEAREST_OVERRIDES, just the other old non-manual value spelled directly - the
# second of the two legacy-key check fixtures (section 4 below).
P2_WALL_OVERRIDES="spike_p2_wall_overrides.json"
P2_BASELINE="out/p2_baseline.gcode"                   # recorded pre-Task-4 (HEAD a157ac6cf8): manual mode, tower off, single PLA filament

OUT_P2_MAN_OFF="out/p2_manual_off.gcode"
OUT_P2_MAN_ON="out/p2_manual_on.gcode"
OUT_P2_LEGACY_OFF_1="out/p2_legacy_off_1.gcode"
OUT_P2_LEGACY_OFF_2="out/p2_legacy_off_2.gcode"
OUT_P2_WALL_OFF_1="out/p2_wall_off_1.gcode"
OUT_P2_WALL_OFF_2="out/p2_wall_off_2.gcode"
OUT_P2_WALL_ON="out/p2_wall_on.gcode"

# ---------------------------------------------------------------------------
# Preflight — fail fast with a clear message if the harness itself isn't set up,
# rather than limping through every check reporting confusing FAILs.
# ---------------------------------------------------------------------------
preflight_fail=0
for f in "$EXE" "$MACHINE_PROFILE" "$FIL_PLA" "$FIL_PETG" "$OBJ_OVERRIDES" "$CHA_OVERRIDES" "$BASELINE" cube30.stl cube30b.stl \
         "$TSHAPE" "$P2_MANUAL_OVERRIDES" "$P2_NEAREST_OVERRIDES" "$P2_WALL_OVERRIDES" "$P2_BASELINE"; do
    if [ ! -f "$f" ]; then
        echo "PREFLIGHT FAIL: missing required file: $f" >&2
        preflight_fail=1
    fi
done
if [ ! -d "$DATADIR" ]; then
    echo "PREFLIGHT FAIL: missing datadir: $DATADIR" >&2
    preflight_fail=1
fi
if [ "$preflight_fail" -ne 0 ]; then
    echo "Aborting: harness prerequisites not met (see above). Build EdgeSlicer.exe and/or check spike/ fixtures." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
RESULTS=()   # "name|PASS/FAIL|detail"
FAILS=0

record() {
    local name="$1" status="$2" detail="$3"
    RESULTS+=("${name}|${status}|${detail}")
    if [ "$status" != "PASS" ]; then
        FAILS=$((FAILS + 1))
    fi
}

# Strip lines/fields known (and verified, see Task 4 report) to legitimately
# differ run-to-run or mode-to-mode without indicating a real regression:
#   - "; generated by ..." header line (build string + wall-clock timestamp)
#   - lines starting "; at " (defensive; currently folded into the line above,
#     kept as its own filter in case a future build ever splits it out)
#   - "; brim_filament_source = ..." config-dump line (value differs
#     object vs nearest_wall by design, and is entirely absent from the
#     pre-Task-3 baseline_clean.gcode, which predates that config key)
#   - "; support_interface_filament_source = ..." AND "; support_filament_matching
#     = ..." config-dump lines (Task 2 added the former key after
#     baseline_clean.gcode was recorded in Part 1, so every current slice —
#     including plain object-mode ones with support untouched — dumps one extra
#     CONFIG_BLOCK line baseline_clean.gcode doesn't have; confirmed via Task
#     4's diff investigation to be the *only* diff for the pre-existing Part 1
#     object-mode checks, i.e. a stale-normalize gap from Task 2/3, not a Task
#     4 regression. v2.6 renamed the key to the latter when the enum became a
#     checkbox - but baseline_clean.gcode was re-recorded at some point under
#     the OLD name (it literally contains "; support_interface_filament_source
#     = manual", verified via direct diff), while every current build now
#     emits the NEW name - so BOTH patterns must stay stripped: dropping the
#     old-name line here would silently reintroduce the exact gap this filter
#     exists to close, just flipped to the opposite fixture.)
#   - the "id:<N> copy 0" suffix on "; printing object ..." / "; stop printing
#     object ..." comments — this per-object numeric id is run-to-run
#     nondeterministic (confirmed by back-to-back identical-command runs in
#     Task 4); everything else on those lines, including the stable
#     "unique label id: NN" companion comment, is left untouched.
#   - M73/M73.2 progress-percentage lines — reordered/shifted by
#     time-estimator noise independent of the actual toolpath (see below).
#   - the "; model printing time: ...; total estimated time: ..." and
#     "; estimated first layer printing time ..." header lines, and the
#     "; filament used [mm]"/"; filament used [cm3]" footer lines — these
#     are downstream fallout of time/flow estimation, not toolpath content.
#   - (Task 4, v2.1, item 1b) degenerate solid-infill micro-extrusion
#     blocks, via normalize_strip_slivers.awk. Root cause and rationale:
#     C:\Dev\SnapmakerOrcaSupports\solid_infill_nondeterminism.md — an
#     address-layout knife edge on a degenerate corner sliver (each variant
#     is itself byte-stable; this is NOT a race) occasionally leaves a
#     ~0.3mm zigzag (footprint ~0.09x0.09mm) present in one slice and
#     absent in another, with fallout in the surrounding travel/retract
#     E-values and M73/filament-used lines (handled above). The
#     group_fills area filter (efb7902553) now removes most of these at
#     the source, but this strip is defense-in-depth per the doc's own
#     "Practical consequences" recommendation: strip any extrusion block
#     (consecutive extruding G1s between travel/retract moves) whose XY
#     bbox is < 0.2mm in both dimensions — real geometry is always far
#     larger than that, so this cannot mask an actual regression. MUST run
#     after the M73 strip above: M73 lines are frequently interspersed
#     inside genuine, large extrusions and would otherwise fragment one
#     real block into a spuriously short tail segment.
normalize() {
    sed -e '/^; generated by/d' \
        -e '/^; at /d' \
        -e '/^; brim_filament_source/d' \
        -e '/^; support_interface_filament_source/d' \
        -e '/^; support_filament_matching/d' \
        -e '/^M73/d' \
        -e '/^; model printing time/d' \
        -e '/^; estimated first layer printing time/d' \
        -e '/^; filament used/d' \
        -e 's/id:[0-9]\+ copy 0/id:X copy 0/' \
        "$1" \
    | awk -f normalize_strip_slivers.awk
}

# byte_identical <fileA> <fileB> -> 0 if identical after normalize, 1 otherwise
byte_identical() {
    diff -q <(normalize "$1") <(normalize "$2") > /dev/null 2>&1
}

# run_slice <overrides_json> <dest_gcode> <log_file>
# Slices the two-cube fixture with the given process-overrides JSON, moving
# the CLI's fixed plate_1.gcode output to a stable per-run name so the next
# run doesn't clobber it. Returns the exe's exit code (or 1 if it exited 0
# but produced no plate_1.gcode).
run_slice() {
    local overrides="$1" dest="$2" logfile="$3"
    rm -f "$PLATE_OUT"
    "$EXE" --datadir "$DATADIR" \
        --load-settings "${MACHINE_PROFILE};${overrides}" \
        --load-filaments "${FIL_PLA};${FIL_PETG}" \
        --slice 0 --outputdir out cube30.stl cube30b.stl \
        > "$logfile" 2>&1
    local rc=$?
    if [ $rc -eq 0 ]; then
        if [ -f "$PLATE_OUT" ]; then
            mv -f "$PLATE_OUT" "$dest"
        else
            rc=1
        fi
    fi
    return $rc
}

# run_slice_p2 <overrides_json> <filaments_semicolon_list> <dest_gcode> <log_file> [extra cli args...]
# Same shape as run_slice, but for the Part 2 (Support Interface Auto-Match)
# fixture: a single tshape.stl model, a caller-supplied filament list (the
# Part 1 fixture always loads two; Part 2's manual-mode identity checks need
# exactly one, to match how out/p2_baseline.gcode was itself recorded), and
# optional trailing generic --key=value CLI overrides (used for
# --enable-prime-tower=1; DynamicPrintAndCLIConfig::read_cli — see
# src/libslic3r/Config.cpp — accepts any FullPrintConfig key this way,
# dash-ified from its snake_case name).
run_slice_p2() {
    local overrides="$1" filaments="$2" dest="$3" logfile="$4"
    shift 4
    rm -f "$PLATE_OUT"
    "$EXE" --datadir "$DATADIR" \
        --load-settings "${MACHINE_PROFILE};${overrides}" \
        --load-filaments "${filaments}" \
        "$@" \
        --slice 0 --outputdir out "$TSHAPE" \
        > "$logfile" 2>&1
    local rc=$?
    if [ $rc -eq 0 ]; then
        if [ -f "$PLATE_OUT" ]; then
            mv -f "$PLATE_OUT" "$dest"
        else
            rc=1
        fi
    fi
    return $rc
}

# support_scope: filter a gcode stream (stdin) down to the regions relevant
# to the Part 2 feature - inside any "; FEATURE: Support" / "; FEATURE:
# Support interface" tagged section, plus toolchange lines (T<n>, M620*,
# M621*). Needed because tshape.stl's solid-infill chaining is independently
# confirmed (Task 3 report) to be non-deterministic run-to-run AND
# build-to-build in the base pipeline (reordered M73 markers / a
# differently-chunked "Internal solid infill" block) - a raw whole-file diff
# is NOT a valid identity check on this fixture.
#
# M73 progress-percentage lines are unconditionally dropped (not just left
# out of scope): Task 4's own diff investigation found the SAME M73 reorder
# non-determinism recurring *inside* "; FEATURE: Support..." tagged sections
# (M73 markers are interspersed by the time-estimator post-pass independent
# of feature boundaries), so leaving them in would let base-pipeline noise
# leak back into an otherwise-clean support-scoped comparison. See the Part
# 2 section below for exactly what this filter does and does not prove.
support_scope() {
    awk '
        /^M73/ { next }
        /^;.*FEATURE: /{ insupport = ($0 ~ /FEATURE: Support/) ? 1 : 0 }
        insupport { print; next }
        /^T[0-9]/ { print; next }
        /^M620/ { print; next }
        /^M621/ { print; next }
    '
}

# support_identical <fileA> <fileB> -> 0 if identical after normalize+support_scope
support_identical() {
    diff -q <(normalize "$1" | support_scope) <(normalize "$2" | support_scope) > /dev/null 2>&1
}

m620_count_of() { grep -cE "^M620 S" "$1"; }
total_layers_of() { grep -m1 "^; total layer number:" "$1" | grep -oE '[0-9]+' | head -1; }

# ---------------------------------------------------------------------------
# 1. Object mode (off-mode) — slice twice, byte-compare each vs baseline and
#    vs each other.
# ---------------------------------------------------------------------------
run_slice "$OBJ_OVERRIDES" "$OUT_OBJ_1" "out/verify_object_1.log"
obj1_rc=$?
if [ $obj1_rc -eq 0 ]; then record "object-run1-exit0" PASS "exit 0"
else record "object-run1-exit0" FAIL "exit $obj1_rc — see out/verify_object_1.log"; fi

run_slice "$OBJ_OVERRIDES" "$OUT_OBJ_2" "out/verify_object_2.log"
obj2_rc=$?
if [ $obj2_rc -eq 0 ]; then record "object-run2-exit0" PASS "exit 0"
else record "object-run2-exit0" FAIL "exit $obj2_rc — see out/verify_object_2.log"; fi

if [ $obj1_rc -eq 0 ]; then
    if byte_identical "$BASELINE" "$OUT_OBJ_1"; then
        record "object-run1-vs-baseline" PASS "byte-identical (normalized)"
    else
        record "object-run1-vs-baseline" FAIL "diff vs $BASELINE (normalized) — see diff manually"
    fi
else
    record "object-run1-vs-baseline" FAIL "skipped: run1 did not slice"
fi

if [ $obj2_rc -eq 0 ]; then
    if byte_identical "$BASELINE" "$OUT_OBJ_2"; then
        record "object-run2-vs-baseline" PASS "byte-identical (normalized)"
    else
        record "object-run2-vs-baseline" FAIL "diff vs $BASELINE (normalized) — see diff manually"
    fi
else
    record "object-run2-vs-baseline" FAIL "skipped: run2 did not slice"
fi

if [ $obj1_rc -eq 0 ] && [ $obj2_rc -eq 0 ]; then
    if byte_identical "$OUT_OBJ_1" "$OUT_OBJ_2"; then
        record "object-determinism" PASS "run1 == run2 (normalized)"
    else
        record "object-determinism" FAIL "run1 != run2 (normalized)"
    fi
else
    record "object-determinism" FAIL "skipped: one or both runs did not slice"
fi

# ---------------------------------------------------------------------------
# 2. nearest_wall mode (chameleon on) — slice twice, sanity-check run1,
#    byte-compare the two runs vs each other (determinism).
# ---------------------------------------------------------------------------
run_slice "$CHA_OVERRIDES" "$OUT_CHA_1" "out/verify_nearestwall_1.log"
cha1_rc=$?
if [ $cha1_rc -eq 0 ]; then record "nearestwall-run1-exit0" PASS "exit 0"
else record "nearestwall-run1-exit0" FAIL "exit $cha1_rc — see out/verify_nearestwall_1.log"; fi

run_slice "$CHA_OVERRIDES" "$OUT_CHA_2" "out/verify_nearestwall_2.log"
cha2_rc=$?
if [ $cha2_rc -eq 0 ]; then record "nearestwall-run2-exit0" PASS "exit 0"
else record "nearestwall-run2-exit0" FAIL "exit $cha2_rc — see out/verify_nearestwall_2.log"; fi

if [ $cha1_rc -eq 0 ]; then
    m620_count=$(grep -cE "^M620 S" "$OUT_CHA_1")
    if [ "$m620_count" -ge "$M620_MIN" ] && [ "$m620_count" -le "$M620_MAX" ]; then
        record "nearestwall-m620-bounds" PASS "M620 count = $m620_count (bound [$M620_MIN,$M620_MAX])"
    else
        record "nearestwall-m620-bounds" FAIL "M620 count = $m620_count, outside bound [$M620_MIN,$M620_MAX]"
    fi

    feature_count=$(grep -c "; FEATURE: Brim" "$OUT_CHA_1")
    if [ "$feature_count" -ge 1 ]; then
        record "nearestwall-feature-brim-present" PASS "'; FEATURE: Brim' occurs $feature_count time(s)"
    else
        record "nearestwall-feature-brim-present" FAIL "'; FEATURE: Brim' not found"
    fi
else
    record "nearestwall-m620-bounds" FAIL "skipped: run1 did not slice"
    record "nearestwall-feature-brim-present" FAIL "skipped: run1 did not slice"
fi

if [ $cha1_rc -eq 0 ] && [ $cha2_rc -eq 0 ]; then
    if byte_identical "$OUT_CHA_1" "$OUT_CHA_2"; then
        record "nearestwall-determinism" PASS "run1 == run2 (normalized)"
    else
        record "nearestwall-determinism" FAIL "run1 != run2 (normalized)"
    fi
else
    record "nearestwall-determinism" FAIL "skipped: one or both runs did not slice"
fi

# ---------------------------------------------------------------------------
# 3. Support Interface Auto-Match (Part 2) — tshape.stl fixture.
#
# CAVEAT (Task 3 report, reconfirmed here): tshape.stl's solid-infill
# chaining is run-to-run AND build-to-build non-deterministic in the base
# pipeline, unrelated to this feature — see support_scope()'s comment above.
# Every check below therefore compares only the support-region + toolchange
# content (support_identical), never a raw whole-file diff. That is
# sufficient to prove the new emission block introduces no divergence in the
# region it touches; it is NOT a whole-file determinism claim, and none is
# made here.
#
# CLI LIMITATION (same family as Part 1's precedent, see spike/FINDINGS.md):
# the CLI cannot assign per-object filaments to a plain STL, so tshape.stl —
# a single object — always resolves to exactly one used extruder no matter
# how many filaments are --load-filaments'd. Print::extruders() (Print.cpp)
# reflects real per-object usage, not the loaded-filament-profile count, so
# chameleon_assign_support_interfaces' own top-level guard
# (print.extruders().size() <= 1) returns before doing any work — no per-
# object "Chameleon support match: ..." BOOST_LOG line is ever emitted on
# THIS fixture, in EITHER mode, regardless of whether an alias/migration
# worked correctly. The checks below therefore CANNOT use that log line as
# migration evidence (unlike a real multi-filament GUI plate) - see 3c's own
# comment for what they use instead.
#
# 3a (v2.6): LEGACY-KEY check, "manual" value. spike_support_overrides.json now
# literally sets support_interface_filament_source = "manual" - the option key
# itself v2.6 removed entirely (replaced by the support_filament_matching
# checkbox, PrintConfig.hpp/.cpp) - so this doubles as the "old value -> matching
# DISABLED" leg of the legacy-key proof, on top of its original identity-vs-
# baseline role.
#
# 3c (v2.4 spec A/D, retargeted v2.6): LEGACY-KEY check, "nearest_surface" value.
# spike_p2_overrides.json still literally sets support_interface_filament_source
# = "nearest_surface" - both the enum value (v2.4) and the enum option itself
# (v2.6) are gone - so slicing with it exercises PrintConfigDef::handle_legacy's
# rename+remap branch (folds together the old v2.4 value-remap and the v2.6
# enum-to-bool migration) end to end. Without that branch,
# ConfigOptionEnum<T>::from_string would fail to find "nearest_surface" (the enum
# no longer even exists) and Config.cpp's set_deserialize_raw would silently
# substitute the option's DEFAULT (matching disabled) instead - so "it still
# slices" alone is NOT sufficient proof the migration worked (a silent-disabled
# substitution also slices without error). Three independent, config-level
# signals distinguish the two outcomes (chosen specifically because the CLI
# LIMITATION above rules out the per-object BOOST_LOG line as evidence on this
# fixture):
#   1. exit 0 (baseline: didn't crash/error).
#   2. The load_config_file log line for this fixture reads "no substitutions
#      performed from file spike_p2_overrides.json" - PROVES handle_legacy
#      consumed "nearest_surface" and rewrote it (opt_key AND value) to
#      something the CURRENT config def DOES recognize BEFORE deserialize ever
#      ran, so the later substitution-logging path (Config.cpp
#      set_deserialize_raw, which WOULD log "Found legacy configuration values,
#      substituted...") never triggers. A substitution-logged run means the
#      migration did NOT fire and the value silently fell back to the default
#      (matching disabled).
#   3. The output gcode's CONFIG_BLOCK dump contains the literal line
#      "; support_filament_matching = 1" (GCode.cpp append_full_config,
#      "; <key> = <value>") - the RESOLVED value after migration, under its NEW
#      key. "= 0" (or the key missing entirely) here would mean the migration
#      silently lost the user's intent even if signal 2 above somehow still
#      passed.
# Section 4 below repeats signals 2-3 for the OTHER old non-manual value,
# "nearest_wall" (spike_p2_wall_overrides.json) - both old values must resolve
# to matching ENABLED. Bounded M620 growth + feature-tag presence (the same
# sanity bar the pre-v2.4 nearest_surface checks used) round out the group as
# cheap defense-in-depth, plus a determinism repeat.
# ---------------------------------------------------------------------------

# --- 3a. Manual (old key) mode, tower OFF: identity (support-scoped) vs the
#         frozen pre-Task-4 baseline. Single PLA filament, matching exactly how
#         p2_baseline.gcode was itself recorded (Task 3), so this is a true
#         apples-to-apples comparison. --debug=3 (see 3c's header comment for
#         why) also lets this run double as the "manual" leg of the v2.6
#         legacy-key proof: two signals below the identity check confirm the
#         old "manual" string resolves to support_filament_matching = disabled
#         (not just silently defaulted to it, which would look identical here).
run_slice_p2 "$P2_MANUAL_OVERRIDES" "$FIL_PLA" "$OUT_P2_MAN_OFF" "out/p2_manual_off.log" --debug=3
p2man_off_rc=$?
if [ $p2man_off_rc -eq 0 ]; then record "p2-manual-off-exit0" PASS "exit 0"
else record "p2-manual-off-exit0" FAIL "exit $p2man_off_rc — see out/p2_manual_off.log"; fi

if [ $p2man_off_rc -eq 0 ]; then
    if support_identical "$P2_BASELINE" "$OUT_P2_MAN_OFF"; then
        record "p2-manual-off-vs-baseline" PASS "support-region + toolchange identical (normalized)"
    else
        record "p2-manual-off-vs-baseline" FAIL "support-region diff vs $P2_BASELINE — see diff manually"
    fi

    # v2.6 legacy-key signal 2 (mirrors 3c's signal 2): handle_legacy consumed
    # "manual" cleanly under the OLD key - no deserialize-failure substitution
    # was ever logged for this file.
    if grep -qE "no substitutions performed from file .*spike_support_overrides\.json" "out/p2_manual_off.log"; then
        record "p2-manual-legacy-no-substitution-notice" PASS "handle_legacy migration fired silently (no Config.cpp fallback substitution logged)"
    else
        record "p2-manual-legacy-no-substitution-notice" FAIL "expected 'no substitutions performed from file ...spike_support_overrides.json' in out/p2_manual_off.log — see log manually"
    fi

    # v2.6 legacy-key signal 3 (mirrors 3c's signal 3): the RESOLVED config
    # value in the output gcode's CONFIG_BLOCK, under the NEW key, is 0
    # (disabled) - proves the old "manual" string migrated to matching-off
    # rather than merely happening to already be the default.
    if grep -qE "^; support_filament_matching = 0" "$OUT_P2_MAN_OFF"; then
        record "p2-manual-legacy-resolved-disabled" PASS "CONFIG_BLOCK shows support_filament_matching = 0"
    else
        record "p2-manual-legacy-resolved-disabled" FAIL "'; support_filament_matching = 0' not found in $OUT_P2_MAN_OFF"
    fi
else
    record "p2-manual-off-vs-baseline" FAIL "skipped: slice did not complete"
    record "p2-manual-legacy-no-substitution-notice" FAIL "skipped: slice did not complete"
    record "p2-manual-legacy-resolved-disabled" FAIL "skipped: slice did not complete"
fi

# --- 3b. Manual mode, tower ON. NOTE: PrintApply.cpp's normalize_fdm_2
#         forces enable_prime_tower back to false whenever used_filaments==1
#         (PrintConfig.cpp ~7602/7683) — exactly this single-PLA,
#         single-STL-object fixture. "Tower ON" here therefore provably
#         collapses to the same tower-OFF code path; this check proves
#         passing the override doesn't crash and doesn't change the support
#         region. A real tower-on/support-interaction test needs the GUI
#         multi-filament fixture where a wipe tower can actually be built.
run_slice_p2 "$P2_MANUAL_OVERRIDES" "$FIL_PLA" "$OUT_P2_MAN_ON" "out/p2_manual_on.log" --enable-prime-tower=1
p2man_on_rc=$?
if [ $p2man_on_rc -eq 0 ]; then record "p2-manual-on-exit0" PASS "exit 0"
else record "p2-manual-on-exit0" FAIL "exit $p2man_on_rc — see out/p2_manual_on.log"; fi

if [ $p2man_on_rc -eq 0 ]; then
    if support_identical "$P2_BASELINE" "$OUT_P2_MAN_ON"; then
        record "p2-manual-on-vs-baseline" PASS "support-region + toolchange identical (normalized; tower forced off by used_filaments==1)"
    else
        record "p2-manual-on-vs-baseline" FAIL "support-region diff vs $P2_BASELINE — see diff manually"
    fi
else
    record "p2-manual-on-vs-baseline" FAIL "skipped: slice did not complete"
fi

# --- 3c. LEGACY-ALIAS check (v2.4 spec A/D): P2_NEAREST_OVERRIDES still says
#         "nearest_surface" literally. exit 0 + no-substitution-notice +
#         resolved-value-is-nearest_wall (NOT manual) + the same bounded-M620/
#         feature-tag sanity bar the pre-v2.4 checks used - see this
#         section's own header comment above for why these three signals
#         (not the per-object BOOST_LOG line) are the evidence used here.
#         --debug=3 (PrintConfig.cpp "debug" option, 3 = info level):
#         Utils.cpp's set_logging_level defaults the CLI to level 2
#         ("warning") - EVERY BOOST_LOG_TRIVIAL(info) line, including both
#         "no substitutions performed from file ..." (Snapmaker_Orca.cpp
#         load_config_file) and chameleon_assign_support_interfaces' own
#         per-object "Chameleon support match: ..." line, is filtered out of
#         the log at the default level. Without --debug=3 the substitution-
#         notice check below would silently see an EMPTY log and could pass
#         for the wrong reason (grep -q on missing text is indistinguishable
#         from grep -q on filtered-out text) - bump to info explicitly so the
#         check actually exercises the log line it claims to.
run_slice_p2 "$P2_NEAREST_OVERRIDES" "${FIL_PLA};${FIL_PETG}" "$OUT_P2_LEGACY_OFF_1" "out/p2_legacy_off_1.log" --debug=3
p2legacy_off1_rc=$?
if [ $p2legacy_off1_rc -eq 0 ]; then record "p2-legacy-off-exit0" PASS "exit 0"
else record "p2-legacy-off-exit0" FAIL "exit $p2legacy_off1_rc — see out/p2_legacy_off_1.log"; fi

if [ $p2legacy_off1_rc -eq 0 ]; then
    # Signal 2: handle_legacy consumed "nearest_surface" cleanly - no
    # deserialize-failure substitution was ever logged for this file. Anchor
    # on the filename (not just the generic phrase) so this doesn't
    # accidentally match a DIFFERENT file's "no substitutions" line further
    # up the same log (machine/filament profiles are loaded first).
    if grep -qE "no substitutions performed from file .*spike_p2_overrides\.json" "out/p2_legacy_off_1.log"; then
        record "p2-legacy-no-substitution-notice" PASS "handle_legacy alias fired silently (no Config.cpp fallback substitution logged)"
    else
        record "p2-legacy-no-substitution-notice" FAIL "expected 'no substitutions performed from file ...spike_p2_overrides.json' in out/p2_legacy_off_1.log — see log manually"
    fi

    # Signal 3: the RESOLVED config value in the output gcode's CONFIG_BLOCK,
    # under the NEW key, is 1 (enabled), not 0 - proves the migration turned
    # the checkbox on rather than a substitution silently defaulting it off.
    if grep -qE "^; support_filament_matching = 1" "$OUT_P2_LEGACY_OFF_1"; then
        record "p2-legacy-resolved-matching-enabled" PASS "CONFIG_BLOCK shows support_filament_matching = 1"
    else
        record "p2-legacy-resolved-matching-enabled" FAIL "'; support_filament_matching = 1' not found in $OUT_P2_LEGACY_OFF_1"
    fi

    total_layers=$(total_layers_of "$OUT_P2_LEGACY_OFF_1")
    [ -z "$total_layers" ] && total_layers=0
    m620_max=$((2 + 2 * 3 * total_layers))
    m620_count=$(m620_count_of "$OUT_P2_LEGACY_OFF_1")
    if [ "$m620_count" -ge 2 ] && [ "$m620_count" -le "$m620_max" ]; then
        record "p2-legacy-m620-bounds" PASS "M620 count = $m620_count (bound [2,$m620_max], total_layers=$total_layers)"
    else
        record "p2-legacy-m620-bounds" FAIL "M620 count = $m620_count, outside bound [2,$m620_max]"
    fi

    feature_count=$(grep -c "; FEATURE: Support interface" "$OUT_P2_LEGACY_OFF_1")
    if [ "$feature_count" -ge 1 ]; then
        record "p2-legacy-feature-present" PASS "'; FEATURE: Support interface' occurs $feature_count time(s)"
    else
        record "p2-legacy-feature-present" FAIL "'; FEATURE: Support interface' not found"
    fi
else
    record "p2-legacy-no-substitution-notice" FAIL "skipped: run did not slice"
    record "p2-legacy-resolved-matching-enabled" FAIL "skipped: run did not slice"
    record "p2-legacy-m620-bounds" FAIL "skipped: run did not slice"
    record "p2-legacy-feature-present" FAIL "skipped: run did not slice"
fi

# --- 3d. Determinism repeat: legacy-alias fixture, tower OFF, two
#         consecutive slices, support-scoped compare (per the CAVEAT above).
run_slice_p2 "$P2_NEAREST_OVERRIDES" "${FIL_PLA};${FIL_PETG}" "$OUT_P2_LEGACY_OFF_2" "out/p2_legacy_off_2.log" --debug=3
p2legacy_off2_rc=$?
if [ $p2legacy_off2_rc -eq 0 ]; then record "p2-legacy-determinism-run2-exit0" PASS "exit 0"
else record "p2-legacy-determinism-run2-exit0" FAIL "exit $p2legacy_off2_rc — see out/p2_legacy_off_2.log"; fi

if [ $p2legacy_off1_rc -eq 0 ] && [ $p2legacy_off2_rc -eq 0 ]; then
    if support_identical "$OUT_P2_LEGACY_OFF_1" "$OUT_P2_LEGACY_OFF_2"; then
        record "p2-legacy-determinism" PASS "run1 == run2, support-region + toolchange (normalized)"
    else
        record "p2-legacy-determinism" FAIL "run1 != run2, support-region + toolchange (normalized)"
    fi
else
    record "p2-legacy-determinism" FAIL "skipped: one or both runs did not slice"
fi

# ---------------------------------------------------------------------------
# 4. Support Interface Auto-Match (Part 2) — LEGACY-KEY check, "nearest_wall"
#    value (v2.2 Task 4, spec C8; v2.4 spec A the sole surviving enum value;
#    retargeted v2.6: the enum option itself is gone, replaced by the
#    support_filament_matching checkbox, so this fixture's old key/value is now
#    this section's own legacy-key fixture, mirroring section 3c's
#    "nearest_surface" one - both old non-manual values must resolve to
#    matching ENABLED). Same tshape.stl fixture and the same CLI
#    LIMITATION/CAVEAT notes as section 3 above apply here verbatim (single
#    used extruder on this CLI fixture, so this validates no-crash /
#    well-formed output / feature-tag presence / determinism, not real
#    cross-extruder matching — see section 3's own comment block for the full
#    rationale). Checks are prefixed "p2-wall-" (NOT "nearestwall-", which section 2 above
#    already uses for the unrelated Part 1 brim_filament_source=nearest_wall
#    checks) so the two features' checks never collide in the results table.
# ---------------------------------------------------------------------------

# --- 4a. nearest_wall (old key) mode, tower OFF: no-crash + bounded M620
#         growth + feature-tag presence, plus the same two legacy-key signals
#         section 3c uses (--debug=3, no-substitution-notice, resolved value) -
#         see 3c's own header comment for why these are the evidence used
#         instead of the per-object BOOST_LOG line.
run_slice_p2 "$P2_WALL_OVERRIDES" "${FIL_PLA};${FIL_PETG}" "$OUT_P2_WALL_OFF_1" "out/p2_wall_off_1.log" --debug=3
p2wall_off1_rc=$?
if [ $p2wall_off1_rc -eq 0 ]; then record "p2-wall-off-exit0" PASS "exit 0"
else record "p2-wall-off-exit0" FAIL "exit $p2wall_off1_rc — see out/p2_wall_off_1.log"; fi

if [ $p2wall_off1_rc -eq 0 ]; then
    total_layers=$(total_layers_of "$OUT_P2_WALL_OFF_1")
    [ -z "$total_layers" ] && total_layers=0
    m620_max=$((2 + 2 * 3 * total_layers))
    m620_count=$(m620_count_of "$OUT_P2_WALL_OFF_1")
    if [ "$m620_count" -ge 2 ] && [ "$m620_count" -le "$m620_max" ]; then
        record "p2-wall-off-m620-bounds" PASS "M620 count = $m620_count (bound [2,$m620_max], total_layers=$total_layers)"
    else
        record "p2-wall-off-m620-bounds" FAIL "M620 count = $m620_count, outside bound [2,$m620_max]"
    fi

    feature_count=$(grep -c "; FEATURE: Support interface" "$OUT_P2_WALL_OFF_1")
    if [ "$feature_count" -ge 1 ]; then
        record "p2-wall-off-feature-present" PASS "'; FEATURE: Support interface' occurs $feature_count time(s)"
    else
        record "p2-wall-off-feature-present" FAIL "'; FEATURE: Support interface' not found"
    fi

    # Legacy-key signal 2 (mirrors 3c): handle_legacy consumed "nearest_wall"
    # cleanly under the OLD key - no deserialize-failure substitution was ever
    # logged for this file.
    if grep -qE "no substitutions performed from file .*spike_p2_wall_overrides\.json" "out/p2_wall_off_1.log"; then
        record "p2-wall-legacy-no-substitution-notice" PASS "handle_legacy migration fired silently (no Config.cpp fallback substitution logged)"
    else
        record "p2-wall-legacy-no-substitution-notice" FAIL "expected 'no substitutions performed from file ...spike_p2_wall_overrides.json' in out/p2_wall_off_1.log — see log manually"
    fi

    # Legacy-key signal 3 (mirrors 3c): the RESOLVED config value in the
    # output gcode's CONFIG_BLOCK, under the NEW key, is 1 (enabled) - proves
    # the old "nearest_wall" string migrated to matching-on too.
    if grep -qE "^; support_filament_matching = 1" "$OUT_P2_WALL_OFF_1"; then
        record "p2-wall-legacy-resolved-matching-enabled" PASS "CONFIG_BLOCK shows support_filament_matching = 1"
    else
        record "p2-wall-legacy-resolved-matching-enabled" FAIL "'; support_filament_matching = 1' not found in $OUT_P2_WALL_OFF_1"
    fi
else
    record "p2-wall-off-m620-bounds" FAIL "skipped: run did not slice"
    record "p2-wall-off-feature-present" FAIL "skipped: run did not slice"
    record "p2-wall-legacy-no-substitution-notice" FAIL "skipped: run did not slice"
    record "p2-wall-legacy-resolved-matching-enabled" FAIL "skipped: run did not slice"
fi

# --- 4b. nearest_wall mode, tower ON: same sanity bar, tower override applied
#         (collapses to tower-off here too, same as 3b/3d).
run_slice_p2 "$P2_WALL_OVERRIDES" "${FIL_PLA};${FIL_PETG}" "$OUT_P2_WALL_ON" "out/p2_wall_on.log" --enable-prime-tower=1
p2wall_on_rc=$?
if [ $p2wall_on_rc -eq 0 ]; then record "p2-wall-on-exit0" PASS "exit 0"
else record "p2-wall-on-exit0" FAIL "exit $p2wall_on_rc — see out/p2_wall_on.log"; fi

if [ $p2wall_on_rc -eq 0 ]; then
    total_layers=$(total_layers_of "$OUT_P2_WALL_ON")
    [ -z "$total_layers" ] && total_layers=0
    m620_max=$((2 + 2 * 3 * total_layers))
    m620_count=$(m620_count_of "$OUT_P2_WALL_ON")
    if [ "$m620_count" -ge 2 ] && [ "$m620_count" -le "$m620_max" ]; then
        record "p2-wall-on-m620-bounds" PASS "M620 count = $m620_count (bound [2,$m620_max], total_layers=$total_layers)"
    else
        record "p2-wall-on-m620-bounds" FAIL "M620 count = $m620_count, outside bound [2,$m620_max]"
    fi

    feature_count=$(grep -c "; FEATURE: Support interface" "$OUT_P2_WALL_ON")
    if [ "$feature_count" -ge 1 ]; then
        record "p2-wall-on-feature-present" PASS "'; FEATURE: Support interface' occurs $feature_count time(s)"
    else
        record "p2-wall-on-feature-present" FAIL "'; FEATURE: Support interface' not found"
    fi
else
    record "p2-wall-on-m620-bounds" FAIL "skipped: run did not slice"
    record "p2-wall-on-feature-present" FAIL "skipped: run did not slice"
fi

# --- 4c. Determinism repeat: nearest_wall, tower OFF, two consecutive slices,
#         support-scoped compare (per section 3's CAVEAT).
run_slice_p2 "$P2_WALL_OVERRIDES" "${FIL_PLA};${FIL_PETG}" "$OUT_P2_WALL_OFF_2" "out/p2_wall_off_2.log"
p2wall_off2_rc=$?
if [ $p2wall_off2_rc -eq 0 ]; then record "p2-wall-determinism-run2-exit0" PASS "exit 0"
else record "p2-wall-determinism-run2-exit0" FAIL "exit $p2wall_off2_rc — see out/p2_wall_off_2.log"; fi

if [ $p2wall_off1_rc -eq 0 ] && [ $p2wall_off2_rc -eq 0 ]; then
    if support_identical "$OUT_P2_WALL_OFF_1" "$OUT_P2_WALL_OFF_2"; then
        record "p2-wall-determinism" PASS "run1 == run2, support-region + toolchange (normalized)"
    else
        record "p2-wall-determinism" FAIL "run1 != run2, support-region + toolchange (normalized)"
    fi
else
    record "p2-wall-determinism" FAIL "skipped: one or both runs did not slice"
fi

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo ""
printf "%-32s %-6s %s\n" "CHECK" "RESULT" "DETAIL"
printf "%-32s %-6s %s\n" "--------------------------------" "------" "----------------------------------------"
for row in "${RESULTS[@]}"; do
    IFS='|' read -r name status detail <<< "$row"
    printf "%-32s %-6s %s\n" "$name" "$status" "$detail"
done
echo ""

total=${#RESULTS[@]}
passed=$((total - FAILS))
echo "${passed}/${total} checks passed."

if [ "$FAILS" -eq 0 ]; then
    echo "RESULT: ALL PASS"
    exit 0
else
    echo "RESULT: FAIL ($FAILS check(s) failed)"
    exit 1
fi
