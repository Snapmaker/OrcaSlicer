#pragma once
#ifndef slic3r_MixedFilament_hpp_
#define slic3r_MixedFilament_hpp_

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

namespace Slic3r {

class PrintObject;
class DynamicPrintConfig;

enum class MixedFilamentColorEngine : uint8_t { FilamentMixer = 0, FullSpectrumKSPairResidual = 1 };

struct MixedFilamentColorInput
{
    std::string                color_hex;
    int                        percent = 0;
    std::optional<double>      td_mm;
    std::optional<std::string> material_id;
};

std::vector<int> fill_continuous_layer_range(const std::vector<int>& sorted_layers);

// Represents a virtual "mixed" filament created from physical filaments
// (layer cadence and/or same-layer interleaved stripe distribution). Display
// colour blending uses FilamentMixer  so pair previews better
//  match expected print mixing
// (for example Blue+Yellow -> Green, Red+Yellow -> Orange, Red+Blue -> Purple).
// Legacy RYB code is retained in source for reference only.
struct MixedFilamentLegacyRow
{
    enum DistributionMode : uint8_t { LayerCycle = 0, SameLayerPointillisme = 1, Simple = 2 };

    // 1-based physical filament IDs that are combined.
    unsigned int component_a = 1;
    unsigned int component_b = 2;

    // Persistent row identity used to keep painted virtual-tool assignments
    // stable even when the visible mixed-filament list is rebuilt.
    uint64_t stable_id = 0;

    // Layer-alternation ratio.  With ratio_a = 2, ratio_b = 1 the cycle is
    // A, A, B, A, A, B, ...
    int ratio_a = 1;
    int ratio_b = 1;

    // Blend percentage of component B in [0..100].
    int mix_b_percent = 50;

    // Optional manual pattern for this mixed filament.
    // Legacy format (no '/'): each '1'-'9' char is a token.
    //   '1'=>component_a, '2'=>component_b, '3'-'9'=>direct physical IDs.
    // Modern format (with '/'): '/' separates tokens, multi-digit IDs supported.
    //   e.g. "1/10/2/11/12". Comma separates per-perimeter groups.
    std::string manual_pattern;

    // Optional explicit gradient multi-color component list, encoded as
    // compact physical filament IDs (for example "123" -> filaments 1,2,3).
    // Interleaved stripe mode is active for gradient rows only when this list has 3+ IDs.
    std::string gradient_component_ids;
    // Optional explicit multi-color weights aligned with gradient_component_ids.
    // Compact integer list joined by '/': for example "50/25/25".
    std::string gradient_component_weights;

    // Legacy compatibility flag from earlier prototype serialization.
    bool pointillism_all_filaments = false;

    // How this mixed row is distributed:
    // - LayerCycle: one filament per layer based on cadence.
    // - SameLayerPointillisme: split painted masks in XY on each layer.
    int distribution_mode = int(Simple);

    // Optional Local-Z cap for this mixed row. 0 disables the cap.
    int local_z_max_sublayers = 0;

    static constexpr float k_default_gradient_dominant = 0.8f;  // Dominant component ratio
    static constexpr float k_default_gradient_minority = 0.2f;  // Minority component ratio
    static constexpr float k_min_gradient_difference   = 0.05f; // Minimum difference for valid gradient

    bool  gradient_enabled = false;
    float gradient_start   = k_default_gradient_dominant;
    float gradient_end     = k_default_gradient_minority;
    // Alternating color stops and transition midpoints, normalized to [0, 1].
    std::vector<float> gradient_stop_positions;
    // Full solid-color widths, aligned with the ordered gradient colors.
    // Empty means inherit the legacy process value; endpoints have zero width.
    std::vector<float> gradient_solid_widths;

    // Additional XY surface offsets, in mm, applied when this mixed row
    // resolves to component A or B for an entire layer. Positive values
    // contract inward; negative values expand outward.
    float component_a_surface_offset = 0.f;
    float component_b_surface_offset = 0.f;

    // Whether this mixed filament is enabled (available for assignment).
    bool enabled = true;

    // True when this mixed filament row was deleted from UI and should stay hidden.
    bool deleted = false;

    // True when this row was user-created (custom) instead of auto-generated.
    bool custom = false;

    // True when this row originated from an auto-generated pair. This remains
    // true even after editing so delete logic can keep the base auto pair
    // tombstoned instead of letting regeneration resurrect it.
    bool origin_auto = false;

    // UI mode that created this row (-1=unknown/legacy, 0=RATIO, 1=CYCLE, 2=MATCH, 3=GRADIENT).
    int ui_mode = -1;

    // Computed display colour as "#RRGGBB".
    std::string display_color;

