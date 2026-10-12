#ifndef slic3r_GUI_AO_AOConfig_hpp_
#define slic3r_GUI_AO_AOConfig_hpp_

// CMake supplies this value to GUI consumers. Standalone consumers default to
// production behavior, with diagnostic launch overrides disabled.
#ifndef ORCA_RENDER_DIAGNOSTICS
#define ORCA_RENDER_DIAGNOSTICS 0
#endif

#include <string>

namespace Slic3r { namespace GUI { namespace AO {

// Product-level parameters. Formed once per frame from AppConfig/UI state by the canvas adapter.
struct AOSettings
{
    enum class Quality { Off, Low, Medium, High };
    static constexpr float DEFAULT_STRENGTH = 0.5f;

    Quality quality{Quality::Off};
    float   radius{2.5f}, thickness{1.0f}, intensity{1.0f};
    // Blend strength is independent of the AO exponent and sampling quality.
    float strength{DEFAULT_STRENGTH};
    // 0: normal composite, 1: depth, 2: normals, 3: AO, 4: composite confidence.
    int debug_view{0};
};

// Resolves a stored AppConfig string ("off"/"auto"/"low"/"medium"/"high") to a quality level.
// Software rasterizers always disable AO regardless of the stored value.
AOSettings::Quality resolve_quality(const std::string& value, const std::string& renderer);
// Parses a strength override; invalid or trailing input keeps the fixed product default.
float resolve_strength(const std::string& value);

// One-time parsed launch options. All ORCA_AO_* environment variables are interpreted here and
// nowhere else; shader registration and per-frame requests consume the same immutable snapshot.
struct AOStartupOptions
{
    enum class Backend { Auto, Fragment, Compute };
    enum class Composite { Pixel, Sample, Reference, Confidence, Split };

    Backend   backend{Backend::Auto};
    Composite composite{Composite::Pixel};
    // High CS sampling: legacy keeps the original 3-slice baseline, unset uses 6.
    bool cs_sampling_legacy{false};
    // 0 keeps the quality default (2 or 3 passes); 1..3 overrides.
    int  cs_denoise_passes{0};
    bool cs_low_legacy{false};
    bool fs_fast_reconstruction{false};
    bool fs_geometry_filter{false};
    int  fs_denoise_passes{0}; // 0 keeps the preset default; 2 or 3 overrides.
    bool fs_directions4_sampling{false};
    bool disable_sample_composite{false};
    bool capture_samples{false};

    // Diagnostic entry points; empty when not opted in.
    std::string comparison_dir;
    std::string comparison_mode; // "" | backends | cslow | qualities | fseval | pixelfs | pixel | split | confidence | composite |
                                 // fssampling | fsdenoise | fsfilter
    std::string capture_dir;
};

// Diagnostic builds parse the environment once; production builds retain defaults.
// Repeated calls return the same snapshot.
const AOStartupOptions& startup_options();

}}} // namespace Slic3r::GUI::AO
#endif
