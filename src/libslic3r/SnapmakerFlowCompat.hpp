#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

// Reading of configuration layouts that predate the Standard / High Flow columns of the Snapmaker
// filament presets.

// Expands promoted_filament_variant_keys() stored per filament to one value per column, using
// filament_self_index (1 based). Must run before the config is split into filament presets, which
// resizes the vectors in place. Returns the number of rebuilt keys.
size_t normalize_promoted_filament_keys(DynamicPrintConfig& config, size_t num_filaments, const std::vector<int>& filament_self_index);

// Snapmaker Orca 2.4 configs: the *_flow_support / filament_flow_step_size keys become variant
// columns, and filament_volume_type is folded into nozzle_volume_type (a tool head takes the type its
// filaments agree on, else differing filaments are dropped into the report). True if any 2.4 key was found.

struct FlowImportReport
{
    struct HeadChange {
        int  head;          // 1 based
        bool to_high_flow;  // the type the tool head was given
    };
    struct DroppedFilament {
        int  filament;          // 1 based
        int  head;              // 1 based, the tool head that prints the filament
        bool wanted_high_flow;  // the type the file asked for
    };
    std::vector<HeadChange>      changed_heads;
    std::vector<DroppedFilament> dropped_filaments;
    // The file said that the filaments had been grouped by flow type by hand.
    bool                         custom_grouping { false };

    bool empty() const { return changed_heads.empty() && dropped_filaments.empty(); }
    void clear() { changed_heads.clear(); dropped_filaments.clear(); custom_grouping = false; }
};

bool has_snapmaker_flow_keys(const DynamicPrintConfig& config);
bool normalize_snapmaker_flow_config(DynamicPrintConfig& config, FlowImportReport& report);

// Removes the 2.4 keys without reading them. The system presets of this application keep their
// *_flow_support entries as markers for a later comparison with upstream; their columns are
// declared by the variant keys next to them.
void drop_snapmaker_flow_keys(DynamicPrintConfig& config);

// Process options that 2.4 stores with one value per flow type while they are single values here.
// The reader of a preset file keeps the first (Standard) value of such an array.
bool is_snapmaker_flow_scalar_key(const std::string& key);

// project_schema_version to write: above 1 when a filament or the process has more than one column,
// so Snapmaker Orca 2.4 (which indexes by filament id) warns that it cannot fully load the file.
int project_schema_version_for(const DynamicPrintConfig& config);

} // namespace Slic3r