    bool operator==(const MixedFilamentLegacyRow& rhs) const
    {
        constexpr float k_surface_offset_epsilon = 1e-6f;
        constexpr float k_gradient_epsilon       = 1e-4f;
        return component_a == rhs.component_a && component_b == rhs.component_b && stable_id == rhs.stable_id &&
               gradient_stop_positions == rhs.gradient_stop_positions && gradient_solid_widths == rhs.gradient_solid_widths &&
               ratio_a == rhs.ratio_a && ratio_b == rhs.ratio_b && mix_b_percent == rhs.mix_b_percent &&
               manual_pattern == rhs.manual_pattern && gradient_component_ids == rhs.gradient_component_ids &&
               gradient_component_weights == rhs.gradient_component_weights && pointillism_all_filaments == rhs.pointillism_all_filaments &&
               distribution_mode == rhs.distribution_mode && local_z_max_sublayers == rhs.local_z_max_sublayers &&
               gradient_enabled == rhs.gradient_enabled && std::abs(gradient_start - rhs.gradient_start) <= k_gradient_epsilon &&
               std::abs(gradient_end - rhs.gradient_end) <= k_gradient_epsilon &&
               std::abs(component_a_surface_offset - rhs.component_a_surface_offset) <= k_surface_offset_epsilon &&
               std::abs(component_b_surface_offset - rhs.component_b_surface_offset) <= k_surface_offset_epsilon &&
               enabled == rhs.enabled && deleted == rhs.deleted && custom == rhs.custom && origin_auto == rhs.origin_auto &&
               ui_mode == rhs.ui_mode;
    }
    bool operator!=(const MixedFilamentLegacyRow& rhs) const { return !(*this == rhs); }
};

using MixedFilament         = MixedFilamentLegacyRow;
using MixedFilamentStableId = uint64_t;

struct MixedFilamentPhysicalRef
{
    // Runtime physical filament id. Kept 1-based to match the existing slicer pipeline.
    unsigned int id = 0;
};

struct MixedFilamentWeightedComponent
{
    MixedFilamentPhysicalRef filament;
    int                      percent = 0;
};

struct MixedFilamentPrimaryPairView
{
    MixedFilamentPhysicalRef component_a;
    MixedFilamentPhysicalRef component_b;
    int                      component_b_percent = 50;
};

struct MixedFilamentWeightedBlend
{
    // Weighted physical filament refs. A two-component blend is the ordinary
    // pair blend; 3+ components is the explicit multi-color blend path.
    // components[0] and components[1] remain the primary A/B pair for legacy-row
    // compatibility, manual-pattern tokens, Local-Z, and surface bias.
    std::vector<MixedFilamentWeightedComponent> components;

    bool is_pair() const { return components.size() == 2; }

    // Pair-shaped view for legacy adapters and slicer behavior that still
    // needs an A/B interpretation. The weighted component array remains the
    // source of truth.
    std::optional<MixedFilamentPrimaryPairView> primary_pair() const;
    MixedFilamentPrimaryPairView                primary_pair_or(unsigned int component_a         = 1,
                                                                unsigned int component_b         = 2,
                                                                int          component_b_percent = 50) const;

    // UI/package-friendly array views over the weighted component source.
    std::vector<unsigned int> component_ids(size_t num_physical = 0) const;
    std::vector<int>          component_percents(size_t num_physical = 0) const;
};

struct MixedFilamentIdentity
{
    MixedFilamentStableId stable_id = 0;
};

enum class MixedFilamentSourceKind : uint8_t { AutoGenerated, Custom };

struct MixedFilamentSource
{
    MixedFilamentSourceKind kind = MixedFilamentSourceKind::AutoGenerated;

    // True when this row belongs to an auto-generated base pair, even if it was edited.
    bool origin_auto = false;
};

struct MixedFilamentVisibility
{
    // Legacy `deleted`: remembered so regeneration does not resurrect the row.
    bool tombstoned = false;
    bool enabled    = true; // Preserve Snorca assignment availability.
};

struct MixedFilamentManualPattern
{
    // One group means a simple repeating cadence; multiple groups map to
    // perimeter/wall groups. Entries are resolved physical filament ids.
    std::vector<std::vector<MixedFilamentPhysicalRef>> groups;
};

enum class MixedFilamentRecipeKind : uint8_t { WeightedBlend, ManualPattern };

struct MixedFilamentRecipe
{
    MixedFilamentWeightedBlend blend;

    // Manual patterns keep their exact perimeter/group sequence. `blend`
    // remains populated as the aggregate weighted view for UI summaries,
    // display color, local-Z, and legacy compatibility.
    std::optional<MixedFilamentManualPattern> manual_pattern;

