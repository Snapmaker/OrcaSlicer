#include "MixedFilamentBatchDialog.hpp"
#include "MixedFilamentBadge.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "Widgets/ComboBox.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "BitmapCache.hpp"

#include <unordered_map>

#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MixedFilamentBatchDialog::MixedFilamentBatchDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("混色匹配"),
                wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    SetSize(FromDIP(760), FromDIP(680));
    SetMinClientSize(wxSize(FromDIP(720), FromDIP(560)));
    CentreOnScreen();

    if (wxGetApp().preset_bundle) {
        ConfigOptionStrings* co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        if (co) m_physical_colors = co->values;
    }
    if (wxGetApp().plater()) {
        m_tray_count = wxGetApp().plater()->get_partplate_list().get_plate_count();
        if (m_tray_count < 1) m_tray_count = 1;
    }
    load_model_colors();
    build_ui();
    update_prompt_text();
    set_match_buttons_state(false);
    m_btn_start_match->Enable(!m_model_colors.empty() && m_physical_colors.size() >= 2);

    // Default to recommended mode — show CMYK card
    if (m_recommended_card) {
        m_recommended_card->Show(true);
        m_recommended_card->Layout();
    }

    // Generate thumbnails for plates that don't have slicing data yet
    wxWeakRef<wxWindow> weak_self(this);
    CallAfter([weak_self]() {
        if (!weak_self) return;
        auto* self = static_cast<MixedFilamentBatchDialog*>(weak_self.get());
        // Force thumbnail generation for all plates using the existing 3D canvas
        // (works without slicing — uses offscreen FBO rendering)
        wxGetApp().plater()->update_all_plate_thumbnails(true);
        self->build_preview_panels();
    });
}

MixedFilamentBatchDialog::~MixedFilamentBatchDialog()
{
    m_cancel_requested->store(true);
    if (m_worker_thread.joinable())
        m_worker_thread.join();
    m_destroyed->store(true);
}

void MixedFilamentBatchDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    SetSize(FromDIP(760), FromDIP(680));
    Layout();
    Refresh();
}

// Convert ThumbnailData (RGBA pixels, OpenGL FBO = bottom-up) to wxBitmap (top-down)
// Blends alpha against the panel background (#F5F5F5)
static wxBitmap thumbnail_to_bitmap(const ThumbnailData& data, int max_w, int max_h)
{
    if (!data.is_valid() || data.width == 0 || data.height == 0)
        return wxNullBitmap;

    wxImage img(data.width, data.height, false);
    img.InitAlpha();
    unsigned char* d  = img.GetData();
    unsigned char* a  = img.GetAlpha();
    if (!d || !a) return wxNullBitmap;

    // OpenGL FBO is bottom-up; wxImage is top-down — flip Y
    for (unsigned int y = 0; y < data.height; ++y) {
        unsigned int src_y = data.height - 1 - y;
        for (unsigned int x = 0; x < data.width; ++x) {
            size_t si = ((size_t)src_y * data.width + x) * 4;
            size_t di = ((size_t)y       * data.width + x) * 3;
            unsigned char r   = data.pixels[si + 0];
            unsigned char g   = data.pixels[si + 1];
            unsigned char b   = data.pixels[si + 2];
            unsigned char alpha = data.pixels[si + 3];
            // Blend with panel background (#F5F5F5) for low-alpha pixels
            if (alpha < 255) {
                float t = alpha / 255.0f;
                r = (unsigned char)(r * t + 245 * (1.0f - t));
                g = (unsigned char)(g * t + 245 * (1.0f - t));
                b = (unsigned char)(b * t + 245 * (1.0f - t));
            }
            d[di + 0] = r;
            d[di + 1] = g;
            d[di + 2] = b;
            a[(size_t)y * data.width + x] = 255;
        }
    }

    double scale = std::min(double(max_w) / data.width, double(max_h) / data.height);
    int w = std::max(1, int(data.width * scale));
    int h = std::max(1, int(data.height * scale));

    return wxBitmap(img.Scale(w, h, wxIMAGE_QUALITY_HIGH));
}

void MixedFilamentBatchDialog::build_preview_panels()
{
    auto* plater = wxGetApp().plater();
    if (!plater) return;

    auto& plates = plater->get_partplate_list();
    const int count = plates.get_plate_count();
    if (count < 1) return;

    // Panel sizes from layout (set in build_ui)
    const int orig_w = std::max(FromDIP(100), m_preview_orig_panel->GetClientSize().GetWidth());
    const int orig_h = std::max(FromDIP(100), m_preview_orig_panel->GetClientSize().GetHeight());
    const int match_w = std::max(FromDIP(160), m_preview_match_panel->GetClientSize().GetWidth());
    const int match_h = std::max(FromDIP(120), m_preview_match_panel->GetClientSize().GetHeight());

    // Create wxStaticBitmaps once
    {
        m_preview_orig_bitmap = new wxStaticBitmap(m_preview_orig_panel, wxID_ANY, wxNullBitmap);
        auto* s = new wxBoxSizer(wxVERTICAL);
        s->AddStretchSpacer();
        s->Add(m_preview_orig_bitmap, 0, wxALIGN_CENTER);
        s->AddStretchSpacer();
        m_preview_orig_panel->SetSizer(s);
    }
    {
        m_preview_match_bitmap = new wxStaticBitmap(m_preview_match_panel, wxID_ANY, wxNullBitmap);
        auto* s = new wxBoxSizer(wxVERTICAL);
        s->AddStretchSpacer();
        s->Add(m_preview_match_bitmap, 0, wxALIGN_CENTER);
        s->AddStretchSpacer();
        m_preview_match_panel->SetSizer(s);
    }

    // Pre-render all plate thumbnails into cache (orig + match start the same)
    m_thumb_cache.resize(count);
    m_match_cache.resize(count);
    for (int i = 0; i < count; ++i) {
        const PartPlate* plate = plates.get_plate(i);
        if (!plate) continue;
        m_thumb_cache[i] = thumbnail_to_bitmap(plate->thumbnail_data, orig_w, orig_h);
        m_match_cache[i] = m_thumb_cache[i]; // initially same as orig
    }

    // Adjust match preview to use its own panel dimensions
    // (orig thumbnail is shared cache, displayed at match panel size via wxStaticBitmap stretch)
    refresh_previews();
}

