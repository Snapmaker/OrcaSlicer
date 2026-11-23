#include "wxExtensions.hpp"
#include "BitmapCache.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "FilamentColorUtils.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "BitmapComboBox.hpp"
#include "MixedFilamentBadge.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/dcclient.h>

#include <cmath>
#include <algorithm>

// Extruder-color icon helpers: application business (they read the plater
// configuration and the preset bundle), deliberately kept in the GUI layer -
// NOT part of the widgets static library. Declarations live in
// wxExtensions.hpp.


wxBitmap* get_default_extruder_color_icon(bool thin_icon/* = false*/)
{
    static Slic3r::GUI::BitmapCache bmp_cache;

    const double em = Slic3r::GUI::wxGetApp().em_unit();
    const int icon_width = lround((thin_icon ? 2 : 4.5) * em);
    const int icon_height = lround(2 * em);
    bool dark_mode = Slic3r::GUI::wxGetApp().dark_mode();

    wxClientDC cdc((wxWindow*)Slic3r::GUI::wxGetApp().mainframe);
    wxMemoryDC dc(&cdc);
    dc.SetFont(::Label::Body_12);

    wxString label = _L("default");
    std::string bitmap_key = std::string("default_color") + "-h" + std::to_string(icon_height) + "-w" + std::to_string(icon_width)
        + "-i" + label.ToStdString();

    wxBitmap* bitmap = bmp_cache.find(bitmap_key);
    if (bitmap == nullptr) {
        // Paint the color icon.
            //Slic3r::GUI::BitmapCache::parse_color(color, rgb);
            // there is no neede to scale created solid bitmap
        wxColor clr(255, 255, 255, 0);
        bitmap = bmp_cache.insert(bitmap_key, wxBitmap(icon_width, icon_height));
        dc.SelectObject(*bitmap);
        dc.SetBackground(wxBrush(clr));
        dc.Clear();
        dc.SetBrush(wxBrush(clr));
        dc.SetPen(*wxGREY_PEN);
        auto size = dc.GetTextExtent(wxString(label));
        dc.SetTextForeground(clr.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
        dc.DrawText(label, (icon_width - size.x) / 2, (icon_height - size.y) / 2);
        dc.SelectObject(wxNullBitmap);
    }

    return bitmap;
}

std::vector<wxBitmap*> get_extruder_color_icons(bool thin_icon/* = false*/)
{
    // Create the bitmap with color bars.
    std::vector<wxBitmap*> bmps;
    std::vector<std::string> colors = Slic3r::GUI::wxGetApp().plater()->get_extruder_colors_from_plater_config();

    if (colors.empty())
        return bmps;

    /* It's supposed that standard size of an icon is 36px*16px for 100% scaled display.
     * So set sizes for solid_colored icons used for filament preset
     * and scale them in respect to em_unit value
     */
    const double em = Slic3r::GUI::wxGetApp().em_unit();
    const int icon_width = lround((thin_icon ? 2 : 4.4) * em);
    const int icon_height = lround(2 * em);

    // Determine the split between physical and mixed filaments.
    const std::vector<std::string> physical_colors =
        Slic3r::GUI::wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false);
    const size_t num_physical = physical_colors.size();

    const Slic3r::DynamicPrintConfig* config = Slic3r::GUI::wxGetApp().preset_bundle != nullptr ?
                                               &Slic3r::GUI::wxGetApp().preset_bundle->project_config : nullptr;
    const Slic3r::ConfigOptionStrings* multiColorOption = config != nullptr && config->has("filament_multi_colors") ?
                                                config->option<Slic3r::ConfigOptionStrings>("filament_multi_colors") : nullptr;
    const Slic3r::ConfigOptionInts* modeOption = config != nullptr && config->has("filament_colour_mode") ?
                                                config->option<Slic3r::ConfigOptionInts>("filament_colour_mode") : nullptr;

    int index = 0;
    for (const std::string &color : colors)
    {
        auto label = std::to_string(++index);
        const size_t color_idx = size_t(index) - 1;

        if (color_idx < num_physical) {
            const std::string multiColors = multiColorOption != nullptr && multiColorOption->values.size() > color_idx ?
                                             multiColorOption->values[color_idx] : std::string();
            int modeValue = 0;
            if (modeOption != nullptr && modeOption->values.size() > color_idx)
                modeValue = modeOption->values[color_idx];
            const Slic3r::FilamentColorMode colorMode = Slic3r::FilamentColorModeFromConfig(modeValue);
            bmps.push_back(Slic3r::GUI::FilamentColorUtils::GetFilamentColorIcon(multiColors, colorMode, color, label,
                                                                                  icon_width, icon_height));
            continue;
        }

        if (color_idx >= num_physical) {
            // Mixed filament — try to draw a gradient icon.
            const size_t mixed_idx = color_idx - num_physical;
            const auto* pb = Slic3r::GUI::wxGetApp().preset_bundle;
            if (pb != nullptr) {
                const auto &mfs = pb->mixed_filaments.mixed_filaments();
                size_t visible = 0;
                for (const auto &mf : mfs) {
                    if (mf.deleted || !mf.enabled) continue;
                    if (visible == mixed_idx) {
                        const bool is_gradient = is_simple_gradient(mf);
                        if (is_gradient) {
                            auto get_c = [&](unsigned fid) -> wxColour {
                                if (fid == 0 || fid > physical_colors.size())
                                    return wxColour("#26A69A");
                                wxColour parsed(physical_colors[fid - 1]);
                                return parsed.IsOk() ? parsed : wxColour("#26A69A");
                            };
                            const wxColour ca = get_c(mf.component_a);
                            const wxColour cb = get_c(mf.component_b);
                            const bool a_to_b = mf.gradient_start >= mf.gradient_end;

                            Slic3r::GUI::ColorBlockParams params;
                            params.mode   = Slic3r::GUI::ColorBlockParams::Gradient;
                            params.width  = icon_width;
                            params.height = icon_height;
                            params.label  = label;
                            params.gradient_colors.push_back(a_to_b ? ca : cb);
                            params.gradient_colors.push_back(a_to_b ? cb : ca);

                            bmps.push_back(Slic3r::GUI::get_color_block_bitmap_cached(params));
                        }
                        break;
                    }
                    ++visible;
                }
                if (bmps.size() > color_idx) continue;
            }
        }

        bmps.push_back(get_extruder_color_icon(color, label, icon_width, icon_height));
    }

    return bmps;
}