    MixedFilamentRecipeKind kind = MixedFilamentRecipeKind::WeightedBlend;
};

// Retained only for compatibility with in-memory legacy callers; project loading normalizes it.
enum class MixedFilamentDistributionMode : uint8_t { LayerCycle, Simple, RetiredSameLayer };

struct MixedFilamentLayerCadence
{
    int component_a_layers = 1;
    int component_b_layers = 1;
    // Legacy rows store an independent A/B percentage beside multi-component
    // weights. Preserve it while that weighted B component is unchanged; an
    // explicit typed weight edit must not inherit a stale legacy percentage.
    struct LegacyMixPercent
    {
        int value                      = 50;
        int source_component_b_percent = 50;
    };
    std::optional<LegacyMixPercent> legacy_mix_b_percent;
};

struct MixedFilamentLocalZBehavior
{
    int max_sublayers = 0;
};

struct MixedFilamentGradientBehavior
{
    bool  enabled           = false;
    float component_a_start = MixedFilamentLegacyRow::k_default_gradient_dominant;
    float component_a_end   = MixedFilamentLegacyRow::k_default_gradient_minority;
    // Normalized UI stop curve, length 2 * component_count - 1 when present.
    std::vector<float> stop_positions;
    std::vector<float> solid_widths;
    // Gradient traversal order is independent of the blend's legacy A/B projection.
    std::vector<unsigned int> color_order;
};

struct MixedFilamentSurfaceBias
{
    // Legacy A/B offsets retained for existing projects. Runtime surface bias
    // uses the canonical signed bias to select which component contracts inward.
    float component_a_offset_mm = 0.f;
    float component_b_offset_mm = 0.f;
};

struct MixedFilamentBehavior
{
    MixedFilamentDistributionMode distribution = MixedFilamentDistributionMode::Simple;
    MixedFilamentLayerCadence     layer_cadence;
    MixedFilamentLocalZBehavior   local_z;
    MixedFilamentGradientBehavior gradient;
    MixedFilamentSurfaceBias      surface_bias;
};

struct MixedFilamentPresentation
{
    std::string display_color;
    int         ui_mode = -1;
};

// Typed, decoded mixed-filament model. This is the
// preferred boundary for UI and package code; compact legacy strings stay in
// adapters and persistence.
struct MixedFilamentDefinition
{
    MixedFilamentIdentity     identity;
    MixedFilamentSource       source;
    MixedFilamentVisibility   visibility;
    MixedFilamentRecipe       recipe;
    MixedFilamentBehavior     behavior;
    MixedFilamentPresentation presentation;
};

std::pair<double, double> mixed_filament_local_z_pair_heights(double height, double minimum, int mix_b_percent);
bool                      mixed_filament_definition_uses_local_z(const MixedFilament& definition, bool global_local_z_enabled);
bool                      mixed_filament_local_z_uses_full_domain(bool global_full_domain_enabled, bool mixed_filament_assigned_to_region);
bool mixed_filament_local_z_painted_override_uses_planner(bool painted_state_is_mixed, bool painted_mixed_row_uses_local_z);
bool mixed_filament_local_z_should_subdivide_layer(size_t layer_id, bool whole_object_mode, bool preserve_first_layer);

struct MixedFilamentPreviewSettings
{
    double nominal_layer_height{0.2};
    double mixed_lower_bound{0.06};
    double mixed_upper_bound{0.16};
    double preferred_a_height{0.0};
    double preferred_b_height{0.0};
    bool   local_z_mode{false};
    bool   local_z_direct_multicolor{true};
    size_t wall_loops{1};
    bool   local_z_independent_layer_height{true};
    double gradient_cycle_height{0.20};
    double gradient_middle_window{0.03};
};

// Preview config precedence: project overrides, then print settings, then defaults.
MixedFilamentPreviewSettings mixed_filament_preview_settings(const DynamicPrintConfig& project_config,
                                                             const DynamicPrintConfig* print_config);

// Recipe preview only: independent cadence can span multiple nominal layers.
std::vector<double> mixed_filament_preview_pass_heights(const MixedFilamentPreviewSettings& settings,
                                                        int                                 mix_b_percent,
                                                        int                                 max_sublayers_limit = 0);

struct MixedFilamentDisplayContext
{
    size_t                                  num_physical{0};
    std::vector<std::string>                physical_colors;
    std::vector<double>                     nozzle_diameters;
    MixedFilamentPreviewSettings            preview_settings;
    bool                                    component_bias_enabled{false};
    std::vector<double>                     physical_tds;
    std::vector<std::string>                physical_material_ids;
    std::optional<MixedFilamentColorEngine> color_engine;
    std::vector<std::vector<bool>>          compatible_filaments;
    std::vector<double>                     max_layer_heights;
};

int  mixed_filament_effective_local_z_preview_mix_b_percent(const MixedFilament& mf, const MixedFilamentPreviewSettings& preview_settings);
bool mixed_filament_supports_bias_apparent_color(const MixedFilament&                mf,
                                                 const MixedFilamentPreviewSettings& preview_settings,
                                                 bool                                bias_mode_enabled);
std::pair<int, int> mixed_filament_apparent_pair_percentages(const MixedFilament&                mf,
                                                             const MixedFilamentPreviewSettings& preview_settings,
                                                             const std::vector<double>&          nozzle_diameters,
                                                             bool                                bias_mode_enabled);
std::string         compute_mixed_filament_display_color(const MixedFilament& entry, const MixedFilamentDisplayContext& context);
struct MixedGradientSample
{
    unsigned int component_a{0};
    unsigned int component_b{0};
    int          mix_b_percent{0};
};
// Printable gradient sample shared by the planner and layered editor preview.
struct MixedGradientLocalZSample
{
    MixedGradientSample mix;
    double              height_a{0.0};
    double              height_b{0.0};
};
MixedGradientLocalZSample sample_mixed_gradient_local_z(const MixedFilament&       entry,
                                                        size_t                     num_physical,
                                                        double                     progress,
                                                        double                     middle_window,
                                                        double                     nominal_height,
                                                        double                     minimum_height,
                                                        const std::vector<double>& max_layer_heights);
std::vector<unsigned int> mixed_gradient_components(const MixedFilament& entry, size_t num_physical);
std::vector<float>        mixed_gradient_stops(const MixedFilament& entry, size_t num_physical);
std::vector<float>        mixed_gradient_solid_half_widths(const MixedFilament& entry, size_t num_physical, double fallback = 0.03);
MixedGradientSample       sample_mixed_gradient(const MixedFilament& entry, size_t num_physical, double progress, double middle_window);
std::string               blend_mixed_components(const std::vector<unsigned int>&   ids,
                                                 const std::vector<int>&            weights,
                                                 const MixedFilamentDisplayContext& context);
std::string mixed_gradient_display_color(const MixedFilament& entry, const MixedFilamentDisplayContext& context, double progress);

std::string compute_mixed_filament_display_color(const MixedFilamentDefinition& definition, const MixedFilamentDisplayContext& context);
bool        mixed_filament_definition_uses_local_z(const MixedFilamentDefinition& definition, bool enabled);
MixedFilamentDefinition mixed_filament_definition_from_legacy_row(const MixedFilamentLegacyRow& row, size_t num_physical = 0);
void apply_mixed_filament_definition_to_legacy_row(const MixedFilamentDefinition& definition, MixedFilamentLegacyRow& row);
MixedFilamentLegacyRow                    mixed_filament_legacy_row_from_definition(const MixedFilamentDefinition& definition);
std::optional<MixedFilamentManualPattern> mixed_filament_manual_pattern_from_string(const std::string& pattern,
                                                                                    unsigned int       component_a,
                                                                                    unsigned int       component_b,
                                                                                    size_t             num_physical = 0);
std::string                               mixed_filament_manual_pattern_string(const MixedFilamentDefinition& definition);
std::vector<unsigned int> mixed_filament_manual_pattern_sequence(const MixedFilamentDefinition& definition, size_t num_physical = 0);
std::vector<unsigned int> mixed_filament_manual_pattern_preview_sequence(const MixedFilamentDefinition& definition,
                                                                         size_t                         num_physical,
                                                                         size_t                         wall_loops);
std::vector<unsigned int> mixed_filament_weighted_blend_sequence(const MixedFilamentDefinition& definition, size_t num_physical = 0);

// ---------------------------------------------------------------------------
// MixedFilamentManager
//
// Owns the list of mixed filaments and provides helpers used by the slicing
// pipeline to resolve virtual IDs back to physical extruders.
//
// Virtual filament IDs are numbered starting at (num_physical + 1).  For a
// 4-extruder printer the first mixed filament has ID 5, the second 6, etc.
// ---- Batch Match Mapping ----

/// Lightweight wx-free entry for batch-inserting matched filaments.
/// The GUI layer populates this from ColorMappingEntry, then passes
/// to MixedFilamentManager::add_batch_custom_filaments().
struct MixedFilamentBatchEntry
{
    unsigned int component_a   = 1;
    unsigned int component_b   = 2;
    int          mix_b_percent = 50;
    std::string  manual_pattern;
    std::string  gradient_component_ids;
    std::string  gradient_component_weights;
    int          distribution_mode = int(MixedFilament::Simple);
    bool         gradient_enabled  = false;
    float        gradient_start    = MixedFilament::k_default_gradient_dominant;
    float        gradient_end      = MixedFilament::k_default_gradient_minority;
    std::string  display_color; // pre-computed "#RRGGBB" hex
};

// ---------------------------------------------------------------------------
class MixedFilamentManager
{
public:
    MixedFilamentManager() = default;
    bool add_custom_filament_definition(MixedFilamentDefinition definition, const std::vector<std::string>& filament_colours);
    std::optional<MixedFilamentDefinition> mixed_filament_definition_from_id(unsigned int filament_id, size_t num_physical) const;
    std::optional<unsigned int>            filament_id_from_stable_id(MixedFilamentStableId stable_id, size_t num_physical) const;

