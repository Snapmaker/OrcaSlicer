#include "AOConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <sstream>

namespace Slic3r { namespace GUI { namespace AO {

AOSettings::Quality resolve_quality(const std::string& value, const std::string& renderer)
{
    std::string name = renderer;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* software : {"llvmpipe", "softpipe", "swiftshader", "gdi generic", "software"})
        if (name.find(software) != std::string::npos)
            return AOSettings::Quality::Off;
    if (value == "low")
        return AOSettings::Quality::Low;
    if (value == "medium" || value == "auto")
        return AOSettings::Quality::Medium;
    if (value == "high")
        return AOSettings::Quality::High;
    return AOSettings::Quality::Off;
}

float resolve_strength(const std::string& value)
{
    // AppConfig uses decimal points regardless of the UI/system locale.
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    double strength = 0.0;
    if (!(input >> strength) || !std::isfinite(strength))
        return AOSettings{}.strength;
    input >> std::ws;
    if (!input.eof())
        return AOSettings{}.strength;
    return static_cast<float>(std::clamp(strength, 0.0, 1.0));
}

#if ORCA_RENDER_DIAGNOSTICS
namespace {
const char* env_value(const char* key)
{
    const char* value = std::getenv(key);
    return value && *value ? value : nullptr;
}
bool env_equals(const char* key, const char* expected)
{
    const char* value = env_value(key);
    return value && std::strcmp(value, expected) == 0;
}
} // namespace
#endif

const AOStartupOptions& startup_options()
{
    static const AOStartupOptions options = [] {
        AOStartupOptions parsed;
#if ORCA_RENDER_DIAGNOSTICS
        if (const char* value = env_value("ORCA_AO_BACKEND")) {
            // Unknown diagnostic requests must never silently enable an experimental backend.
            if (std::strcmp(value, "cs") == 0)
                parsed.backend = AOStartupOptions::Backend::Compute;
            else if (std::strcmp(value, "auto") != 0)
                parsed.backend = AOStartupOptions::Backend::Fragment;
        }
        if (const char* value = env_value("ORCA_AO_COMPOSITE")) {
            if (std::strcmp(value, "pixel") == 0)
                parsed.composite = AOStartupOptions::Composite::Pixel;
            else if (std::strcmp(value, "reference") == 0)
                parsed.composite = AOStartupOptions::Composite::Reference;
            else if (std::strcmp(value, "confidence") == 0)
                parsed.composite = AOStartupOptions::Composite::Confidence;
            else if (std::strcmp(value, "split") == 0)
                parsed.composite = AOStartupOptions::Composite::Split;
            else
                parsed.composite = AOStartupOptions::Composite::Sample;
        }
        if (const char* value = env_value("ORCA_AO_CS_SAMPLING"))
            parsed.cs_sampling_legacy = std::strcmp(value, "default") == 0 || std::strcmp(value, "legacy") == 0;
        if (const char* value = env_value("ORCA_AO_CS_DENOISE"))
            if (value[0] >= '1' && value[0] <= '3' && value[1] == '\0')
                parsed.cs_denoise_passes = value[0] - '0';
        parsed.cs_low_legacy          = env_equals("ORCA_AO_CS_LOW", "legacy");
        parsed.fs_fast_reconstruction = env_equals("ORCA_AO_FS_RECONSTRUCTION", "fast");
        parsed.fs_geometry_filter     = env_equals("ORCA_AO_FS_FILTER", "geometry");
        if (env_equals("ORCA_AO_FS_DENOISE", "2"))
            parsed.fs_denoise_passes = 2;
        else if (env_equals("ORCA_AO_FS_DENOISE", "3"))
            parsed.fs_denoise_passes = 3;
        parsed.fs_directions4_sampling  = env_equals("ORCA_AO_FS_SAMPLING", "directions4");
        parsed.disable_sample_composite = env_equals("ORCA_AO_DISABLE_SAMPLE_COMPOSITE", "1");
        parsed.capture_samples          = env_equals("ORCA_AO_CAPTURE_SAMPLES", "1");
        if (const char* value = env_value("ORCA_AO_COMPARISON_DIR"))
            parsed.comparison_dir = value;
        if (const char* value = env_value("ORCA_AO_COMPARISON_MODE"))
            parsed.comparison_mode = value;
        if (const char* value = env_value("ORCA_AO_CAPTURE_DIR"))
            parsed.capture_dir = value;
#endif
        return parsed;
    }();
    return options;
}

}}} // namespace Slic3r::GUI::AO
