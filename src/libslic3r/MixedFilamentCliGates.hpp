#pragma once
#ifndef slic3r_MixedFilamentCliGates_hpp_
#define slic3r_MixedFilamentCliGates_hpp_

// Pure, GUI-free CLI decision logic for the Mixed Filament wipe/flush/type gates
// (ported from OrcaSlicer #15636, adapted onto this fork's MixedFilamentManager).
// Extracted out of Snapmaker_Orca.cpp (the app's non-unit-testable main) so it can be
// exercised directly by Catch2 tests in tests/libslic3r. Behaviour is unchanged from
// the inline version: the CLI call sites forward to these functions verbatim.

#include <string>
#include <vector>

#include "MixedFilament.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

class Model;
class ModelObject;
class ModelVolume;

// Verdict returned by the CLI mixed-filament gates below. `ok == true` means the
// slice may proceed; otherwise `message` explains why (already formatted for
// BOOST_LOG_TRIVIAL(error) / cli_errors[CLI_MIXED_FILAMENT_INVALID] use).
struct CliMixedFilamentVerdict
{
    bool        ok = true;
    std::string message;

    static CliMixedFilamentVerdict pass() { return CliMixedFilamentVerdict{true, {}}; }
    static CliMixedFilamentVerdict fail(std::string msg) { return CliMixedFilamentVerdict{false, std::move(msg)}; }
};

// Reads "mixed_filament_definitions" preferring extra_config (CLI --extra-config,
// not yet merged into print_config) over print_config itself. Empty string if unset
// anywhere.
std::string cli_mixed_filament_definitions(const DynamicPrintConfig &print_config, const DynamicPrintConfig *extra_config);

// Rebuilds `mgr` from scratch: auto-generates the C(N,2) pairwise combinations from
// `filament_colours` (padded/truncated to `num_physical`), then overlays any custom
// rows from mixed_filament_definitions (extra_config wins over print_config).
void populate_cli_mixed_filament_manager(MixedFilamentManager           &mgr,
                                          const DynamicPrintConfig       &print_config,
                                          const DynamicPrintConfig       *extra_config,
                                          const std::vector<std::string> &filament_colours,
                                          size_t                          num_physical);

// Zeros flush_vol_matrix[from][to] wherever from==to or either side is a mixed slot
// (a mixed slot never reaches a nozzle, so it never needs a purge to/from itself).
// matrix_filament_count is the matrix's own N (matrix must be N*N or larger); a
// mismatched/too-small matrix is left untouched.
void zero_mixed_flush_rows_and_cols(std::vector<double>        &flush_vol_matrix,
                                     size_t                      matrix_filament_count,
                                     const MixedFilamentManager &mgr,
                                     size_t                      num_physical);

// True when a serialized mixed_filament_definitions row (semicolon-separated,
// "component_a,component_b,enabled,...") names a physical slot ID outside
// [1, num_physical] for an enabled, non-deleted row. Deleted rows ("d1" token) and
// disabled rows (third field == 0) are skipped. Malformed/short rows are ignored.
bool mixed_definitions_have_slot_without_filament(const std::string &serialized, size_t num_physical);

// Appends every filament id (1-based) referenced by print_config's per-feature
// filament options (wall/outer-wall/sparse-infill/solid-infill/support/support-interface)
// to `ids`. IDs <= 0 (meaning "use default" / "follow walls") are skipped.
void append_config_filament_ids(const DynamicPrintConfig &cfg, std::vector<int> &ids);

// Per-feature filament ids (>0) set directly on a modifier / model-part / height-range
// config: wall_filament, outer_wall_filament, sparse_infill_filament, solid_infill_filament.
// Disabled features are skipped (same gates as PrintRegion::collect_object_printing_extruders):
// walls when wall_loops==0 (unless brim), sparse when density==0, solid when both shell counts
// are 0. Support keys are object-level and are not collected here.
void append_feature_filament_overrides(const ConfigBase &cfg, std::vector<int> &ids);

// True for volumes whose own config can pin a per-feature filament (MODEL_PART and
// PARAMETER_MODIFIER). Precise Seam / negative / support volumes are excluded here so
// that work can add a clause in one place.
bool volume_contributes_feature_filaments(const ModelVolume &volume);

// Bound passed to region_config_from_model_volume. Print-options presets have no
// filament_diameter / filament_colour; INT_MAX means "do not clamp" so object/part
// extruder ids and outer_wall_filament survive on the GUI path.
size_t plate_filament_bound(const DynamicPrintConfig &cfg_with_filaments, const MixedFilamentManager *mixed = nullptr);