    static void                     set_auto_generate_enabled(bool enabled);
    static bool                     auto_generate_enabled();
    static void                     set_color_engine(MixedFilamentColorEngine engine);
    static MixedFilamentColorEngine color_engine();
    static MixedFilamentColorEngine color_engine_from_string(const std::string& value);
    static const char*              color_engine_to_string(MixedFilamentColorEngine engine);
    static void                     set_use_td_for_color_prediction(bool enabled);
    static bool                     use_td_for_color_prediction();

    // ---- Auto-generation ------------------------------------------------

    // Rebuild the mixed-filament list from the current set of physical
    // filament colours.  Generates all C(N,2) pairwise combinations.
    // Previous ratio/enabled state is preserved when a combination still
    // exists.
    void auto_generate(const std::vector<std::string>& filament_colours);

    // Remove a physical filament (1-based ID) from the mixed list.
    // Any mixed filament that contains the removed component is deleted.
    // Remaining component IDs are shifted down to stay aligned with physical IDs.
    void remove_physical_filament(unsigned int deleted_filament_id);

    // Add a custom mixed filament.
    void add_custom_filament(unsigned int                    component_a,
                             unsigned int                    component_b,
                             int                             mix_b_percent,
                             const std::vector<std::string>& filament_colours);

    // Batch-insert custom mixed filaments from a match-mapping operation.
    // Each entry is fully specified except `stable_id`, which is allocated internally.
    // Entries exceeding `MAXIMUM_FILAMENT_NUMBER` are silently dropped.
    // All entries receive `ui_mode = 2` (MATCH), `custom = true`.
    // Calls `refresh_display_colors()` exactly once after all inserts.
    // If `out_assigned_ids` is non-null it is filled with one entry per input
    // `entries[i]`: the 1-based virtual filament id actually created, or 0 when
    // the entry was dropped (cap reached, or invalid components).
    void add_batch_custom_filaments(const std::vector<MixedFilamentBatchEntry>& entries,
                                    const std::vector<std::string>&             filament_colours,
                                    std::vector<unsigned int>*                  out_assigned_ids = nullptr);

