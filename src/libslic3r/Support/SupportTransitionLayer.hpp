#pragma once

#include "../PrintConfig.hpp"

namespace Slic3r {

/// @brief Configuration for support transition layers.
/// Encapsulates all parameters controlling the transition zone
/// between support interface and support body.
struct SupportTransitionConfig
{
    int   layer_count   = 2;      ///< 0 = disabled
    bool  use_perimeter = true;   ///< Generate perimeter before infill
    float speed         = 30.0f;  ///< mm/s, independent transition speed
    float flow_ratio    = 0.85f;  ///< Relative to normal support flow

    /// @brief Create from PrintObjectConfig
    static SupportTransitionConfig from_config(const PrintObjectConfig& cfg)
    {
        SupportTransitionConfig c;
        c.layer_count   = cfg.tree_support_transition_layers.value;
        c.use_perimeter = cfg.support_transition_perimeter.value;
        c.speed         = cfg.support_transition_speed.value;
        c.flow_ratio    = cfg.support_transition_flow_ratio.get_abs_value(1.0f);
        return c;
    }

    /// @brief Check if transition layers are enabled
    bool enabled() const { return layer_count > 0; }
};

} // namespace Slic3r