void MixedFilamentBatchDialog::refresh_previews()
{
    const int idx = m_tray_index - 1;
    if (idx < 0 || idx >= (int)m_thumb_cache.size()) return;

    const int orig_w = std::max(FromDIP(100), m_preview_orig_panel->GetClientSize().GetWidth());
    const int orig_h = std::max(FromDIP(100), m_preview_orig_panel->GetClientSize().GetHeight());
    const int match_w = std::max(FromDIP(160), m_preview_match_panel->GetClientSize().GetWidth());
    const int match_h = std::max(FromDIP(120), m_preview_match_panel->GetClientSize().GetHeight());

    if (m_preview_orig_bitmap && idx < (int)m_thumb_cache.size() && m_thumb_cache[idx].IsOk()) {
        m_preview_orig_bitmap->SetBitmap(m_thumb_cache[idx]);
        m_preview_orig_bitmap->SetSize(orig_w, orig_h);
        m_preview_orig_panel->Layout();
    }

    // Lazy-render match thumbnail for current plate on demand
    if (m_match_completed && !m_match_colors.empty() &&
        idx < (int)m_match_cache.size() && !m_match_cache[idx].IsOk()) {
        render_match_thumb_for_plate(idx);
    }

    if (m_preview_match_bitmap && idx < (int)m_match_cache.size() && m_match_cache[idx].IsOk()) {
        m_preview_match_bitmap->SetBitmap(m_match_cache[idx]);
        m_preview_match_bitmap->SetSize(match_w, match_h);
        m_preview_match_panel->Layout();
    }
}

void MixedFilamentBatchDialog::rebuild_match_thumb_cache()
{
    if (!m_match_completed || m_result.mappings.empty()) return;

    auto* plater = wxGetApp().plater();
    if (!plater) return;

    // Build ColorRGBA vector from current filament_colour config (once, reused by lazy renders).
    // Physical filaments first, then virtual mixed-filament display colors appended
    // to match the indexing used by update_colors_by_extruder() in the normal 3D view.
    m_match_colors.clear();
    {
        auto* pb = wxGetApp().preset_bundle;
        if (pb) {
            auto* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
            if (co) {
                for (const std::string& hex : co->values) {
                    unsigned char rgba[4] = {};
                    BitmapCache::parse_color4(hex, rgba);
                    m_match_colors.push_back({
                        float(rgba[0]) / 255.f,
                        float(rgba[1]) / 255.f,
                        float(rgba[2]) / 255.f,
                        float(rgba[3]) / 255.f,
                    });
                }
            }
            // Append virtual mixed-filament display colors so that volumes
            // with extruder_ids pointing to mixed filaments resolve correctly.
            for (const std::string& dc : pb->mixed_filaments.display_colors()) {
                if (dc.empty()) continue;
                unsigned char rgba[4] = {};
                BitmapCache::parse_color4(dc, rgba);
                m_match_colors.push_back({
                    float(rgba[0]) / 255.f,
                    float(rgba[1]) / 255.f,
                    float(rgba[2]) / 255.f,
                    float(rgba[3]) / 255.f,
                });
            }
        }
    }
    // Apply match mappings: find which original extruder slot each
    // source_color belongs to, and replace it with the matched_color.
    // This keeps the array indexed by the ORIGINAL extruder ID, which is
    // what the render pipeline reads (extruder_colors[extruder_id-1]).
    for (const auto& mapping : m_result.mappings) {
        wxColour src   = mapping.source_color;
        bool     found = false;
        for (size_t i = 0; i < m_match_colors.size(); ++i) {
            wxColour slot_color(
                static_cast<unsigned char>(m_match_colors[i].r() * 255.f + 0.5f),
                static_cast<unsigned char>(m_match_colors[i].g() * 255.f + 0.5f),
                static_cast<unsigned char>(m_match_colors[i].b() * 255.f + 0.5f));
            if (slot_color == src) {
                m_match_colors[i] = {
                    float(mapping.matched_color.Red())   / 255.f,
                    float(mapping.matched_color.Green()) / 255.f,
                    float(mapping.matched_color.Blue())  / 255.f,
                    1.0f,
                };
                found = true;
                break;
            }
        }
        // If source_color is not in any physical slot (virtual extruder /
        // mixed filament from a prior match), add a new slot.
        if (!found) {
            size_t slot = (mapping.target_filament_id > 0)
                ? static_cast<size_t>(mapping.target_filament_id - 1)
                : m_match_colors.size();
            if (slot >= m_match_colors.size())
                m_match_colors.resize(slot + 1, {0.5f, 0.5f, 0.5f, 1.0f});
            m_match_colors[slot] = {
                float(mapping.matched_color.Red())   / 255.f,
                float(mapping.matched_color.Green()) / 255.f,
                float(mapping.matched_color.Blue())  / 255.f,
                1.0f,
            };
        }
    }

    // Invalidate all cached match bitmaps; only current plate is rendered now
    const int count = plater->get_partplate_list().get_plate_count();
    m_match_cache.clear();
    m_match_cache.resize(count);

    // Immediately render the currently displayed plate
    render_match_thumb_for_plate(m_tray_index - 1);
}