    // Remove all custom rows, keep auto-generated ones.
    void clear_custom_entries();

    // Clean up deleted mixed filaments from memory.
    // This should be called after serializing to remove deleted entries that are no longer needed.
    void cleanup_deleted_entries();

    // Recompute cadence ratios from gradient settings.
    // gradient_mode: 0 = Layer cycle weighted, 1 = Height weighted.
    void apply_gradient_settings(int gradient_mode, float lower_bound, float upper_bound, bool advanced_dithering = false);

    // Persist mixed rows, including auto/deleted state, into the compact
    // project-settings string.
    std::string serialize_custom_entries();
    // Exact is for persistence: retain stored auto rows without merging newly
    // generated pairs or depending on the user's auto-generation preference.
    enum class LoadMode { MergeGenerated, Exact };
    void load_custom_entries(const std::string&              serialized,
                             const std::vector<std::string>& filament_colours,
                             LoadMode                        mode = LoadMode::MergeGenerated);

    // ---- Pattern string functions -------------------------------------------
    // Normalize a manual mixed-pattern string into canonical form.
    // Format: digits 1-9 for IDs 1-9, [N] for IDs >= 10, comma for group separator.
    // Returns empty string if invalid.
    static std::string normalize_manual_pattern(const std::string& pattern);
    static int         mix_percent_from_manual_pattern(const std::string& pattern);

    // Tokenize a single pattern group (no commas) into token strings.
    // Handles single-digit and bracket ([N]) tokens.
    static std::vector<std::string> split_pattern_group_to_tokens(const std::string& group, size_t num_physical);

    // Map a string token to a physical extruder ID.
    // "1" => component_a, "2" => component_b, "3"+ => direct physical ID.
    static unsigned int physical_filament_from_token(const std::string& token, const MixedFilament& mf, size_t num_physical);

    // Split a normalized pattern string by comma into group strings.
    static std::vector<std::string> split_pattern_groups(const std::string& pattern);

    // ---- Gradient component ID encoding / decoding ------------------------

    // Maximum number of physical filaments supported by the encoding.
    static constexpr size_t kMaxPhysicalFilaments = 64;

    // Encode filament IDs (1-based) to a compact string.
    // Legacy format (all IDs ≤ 9):  concatenated single chars, e.g. "132".
    // Extended format (any ID > 9): '/' separated decimals, e.g. "1/12/3".
    // Single-ID extended uses leading '/' to disambiguate, e.g. "/12".
    static std::string encode_gradient_component_ids(const std::vector<unsigned int>& ids);