PrintRegionConfig default_region_config(const DynamicPrintConfig &global);

// Shared by PartPlate::get_extruders, get_extruders_under_cli, and collect_cli_filament_ids.
// Builds each MODEL_PART region the way slicing does (region_config_from_model_volume per
// LayerRanges interval, including default-config gaps below ModelObject::max_z()), applies
// modifiers that intersect the parent part/range, then the unclamped four-gate check.
// Painted MMU states are appended as 1-based ids. Support filaments only when support/raft is on.
// Objects with no contributing volumes fall back to object-level feature keys (slot-gate tests).
// Callers that already know the plate's default region and filament bound (once per plate)
// should pass them; the 3-argument overload derives both from global_config.
void append_object_plate_filament_ids(const ModelObject        &object,
                                      const DynamicPrintConfig &global_config,
                                      std::vector<int>         &ids,
                                      const PrintRegionConfig  &default_region,
                                      size_t                    num_total);
void append_object_plate_filament_ids(const ModelObject &object, const DynamicPrintConfig &global_config, std::vector<int> &ids);

// 1-based filament printing the outer wall of an object whose own config is `object_config`
// (may be null) over `global_config`, or 0 when it follows wall_filament (already counted)
// or no walls are printed. Mirrors PrintRegion::extruder(frExternalPerimeter) and the
// wall_loops gate in PrintRegion::collect_object_printing_extruders. An object key of 0
// wins over a global outer filament (explicit "follow walls").
int resolve_outer_wall_filament(const ConfigBase *object_config, const ConfigBase &global_config);

// Same as append_config_filament_ids, then append_object_plate_filament_ids for every
// object in the loaded models - i.e. every place a CLI-loaded 3mf can pin a filament id,
// mixed slots included. `extruder` is applied the same way slicing does (overrides
// wall/sparse/solid, zeroes outer wall, ignores feature values of 0).
void collect_cli_filament_ids(const std::vector<Model> &models, const DynamicPrintConfig &print_config, std::vector<int> &ids);

// Resolves a MixedFilament's components (manual pattern tokens, or component_a/b
// plus gradient IDs when there is no manual pattern) down to the sorted, deduplicated
// set of physical filament ids (1-based, each in [1, num_physical]) it actually uses.
std::vector<unsigned int> mixed_physical_component_ids(const MixedFilament &mf, size_t num_physical);

// True when a mixed filament's resolved physical components do not all share the
// same filament_type (PLA/PETG/...). An empty/unset type reads as "PLA", matching
// cfg.get_filament_type's own fallback.
bool mixed_components_differ_in_filament_type(const MixedFilament &mf, DynamicPrintConfig &cfg, size_t num_physical);

// ---------------------------------------------------------------------------
// Gate entry points. Each returns a verdict; the CLI caller is responsible for the
// exit-code/record_exit_reson/flush_and_exit plumbing around a failing verdict, so
// these stay free of process-exit and file-I/O side effects.
// ---------------------------------------------------------------------------

// Gate 1: refuses a project whose mixed_filament_definitions name a mixed slot with
// no filament of its own (either the serialized definitions reference an
// out-of-range physical id, or a filament id used anywhere in the loaded models is
// beyond num_physical and is not itself a valid mixed slot). Mirrors the two checks
// inlined in CLI::run() right after the 3mf merge/normalize step. When
// mixed_filament_definitions is empty AND no mixed rows are enabled, the gate is
// skipped entirely (verdict passes trivially) - this is how "feature off" is
// represented on this fork (there is no separate on/off toggle option).
CliMixedFilamentVerdict cli_check_mixed_filament_slots_have_filament(const MixedFilamentManager &mgr,
                                                                      const std::string           &mixed_defs,
                                                                      size_t                       num_physical,
                                                                      const std::vector<Model>   &models,
                                                                      const DynamicPrintConfig    &print_config,
                                                                      int                          filament_count);

// Gate 2: refuses a plate that uses a mixed slot whose resolved components differ in
// filament_type. `plate_slots` is the plate's own get_extruders_under_cli(...,
// expand_mixed_slots=false) result (mixed IDs kept as slots, not expanded).
CliMixedFilamentVerdict cli_check_mixed_filament_type_compatibility(const MixedFilamentManager    &mgr,
                                                                     const std::vector<int>         &plate_slots,
                                                                     size_t                          num_physical,
                                                                     DynamicPrintConfig             &plate_config,
                                                                     int                              plate_index_1based);

} // namespace Slic3r

#endif /* slic3r_MixedFilamentCliGates_hpp_ */
