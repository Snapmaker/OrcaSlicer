#pragma once

#include <vector>

namespace Slic3r {
namespace GUI {
namespace app {

enum class FilamentTempMixingState
{
    Compatible,
    AllowedWarning,
    BlockedError
};

// Per-plate breakdown of used filaments that participate in a high/low
// temperature mixing conflict. Slot numbers are 1-based to match what the
// user sees in the UI. Vectors are sorted ascending and de-duplicated.
struct FilamentTempMixingDetail {
    std::vector<int> high_temp_slots_1based;
    std::vector<int> low_temp_slots_1based;
};

// One plate's mixing detail, paired with its 1-based plate number.
// Used to build the Slice All notification body.
struct PlateMixingInfo {
    int                     plate_index_1based;
    FilamentTempMixingDetail detail;
};

// Per-plate breakdown of filaments whose flow ratio is 0 and would produce
// zero extrusion. Slot numbers are 1-based to match the UI display.
struct FlowRatioZeroDetail {
    std::vector<int> offender_slots_1based;
};

/// \brief Cold plate (CSP) per-plate compatibility state.
enum class ColdPlateCompatState
{
    /// \brief All used filaments compatible and no TPU in use.
    Compatible,
    /// \brief All used filaments compatible but TPU in use; non-blocking warning.
    SeriousWarning,
    /// \brief At least one used filament has zero cold-plate bed temperature; blocks slicing.
    BlockedError
};

/// \brief Aggregate per-plate cold-plate compatibility result, computed in one pass.
/// \details Combines state with the data that produced it (unsupported filament slots,
///          TPU slots) so callers can render notifications without re-querying.
///          Slot lists are 1-based to match filament_display_label / FlowRatioZeroDetail.
struct ColdPlateCompatResult
{
    ColdPlateCompatState   state = ColdPlateCompatState::Compatible;
    std::vector<int>       unsupported_slots_1_based{};
    std::vector<int>       tpu_slots_1_based{};
    bool                   uses_tpu = false;
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