    // Decode a gradient_component_ids string to a vector of filament IDs (1-based).
    // Handles both legacy and extended formats.  When num_physical > 0 each ID is
    // validated to be ≤ num_physical.
    static std::vector<unsigned int> decode_gradient_component_ids(const std::string& components, size_t num_physical = 0);

    // Expand virtual mixed-filament IDs in a sorted/deduplicated vector into
    // their physical component IDs (component_a, component_b, and gradient
    // component IDs).  IDs ≤ num_physical are left unchanged.  The caller is
    // responsible for re-sorting and re-deduplicating after the call.
    void expand_virtual_extruder_ids(std::vector<int>& ids, size_t num_physical) const;

    // Normalize a gradient_component_ids string to canonical form.
    // Canonical form uses legacy encoding when all IDs ≤ 9, extended otherwise.
    static std::string normalize_gradient_component_ids(const std::string& components);

    // ---- Queries --------------------------------------------------------

    // True when `filament_id` (1-based) refers to a mixed filament.
    bool is_mixed(unsigned int filament_id, size_t num_physical) const
    {
        return mixed_index_from_filament_id(filament_id, num_physical) >= 0;
    }

    // Resolve a mixed filament ID to a physical extruder (1-based) for the
    // given layer context. Returns `filament_id` unchanged when it is not a
    // mixed filament.
    unsigned int resolve(unsigned int       filament_id,
                         size_t             num_physical,
                         int                layer_index,
                         float              layer_print_z         = 0.f,
                         float              layer_height          = 0.f,
                         bool               force_height_weighted = false,
                         const PrintObject* current_object        = nullptr) const;
    unsigned int resolve_perimeter(unsigned int       filament_id,
                                   size_t             num_physical,
                                   int                layer_index,
                                   int                perimeter_index,
                                   float              layer_print_z         = 0.f,
                                   float              layer_height          = 0.f,
                                   bool               force_height_weighted = false,
                                   const PrintObject* current_object        = nullptr) const;
    // Resolve the filament ID that should own painted regions on this layer.
    // Modes that require virtual identity later in G-code generation keep the
    // original mixed ID; ordinary mixed rows collapse to the current physical
    // extruder so adjacent same-tool regions can merge.
    unsigned int              effective_painted_region_filament_id(unsigned int filament_id,
                                                                   size_t       num_physical,
                                                                   int          layer_index,
                                                                   float        layer_print_z     = 0.f,
                                                                   float        layer_height      = 0.f,
                                                                   float        layer_height_a    = 0.f,
                                                                   float        layer_height_b    = 0.f,
                                                                   float        base_layer_height = 0.2f) const;
    float                     component_surface_offset(unsigned int filament_id,
                                                       size_t       num_physical,
                                                       int          layer_index,
                                                       float        layer_print_z         = 0.f,
                                                       float        layer_height          = 0.f,
                                                       bool         force_height_weighted = false) const;
    std::vector<unsigned int> ordered_perimeter_extruders(unsigned int filament_id,
                                                          size_t       num_physical,
                                                          int          layer_index,
                                                          float        layer_print_z         = 0.f,
                                                          float        layer_height          = 0.f,
                                                          bool         force_height_weighted = false) const;

    // Map virtual filament ID (1-based, after physical IDs) to index into
    // the stored definitions. Virtual IDs enumerate enabled mixed rows only.
    int mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const;

    // Blend N colours using weighted FilamentMixer blending.
    // color_percents: vector of (hex_color, percent) where percents sum to 100.
    static std::string blend_color_multi(const std::vector<std::pair<std::string, int>>& color_percents);
    static std::string blend_color_multi(const std::vector<MixedFilamentColorInput>& color_percents);
    static std::string blend_color_multi(const std::vector<MixedFilamentColorInput>& inputs, MixedFilamentColorEngine engine);

    const MixedFilament* mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const;

    // Get all mixed filament indices that depend on a specific physical filament (1-based ID).
    // Returns indices into the stored definitions for mixes that use the physical filament
    // as a component (either component_a, component_b, or in gradient_component_ids).
    std::vector<size_t> mixed_filaments_using_physical(unsigned int physical_filament_1based) const;