void MixedFilamentBatchDialog::render_match_thumb_for_plate(int plate_idx)
{
    if (plate_idx < 0 || plate_idx >= (int)m_match_cache.size()) return;
    if (m_match_colors.empty()) return;

    auto* plater = wxGetApp().plater();
    if (!plater) return;
    auto* canvas = plater->canvas3D();
    if (!canvas) return;

    const int match_w = std::max(FromDIP(160), m_preview_match_panel->GetClientSize().GetWidth());
    const int match_h = std::max(FromDIP(120), m_preview_match_panel->GetClientSize().GetHeight());

    // Save original volume colors, apply matched colors by extruder_id
    const auto& volumes = canvas->get_volumes();
    std::vector<ColorRGBA> saved_colors;
    saved_colors.reserve(volumes.volumes.size());
    for (GLVolume* vol : volumes.volumes) {
        saved_colors.push_back(vol->color);
        if (vol->extruder_id > 0 && static_cast<size_t>(vol->extruder_id - 1) < m_match_colors.size())
            vol->color = m_match_colors[vol->extruder_id - 1];
    }

    // RAII guard: restore original volume colors on scope exit (normal + exception)
    struct ColorRestoreGuard {
        const GLVolumeCollection& vols;
        std::vector<ColorRGBA>    saved;
        ~ColorRestoreGuard() {
            for (size_t i = 0; i < saved.size() && i < vols.volumes.size(); ++i)
                vols.volumes[i]->color = saved[i];
        }
    } guard{volumes, std::move(saved_colors)};

    ThumbnailsParams tp = { {}, false, true, true, true, plate_idx };
    ThumbnailData td;
    canvas->render_thumbnail(td,
        static_cast<unsigned int>(match_w), static_cast<unsigned int>(match_h),
        tp, canvas->get_volumes(), m_match_colors,
        Camera::EType::Ortho, false, false, false);
    m_match_cache[plate_idx] = thumbnail_to_bitmap(td, match_w, match_h);
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::load_model_colors()
{
    m_model_colors.clear();

    auto* plater = wxGetApp().plater();
    if (!plater) return;

    const auto all_colors = plater->get_extruder_colors_from_plater_config(nullptr, true);
    const size_t num_physical = m_physical_colors.size();

    for (size_t i = 0; i < all_colors.size(); ++i) {
        const std::string& hex = all_colors[i];
        if (hex.empty()) continue;
        wxColour c;
        if (!try_parse_color_match_hex(hex, c)) continue;

        const bool is_physical = (i < num_physical);
        m_model_colors.push_back({
            static_cast<unsigned int>(m_model_colors.size() + 1),
            c, hex,
            is_physical
                ? std::vector<unsigned int>{static_cast<unsigned int>(i + 1)}
                : std::vector<unsigned int>{}
        });
    }
}

void MixedFilamentBatchDialog::update_recommended_card()
{
    if (!m_recommended_card) return;
    const auto& colors = m_result.recommended_physical_colors;
    if (colors.size() < 4) return;

    // Color name lookup — maps from hex in CMYG palette back to a
    // human-readable label.  Covers the 4 canonical colors.
    static const std::unordered_map<std::string, wxString> kColorName = {
        {"#00FFFF", _L("Cyan")},
        {"#FF00FF", _L("Magenta")},
        {"#FFFF00", _L("Yellow")},
        {"#00FF00", _L("Green")},
    };

    for (int i = 0; i < 4; ++i) {
        // Update swatch bitmap
        if (m_recommended_swatches[i]) {
            wxBitmap* icon = get_extruder_color_icon(
                colors[i], std::to_string(i + 1),
                FromDIP(20), FromDIP(20));
            if (icon) {
                m_recommended_swatches[i]->SetBitmap(*icon);
                delete icon;
            }
        }

        // Update label text
        if (m_recommended_labels[i]) {
            auto it = kColorName.find(colors[i]);
            m_recommended_labels[i]->SetLabel(
                it != kColorName.end()
                    ? it->second
                    : wxString::Format("F%d", int(i + 1)));
        }
    }

    m_recommended_card->Layout();
}

// ---------------------------------------------------------------------------
// UI — strictly matches prototype-mockup.html
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::build_ui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    auto* root = new wxBoxSizer(wxVERTICAL);
    const int M  = FromDIP(8);
    const int M2 = FromDIP(4);

    // ---- Error panel ----
    {
        m_error_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_error_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FDE8E8")));
        m_error_panel->Hide();
        auto* s = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap bmp(m_error_panel, "error_icon_red_exclamation", 14);
        s->Add(new wxStaticBitmap(m_error_panel, wxID_ANY, bmp.bmp()), 0, wxALL, FromDIP(8));
        m_error_text = new Label(m_error_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_error_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D32F2F")));
        s->Add(m_error_text, 1, wxALL, FromDIP(8));
        m_error_panel->SetSizer(s);
        root->Add(m_error_panel, 0, wxEXPAND);
    }
    // ---- Warning panel ----
    {
        m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFF3EB")));
        m_warning_panel->Hide();
        auto* s = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap bmp(m_warning_panel, "icon_warning_triangle", 14);
        s->Add(new wxStaticBitmap(m_warning_panel, wxID_ANY, bmp.bmp()), 0, wxALL, FromDIP(8));
        m_warning_text = new Label(m_warning_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF842D")));
        s->Add(m_warning_text, 1, wxALL, FromDIP(8));
        m_warning_panel->SetSizer(s);
        root->Add(m_warning_panel, 0, wxEXPAND);
    }

    // ---- Row 1: 匹配方式 + 按钮 ----
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        {
            auto* lbl = new wxStaticText(this, wxID_ANY, _L("匹配方式"));
            lbl->SetFont(Label::Body_14);
            lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
        }
        m_method_combo = new wxComboBox(this, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        m_method_combo->Append(_L("推荐匹配"));
        m_method_combo->Append(_L("手动选择"));
        m_method_combo->SetSelection(0);
        m_method_combo->Bind(wxEVT_COMBOBOX, &MixedFilamentBatchDialog::on_method_changed, this);
        row->Add(m_method_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);

        row->AddStretchSpacer();

        m_btn_start_match = new Button(this, _L("开始匹配"));
        m_btn_start_match->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_start_match->SetCornerRadius(FromDIP(4));
        m_btn_start_match->SetBorderWidth(0);
        m_btn_start_match->SetBackgroundColor(StateColor(
            std::pair(wxColour("#DFDFDF"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#019687"), static_cast<int>(StateColor::Normal))));
        m_btn_start_match->SetTextColor(StateColor(
            std::pair(wxColour("#6B6A6A"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#FEFEFE"), static_cast<int>(StateColor::Normal))));
        row->Add(m_btn_start_match, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
        m_btn_rematch = new Button(this, _L("重新匹配"));
        m_btn_rematch->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_rematch->SetCornerRadius(FromDIP(4));
        m_btn_rematch->SetBorderWidth(0);
        m_btn_rematch->SetBackgroundColor(StateColor(
            std::pair(wxColour("#DFDFDF"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#019687"), static_cast<int>(StateColor::Normal))));
        m_btn_rematch->SetTextColor(StateColor(
            std::pair(wxColour("#6B6A6A"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#FEFEFE"), static_cast<int>(StateColor::Normal))));
        row->Add(m_btn_rematch, 0, wxALIGN_CENTER_VERTICAL);

        m_btn_start_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });
        m_btn_rematch->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });

        root->Add(row, 0, wxEXPAND | wxALL, FromDIP(14));
    }

    // ---- Card: manual filament config (hidden by default, above preview) ----
    {
        auto* card = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        card->SetCornerRadius(FromDIP(4));
        card->SetBorderWidth(FromDIP(1));
        card->SetBorderColorNormal(wxColour("#F0F0F0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
        card->Hide();
        m_manual_card = card;
        auto* s = new wxBoxSizer(wxVERTICAL);

        // Title row with add/remove buttons (same pattern as MixedFilamentDialog)
        {
            auto* title_row = new wxBoxSizer(wxHORIZONTAL);
            {
                auto* lbl = new wxStaticText(card, wxID_ANY, _L("耗材配置 (对应 1-4 号喷嘴)"));
                lbl->SetFont(Label::Body_14);
                lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
                title_row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
            }
            title_row->AddStretchSpacer();

            auto* btn_remove = new ScalableButton(card, wxID_ANY, "icon_minus");
            btn_remove->SetToolTip(_L("Remove filament"));
            btn_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (m_manual_filament_count > 2) {
                    --m_manual_filament_count;
                    m_manual_row_panels[m_manual_filament_count]->Hide();
                    on_manual_selection_changed();
                    m_manual_card->Layout();
                    m_manual_card->GetParent()->Layout();
                }
            });
            title_row->Add(btn_remove, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

            auto* btn_add = new ScalableButton(card, wxID_ANY, "icon_plus");
            btn_add->SetToolTip(_L("Add filament"));
            btn_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (m_manual_filament_count < 4) {
                    m_manual_row_panels[m_manual_filament_count]->Show();
                    ++m_manual_filament_count;
                    on_manual_selection_changed();
                    m_manual_card->Layout();
                    m_manual_card->GetParent()->Layout();
                }
            });
            title_row->Add(btn_add, 0, wxALIGN_CENTER_VERTICAL);

            s->Add(title_row, 0, wxEXPAND | wxALL, M);
        }

        // Divider
        {
            auto* divider = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            s->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, M);
        }

        // 2-column grid of filament rows
        PresetBundle* pb = wxGetApp().preset_bundle;
        const std::vector<std::string>& fps = pb ? pb->filament_presets : std::vector<std::string>();
        auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(6));
        grid->AddGrowableCol(0, 1);
        grid->AddGrowableCol(1, 1);

        for (int i = 0; i < 4; ++i) {
            auto* panel = new wxPanel(card, wxID_ANY);
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(panel, wxID_ANY, wxString::Format(_L("耗材 %d"), i + 1),
                                       wxDefaultPosition, wxSize(FromDIP(40), -1)),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

            auto* cb = new ComboBox(panel, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(-1, FromDIP(30)),
                                     0, nullptr, wxCB_READONLY);
            for (size_t j = 0; j < m_physical_colors.size(); ++j) {
                wxString name;
                if (pb && j < fps.size()) {
                    const Preset* preset = pb->filaments.find_preset(fps[j]);
                    if (preset) name = from_u8(preset->label(false));
                }
                if (name.empty()) name = wxString::Format("F%zu", j + 1);
                wxBitmap* icon = get_extruder_color_icon(m_physical_colors[j], std::to_string(j + 1), FromDIP(20), FromDIP(20));
                cb->Append(name, icon ? icon->ConvertToImage() : wxNullImage);
            }
            if (m_filament_selections[i] >= 0 && m_filament_selections[i] < (int)m_physical_colors.size())
                cb->SetSelection(m_filament_selections[i]);
            else if (!m_physical_colors.empty())
                cb->SetSelection(0);
            cb->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_manual_selection_changed(); });
            row->Add(cb, 1, wxALIGN_CENTER_VERTICAL);
            m_filament_combo[i] = cb;
            panel->SetSizer(row);
            if (i >= m_manual_filament_count) panel->Hide();
            m_manual_row_panels[i] = panel;
            grid->Add(panel, 0, wxEXPAND);
        }
        s->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M);
        card->SetSizer(s);
        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(14));
    }

    // ---- Card: recommended mode CMYG (hidden by default, shown for RECOMMENDED) ----
    {
        auto* card = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        card->SetCornerRadius(FromDIP(4));
        card->SetBorderWidth(FromDIP(1));
        card->SetBorderColorNormal(wxColour("#F0F0F0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
        card->Hide();
        m_recommended_card = card;
        auto* s = new wxBoxSizer(wxVERTICAL);

        // Title
        {
            auto* lbl = new wxStaticText(card, wxID_ANY, _L("耗材配置 (CMYG 四色)"));
            lbl->SetFont(Label::Body_14);
            lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            s->Add(lbl, 0, wxALL, M);
        }

        // Divider
        {
            auto* divider = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            s->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, M);
        }

        // 2-column grid: 4 fixed CMYG rows
        static const std::vector<std::pair<std::string, wxString>> CMYG_ENTRIES = {
            {"#00FFFF", _L("Cyan")},
            {"#FF00FF", _L("Magenta")},
            {"#FFFF00", _L("Yellow")},
            {"#00FF00", _L("Green")},
        };

        auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(6));
        grid->AddGrowableCol(0, 1);
        grid->AddGrowableCol(1, 1);

        // Build swatch + label for each of the 4 slots.
        // Initially populated with the canonical CMYG order; updated after
        // batch match completes to reflect the actual palette order chosen
        // by recommend_best_filament_combo (which may reorder by DeltaE).
        for (int i = 0; i < 4; ++i) {
            auto* panel = new wxPanel(card, wxID_ANY);
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(panel, wxID_ANY, wxString::Format(_L("耗材 %d"), i + 1),
                                       wxDefaultPosition, wxSize(FromDIP(40), -1)),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

            // Color swatch — stored for later update
            wxBitmap* icon = get_extruder_color_icon(
                CMYG_ENTRIES[i].first, std::to_string(i + 1),
                FromDIP(20), FromDIP(20));
            if (icon) {
                auto* swatch = new wxStaticBitmap(panel, wxID_ANY, *icon);
                m_recommended_swatches[i] = swatch;
                row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            }

            // Color name label — stored for later update
            auto* name = new wxStaticText(panel, wxID_ANY, CMYG_ENTRIES[i].second);
            name->SetFont(Label::Body_13);
            name->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6A6A")));
            m_recommended_labels[i] = name;
            row->Add(name, 0, wxALIGN_CENTER_VERTICAL);

            panel->SetSizer(row);
            grid->Add(panel, 0, wxEXPAND);
        }
        s->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M);
        card->SetSizer(s);
        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(14));
    }

    // ---- Preview row ----
    {
        auto* prow = new wxBoxSizer(wxHORIZONTAL);

        // Left: 原模型 (42%)
        auto* lcol = new wxBoxSizer(wxVERTICAL);
        {
            auto* lbl = new wxStaticText(this, wxID_ANY, _L("原模型"));
            lbl->SetFont(Label::Body_14);
            lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            lcol->Add(lbl, 0, wxBOTTOM, FromDIP(6));
        }
        m_preview_orig_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                            wxSize(-1, FromDIP(180)), wxBORDER_NONE);
        m_preview_orig_panel->SetBackgroundColour(wxColour(245, 245, 245));
        lcol->Add(m_preview_orig_panel, 0, wxEXPAND);

        // Tray controls below original preview
        {
            auto* tray_row = new wxBoxSizer(wxHORIZONTAL);
            tray_row->AddSpacer(FromDIP(4));
            m_btn_tray_prev = new wxButton(this, wxID_ANY, "◄", wxDefaultPosition, wxSize(FromDIP(28), FromDIP(28)));
            m_btn_tray_prev->SetWindowStyleFlag(wxBORDER_NONE);
            m_btn_tray_prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (m_tray_index > 1) { --m_tray_index; m_tray_combo->SetSelection(m_tray_index - 1); refresh_previews(); }
            });
            tray_row->Add(m_btn_tray_prev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
            tray_row->Add(new wxStaticText(this, wxID_ANY, _L("盘号")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
            m_tray_combo = new ComboBox(this, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxSize(FromDIP(56), FromDIP(24)),
                                         0, nullptr, wxCB_READONLY);
            for (int i = 1; i <= m_tray_count; ++i)
                m_tray_combo->Append(wxString::Format("%d", i));
            m_tray_combo->SetSelection(0);
            m_tray_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
                m_tray_index = m_tray_combo->GetSelection() + 1;
                refresh_previews();
            });
            tray_row->Add(m_tray_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
            m_btn_tray_next = new wxButton(this, wxID_ANY, "►", wxDefaultPosition, wxSize(FromDIP(28), FromDIP(28)));
            m_btn_tray_next->SetWindowStyleFlag(wxBORDER_NONE);
            m_btn_tray_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (m_tray_index < m_tray_count) { ++m_tray_index; m_tray_combo->SetSelection(m_tray_index - 1); refresh_previews(); }
            });
            tray_row->Add(m_btn_tray_next, 0, wxALIGN_CENTER_VERTICAL);
            lcol->Add(tray_row, 0, wxEXPAND | wxTOP, FromDIP(8));
        }
        prow->Add(lcol, 3, wxEXPAND | wxRIGHT, FromDIP(16));

        // Right: 混色匹配后 (58%)
        auto* rcol = new wxBoxSizer(wxVERTICAL);
        {
            auto* lbl = new wxStaticText(this, wxID_ANY, _L("混色匹配后"));
            lbl->SetFont(Label::Body_14);
            lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            rcol->Add(lbl, 0, wxBOTTOM, FromDIP(6));
        }
        m_preview_match_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                             wxSize(-1, FromDIP(220)), wxBORDER_NONE);
        m_preview_match_panel->SetBackgroundColour(wxColour(245, 245, 245));
        rcol->Add(m_preview_match_panel, 0, wxEXPAND);
        prow->Add(rcol, 4, wxEXPAND);

        root->Add(prow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
    }

    // ---- Scroll container (mapping + prompt) ----
    m_scrolled_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolled_content->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    m_scrolled_content->SetScrollRate(0, FromDIP(8));
    m_scrolled_content->Bind(wxEVT_CHILD_FOCUS, [](wxChildFocusEvent&) {});
    auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);

    // ---- 颜色映射 ----
    {
        auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        card->SetCornerRadius(FromDIP(4));
        card->SetBorderWidth(FromDIP(1));
        card->SetBorderColorNormal(wxColour("#F0F0F0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
        auto* cs = new wxBoxSizer(wxVERTICAL);
        {
            auto* lbl = new wxStaticText(card, wxID_ANY, _L("颜色映射"));
            lbl->SetFont(Label::Body_14);
            lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            cs->Add(lbl, 0, wxALL, M);
        }

        // Divider
        {
            auto* divider = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            cs->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, M);
        }

        m_legend_scroller = new wxScrolledWindow(card, wxID_ANY, wxDefaultPosition,
                                                  wxSize(-1, FromDIP(140)), wxHSCROLL);
        m_legend_scroller->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_legend_sizer = new wxWrapSizer(wxHORIZONTAL);
        m_legend_scroller->SetSizer(m_legend_sizer);
        m_legend_scroller->SetScrollRate(FromDIP(10), 0);
        cs->Add(m_legend_scroller, 0, wxEXPAND | wxALL, M);
        card->SetSizer(cs);
        scroll_sizer->Add(card, 0, wxEXPAND | wxBOTTOM, M);
    }

    // ---- 提示文案 ----
    m_prompt_label = new Label(m_scrolled_content, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
    scroll_sizer->Add(m_prompt_label, 0, wxEXPAND | wxLEFT | wxRIGHT, M);

    m_scrolled_content->SetSizer(scroll_sizer);
    m_scrolled_content->SetMinSize(wxSize(-1, FromDIP(80)));
    root->Add(m_scrolled_content, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(14));

    // ---- Progress bar ----
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_progress_label = new Label(this, wxEmptyString);
        row->Add(m_progress_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
        m_progress_bar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(8)),
                                      wxGA_HORIZONTAL | wxGA_SMOOTH);
        m_progress_bar->SetValue(0);
        m_progress_bar->Hide();
        row->Add(m_progress_bar, 1, wxALIGN_CENTER_VERTICAL);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M);
    }

    // ---- 底部按钮: 取消 + 确定 ----
    {
        auto* btn_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        btn_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

        auto* panel_sizer = new wxBoxSizer(wxVERTICAL);

        // #F0F0F0 top border (1px)
        auto* top_line = new wxPanel(btn_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        top_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
        panel_sizer->Add(top_line, 0, wxEXPAND);

        panel_sizer->AddSpacer(FromDIP(12));

        auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

        m_btn_cancel_match = new Button(btn_panel, _L("取消"));
        m_btn_cancel_match->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_cancel_match->SetCornerRadius(FromDIP(4));
        m_btn_cancel_match->SetBorderWidth(FromDIP(1));
        m_btn_cancel_match->SetBorderColorNormal(wxColour("#D1D5DC"));
        m_btn_cancel_match->SetBackgroundColorNormal(wxColour("#FFFFFF"));
        m_btn_cancel_match->SetTextColorNormal(wxColour("#242424"));

        m_btn_confirm = new Button(btn_panel, _L("确定"));
        m_btn_confirm->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_confirm->SetCornerRadius(FromDIP(4));
        m_btn_confirm->SetBorderWidth(0);
        m_btn_confirm->SetBackgroundColor(StateColor(
            std::pair(wxColour("#DFDFDF"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#019687"), static_cast<int>(StateColor::Normal))));
        m_btn_confirm->SetTextColor(StateColor(
            std::pair(wxColour("#6B6A6A"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#FEFEFE"), static_cast<int>(StateColor::Normal))));

        btn_sizer->Add(m_btn_cancel_match, 1, wxRIGHT, FromDIP(8));
        btn_sizer->Add(m_btn_confirm, 1);
        panel_sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

        panel_sizer->AddSpacer(FromDIP(12));

        m_btn_cancel_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            EndModal(wxID_OK);
        });

        btn_panel->SetSizer(panel_sizer);
        root->Add(btn_panel, 0, wxEXPAND);
    }

    SetSizer(root);
    Layout();
    SetSize(FromDIP(760), FromDIP(680));
}

// ---------------------------------------------------------------------------
// State management
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::display_warning(const wxString& msg)
{
    if (!m_warning_panel || !m_warning_text || !m_error_panel) return;
    m_error_panel->Hide();
    m_warning_panel->Show();
    Layout();
    m_warning_text->SetLabel(msg);
    Layout();
}

void MixedFilamentBatchDialog::set_error(const wxString& msg)
{
    if (!m_error_panel || !m_error_text || !m_warning_panel) return;
    m_warning_panel->Hide();
    m_error_panel->Show();
    Layout();
    m_error_text->SetLabel(msg);
    if (m_btn_confirm) m_btn_confirm->Disable();
    Layout();
}

void MixedFilamentBatchDialog::set_match_buttons_state(bool matching)
{
    m_btn_start_match->Enable(!matching && !m_match_completed);
    m_btn_rematch->Enable(!matching && m_match_completed);
    m_btn_cancel_match->Enable(matching || !m_match_completed); // Cancel closes dialog when idle
    m_btn_confirm->Enable(!matching && m_match_completed && m_result.success);
    if (matching) {
        m_progress_bar->Show();
        m_progress_bar->SetValue(0);
        m_progress_label->SetLabel(_L("Matching..."));
    } else {
        m_progress_bar->Hide();
        m_progress_label->SetLabel(wxEmptyString);
    }
}

void MixedFilamentBatchDialog::update_prompt_text()
{
    if (m_matching_method == RECOMMENDED)
        m_prompt_label->SetLabel(_L("1-4号为主耗材。系统会自动选取耗材颜色及混色配方。点击确定后还可手动调整方案。"));
    else
        m_prompt_label->SetLabel(_L("1-4号为主耗材。请选择耗材，系统会根据耗材进行混色匹配，点击确定后还可手动调整方案。"));
}

void MixedFilamentBatchDialog::on_method_changed(wxCommandEvent&)
{
    int sel = m_method_combo->GetSelection();
    m_matching_method = (sel == 1) ? MANUAL : RECOMMENDED;
    m_result = BatchMatchResult{};
    m_match_completed = false;
    m_error_panel->Hide();
    m_warning_panel->Hide();
    if (m_manual_card)
        m_manual_card->Show(m_matching_method == MANUAL);
    if (m_recommended_card)
        m_recommended_card->Show(m_matching_method == RECOMMENDED);
    // Force parent re-layout since card visibility changed
    if (m_matching_method == MANUAL && m_manual_card) {
        m_manual_card->Layout();
        m_manual_card->GetParent()->Layout();
    }
    if (m_matching_method == RECOMMENDED && m_recommended_card) {
        m_recommended_card->Layout();
        m_recommended_card->GetParent()->Layout();
    }
    update_prompt_text();
    update_mapping_legend();
    set_match_buttons_state(false);
    m_btn_start_match->Enable(true);
    Layout();
}

void MixedFilamentBatchDialog::on_manual_selection_changed()
{
    if (m_match_completed) {
        m_result = BatchMatchResult{};
        m_match_completed = false;
        update_mapping_legend();
    }
    m_btn_start_match->Enable(true);
    m_btn_rematch->Enable(false);
    m_btn_confirm->Enable(false);
}

// ---------------------------------------------------------------------------
// Match
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::start_batch_match()
{
    if (m_match_running) return;
    if (m_model_colors.empty()) {
        set_error(_L("No model colors found. Please load a multi-color model."));
        return;
    }
    m_match_running = true;
    m_error_panel->Hide();
    m_warning_panel->Hide();
    // Clear stale legend from previous match before starting new one
    m_result = BatchMatchResult{};
    m_match_completed = false;
    update_mapping_legend();
    m_legend_scroller->Layout();
    set_match_buttons_state(true);
    launch_background_match();
}

void MixedFilamentBatchDialog::cancel_batch_match()
{
    m_cancel_requested->store(true);
}

void MixedFilamentBatchDialog::launch_background_match()
{
    if (m_worker_thread.joinable()) m_worker_thread.join();
    m_cancel_requested->store(false);

    const auto model_colors = m_model_colors;
    std::vector<std::string> active_colors;
    std::vector<unsigned int> manual_full_ids; // 1-based indices into m_physical_colors
    if (m_matching_method == MANUAL) {
        for (int i = 0; i < m_manual_filament_count; ++i) {
            int sel = m_filament_combo[i] ? m_filament_combo[i]->GetSelection() : i;
            if (sel >= 0 && sel < (int)m_physical_colors.size()) {
                active_colors.push_back(m_physical_colors[sel]);
                manual_full_ids.push_back(static_cast<unsigned int>(sel + 1)); // 1-based
            }
        }
        if (active_colors.size() < 2) {
            active_colors = m_physical_colors;
            manual_full_ids.clear();
            for (size_t i = 0; i < m_physical_colors.size(); ++i)
                manual_full_ids.push_back(static_cast<unsigned int>(i + 1));
        }
    }
    const auto manual_colors     = std::move(active_colors);
    auto manual_full_ids_c = std::move(manual_full_ids);
    const auto all_physical      = m_physical_colors;

    const auto matching_method = m_matching_method;
    // Fixed CMYG palette for recommended mode.
    static const std::vector<std::string> CMYG_COLORS = {
        "#00FFFF",  // C: Cyan
        "#FF00FF",  // M: Magenta
        "#FFFF00",  // Y: Yellow
        "#00FF00",  // G: Green
    };
    const std::vector<std::string> preset_colors = CMYG_COLORS;
    // Use enabled_count() (skips deleted/disabled) to match the virtual ID
    // numbering scheme used by mixed_index_from_filament_id() and
    // add_batch_custom_filaments(), both of which count only enabled entries.
    const size_t existing_mixed_count = wxGetApp().preset_bundle
        ? wxGetApp().preset_bundle->mixed_filaments.enabled_count()
        : size_t(0);

    auto destroyed = m_destroyed;
    auto cancel_token = m_cancel_requested;
    auto progress_bar = m_progress_bar;

    m_worker_thread = std::thread([this, model_colors, manual_colors, all_physical,
                                    preset_colors, matching_method, existing_mixed_count,
                                    destroyed, cancel_token, progress_bar,
                                    manual_full_ids = std::move(manual_full_ids_c)]()
    {
        std::vector<std::string> physical_colors;
        if (matching_method == MANUAL) {
            physical_colors = manual_colors;
        } else {
            if (preset_colors.size() >= 4) {
                auto best = recommend_best_filament_combo(model_colors, preset_colors, 15, cancel_token);
                physical_colors = best.empty()
                    ? std::vector<std::string>(preset_colors.begin(), preset_colors.begin() + std::min<size_t>(4, preset_colors.size()))
                    : std::move(best);
            } else {
                physical_colors = all_physical;
            }
        }

        BatchMatchResult result;
        result.success = true;
        std::vector<ModelColorEntry> unmatched_colors;

        // Build palette of all existing filament colors (physical + mixed)
        const size_t num_physical = physical_colors.size();
        std::vector<wxColour> existing_palette;
        std::vector<unsigned int> existing_ids;
        for (size_t i = 0; i < physical_colors.size(); ++i) {
            wxColour c;
            if (try_parse_color_match_hex(physical_colors[i], c)) {
                existing_palette.push_back(c);
                existing_ids.push_back(static_cast<unsigned int>(i + 1));
            }
        }
        // Recommended mode: only use the recommended CMYG physical colors as the
        // Pass-1 reuse palette. Existing mixed filaments are NOT included because
        // (a) their display_colors reference the old physical palette that is about
        // to be replaced, and (b) a passthrough recipe targeting a virtual filament
        // ID (e.g. component_a=5) would be silently clamped in add_batch_custom_filaments.

        // Pass 1: map each model color to closest existing filament
        constexpr double K_REUSE_THRESHOLD = 1.5;
        for (const auto& mc : model_colors) {
            double best_de = std::numeric_limits<double>::max();
            size_t best_idx = 0;
            for (size_t j = 0; j < existing_palette.size(); ++j) {
                double de = color_delta_e00(mc.color, existing_palette[j]);
                if (de < best_de) { best_de = de; best_idx = j; }
            }
            if (best_de < K_REUSE_THRESHOLD && best_idx < existing_ids.size()) {
                // Direct mapping to existing filament
                ColorMappingEntry mapping;
                mapping.model_color_index   = mc.color_index;
                mapping.source_color         = mc.color;
                mapping.target_filament_id   = existing_ids[best_idx];
                mapping.matched_color        = existing_palette[best_idx];
                mapping.delta_e              = best_de;
                mapping.is_pure_recipe       = (existing_ids[best_idx] <= (unsigned int)num_physical);
                mapping.pure_delta_e         = best_de;
                mapping.merged_model_indices = {mc.color_index};
                mapping.source_extruder_ids  = mc.extruder_ids;
                mapping.recipe.valid         = true;
                mapping.recipe.component_a  = existing_ids[best_idx];
                mapping.recipe.mix_b_percent = 0;
                mapping.recipe.preview_color = existing_palette[best_idx];
                mapping.recipe.delta_e       = best_de;
                result.mappings.push_back(mapping);
            } else {
                unmatched_colors.push_back(mc);
            }
        }

        // Pass 2: batch-match remaining colors
        if (!unmatched_colors.empty()) {
            try {
                auto sub_result = batch_match_model_colors(unmatched_colors, physical_colors, 15, cancel_token,
                    [progress_bar, destroyed](int done, int total) {
                        if (progress_bar && !destroyed->load()) {
                            wxGetApp().CallAfter([progress_bar, done, total, destroyed]() {
                                if (destroyed->load()) return;
                                if (total > 0) progress_bar->SetValue(done * 100 / total);
                            });
                        }
                    });
                if (sub_result.success) {
                    // Offset virtual IDs: start after all existing filaments
                    assign_batch_virtual_filament_ids(sub_result, physical_colors.size(), existing_mixed_count);
                    // Merge
                    result.mappings.insert(result.mappings.end(),
                        sub_result.mappings.begin(), sub_result.mappings.end());
                } else {
                    result.success = false;
                    result.error_message = sub_result.error_message;
                    result.error_code = sub_result.error_code;
                }
            } catch (const std::exception& e) {
                result.success = false;
                result.error_message = std::string("Error: ") + e.what();
                result.error_code = 3;
            } catch (...) {
                result.success = false;
                result.error_message = "Unknown error during batch match";
                result.error_code = 3;
            }
        }

        // Manual mode: remap recipe component IDs from the user-selected subset
        // back to their original 1-based indices in the project filament_colour list.
        // Without this, add_batch_custom_filaments() interprets subset-relative
        // indices (e.g. "1,2" for a 2-filament manual selection) as project-level
        // indices, creating mixed filaments that reference the wrong physical spools.
        const bool need_manual_remap = (matching_method == MANUAL
            && !manual_full_ids.empty()
            && manual_full_ids.size() == physical_colors.size()
            && physical_colors.size() != all_physical.size());
        if (need_manual_remap) {

            // Build lookup: subset_idx(1-based) → project_idx(1-based)
            std::vector<unsigned int> remap(physical_colors.size() + 1, 0);
            for (size_t i = 0; i < manual_full_ids.size(); ++i)
                remap[i + 1] = manual_full_ids[i];

            auto remap_id = [&](unsigned int& id) {
                if (id > 0 && id < remap.size() && remap[id] > 0)
                    id = remap[id];
            };

            for (auto& mapping : result.mappings) {
                remap_id(mapping.target_filament_id);
                remap_id(mapping.recipe.component_a);
                remap_id(mapping.recipe.component_b);
                if (!mapping.recipe.gradient_component_ids.empty()) {
                    auto ids = MixedFilamentManager::decode_gradient_component_ids(
                        mapping.recipe.gradient_component_ids);
                    for (auto& id : ids) remap_id(id);
                    mapping.recipe.gradient_component_ids =
                        MixedFilamentManager::encode_gradient_component_ids(ids);
                }
            }

            // After remapping, pure-recipe component_a values may now be > the
            // old subset-size threshold used by assign_batch_virtual_filament_ids.
            // Re-evaluate is_pure_recipe against the full project physical count.
            const unsigned int full_phys = (unsigned int)all_physical.size();
            for (auto& mapping : result.mappings) {
                if (mapping.is_pure_recipe)
                    mapping.is_pure_recipe = (mapping.recipe.component_a <= full_phys);
            }

            // Reassign virtual filament IDs using the full project physical count
            // so they don't collide with remapped pure-recipe IDs.
            unsigned int next_vid = (unsigned int)(all_physical.size()
                                                   + existing_mixed_count + 1);
            for (auto& mapping : result.mappings) {
                if (!mapping.is_pure_recipe)
                    mapping.target_filament_id = next_vid++;
            }
        }

        if (result.success && result.mappings.empty()) {
            result.success = false;
            result.error_message = "No valid recipes found for any model color";
            result.error_code = 1;
        }
        if (result.success) {
            // Each model color always gets its own mapping entry — no deduplication.
            // Merging by matched_color would lose distinct model-source colors whose
            // recipes happen to produce similar output shades.
            double sum_de = 0.0;
            for (const auto& m : result.mappings) sum_de += m.delta_e;
            result.avg_delta_e = sum_de / double(result.mappings.size());
            // Use full project filament indices for manual mode
            if (need_manual_remap) {
                result.selected_physical_ids = manual_full_ids;
            } else {
                for (size_t i = 1; i <= physical_colors.size(); ++i)
                    result.selected_physical_ids.push_back((unsigned int)i);
            }
            if (matching_method == RECOMMENDED) {
                result.is_recommended_mode = true;
                result.recommended_physical_colors = physical_colors;
            }
        }

        wxGetApp().CallAfter([this, destroyed, result = std::move(result)]() mutable {
            if (destroyed->load()) return;
            handle_batch_match_result(result);
        });
    });
}

void MixedFilamentBatchDialog::handle_batch_match_result(const BatchMatchResult& result)
{
    if (m_destroyed->load()) return;
    m_match_running = false;
    m_progress_bar->SetValue(100);
    m_progress_label->SetLabel(wxEmptyString);
    m_progress_bar->Hide();

    if (!result.success) {
        set_error(wxString::FromUTF8(result.error_message));
        m_btn_start_match->Enable(true);
        m_btn_rematch->Enable(true);
        Layout();
        return;
    }

    m_match_completed = false;
    m_result = result;
    m_match_completed = true;
    set_match_buttons_state(false);  // after m_match_completed=true so confirm enables
    update_mapping_legend();
    rebuild_match_thumb_cache();
    if (result.is_recommended_mode)
        update_recommended_card();
    refresh_previews();
    BOOST_LOG_TRIVIAL(info) << "Batch match: " << result.mappings.size()
                            << " mappings, avg ΔE=" << result.avg_delta_e;
    Layout();
}

// ---------------------------------------------------------------------------
// Legend (matches prototype: src-swatch → badge-number  ΔE-badge)
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::update_mapping_legend()
{
    while (m_legend_sizer->GetItemCount() > 0) {
        wxSizerItem* item = m_legend_sizer->GetItem(size_t(0));
        if (item && item->GetWindow()) item->GetWindow()->Destroy();
        m_legend_sizer->Remove(0);
    }

    if (m_result.mappings.empty()) {
        m_legend_sizer->Add(new Label(m_legend_scroller,
            _L("No color mappings yet. Click \"Start Match\" to begin.")), 0, wxALL, FromDIP(4));
    } else {
        for (const auto& mapping : m_result.mappings) {
            auto* item = new wxPanel(m_legend_scroller, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxBORDER_SIMPLE);
            item->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F9F9F9")));
            auto* s = new wxBoxSizer(wxHORIZONTAL);

            // Source swatch
            ColorBlockParams src;
            src.mode = ColorBlockParams::Solid;
            src.solid_color = mapping.source_color.IsOk() ? mapping.source_color : wxColour(128,128,128);
            src.width  = FromDIP(18);
            src.height = FromDIP(18);
            s->Add(new wxStaticBitmap(item, wxID_ANY, *get_color_block_bitmap_cached(src)),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

            // Arrow
            s->Add(new wxStaticText(item, wxID_ANY, "→"), 0,
                   wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

            // Badge with filament number
            ColorBlockParams badge;
            badge.mode = ColorBlockParams::Solid;
            badge.solid_color = mapping.matched_color.IsOk() ? mapping.matched_color : wxColour(128,128,128);
            badge.width  = FromDIP(28);
            badge.height = FromDIP(28);
            badge.label  = wxString::Format("%u", mapping.target_filament_id);
            s->Add(new wxStaticBitmap(item, wxID_ANY, *get_color_block_bitmap_cached(badge)),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

            // ΔE badge
            wxString de_str = wxString::Format("ΔE %.1f", mapping.delta_e);
            auto* de = new wxStaticText(item, wxID_ANY, de_str);
            de->SetFont(Label::Body_10);
            if (mapping.delta_e <= 1.0)
                de->SetForegroundColour(wxColour(46, 125, 50));
            else if (mapping.delta_e <= 3.0)
                de->SetForegroundColour(wxColour(239, 108, 0));
            else
                de->SetForegroundColour(wxColour(198, 40, 40));
            s->Add(de, 0, wxALIGN_CENTER_VERTICAL);

            item->SetSizer(s);
            m_legend_sizer->Add(item, 0, wxALL, FromDIP(3));
        }
    }
    m_legend_scroller->Layout();
    m_legend_scroller->FitInside();
    m_legend_scroller->Refresh();
}

}} // namespace Slic3r::GUI
