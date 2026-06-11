#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <wx/colour.h>

namespace Slic3r
{
namespace GUI
{

// ============================================================
// Luminance helpers — determine text colour for a background
// ============================================================
constexpr double g_lumR = 0.299;
constexpr double g_lumG = 0.587;
constexpr double g_lumB = 0.114;
constexpr int    g_luminanceThreshold = 140;

inline bool isDarkColour(const wxColour& c)
{
    return (c.Red() * g_lumR + c.Green() * g_lumG + c.Blue() * g_lumB) < g_luminanceThreshold;
}

inline wxColour getTextColour(const wxColour& bg)
{
    return isDarkColour(bg) ? *wxWHITE : *wxBLACK;
}

struct FilamentData
{
    // 0-based index from the filament list; used for matching with machine filaments
    unsigned int m_index   = 0;
    std::string  m_name;
    std::string  m_type;
    uint8_t      m_color_r = 0;
    uint8_t      m_color_g = 0;
    uint8_t      m_color_b = 0;
};

using FilamentInfoCallback = std::function<void(const FilamentData& data)>;

} // namespace GUI
} // namespace Slic3r