    // Compute a display colour by blending two colours with FilamentMixer.
    static std::string             blend_color(const std::string& color_a, const std::string& color_b, int ratio_a, int ratio_b);
    static std::string             blend_color(const std::string&           color_a,
                                               const std::string&           color_b,
                                               int                          ratio_a,
                                               int                          ratio_b,
                                               const std::optional<double>& td_a_mm,
                                               const std::optional<double>& td_b_mm);
    static std::string             blend_color(const std::string&                color_a,
                                               const std::string&                color_b,
                                               int                               ratio_a,
                                               int                               ratio_b,
                                               const std::optional<double>&      td_a_mm,
                                               const std::optional<double>&      td_b_mm,
                                               const std::optional<std::string>& material_id_a,
                                               const std::optional<std::string>& material_id_b);
    static float                   max_component_surface_offset_mm(float reference_width_mm = 0.4f);
    static float                   max_pair_bias_mm(float reference_width_mm = 0.4f);
    static std::pair<float, float> surface_offset_pair_from_signed_bias(float bias_mm, float reference_width_mm = 0.4f);
    static float                   bias_ui_value_from_surface_offsets(float component_a_surface_offset,
                                                                      float component_b_surface_offset,
                                                                      float reference_width_mm = 0.4f);
    static int                     apparent_mix_b_percent(int   mix_b_percent,
                                                          float component_a_surface_offset,
                                                          float component_b_surface_offset,
                                                          float reference_width_mm = 0.4f);

    // Exposed for unit testing — pure logic helpers.
    static int         safe_mod(int x, int m);
    static void        normalize_ratio_pair(int& a, int& b);
    static float       canonical_signed_bias_value(float component_a_surface_offset, float component_b_surface_offset);
    static std::string format_surface_offset_token(float value);
    static double      mixed_filament_reference_nozzle_mm(unsigned int               component_a,
                                                          unsigned int               component_b,
                                                          const std::vector<double>& nozzle_diameters);

    // Build the T2(pre-delete) -> T3(post-delete) painting remap for the batch-
    // match cleanup path that marks redundant mixed rows deleted. Virtual IDs
    // are enumerated over *enabled* rows, so deleting a row renumbers every
    // higher survivor down by one; the model's painted facets still hold the
    // pre-deletion IDs and must be remapped or they resolve to the wrong row.
    //
    // Rule (per old_vid in T2 space):
    //   old_vid in deleted_t2_vids        -> 0          (NONE; the deleted row)
    //   old_vid <= num_physical           -> old_vid    (physical slots: identity)
    //   else                              -> old_vid - count(deleted vids < old_vid)
    //
    // num_physical:        physical filament count (unchanged by mixed-only deletion).
    // t2_total_filaments:  total filament count BEFORE deletion (physical + enabled mixed).
    // deleted_t2_vids:     1-based virtual IDs of the rows being deleted (any order).
    // Returns:             vector of size t2_total_filaments + 1; remap[0] is unused (0).
    //
    // Pure function: no stable_id lookup, no (a,b) pair fallback, no dependence on the
    // MixedFilament payload. The offset math is the whole computation, which is why it
    // is unit-testable without the GUI cleanup harness.
    static std::vector<unsigned int> build_mixed_deletion_painting_remap(size_t                           num_physical,
                                                                         size_t                           t2_total_filaments,
                                                                         const std::vector<unsigned int>& deleted_t2_vids);

    // ---- Accessors ------------------------------------------------------

    std::vector<MixedFilamentDefinition> mixed_filament_definitions(size_t num_physical = 0) const;
    bool                                 set_mixed_filament_definition(size_t                          index,
                                                                       const MixedFilamentDefinition&  definition,
                                                                       const std::vector<std::string>& filament_colours = {});

    void set_mixed_filament_definitions(std::vector<MixedFilamentDefinition> definitions,
                                        const std::vector<std::string>&      filament_colours = {});
    bool set_mixed_filament_legacy_row(size_t                          index,
                                       const MixedFilamentLegacyRow&   row,
                                       size_t                          num_physical     = 0,
                                       const std::vector<std::string>& filament_colours = {});
    void set_mixed_filament_legacy_rows(const std::vector<MixedFilamentLegacyRow>& rows,
                                        size_t                                     num_physical     = 0,
                                        const std::vector<std::string>&            filament_colours = {});

    // Read-only legacy-row snapshot for persistence and compatibility tests.
    // The manager owns typed definitions; this vector is rebuilt from them on demand.
    const std::vector<MixedFilamentLegacyRow>& mixed_filament_legacy_rows() const;
    // Compatibility-only mutable borrowing. Production edits use explicit setters;
    // retained for external callers and compatibility regression tests.
    std::vector<MixedFilamentLegacyRow>&       mixed_filaments();
    const std::vector<MixedFilamentLegacyRow>& mixed_filaments() const;
    size_t                                     mixed_filament_count() const;

    size_t visible_count() const;
    size_t enabled_count() const { return visible_count(); }

    // Total filament count = num_physical + number of *enabled* mixed filaments.
    size_t total_filaments(size_t num_physical) const { return num_physical + enabled_count(); }

    // Return the display colours of all enabled mixed filaments (in order).
    std::vector<std::string> display_colors() const;
    void                     set_display_context(const MixedFilamentDisplayContext& context);
    // Recompute every mixed filament's display_color from its recipe against the
    // given physical colours (also refreshes the internal display context).
    void refresh_display_colors(const std::vector<std::string>& filament_colours);

private:
    // Convert a 1-based virtual ID to a 0-based index into the stored definitions.
    size_t index_of(unsigned int filament_id, size_t num_physical) const { return static_cast<size_t>(filament_id - num_physical - 1); }

