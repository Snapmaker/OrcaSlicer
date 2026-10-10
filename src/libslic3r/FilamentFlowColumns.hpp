#pragma once

// Snapmaker Orca: the Standard / High Flow columns of a filament preset. Every key of
// filament_options_with_variant holds one value per entry of "filament_extruder_variant"; these
// helpers add, check, repair, compare and transfer those columns.

#include <cstddef>
#include <string>
#include <vector>

#include "PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// The variant list of a filament preset ("filament_extruder_variant"); empty without one.
std::vector<std::string> filament_variants(const DynamicPrintConfig &config);

// The column of `type` in the variant list (Direct Drive or Bowden), -1 when there is none.
int filament_flow_column(const DynamicPrintConfig &config, NozzleVolumeType type);

// A variant name made of a known drive and a known nozzle volume type ("Direct Drive High Flow").
bool is_known_filament_variant(const std::string &variant);

// Adds the column `variant` as a copy of column 0: the variant list gains the name, every present
// variant key gets column 0's value (nil stays nil). False when the column exists, the name is
// empty or the config has no variant list.
bool filament_add_variant_column(DynamicPrintConfig &config, const std::string &variant);

// Adds the column of `type` with the drive of column 0 ("Bowden High Flow" when column 0 is a
// Bowden column). False for Standard, Hybrid, an existing column or a config without variant list.
bool filament_add_flow_column(DynamicPrintConfig &config, NozzleVolumeType type);

// Every present variant key is as wide as the variant list.
bool filament_columns_consistent(const DynamicPrintConfig &config);

// Trims longer variant keys to the width of the variant list and pads shorter ones with their
// first value. Returns whether anything changed.
bool filament_repair_columns(DynamicPrintConfig &config);

// The columns of the float key `key` whose value is below `min` (nil and missing columns are skipped).
std::vector<size_t> filament_columns_below(const DynamicPrintConfig &config, const std::string &key, double min);

// Copies the indexed keys ("filament_max_volumetric_speed#1") of `source` into `target` by variant
// name: a column the target lacks is added first (as a copy of its column 0). Unknown variants and
// keys outside filament_options_with_variant are skipped. Ends with filament_repair_columns().
void filament_transfer_columns(DynamicPrintConfig &target, const DynamicPrintConfig &source, const std::vector<std::string> &indexed_keys);

// `parent` laid out like `child`: unchanged when the child has no variant the parent lacks, else a
// copy in `storage` widened by each such variant (a copy of the parent's column 0), in the child's order.
const DynamicPrintConfig &filament_reference_in_layout_of(const DynamicPrintConfig &child, const DynamicPrintConfig &parent, DynamicPrintConfig &storage);

// Variant keys whose Standard value was changed against `saved` while the High Flow value still
// holds the saved one, which was a copy of the saved Standard value: the candidates of "Apply to
// High Flow too". Both configs need the same High Flow column.
std::vector<std::string> filament_standard_edits_not_followed(const DynamicPrintConfig &edited, const DynamicPrintConfig &saved);

// Copies the Standard value of each key in `keys` into the High Flow column. Returns whether a value changed.
bool filament_copy_standard_to_high_flow(DynamicPrintConfig &config, const std::vector<std::string> &keys);

// Drops the columns whose variant is not in `variants`; column 0 stays when none would be left.
// Returns whether a column was dropped.
bool filament_drop_columns_not_in(DynamicPrintConfig &config, const std::vector<std::string> &variants);

// A Filament tab field with one value for the Standard and the High Flow column (a key outside
// filament_options_with_variant, such as the cooling layer times or the overhang fan); such a
// field is read-only while a High Flow column is shown.
bool filament_field_shared_under_high_flow(const std::string &key);

// "filament_self_index" of a project config: filament f (1-based) repeated counts[f - 1] times. False,
// with `out` untouched, when a count is negative or the counts do not add up to `list_size` (the
// length of the joined filament_extruder_variant list).
bool filament_self_index_from_counts(const std::vector<int> &counts, size_t list_size, std::vector<int> &out);

} // namespace Slic3r
