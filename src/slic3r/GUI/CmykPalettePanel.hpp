#pragma once

// "CMYK Palette" sidebar section + preview dialog for the "Convert to CMYK" feature.
//
// The palette (the filaments the user physically owns, e.g. C/M/Y/K/W) is stored in the
// project as the `color_conversion_palette` strings option. "Convert to CMYK" then replaces
// every model colour by the best mix of palette filaments (see Sidebar::convert_to_cmyk_palette).

#include "libslic3r/ColorConversionPalette.hpp"

#include <wx/dialog.h>
#include <wx/panel.h>

#include <functional>
#include <string>
#include <vector>

class Button;

namespace Slic3r {
class PresetBundle;
namespace GUI {

// Palette stored in the project, or the default C/M/Y/W palette when none was stored yet.
// Default entries have their preset name resolved to a selectable preset (may be empty).
std::vector<ColorConversionPaletteEntry> get_color_conversion_palette(PresetBundle& preset_bundle);
void                                     set_color_conversion_palette(PresetBundle& preset_bundle,
                                                                      const std::vector<ColorConversionPaletteEntry>& entries);

// Editable list of palette filaments: swatch (click to change colour), hex, filament preset
// and a remove button, plus "Add filament". Every edit is written to the project config.
class CmykPalettePanel : public wxPanel
{
public:
    // on_layout_changed is invoked after rows were added/removed so the owner can re-layout.
    CmykPalettePanel(wxWindow* parent, std::function<void()> on_layout_changed);

    // Re-read the palette from the project config and rebuild the rows.
    void reload();

private:
    void rebuild_rows();
    void commit();
    void on_add();
    void on_remove(size_t index);
    void on_pick_color(size_t index);
    void on_preset_selected(size_t index, int selection);

    std::vector<ColorConversionPaletteEntry> m_entries;
    std::vector<std::string>                 m_preset_names; // selectable filament presets, choice item i+1
    wxSizer*                                 m_rows       = nullptr;
    wxWindow*                                m_add_button = nullptr;
    std::function<void()>                    m_on_layout_changed;
};

// One line of the conversion preview.
struct CmykPreviewRow
{
    std::string source_hex;   // original model colour
    std::string matched_hex;  // colour the mix is expected to give
    std::string recipe_text;  // e.g. "1 40% + 2 30% + 4 30%" (palette positions)
    double      delta_e = 0.; // CIEDE2000 between source and matched colour
};

// Shows what "Convert to CMYK" is about to do and asks for confirmation.
class CmykConversionPreviewDialog : public wxDialog
{
public:
    CmykConversionPreviewDialog(wxWindow*                                       parent,
                                const std::vector<ColorConversionPaletteEntry>& palette,
                                const std::vector<CmykPreviewRow>&              rows,
                                size_t                                          current_filament_count,
                                size_t                                          mixed_to_create);
};

}} // namespace Slic3r::GUI