    uint64_t allocate_stable_id();
    uint64_t normalize_stable_id(uint64_t stable_id);

    void                                        invalidate_legacy_cache() const;
    void                                        rebuild_legacy_cache() const;
    void                                        sync_mutable_legacy_cache_to_definitions(size_t num_physical = 0);
    std::vector<MixedFilamentDefinition>        m_definitions;
    mutable std::vector<MixedFilamentLegacyRow> m_legacy_cache;
    mutable std::vector<MixedFilamentLegacyRow> m_legacy_synced_cache;
    mutable bool                                m_legacy_cache_dirty            = true;
    mutable bool                                m_legacy_cache_mutable_borrowed = false;
    int                                         m_gradient_mode                 = 0;
    float                                       m_height_lower_bound            = 0.04f;
    float                                       m_height_upper_bound            = 0.16f;
    bool                                        m_advanced_dithering            = false;
    uint64_t                                    m_next_stable_id                = 1;
    MixedFilamentDisplayContext                 m_display_context;
};

/// Result of computing which filaments are redundant after a batch colour match.
struct RedundantFilamentSet
{
    /// 1-based physical filament IDs that are not in the kept set, sorted descending.
    std::vector<unsigned int> redundant_physical;
    /// 1-based virtual filament IDs that are not in the kept set or whose
    /// physical components were deleted (cascade), sorted ascending (enumerated
    /// order of the mixed-filament list).
    std::vector<unsigned int> redundant_mixed;
    /// Number of physical filaments after removing all redundants.
    size_t new_num_physical = 1;
    /// Redundant mixed rows deleted because a physical component was dropped
    /// rather than because the row itself was not in the kept-mixed set.
    size_t cascade_mixed_count = 0;
};

/// Compute which physical and mixed filaments are redundant after a batch colour
/// match, given the sets the match decided to keep.
///
/// @param num_physical        Current number of physical filament presets.
/// @param kept_physical_ids   1-based physical IDs the match selected.
/// @param kept_mixed_ids      1-based virtual IDs of mixed filaments the match
///                            just created (non-pure mapping targets).
/// @param mixed_filaments     The current mixed-filament list (const ref).
///
/// @return  A RedundantFilamentSet.  redundant_mixed is in ascending
///          (enumerated) order; redundant_physical is descending.
///          The survivor floor guarantees at least one physical filament is
///          kept (filament 1 when kept_physical_ids is empty).
///
/// KNOWN DIVERGENCE: cascade detection bounds literal pattern/gradient tokens
/// by `num_physical` here, while remove_physical_filament bounds them by
/// kMaxPhysicalFilaments (64) and uses an ==deleted criterion.  The two disagree
/// only for a literal token > num_physical, which cannot exist in the live
/// mixed list: the validation perimeter is enforced at EVERY write path —
/// load_custom_entries AND add_batch_custom_filaments both call
/// references_exceed_physical to reject such rows, and add_custom_filament /
/// auto_generate never set a manual_pattern/gradient at all.  This is
/// intentional and guarded by the [!shouldfail] test tagged "m1"; weakening
/// the validation perimeter (removing the add_batch guard) makes that state
/// reachable.
RedundantFilamentSet compute_redundant_filaments(size_t                            num_physical,
                                                 const std::vector<unsigned int>&  kept_physical_ids,
                                                 const std::vector<unsigned int>&  kept_mixed_ids,
                                                 const std::vector<MixedFilament>& mixed_filaments);

// A vertical gradient can contain two to four ordered materials.
inline bool is_simple_gradient(const MixedFilament& mf)
{
    // Lightweight ID count without heap allocation.
    // Canonical form: legacy "12" = two IDs, extended "1/12/3" = three IDs.
    auto count_ids = [](const std::string& s) -> size_t {
        if (s.empty())
            return 0;
        if (s.find('/') != std::string::npos) {
            size_t n        = 0;
            bool   in_token = false;
            for (char c : s) {
                if (c == '/') {
                    if (in_token)
                        ++n;
                    in_token = false;
                } else {
                    in_token = true;
                }
            }
            if (in_token)
                ++n;
            return n;
        }
        return s.size();
    };
    return mf.gradient_enabled && mf.component_a != mf.component_b &&
           MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).empty() && count_ids(mf.gradient_component_ids) < 3;
}

inline bool is_layer_gradient(const MixedFilament& mf)
{
    return mf.gradient_enabled && mf.manual_pattern.empty() && !mf.deleted && mf.component_a != mf.component_b;
}

} // namespace Slic3r

#endif /* slic3r_MixedFilament_hpp_ */