wxBitmap *get_extruder_color_icon(std::string color, std::string label, int icon_width, int icon_height)
{
    // Smallest font scale the badge label is allowed to shrink to. Numeric filament numbers
    // ("10".."45") land around 0.85 and are rendered at that size; anything that would need to
    // go below this stays at the full size and is clipped as before, rather than becoming
    // unreadable (e.g. a call site that passes a tray name instead of a number).
    static const double kMinLabelShrinkRatio = 0.6;

    static Slic3r::GUI::BitmapCache bmp_cache;

    std::string bitmap_key = color + "-h" + std::to_string(icon_height) + "-w" + std::to_string(icon_width) + "-i" + label;

    wxBitmap *bitmap = bmp_cache.find(bitmap_key);
    if (bitmap == nullptr) {
        // Paint the color icon.
        // Slic3r::GUI::BitmapCache::parse_color(color, rgb);
        // there is no neede to scale created solid bitmap
        wxColor clr(color);
        bitmap = bmp_cache.insert(bitmap_key, wxBitmap(icon_width, icon_height));
#ifndef __WXMSW__
        wxMemoryDC dc;
#else
        wxClientDC cdc((wxWindow *) Slic3r::GUI::wxGetApp().mainframe);
        wxMemoryDC dc(&cdc);
#endif
        dc.SetFont(::Label::Body_12);
        dc.SelectObject(*bitmap);
        if (clr.Alpha() == 0) {
            int             size        = icon_height * 2;
            static wxBitmap transparent = *Slic3r::GUI::BitmapCache().load_svg("transparent", size, size);
            if (transparent.GetHeight() != size) transparent = *Slic3r::GUI::BitmapCache().load_svg("transparent", size, size);
            wxPoint pt(0, 0);
            while (pt.x < icon_width) {
                dc.DrawBitmap(transparent, pt);
                pt.x += size;
            }
            clr.SetRGB(0xffffff); // for text color
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
        } else {
            dc.SetBackground(wxBrush(clr));
            dc.Clear();
            dc.SetBrush(wxBrush(clr));
        }
        if (clr.Red() > 224 && clr.Blue() > 224 && clr.Green() > 224) {
            dc.SetPen(*wxGREY_PEN);
            dc.DrawRectangle(0, 0, icon_width, icon_height);
        }
        // Fit the label inside the badge. The badge has a fixed DIP size while the label grows
        // with the filament count ("1" .. "45"): a two digit label used to be drawn centred and
        // clipped on both sides, which showed up as a truncated "2<garbage>" from filament 21 on.
        //
        // The font is sized once from the widest label the badge must hold - measured from both
        // "00" and the label being drawn, so that "1" and "45" share one font size while a wider
        // label (three digits) still shrinks to fit instead of being clipped.
        const int max_label_width = std::max(icon_width - 2, 1);
        wxFont    label_font      = dc.GetFont();
        const double base_size    = label_font.GetFractionalPointSize();
        if (base_size > 0) {
            const int two_digit_width = dc.GetTextExtent("00").x;
            const int label_width     = dc.GetTextExtent(wxString(label)).x;
            const int widest_width    = std::max(two_digit_width, label_width);
            if (widest_width > max_label_width) {
                // Only shrink while the result stays readable. Numeric filament numbers need a
                // few percent, while a call site passing a tray name (PresetComboBoxes) would be
                // squeezed far below legibility - those keep the previous behaviour (full size,
                // possibly clipped) instead of turning into unreadable specks.
                const double ratio = double(max_label_width) / double(widest_width);
                if (ratio >= kMinLabelShrinkRatio) {
                    label_font.SetFractionalPointSize(std::max(base_size * ratio, 1.0));
                    dc.SetFont(label_font);
                }
            }
        }
        const wxString label_text(label);
        const wxSize   size = dc.GetTextExtent(label_text);
        dc.SetTextForeground(clr.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
        dc.DrawText(label_text, (icon_width - size.x) / 2, (icon_height - size.y) / 2);
        dc.SelectObject(wxNullBitmap);
    }
    return bitmap;
}

void apply_extruder_selector(Slic3r::GUI::BitmapComboBox** ctrl,
                             wxWindow* parent,
                             const std::string& first_item/* = ""*/,
                             wxPoint pos/* = wxDefaultPosition*/,
                             wxSize size/* = wxDefaultSize*/,
                             bool use_thin_icon/* = false*/)
{
    std::vector<wxBitmap*> icons = get_extruder_color_icons(use_thin_icon);

    if (!*ctrl) {
        *ctrl = new Slic3r::GUI::BitmapComboBox(parent, wxID_ANY, wxEmptyString, pos, size, 0, nullptr, wxCB_READONLY);
        Slic3r::GUI::wxGetApp().UpdateDarkUI(*ctrl);
    }
    else
    {
        (*ctrl)->SetPosition(pos);
        (*ctrl)->SetMinSize(size);
        (*ctrl)->SetSize(size);
        (*ctrl)->Clear();
    }
    if (first_item.empty())
        (*ctrl)->Hide();    // to avoid unwanted rendering before layout (ExtruderSequenceDialog)

    if (icons.empty() && !first_item.empty()) {
        (*ctrl)->Append(_(first_item), wxNullBitmap);
        return;
    }

    // For ObjectList we use short extruder name (just a number)
    const bool use_full_item_name = dynamic_cast<Slic3r::GUI::ObjectList*>(parent) == nullptr;

    int i = 0;
    wxString str = _(L("Extruder"));
    for (wxBitmap* bmp : icons) {
        if (i == 0) {
            if (!first_item.empty())
                (*ctrl)->Append(_(first_item), *bmp);
            ++i;
        }

        (*ctrl)->Append(use_full_item_name
                        ? Slic3r::GUI::from_u8((boost::format("%1% %2%") % str % i).str())
                        : wxString::Format("%d", i), *bmp);
        ++i;
    }
    (*ctrl)->SetSelection(0);
}
