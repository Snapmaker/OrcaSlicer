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

    // Generate thumbnails for plates that don't have slicing data yet
    wxWeakRef<wxWindow> weak_self(this);
    CallAfter([weak_self]() {
        if (!weak_self) return;
        auto* self = static_cast<MixedFilamentBatchDialog*>(weak_self.get());
        // Force thumbnail generation for all plates using the existing 3D canvas
        // (works without slicing — uses offscreen FBO rendering)
        wxGetApp().plater()->update_all_plate_thumbnails(true);
        self->update_orig_preview();
        self->update_match_preview();
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

void MixedFilamentBatchDialog::update_orig_preview()
{
    if (!m_preview_orig_bitmap) {
        m_preview_orig_bitmap = new wxStaticBitmap(m_preview_orig_panel, wxID_ANY, wxNullBitmap);
        auto* s = new wxBoxSizer(wxVERTICAL);
        s->AddStretchSpacer();
        s->Add(m_preview_orig_bitmap, 0, wxALIGN_CENTER);
        s->AddStretchSpacer();
        m_preview_orig_panel->SetSizer(s);
    }

    const int panel_w = std::max(FromDIP(100), m_preview_orig_panel->GetSize().GetWidth());
    const int panel_h = std::max(FromDIP(100), m_preview_orig_panel->GetSize().GetHeight());

    auto* plater = wxGetApp().plater();
    if (!plater) return;

    auto& plate_list = plater->get_partplate_list();
    int idx = m_tray_index - 1;
    if (idx < 0 || idx >= plate_list.get_plate_count()) return;

    const PartPlate* plate = plate_list.get_plate(idx);
    if (!plate) return;

    wxBitmap bmp = thumbnail_to_bitmap(plate->thumbnail_data, panel_w, panel_h);
    if (bmp.IsOk())
        m_preview_orig_bitmap->SetBitmap(bmp);
    m_preview_orig_bitmap->SetSize(panel_w, panel_h);
    m_preview_orig_panel->Layout();
}

void MixedFilamentBatchDialog::update_match_preview()
{
    m_preview_match_panel->Layout();

    const int panel_w = std::max(FromDIP(160), m_preview_match_panel->GetClientSize().GetWidth());
    const int panel_h = std::max(FromDIP(120), m_preview_match_panel->GetClientSize().GetHeight());

    auto* plater = wxGetApp().plater();
    if (!plater) return;
    auto* canvas = plater->canvas3D();
    if (!canvas) return;

    // Temporarily override filament_colour with matched recipe colors
    // so the thumbnail renderer picks up the new colors
    auto* pb = wxGetApp().preset_bundle;
    if (!pb) return;
    auto* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!co) return;

    const std::vector<std::string> saved_colors = co->values;
    std::vector<std::string> matched_colors = saved_colors;

    for (const auto& mapping : m_result.mappings) {
        if (mapping.target_filament_id < 1) continue;
        size_t slot = mapping.target_filament_id - 1;
        if (slot >= matched_colors.size()) {
            // Extend the array for virtual filament IDs
            matched_colors.resize(slot + 1, "#808080");
        }
        matched_colors[slot] = mapping.matched_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    }
    co->values = matched_colors;

    // Render thumbnail at exactly the panel's size
    ThumbnailsParams tp;
    tp.plate_id = m_tray_index;
    ThumbnailData td;
    canvas->render_thumbnail(td,
        (unsigned int)(panel_w),
        (unsigned int)(panel_h),
        tp, Camera::EType::Ortho, true, false, false);

    // Restore original filament colors immediately
    co->values = saved_colors;

    wxBitmap bmp = thumbnail_to_bitmap(td, panel_w, panel_h);
    if (bmp.IsOk()) {
        if (!m_preview_match_bitmap) {
            m_preview_match_bitmap = new wxStaticBitmap(m_preview_match_panel, wxID_ANY, bmp);
            m_preview_match_bitmap->SetSize(panel_w, panel_h);
            auto* s = new wxBoxSizer(wxVERTICAL);
            s->AddStretchSpacer();
            s->Add(m_preview_match_bitmap, 0, wxALIGN_CENTER);
            s->AddStretchSpacer();
            m_preview_match_panel->SetSizer(s);
        } else {
            m_preview_match_bitmap->SetBitmap(bmp);
            m_preview_match_bitmap->SetSize(panel_w, panel_h);
        }
        m_preview_match_panel->Layout();
    }
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::load_model_colors()
{
    m_model_colors.clear();

    // Primary: extract actual model colors from MMU-painted regions.
    // Walks ModelVolume::get_extruders() → filament_colour[] to find every
    // unique color used on the model surface.
    if (wxGetApp().plater()) {
        const Print& print = wxGetApp().plater()->fff_print();
        if (!print.objects().empty())
            m_model_colors = extract_model_colors(print);
    }

    // Fallback: if the model has no MMU painting data (extract returned empty),
    // use the project's filament_colour config as the target color set.
    if (m_model_colors.empty()) {
        auto* pb = wxGetApp().preset_bundle;
        if (!pb) return;
        ConfigOptionStrings* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
        if (!co || co->values.empty()) return;
        for (size_t i = 0; i < co->values.size(); ++i) {
            wxColour c;
            if (!try_parse_color_match_hex(co->values[i], c)) continue;
            bool dup = false;
            for (const auto& e : m_model_colors) {
                if (e.hex_value == co->values[i] || color_delta_e00(e.color, c) < 0.5)
                    { dup = true; break; }
            }
            if (dup) continue;
            m_model_colors.push_back({(unsigned int)(m_model_colors.size() + 1), c, co->values[i]});
        }
    }
}

// ---------------------------------------------------------------------------
// UI — strictly matches prototype-mockup.html
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::build_ui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
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
        row->Add(new wxStaticText(this, wxID_ANY, _L("匹配方式")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
        m_method_combo = new wxComboBox(this, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        m_method_combo->Append(_L("推荐匹配"));
        m_method_combo->Append(_L("手动选择"));
        m_method_combo->SetSelection(0);
        m_method_combo->Bind(wxEVT_COMBOBOX, &MixedFilamentBatchDialog::on_method_changed, this);
        row->Add(m_method_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);

        row->AddStretchSpacer();

        m_btn_start_match = new Button(this, _L("开始匹配"));
        m_btn_start_match->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
        row->Add(m_btn_start_match, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
        m_btn_rematch = new Button(this, _L("重新匹配"));
        m_btn_rematch->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
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
        card->SetBorderColorNormal(wxColour("#E0E0E0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FAFAFA"), (int)StateColor::Normal)));
        card->Hide();
        m_manual_card = card;
        auto* s = new wxBoxSizer(wxVERTICAL);

        // Title row with add/remove buttons (same pattern as MixedFilamentDialog)
        {
            auto* title_row = new wxBoxSizer(wxHORIZONTAL);
            title_row->Add(new Label(card, _L("耗材配置 (对应 1-4 号喷嘴)")), 0, wxALIGN_CENTER_VERTICAL);
            title_row->AddStretchSpacer();

            auto* btn_remove = new ScalableButton(card, wxID_ANY, "icon_minus");
            btn_remove->SetToolTip(_L("Remove filament"));
            title_row->Add(btn_remove, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

            auto* btn_add = new ScalableButton(card, wxID_ANY, "icon_plus");
            btn_add->SetToolTip(_L("Add filament"));
            title_row->Add(btn_add, 0, wxALIGN_CENTER_VERTICAL);

            s->Add(title_row, 0, wxEXPAND | wxALL, M);
        }

        // 2-column grid of filament rows
        PresetBundle* pb = wxGetApp().preset_bundle;
        const std::vector<std::string>& fps = pb ? pb->filament_presets : std::vector<std::string>();
        auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(6));
        grid->AddGrowableCol(0, 1);
        grid->AddGrowableCol(1, 1);

        for (int i = 0; i < 4; ++i) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(card, wxID_ANY, wxString::Format(_L("耗材 %d"), i + 1),
                                       wxDefaultPosition, wxSize(FromDIP(40), -1)),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

            auto* cb = new ComboBox(card, wxID_ANY, wxEmptyString,
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
            grid->Add(row, 0, wxEXPAND);
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
        lcol->Add(new wxStaticText(this, wxID_ANY, _L("原模型")), 0, wxBOTTOM, FromDIP(6));
        m_preview_orig_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                            wxSize(-1, FromDIP(180)), wxBORDER_SIMPLE);
        m_preview_orig_panel->SetBackgroundColour(wxColour(245, 245, 245));
        lcol->Add(m_preview_orig_panel, 0, wxEXPAND);

        // Tray controls below original preview
        {
            auto* tray_row = new wxBoxSizer(wxHORIZONTAL);
            tray_row->AddSpacer(FromDIP(4));
            m_btn_tray_prev = new wxButton(this, wxID_ANY, "◄", wxDefaultPosition, wxSize(FromDIP(28), FromDIP(28)));
            m_btn_tray_prev->SetWindowStyleFlag(wxBORDER_NONE);
            m_btn_tray_prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (m_tray_index > 1) { --m_tray_index; m_tray_combo->SetSelection(m_tray_index - 1); update_orig_preview(); update_match_preview(); }
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
                update_orig_preview(); update_match_preview();
            });
            tray_row->Add(m_tray_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
            m_btn_tray_next = new wxButton(this, wxID_ANY, "►", wxDefaultPosition, wxSize(FromDIP(28), FromDIP(28)));
            m_btn_tray_next->SetWindowStyleFlag(wxBORDER_NONE);
            m_btn_tray_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (m_tray_index < m_tray_count) { ++m_tray_index; m_tray_combo->SetSelection(m_tray_index - 1); update_orig_preview(); update_match_preview(); }
            });
            tray_row->Add(m_btn_tray_next, 0, wxALIGN_CENTER_VERTICAL);
            lcol->Add(tray_row, 0, wxEXPAND | wxTOP, FromDIP(8));
        }
        prow->Add(lcol, 3, wxEXPAND | wxRIGHT, FromDIP(16));

        // Right: 混色匹配后 (58%)
        auto* rcol = new wxBoxSizer(wxVERTICAL);
        rcol->Add(new wxStaticText(this, wxID_ANY, _L("混色匹配后")), 0, wxBOTTOM, FromDIP(6));
        m_preview_match_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                             wxSize(-1, FromDIP(220)), wxBORDER_SIMPLE);
        m_preview_match_panel->SetBackgroundColour(wxColour(245, 245, 245));
        rcol->Add(m_preview_match_panel, 0, wxEXPAND);
        prow->Add(rcol, 4, wxEXPAND);

        root->Add(prow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
    }

    // ---- Scroll container (mapping + prompt) ----
    m_scrolled_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolled_content->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    m_scrolled_content->SetScrollRate(0, FromDIP(8));
    m_scrolled_content->Bind(wxEVT_CHILD_FOCUS, [](wxChildFocusEvent&) {});
    auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);

    // ---- 颜色映射 ----
    {
        auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        card->SetCornerRadius(FromDIP(4));
        card->SetBorderWidth(FromDIP(1));
        card->SetBorderColorNormal(wxColour("#F0F0F0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        auto* cs = new wxBoxSizer(wxVERTICAL);
        cs->Add(new Label(card, _L("颜色映射")), 0, wxALL, M);

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
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer();
        m_btn_cancel_match = new Button(this, _L("取消"));
        m_btn_cancel_match->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        row->Add(m_btn_cancel_match, 0, wxRIGHT, M2);
        m_btn_confirm = new Button(this, _L("确定"));
        m_btn_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
        row->Add(m_btn_confirm, 0);

        m_btn_cancel_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

        root->Add(row, 0, wxEXPAND | wxALL, M);
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
    if (m_manual_card) {
        m_manual_card->Show(m_matching_method == MANUAL);
        if (m_matching_method == MANUAL) {
            m_manual_card->Layout();
            m_manual_card->GetParent()->Layout();
        }
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
    if (m_matching_method == MANUAL) {
        for (int i = 0; i < 4; ++i) {
            int sel = m_filament_combo[i] ? m_filament_combo[i]->GetSelection() : i;
            if (sel >= 0 && sel < (int)m_physical_colors.size())
                active_colors.push_back(m_physical_colors[sel]);
        }
        if (active_colors.size() < 2) active_colors = m_physical_colors;
    }
    const auto manual_colors = std::move(active_colors);
    const auto all_physical  = m_physical_colors;

    std::vector<std::string> all_preset_colors;
    if (m_matching_method == RECOMMENDED) {
        auto* pb = wxGetApp().preset_bundle;
        if (pb) {
            for (const std::string& alias : pb->filament_presets) {
                const Preset* preset = pb->filaments.find_preset(alias);
                if (!preset) continue;
                auto* opt = preset->config.option<ConfigOptionStrings>("filament_colour");
                if (opt && !opt->values.empty())
                    all_preset_colors.push_back(opt->values[0]);
            }
        }
    }
    const auto preset_colors  = std::move(all_preset_colors);
    const auto matching_method = m_matching_method;

    auto destroyed = m_destroyed;
    auto cancel_token = m_cancel_requested;
    auto progress_bar = m_progress_bar;

    m_worker_thread = std::thread([this, model_colors, manual_colors, all_physical,
                                    preset_colors, matching_method,
                                    destroyed, cancel_token, progress_bar]()
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
        try {
            result = batch_match_model_colors(model_colors, physical_colors, 15, cancel_token,
                [progress_bar, destroyed](int done, int total) {
                    if (progress_bar && !destroyed->load()) {
                        wxGetApp().CallAfter([progress_bar, done, total, destroyed]() {
                            if (destroyed->load()) return;
                            if (total > 0) progress_bar->SetValue(done * 100 / total);
                        });
                    }
                });
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("Error: ") + e.what();
            result.error_code = 3;
        } catch (...) {
            result.success = false;
            result.error_message = "Unknown error during batch match";
            result.error_code = 3;
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
    set_match_buttons_state(false);

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
    update_mapping_legend();
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
}

}} // namespace Slic3r::GUI
