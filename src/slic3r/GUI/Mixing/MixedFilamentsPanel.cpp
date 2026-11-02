// MixedFilamentsPanel 实现（阶段 B2，手册 §2/§3）。
// 迁移纪律：四方法 + 混色标题栏构造块整体自 Sidebar.cpp 剪切粘贴；
// 宿主解引用改写仅限：p->m_* → m_*、p->combos_filament → m_host、m_bpButton_add_filament → m_host、
// m_scrolled_sizer → m_host、FromDIP → m_owner->FromDIP、Plater 操作 → m_host 回调。
// 依赖规则：本 cpp 禁 include Plater.hpp（B1 护栏）；GUI_App.hpp 为 B1 既有先例（wxGetApp）。
#include "MixedFilamentsPanel.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>

#include <boost/log/trivial.hpp>

#include "../GUI.hpp"                     // show_error
#include "../GUI_App.hpp"                 // wxGetApp()（B1 既有 Mixing 先例）
#include "../I18N.hpp"                    // _L
#include "../MainFrame.hpp"               // mainframe->on_config_changed / PopupMenu
#include "../MixedFilamentBadge.hpp"      // MixedFilamentBadge（GUI 根目录）
#include "../MixedFilamentDialog.hpp"     // MixedFilamentDialog（GUI 根目录）
#include "MixedFilamentConfigPanel.hpp"   // B1 共享 helper 声明（D3 出头文件）
#include "MixedFilamentDragHandle.hpp"    // build_mixed_filament_ui_indices
#include "../Sidebar.hpp"                 // SidebarProps（cpp-only；hpp 侧零宿主依赖）
#include "../Tab.hpp"                     // print_tab->update_dirty
#include "../wxExtensions.hpp"            // ScalableButton
#include "MixingColorMatchDialog.hpp"     // prompt_best_color_match_recipe
#include "../MixedColorMatchHelpers.hpp"  // build_mixed_filament_display_context 等（GUI 根目录）
#include "../Widgets/Button.hpp"
#include "../Widgets/Label.hpp"
#include "../Widgets/StateColor.hpp"
#include "../Widgets/StaticBox.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

MixedFilamentsPanel::MixedFilamentsPanel(wxWindow* owner, MixedFilamentsPanelHost host)
    : m_owner(owner), m_host(std::move(host))
{
}

// Sidebar::msw_rescale / sys_color_changed 转发（原 Sidebar::priv::m_color_mix_icon 直接访问）
void MixedFilamentsPanel::msw_rescale()
{
    if (m_color_mix_icon)
        m_color_mix_icon->msw_rescale();
}

// Sidebar 侧“组合耗材上限”联动（原 Sidebar::priv::m_btn_add_color_mix 直接访问）
void MixedFilamentsPanel::set_add_color_mix_enabled(bool enabled)
{
    if (m_btn_add_color_mix)
        m_btn_add_color_mix->Enable(enabled);
}

// 原 Sidebar 构造器内「Mixed Filaments Panel (Collapsible)」块逐字迁入
void MixedFilamentsPanel::init_mixed_filaments_panel(wxWindow* scrolled, wxBoxSizer* scrolled_sizer, const wxColour& title_bg)
{
    // Create title bar (StaticBox for collapsible header)
    m_panel_mixed_filaments_title = new StaticBox(scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_panel_mixed_filaments_title->SetBackgroundColor(title_bg);
    m_panel_mixed_filaments_title->SetBackgroundColor2(0xF1F1F1);

    // Create icon
    m_mixed_filaments_icon = new ScalableButton(m_panel_mixed_filaments_title, wxID_ANY, "filament");

    // Create title text
    m_staticText_mixed_filaments = new Label(m_panel_mixed_filaments_title, _L("Mixed Filaments"), LB_PROPAGATE_MOUSE_EVENT);

    // Create "Add Gradient" button
    m_btn_add_gradient = new Button(m_panel_mixed_filaments_title, _L("Add Gradient"));
    m_btn_add_gradient->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    m_btn_add_gradient->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        // Add gradient mixed filament
        if (wxGetApp().preset_bundle) {
            auto &mgr = wxGetApp().preset_bundle->mixed_filaments;
            // Get physical filament colors
            ConfigOptionStrings *co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            std::vector<std::string> colors = co ? co->values : std::vector<std::string>();
            // Add a custom gradient (50% mix)
            mgr.add_custom_filament(1, 2, 50, colors);
            // Persist the custom entries so they survive the clear/load cycle in update_mixed_filament_panel
            if (ConfigOptionString *opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                opt->value = mgr.serialize_custom_entries();
            update_mixed_filament_panel(false);
            m_host.layout_scrolled();
        }
    });

    // Create "Add Pattern" button
    m_btn_add_pattern = new Button(m_panel_mixed_filaments_title, _L("Add Pattern"));
    m_btn_add_pattern->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    m_btn_add_pattern->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        // Add pattern mixed filament
        if (wxGetApp().preset_bundle) {
            auto &mgr = wxGetApp().preset_bundle->mixed_filaments;
            // Get physical filament colors
            ConfigOptionStrings *co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            std::vector<std::string> colors = co ? co->values : std::vector<std::string>();
            // Add a custom pattern filament (will be configured by user)
            mgr.add_custom_filament(1, 2, 50, colors);
            // Set manual pattern for the newly added filament
            auto &mfs = mgr.mixed_filaments();
            if (!mfs.empty()) {
                mfs.back().manual_pattern = "12";
                mfs.back().custom = true;
            }
            // Persist the custom entries so they survive the clear/load cycle in update_mixed_filament_panel
            if (ConfigOptionString *opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                opt->value = mgr.serialize_custom_entries();
            update_mixed_filament_panel(false);
            m_host.layout_scrolled();
        }
    });

    // Create "Add Color" button
    m_btn_add_color = new Button(m_panel_mixed_filaments_title, _L("Add Color"));
    m_btn_add_color->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    m_btn_add_color->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxGetApp().preset_bundle == nullptr)
            return;

        ConfigOptionStrings *co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const std::vector<std::string> colors = co ? co->values : std::vector<std::string>();
        if (colors.size() < 2)
            return;

        const MixedColorMatchRecipeResult recipe =
            prompt_best_color_match_recipe(m_owner, colors, m_host.next_color_for_filament());
        if (recipe.cancelled)
            return;
        if (!recipe.valid) {
            show_error(m_owner, _L("Unable to create a color match from the current physical filament colors."));
            return;
        }

        const MixedFilamentDisplayContext display_context = build_mixed_filament_display_context(colors);
        auto &mgr = wxGetApp().preset_bundle->mixed_filaments;
        mgr.set_display_context(display_context);
        mgr.add_custom_filament(recipe.component_a, recipe.component_b, recipe.mix_b_percent, colors);
        auto &mfs = mgr.mixed_filaments();
        if (!mfs.empty()) {
            MixedFilament &created = mfs.back();
            created.manual_pattern = recipe.manual_pattern;
            created.mix_b_percent  = recipe.mix_b_percent;
            created.gradient_component_ids = recipe.gradient_component_ids;
            created.gradient_component_weights = recipe.gradient_component_weights;
            created.pointillism_all_filaments = false;
            created.distribution_mode = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
            created.custom = true;
            created.display_color = compute_color_match_recipe_display_color(recipe, display_context).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        }

        if (ConfigOptionString *opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
            opt->value = mgr.serialize_custom_entries();
        update_mixed_filament_panel(false);
        m_host.layout_scrolled();
    });

    // Create "Add Color" button
    wxBoxSizer* h_sizer_mixed_title = new wxBoxSizer(wxHORIZONTAL);
    h_sizer_mixed_title->Add(m_mixed_filaments_icon, 0, wxALIGN_CENTER | wxLEFT, m_owner->FromDIP(SidebarProps::TitlebarMargin()));
    h_sizer_mixed_title->AddSpacer(m_owner->FromDIP(SidebarProps::ElementSpacing()));
    h_sizer_mixed_title->Add(m_staticText_mixed_filaments, 0, wxALIGN_CENTER);
    h_sizer_mixed_title->AddStretchSpacer();
    h_sizer_mixed_title->Add(m_btn_add_gradient, 0, wxALIGN_CENTER | wxRIGHT, m_owner->FromDIP(SidebarProps::ElementSpacing()));
    h_sizer_mixed_title->Add(m_btn_add_pattern, 0, wxALIGN_CENTER | wxRIGHT, m_owner->FromDIP(SidebarProps::ElementSpacing()));
    h_sizer_mixed_title->Add(m_btn_add_color, 0, wxALIGN_CENTER | wxRIGHT, m_owner->FromDIP(SidebarProps::TitlebarMargin()));
    h_sizer_mixed_title->SetMinSize(-1, m_owner->FromDIP(30));

    m_panel_mixed_filaments_title->SetSizer(h_sizer_mixed_title);
    m_panel_mixed_filaments_title->Layout();

    // Add splitter line before title
    auto spliter_mixed_1 = new ::StaticLine(scrolled);
    spliter_mixed_1->SetLineColour("#A6A9AA");
    scrolled_sizer->Add(spliter_mixed_1, 0, wxEXPAND);

    // Add title bar to scrolled sizer
    scrolled_sizer->Add(m_panel_mixed_filaments_title, 0, wxEXPAND | wxALL, 0);

    // Add splitter line after title
    auto spliter_mixed_2 = new ::StaticLine(scrolled);
    spliter_mixed_2->SetLineColour("#CECECE");
    scrolled_sizer->Add(spliter_mixed_2, 0, wxEXPAND);

    // Create content panel (collapsible)
    m_panel_mixed_filaments_content = new wxPanel(scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    {
        const bool is_dark = wxGetApp().dark_mode();
        m_panel_mixed_filaments_content->SetBackgroundColour(is_dark ? wxColour(45, 45, 49) : wxColour(255, 255, 255));
    }

    // Content sizer - store in member variable for later use
    m_sizer_mixed_filaments_content = new wxBoxSizer(wxVERTICAL);
    m_sizer_mixed_filaments_content->AddSpacer(m_owner->FromDIP(SidebarProps::ContentMargin()));
    m_panel_mixed_filaments_content->SetSizer(m_sizer_mixed_filaments_content);
    m_panel_mixed_filaments_content->Layout();

    // Add content panel to scrolled sizer
    scrolled_sizer->Add(m_panel_mixed_filaments_content, 0, wxEXPAND, 0);

    // Bind collapse/expand event to title bar
    m_panel_mixed_filaments_title->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        // Exclude button areas from collapse/expand
        int button_left = m_panel_mixed_filaments_title->GetClientSize().x;
        auto consider_button = [&button_left](wxWindow *button) {
            if (button && button->IsShown())
                button_left = std::min(button_left, button->GetPosition().x);
        };
        consider_button(m_btn_add_gradient);
        consider_button(m_btn_add_pattern);
        consider_button(m_btn_add_color);
        if (e.GetPosition().x > button_left - m_owner->FromDIP(12))
            return;
        
        const bool expanded = m_panel_mixed_filaments_content->IsShown();
        m_panel_mixed_filaments_content->Show(!expanded);
        m_panel_mixed_filaments_content->SetMaxSize(expanded ? wxSize(-1, 0) : wxSize(-1, -1));
        m_host.layout_scrolled();
    });

    // Initially hidden until 2+ filaments
    m_panel_mixed_filaments_title->Hide();
    m_panel_mixed_filaments_content->Hide();
}

void MixedFilamentsPanel::init_color_mix_panel(wxWindow* parent, wxSizer* sizer)
{
    // Title bar
    m_panel_color_mix_title = new StaticBox(parent, wxID_ANY, wxDefaultPosition,
                                               wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_panel_color_mix_title->SetBackgroundColor(wxColour(248, 248, 248));
    m_panel_color_mix_title->SetBackgroundColor2(0xF1F1F1);
    m_panel_color_mix_title->SetMinSize(wxSize(-1, m_owner->FromDIP(30)));
    m_panel_color_mix_title->SetMaxSize(wxSize(-1, m_owner->FromDIP(30)));

    m_color_mix_icon = new ScalableButton(m_panel_color_mix_title, wxID_ANY, "color_palette");
    auto* label = new Label(m_panel_color_mix_title, _L("Color Mixing"), LB_PROPAGATE_MOUSE_EVENT);

    m_btn_del_color_mix = new ScalableButton(m_panel_color_mix_title, wxID_ANY, "delete_filament");
    m_btn_add_color_mix = new ScalableButton(m_panel_color_mix_title, wxID_ANY, "add_filament");

    auto* h_title = new wxBoxSizer(wxHORIZONTAL);
    auto* white_left_c = new wxPanel(m_panel_color_mix_title, wxID_ANY, wxDefaultPosition, wxSize(m_owner->FromDIP(SidebarProps::ContentMargin()), -1));
    white_left_c->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    h_title->Add(white_left_c, 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
    h_title->Add(m_color_mix_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, m_owner->FromDIP(SidebarProps::TitlebarMargin()));
    h_title->AddSpacer(m_owner->FromDIP(SidebarProps::ElementSpacing()));
    h_title->Add(label, 0, wxALIGN_CENTER_VERTICAL);
    h_title->AddStretchSpacer();
    h_title->Add(m_btn_del_color_mix, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_owner->FromDIP(4));
    h_title->Add(m_btn_add_color_mix, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_owner->FromDIP(8));
    auto* white_right_c = new wxPanel(m_panel_color_mix_title, wxID_ANY, wxDefaultPosition, wxSize(m_owner->FromDIP(SidebarProps::ContentMargin()), -1));
    white_right_c->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    h_title->Add(white_right_c, 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
    m_panel_color_mix_title->SetSizer(h_title);
    m_panel_color_mix_title->Layout();

    // Scrolled window for content with max height of 3 rows
    m_scrolled_color_mix = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolled_color_mix->SetScrollRate(0, 5);
    m_scrolled_color_mix->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    // Content panel — match physical filament content panel (sizer set dynamically in update)
    m_panel_color_mix_content = new wxPanel(m_scrolled_color_mix, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_color_mix_content->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    // Add content panel to scrolled window
    auto* scrolled_sizer = new wxBoxSizer(wxVERTICAL);
    scrolled_sizer->Add(m_panel_color_mix_content, 0, wxEXPAND);
    m_scrolled_color_mix->SetSizer(scrolled_sizer);

    sizer->Add(m_panel_color_mix_title, 0, wxEXPAND, 0);
    sizer->Add(m_scrolled_color_mix, 0, wxEXPAND, 0);

    // Add button: open dialog to create new mix
    m_btn_add_color_mix->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        PresetBundle* pb = wxGetApp().preset_bundle;
        if (!pb) return;

        auto* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
        const std::vector<std::string> colors = co ? co->values : std::vector<std::string>{};
        if (colors.size() < 2) return;

        MixedFilamentDialog dlg(wxGetApp().mainframe, colors);
        if (dlg.ShowModal() != wxID_OK) return;

        auto& mgr = pb->mixed_filaments;
        if (mgr.total_filaments(colors.size()) >= MAXIMUM_FILAMENT_NUMBER) return;
        const MixedFilament& r = dlg.GetResult();
        mgr.add_custom_filament(r.component_a, r.component_b, r.mix_b_percent, colors);
        auto& mfs = mgr.mixed_filaments();
        if (!mfs.empty()) {
            mfs.back().distribution_mode       = r.distribution_mode;
            mfs.back().manual_pattern          = r.manual_pattern;
            mfs.back().gradient_component_ids      = r.gradient_component_ids;
            mfs.back().gradient_component_weights  = r.gradient_component_weights;
            mfs.back().ratio_a                     = r.ratio_a;
            mfs.back().ratio_b                     = r.ratio_b;
            mfs.back().local_z_max_sublayers       = r.local_z_max_sublayers;
            mfs.back().gradient_enabled            = r.gradient_enabled;
            mfs.back().gradient_start              = r.gradient_start;
            mfs.back().gradient_end                = r.gradient_end;
            mfs.back().display_color             = r.display_color;
            mfs.back().ui_mode                       = r.ui_mode;
            mfs.back().custom                  = true;
        }
        if (auto* opt = pb->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
            opt->value = mgr.serialize_custom_entries();
        m_host.plater_post_slice_state_change_update();
        m_host.plater_on_filaments_change(m_host.filament_count());
        update_color_mix_panel();
        m_host.layout_scrolled();
    });

    // Delete button: remove last custom entry
    m_btn_del_color_mix->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        PresetBundle* pb = wxGetApp().preset_bundle;
        if (!pb) return;

        auto& mgr = pb->mixed_filaments;
        auto& mfs = mgr.mixed_filaments();
        for (int i = static_cast<int>(mfs.size()) - 1; i >= 0; --i) {
            if (mfs[i].custom && !mfs[i].deleted) {
                mfs[i].deleted = true;
                break;
            }
        }
        if (auto* opt = pb->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
            opt->value = mgr.serialize_custom_entries();
        m_host.plater_post_slice_state_change_update();
        m_host.plater_on_filaments_change(m_host.filament_count());
        update_color_mix_panel();
        m_host.layout_scrolled();
    });

    // Initial visibility: hide if fewer than 2 physical filaments
    update_color_mix_panel();
}

void MixedFilamentsPanel::update_color_mix_panel()
{
    if (!m_panel_color_mix_content) return;

    auto* co = wxGetApp().preset_bundle
                   ? wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour")
                   : nullptr;
    const int n_physical = co ? static_cast<int>(co->values.size()) : 0;
    const bool show = (n_physical >= 2);
    m_panel_color_mix_title->Show(show);
    m_scrolled_color_mix->Show(show);
    if (!show) {
        return;
    }

    wxWindowUpdateLocker no_updates(m_panel_color_mix_content);
    m_panel_color_mix_content->DestroyChildren();

    auto* preset_bundle = wxGetApp().preset_bundle;
    const size_t num_physical = m_host.filament_count();

    std::vector<std::string> physical_colors = co->values;
    physical_colors.resize(num_physical, "#26A69A");

    std::vector<double> nozzle_diameters(num_physical, 0.4);
    if (const ConfigOptionFloats* opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0)
            for (size_t i = 0; i < num_physical; ++i)
                nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
    }

    float lower_bound = 0.04f, upper_bound = 0.16f;
    if (preset_bundle->project_config.has("mixed_filament_height_lower_bound"))
        lower_bound = std::max(0.01f, float(preset_bundle->project_config.opt_float("mixed_filament_height_lower_bound")));
    if (preset_bundle->project_config.has("mixed_filament_height_upper_bound"))
        upper_bound = std::max(lower_bound, float(preset_bundle->project_config.opt_float("mixed_filament_height_upper_bound")));

    bool local_z_mode = false;
    if (const ConfigOptionBool* opt = preset_bundle->project_config.option<ConfigOptionBool>("dithering_local_z_mode"))
        local_z_mode = opt->value;

    bool component_bias_enabled = false;
    if (const ConfigOptionBool* opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_component_bias_enabled"))
        component_bias_enabled = opt->value;

    const MixedFilamentPreviewSettings preview_settings {
        0.2f, lower_bound, upper_bound, 0.f, 0.f, local_z_mode, false, 1
    };
    const MixedFilamentDisplayContext display_context {
        num_physical, physical_colors, nozzle_diameters, preview_settings, component_bias_enabled
    };
    preset_bundle->mixed_filaments.set_display_context(display_context);

    auto& mfs = preset_bundle->mixed_filaments.mixed_filaments();
    bool any_visible = false;
    for (const MixedFilament& mf : mfs)
        if (!mf.deleted) { any_visible = true; break; }

    if (!any_visible) {
        m_scrolled_color_mix->Hide();
        m_btn_del_color_mix->Hide();
        m_btn_add_color_mix->SetBitmap_("icon_add_circle");
        m_host.layout_scrolled();
        return;
    }
    m_scrolled_color_mix->Show();
    m_btn_del_color_mix->Show();
    m_btn_add_color_mix->SetBitmap_("add_filament");

    // 2-column grid matching the physical filament panel layout
    auto* grid_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* col0 = new wxBoxSizer(wxVERTICAL);
    auto* col1 = new wxBoxSizer(wxVERTICAL);
    grid_sizer->Add(col0, 1, wxEXPAND);
    grid_sizer->Add(col1, 1, wxEXPAND);

    int visible_idx = 0;
    for (size_t i = 0; i < mfs.size(); ++i) {
        MixedFilament& mf = mfs[i];
        if (mf.deleted) continue;

        const std::string synced_color = compute_mixed_filament_display_color(mf, display_context);
        if (mf.display_color != synced_color)
            mf.display_color = synced_color;

        const int virtual_id = static_cast<int>(num_physical) + visible_idx + 1;

        // Badge button: colored background + virtual filament number with gradient support
        auto* badge = new MixedFilamentBadge(m_panel_color_mix_content, wxID_ANY,
                                             virtual_id, mf, display_context);

        const std::string normalized_pattern_cm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        std::vector<unsigned int> gradient_ids = MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, 0);
        const bool z_gradient_tile = mf.gradient_enabled && mf.component_a != mf.component_b
                                  && normalized_pattern_cm.empty() && gradient_ids.size() < 3;
        wxString lbl;
        if (!normalized_pattern_cm.empty())
            lbl = wxString(summarize_cycle_pattern_text(normalized_pattern_cm, mf, int(num_physical)));
        else if (gradient_ids.size() >= 3) {
            // parse weights
            const size_t n = gradient_ids.size();
            std::vector<int> weights;
            {
                std::string token;
                for (const char c : mf.gradient_component_weights) {
                    if (c >= '0' && c <= '9') { token.push_back(c); continue; }
                    if (!token.empty()) { weights.emplace_back(std::max(0, std::atoi(token.c_str()))); token.clear(); }
                }
                if (!token.empty()) weights.emplace_back(std::max(0, std::atoi(token.c_str())));
                if (weights.size() != n) weights.assign(n, int(100 / n));
            }
            // normalize to 100
            int sum = 0; for (int v : weights) sum += v;
            if (sum <= 0) { weights.assign(n, 0); weights[0] = 100; sum = 100; }
            for (size_t k = 0; k < n; ++k) {
                const unsigned int fid = gradient_ids[k];
                const int pct = int(std::round(100.0 * weights[k] / sum));
                if (k > 0) lbl += "+";
                lbl += wxString::Format("F%u %d%%", fid, pct);
            }
        } else if (z_gradient_tile) {
            const unsigned from_id =
                mf.gradient_start >= mf.gradient_end ? mf.component_a : mf.component_b;
            const unsigned to_id =
                mf.gradient_start >= mf.gradient_end ? mf.component_b : mf.component_a;
            lbl = wxString::Format("F%u->F%u", from_id, to_id);
        } else {
            const int pct_b = std::clamp(mf.mix_b_percent, 0, 100);
            const int pct_a = 100 - pct_b;
            lbl = wxString::Format("F%u %d%%+F%u %d%%", mf.component_a, pct_a, mf.component_b, pct_b);
            if (mf.distribution_mode != int(MixedFilament::Simple))
                for (unsigned int fid : gradient_ids)
                    lbl += wxString::Format("+F%u", fid);
        }
        
        bool has_error = !is_filament_compatible(mf);
        
        // Create a panel with border for the text
        auto* name_panel = new wxPanel(m_panel_color_mix_content, wxID_ANY);
        name_panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

        auto* name_sizer = new wxBoxSizer(wxHORIZONTAL);

        // Add error icon if there's an error
        if (has_error) {
            name_sizer->AddSpacer(m_owner->FromDIP(8));
            ScalableBitmap error_bmp(name_panel, "error_icon_red_exclamation", 14);
            auto* error_icon = new wxStaticBitmap(name_panel, wxID_ANY, error_bmp.bmp());
            name_sizer->Add(error_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_owner->FromDIP(4));
        }

        auto* name_btn = new wxStaticText(name_panel, wxID_ANY, lbl, wxDefaultPosition, wxDefaultSize, 0);
        name_btn->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        name_btn->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
        name_btn->SetCursor(wxCursor(wxCURSOR_HAND));
        name_btn->SetMinSize(wxSize(0, -1)); // allow sizer to shrink below text width

        int name_flags = wxEXPAND | (has_error ? (wxTOP | wxBOTTOM | wxRIGHT) : wxALL);
        name_sizer->Add(name_btn, 1, name_flags, m_owner->FromDIP(8));
        name_panel->SetSizer(name_sizer);
        name_panel->SetMinSize(wxSize(name_panel->FromDIP(100), name_panel->FromDIP(30)));
        name_panel->SetMaxSize(wxSize(-1, name_panel->FromDIP(30)));

        // Use wxControl::Ellipsize on resize to match combo truncation behavior
        name_panel->Bind(wxEVT_SIZE, [lbl, name_btn, name_panel](wxSizeEvent& evt) {
            name_panel->Layout(); // force sizer layout so name_btn has its current size
            int avail = name_btn->GetSize().x;
            if (avail > 0) {
                wxClientDC dc(name_btn);
                dc.SetFont(name_btn->GetFont());
                wxString ellipsized = wxControl::Ellipsize(lbl, dc, wxELLIPSIZE_END, avail);
                name_btn->SetLabel(ellipsized);
            }
            evt.Skip();
        });

        // Add border to the panel
        name_panel->Bind(wxEVT_PAINT, [](wxPaintEvent& evt) {
            wxPanel* panel = dynamic_cast<wxPanel*>(evt.GetEventObject());
            if (!panel) return;

            wxPaintDC dc(panel);
            wxRect rect = panel->GetClientRect();

            // Draw border matching combo (#dbdbdb, 1px)
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(0xdb, 0xdb, 0xdb)), 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(rect);
        });

        name_btn->SetToolTip(lbl);
        name_btn->Bind(wxEVT_LEFT_DOWN, [this, i](wxMouseEvent&) {
            auto* co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            const std::vector<std::string> colors = co ? co->values : std::vector<std::string>{};
            if (colors.size() < 2) return;
            auto& mgr = wxGetApp().preset_bundle->mixed_filaments;
            auto& mfs2 = mgr.mixed_filaments();
            if (i >= mfs2.size()) return;
            MixedFilamentDialog dlg(wxGetApp().mainframe, colors, mfs2[i]);
            if (dlg.ShowModal() != wxID_OK) return;
            const MixedFilament& r = dlg.GetResult();
            mfs2[i].component_a                = r.component_a;
            mfs2[i].component_b                = r.component_b;
            mfs2[i].mix_b_percent              = r.mix_b_percent;
            mfs2[i].distribution_mode          = r.distribution_mode;
            mfs2[i].manual_pattern             = r.manual_pattern;
            mfs2[i].gradient_component_ids     = r.gradient_component_ids;
            mfs2[i].gradient_component_weights = r.gradient_component_weights;
            mfs2[i].ratio_a                    = r.ratio_a;
            mfs2[i].ratio_b                    = r.ratio_b;
            mfs2[i].local_z_max_sublayers      = r.local_z_max_sublayers;
            mfs2[i].gradient_enabled           = r.gradient_enabled;
            mfs2[i].gradient_start             = r.gradient_start;
            mfs2[i].gradient_end               = r.gradient_end;
            mfs2[i].display_color               = r.display_color;
            mfs2[i].ui_mode                       = r.ui_mode;
            mfs2[i].custom                      = true;
            if (auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                opt->value = mgr.serialize_custom_entries();
            m_host.plater_post_slice_state_change_update();
            m_host.plater_on_filaments_change(m_host.filament_count());
            wxWeakRef<MixedFilamentsPanel> weak_this(this);
            wxTheApp->CallAfter([weak_this]() {
                MixedFilamentsPanel* panel = weak_this.get();
                if (panel) {
                    panel->update_color_mix_panel();
                    panel->layout_scrolled();
                }
            });
        });

        auto* menu_btn = new ScalableButton(m_panel_color_mix_content, wxID_ANY, "menu_filament");
        menu_btn->SetToolTip(_L("Options"));
        menu_btn->Bind(wxEVT_BUTTON, [this, i, visible_idx, num_physical, menu_btn](wxCommandEvent&) {
            wxMenu menu;
            const int edit_id = wxWindow::NewControlId();
            const int del_id  = wxWindow::NewControlId();
            const int merge_to_id = wxWindow::NewControlId();
            
            menu.Append(edit_id, _L("Edit"));
            menu.Bind(wxEVT_MENU, [this, i](wxCommandEvent&) {
                auto* co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
                const std::vector<std::string> colors = co ? co->values : std::vector<std::string>{};
                if (colors.size() < 2) return;
                auto& mgr = wxGetApp().preset_bundle->mixed_filaments;
                auto& mfs2 = mgr.mixed_filaments();
                if (i >= mfs2.size()) return;
                MixedFilamentDialog dlg(wxGetApp().mainframe, colors, mfs2[i]);
                if (dlg.ShowModal() != wxID_OK) return;
                const MixedFilament& r = dlg.GetResult();
                mfs2[i].component_a                = r.component_a;
                mfs2[i].component_b                = r.component_b;
                mfs2[i].mix_b_percent              = r.mix_b_percent;
                mfs2[i].distribution_mode          = r.distribution_mode;
                mfs2[i].manual_pattern             = r.manual_pattern;
                mfs2[i].gradient_component_ids     = r.gradient_component_ids;
                mfs2[i].gradient_component_weights = r.gradient_component_weights;
                mfs2[i].ratio_a                    = r.ratio_a;
                mfs2[i].ratio_b                    = r.ratio_b;
                mfs2[i].local_z_max_sublayers      = r.local_z_max_sublayers;
                mfs2[i].gradient_enabled           = r.gradient_enabled;
                mfs2[i].gradient_start             = r.gradient_start;
                mfs2[i].gradient_end               = r.gradient_end;
                mfs2[i].ui_mode                       = r.ui_mode;
                mfs2[i].custom                      = true;
                if (auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                    opt->value = mgr.serialize_custom_entries();
                m_host.plater_post_slice_state_change_update();
                m_host.plater_on_filaments_change(m_host.filament_count());
                wxWeakRef<MixedFilamentsPanel> weak_this(this);
                wxTheApp->CallAfter([weak_this]() {
                    MixedFilamentsPanel* panel = weak_this.get();
                    if (panel) {
                        panel->update_color_mix_panel();
                        panel->layout_scrolled();
                    }
                });
            }, edit_id);

            // Add "Merge with" submenu - allows merging to any other filament (physical or mixed)
            // Build list of all available target filaments
            wxMenu* merge_submenu = new wxMenu();
            
            // Get physical filament icons
            std::vector<wxBitmap*> icons = get_extruder_color_icons(true);
            
            // Add physical filaments as targets
            for (size_t phys_idx = 0; phys_idx < num_physical; ++phys_idx) {
                const int target_id = wxWindow::NewControlId();
                auto preset = wxGetApp().preset_bundle->filaments.find_preset(wxGetApp().preset_bundle->filament_presets[phys_idx]);
                wxString target_label = preset ? from_u8(preset->label(false)) : wxString::Format(_L("Filament %d"), phys_idx + 1);
                
                // Use icon if available
                wxMenuItem* item = nullptr;
                if (phys_idx < icons.size() && icons[phys_idx]) {
                    item = new wxMenuItem(merge_submenu, target_id, target_label);
                    item->SetBitmap(*icons[phys_idx]);
                    merge_submenu->Append(item);
                } else {
                    merge_submenu->Append(target_id, target_label);
                }
                
                merge_submenu->Bind(wxEVT_MENU, [this, visible_idx, phys_idx, num_physical](wxCommandEvent&) {
                    // Source: mixed filament with visible_idx (0-based in visible list)
                    // Target: physical filament phys_idx (0-based)
                    // Mixed filament virtual ID = num_physical + visible_idx + 1 (1-based), convert to 0-based
                    size_t source_virtual_id = num_physical + visible_idx;
                    m_host.change_filament(source_virtual_id, phys_idx);
                }, target_id);
            }
            
            // Add other mixed filaments as targets
            auto& mgr = wxGetApp().preset_bundle->mixed_filaments;
            auto& mfs_for_menu = mgr.mixed_filaments();
            const size_t total_mixed = mfs_for_menu.size();
            
            // Get icon dimensions for mixed filaments
            const double em = Slic3r::GUI::wxGetApp().em_unit();
            const int icon_width = lround(2 * em);
            const int icon_height = lround(2 * em);
            
            size_t target_visible_idx = 0;
            for (size_t j = 0; j < total_mixed; ++j) {
                if (mfs_for_menu[j].deleted) continue;
                
                // Skip self (compare by visible index)
                if (target_visible_idx == visible_idx) {
                    target_visible_idx++;
                    continue;
                }
                
                const int target_virtual_id = static_cast<int>(num_physical) + target_visible_idx + 1;
                const wxString target_label = wxString::Format(_L("Mixed Filament %d"), target_virtual_id);
                const int target_id = wxWindow::NewControlId();
                
                // Create colored bitmap for mixed filament — gradient filaments get a gradient icon
                MixedFilamentDisplayContext menu_ctx;
                {
                    auto* co2 = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
                    menu_ctx.physical_colors = co2 ? co2->values : std::vector<std::string>{};
                    menu_ctx.num_physical = num_physical;
                }
                wxBitmap* mixed_bmp = create_mixed_filament_menu_bitmap(
                    mfs_for_menu[j], menu_ctx, icon_width, icon_height,
                    wxString::Format("%d", target_virtual_id));

                wxMenuItem* item = new wxMenuItem(merge_submenu, target_id, target_label);
                item->SetBitmap(*mixed_bmp);
                merge_submenu->Append(item);
                
                merge_submenu->Bind(wxEVT_MENU, [this, visible_idx, target_visible_idx, num_physical](wxCommandEvent&) {
                    // Source: mixed filament with visible_idx (0-based in visible list)
                    // Target: mixed filament with target_visible_idx (0-based in visible list)
                    // Virtual ID (0-based) = num_physical + visible_idx
                    size_t source_virtual_id = num_physical + visible_idx;
                    size_t target_virtual_id = num_physical + target_visible_idx;
                    m_host.change_filament(source_virtual_id, target_virtual_id);
                }, target_id);
                
                target_visible_idx++;
            }
            
            menu.AppendSubMenu(merge_submenu, _L("Merge with"));
            
            menu.Append(del_id, _L("Delete"));
            menu.Bind(wxEVT_MENU, [this, i, num_physical](wxCommandEvent&) {
                auto& mgr2 = wxGetApp().preset_bundle->mixed_filaments;
                auto& mfs2 = mgr2.mixed_filaments();
                const std::vector<MixedFilament> old_mixed = mfs2;
                if (i < mfs2.size()) { mfs2[i].deleted = true; mfs2[i].enabled = false; }
                if (auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                    opt->value = mgr2.serialize_custom_entries();
                wxGetApp().preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical, i);
                m_host.plater_post_slice_state_change_update();
                m_host.plater_on_filaments_change(num_physical);
                wxWeakRef<MixedFilamentsPanel> weak_this(this);
                wxTheApp->CallAfter([weak_this]() {
                    MixedFilamentsPanel* panel = weak_this.get();
                    if (panel) {
                        panel->update_color_mix_panel();
                        panel->layout_scrolled();
                    }
                });
            }, del_id);
            
            wxPoint pt{0, menu_btn->GetSize().GetHeight()};
            pt = menu_btn->ClientToScreen(pt);
            pt = wxGetApp().mainframe->ScreenToClient(pt);
            wxGetApp().mainframe->PopupMenu(&menu, pt);
        });

        auto* cell = new wxBoxSizer(wxHORIZONTAL);
        // Match physical filament asymmetric layout: left column gets left spacer, right doesn't
        if (visible_idx % 2 == 0)
            cell->AddSpacer(m_owner->FromDIP(SidebarProps::ContentMargin()));
        cell->Add(badge,    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_owner->FromDIP(SidebarProps::ElementSpacing()) - m_owner->FromDIP(2));
        cell->Add(name_panel, 1, wxEXPAND | wxALL, m_owner->FromDIP(2));
        cell->Add(menu_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, m_owner->FromDIP(SidebarProps::ElementSpacing()) - m_owner->FromDIP(2));
        cell->AddSpacer(m_owner->FromDIP(SidebarProps::ContentMargin()));

        (visible_idx % 2 == 0 ? col0 : col1)->Add(cell, 0, wxEXPAND | wxBOTTOM, m_owner->FromDIP(4));

        ++visible_idx;
    }

    // If odd count, pad right column so left column doesn't stretch
    if (visible_idx % 2 == 1)
        col1->AddStretchSpacer(1);

    // Wrap grid in vertical sizer for padding, then set as panel sizer (matches physical structure)
    auto* wrapper = new wxBoxSizer(wxVERTICAL);
    wrapper->Add(grid_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, m_owner->FromDIP(8));
    m_panel_color_mix_content->SetSizer(wrapper);
    m_panel_color_mix_content->Layout();

    // Dynamic height: grow with rows up to 3, only cap when > 3 rows
    const wxSize content_best = m_panel_color_mix_content->GetBestSize();
    const int row_count = (visible_idx + 1) / 2; // rows (2 columns)
    const int desired_h = row_count > 3
        ? (content_best.GetHeight() / row_count) * 3
        : content_best.GetHeight();
    m_scrolled_color_mix->SetMinSize({-1, desired_h});
    m_scrolled_color_mix->SetMaxSize({-1, desired_h});
    
    m_host.layout_scrolled();
    wxWeakRef<MixedFilamentsPanel> weak_this(this);
    wxTheApp->CallAfter([weak_this]() {
        MixedFilamentsPanel* panel = weak_this.get();
        if (panel && panel->m_scrolled_color_mix) {
            int vh = panel->m_scrolled_color_mix->GetVirtualSize().y;
            int ch = panel->m_scrolled_color_mix->GetClientSize().y;
            panel->m_scrolled_color_mix->Scroll(0, std::max(0, vh - ch));
        }
    });
    m_panel_color_mix_content->Refresh();

    // Disable add buttons when combined filament limit reached
    if (preset_bundle) {
        const bool can_add = preset_bundle->mixed_filaments.total_filaments(num_physical) < MAXIMUM_FILAMENT_NUMBER;
        m_host.set_add_filament_enabled(can_add);
        if (m_btn_add_color_mix)
            m_btn_add_color_mix->Enable(can_add);
    }
}

void MixedFilamentsPanel::update_mixed_filament_panel(bool sync_manager)
{
    // Check for new collapsible structure
    if (!m_panel_mixed_filaments_title || !m_panel_mixed_filaments_content)
        return;

    wxWindowUpdateLocker noUpdates_sidebar(m_owner);
    wxWindowUpdateLocker noUpdates_mixed_panel(m_panel_mixed_filaments_content);

    auto refresh_model_canvas_colors = [this]() { m_host.refresh_model_canvas_colors(); };

    int prev_rows_view_y = 0;
    for (wxWindow *child : m_panel_mixed_filaments_content->GetChildren()) {
        if (auto *scrolled = dynamic_cast<wxScrolledWindow*>(child)) {
            int tmp_x = 0;
            scrolled->GetViewStart(&tmp_x, &prev_rows_view_y);
            break;
        }
    }

    auto *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return;
    DynamicPrintConfig *print_cfg = &preset_bundle->prints.get_edited_preset().config;

    const size_t num_physical = m_host.filament_count();
    ConfigOptionStrings *color_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    std::vector<std::string> physical_colors = color_opt ? color_opt->values : std::vector<std::string>();
    physical_colors.resize(num_physical, "#26A69A");
    std::vector<double> nozzle_diameters(num_physical, 0.4);
    if (const ConfigOptionFloats *opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0) {
            for (size_t i = 0; i < num_physical; ++i)
                nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
        }
    }

    auto get_mixed_bool = [preset_bundle, print_cfg](const std::string &key, bool fallback) {
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg) {
            if (const ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_mode = [preset_bundle, print_cfg](bool fallback) {
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
            return opt->value;
        if (const ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>("mixed_filament_gradient_mode"))
            return opt->value != 0;
        if (print_cfg) {
            if (const ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>("mixed_filament_gradient_mode"))
                return opt->value;
            if (const ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>("mixed_filament_gradient_mode"))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [preset_bundle, print_cfg](const std::string &key, float fallback) {
        if (preset_bundle->project_config.has(key))
            return float(preset_bundle->project_config.opt_float(key));
        if (print_cfg && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };
    auto get_mixed_string = [preset_bundle, print_cfg](const std::string &key, const std::string &fallback = std::string()) {
        std::string project_value;
        if (preset_bundle->project_config.has(key))
            project_value = preset_bundle->project_config.opt_string(key);
        if (!project_value.empty())
            return project_value;
        if (print_cfg && print_cfg->has(key)) {
            const std::string print_value = print_cfg->opt_string(key);
            if (!print_value.empty())
                return print_value;
        }
        return project_value.empty() ? fallback : project_value;
    };
    auto set_mixed_float = [preset_bundle, print_cfg](const std::string &key, float value) {
        if (print_cfg) {
            if (ConfigOptionFloat *opt = print_cfg->option<ConfigOptionFloat>(key))
                opt->value = value;
        }
        if (ConfigOptionFloat *opt = preset_bundle->project_config.option<ConfigOptionFloat>(key))
            opt->value = value;
        else
            preset_bundle->project_config.set_key_value(key, new ConfigOptionFloat(value));
    };
    auto set_mixed_string = [preset_bundle, print_cfg](const std::string &key, const std::string &value) {
        if (print_cfg) {
            if (ConfigOptionString *opt = print_cfg->option<ConfigOptionString>(key))
                opt->value = value;
        }
        if (ConfigOptionString *opt = preset_bundle->project_config.option<ConfigOptionString>(key))
            opt->value = value;
        else
            preset_bundle->project_config.set_key_value(key, new ConfigOptionString(value));
    };
    auto set_mixed_bool = [preset_bundle, print_cfg](const std::string &key, bool value) {
        if (print_cfg) {
            if (ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>(key))
                opt->value = value;
            else if (ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>(key))
                opt->value = value ? 1 : 0;
        }
        if (ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            opt->value = value;
        else if (ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            opt->value = value ? 1 : 0;
        else
            preset_bundle->project_config.set_key_value(key, new ConfigOptionBool(value));
    };
    auto set_mixed_mode = [preset_bundle, print_cfg](bool enabled) {
        if (print_cfg) {
            if (ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>("mixed_filament_gradient_mode"))
                opt->value = enabled;
            else if (ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>("mixed_filament_gradient_mode"))
                opt->value = enabled ? 1 : 0;
        }
        if (ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
            opt->value = enabled;
        else if (ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>("mixed_filament_gradient_mode"))
            opt->value = enabled ? 1 : 0;
        else
            preset_bundle->project_config.set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(enabled));
    };
    auto notify_mixed_change = [print_cfg]() {
        if (!print_cfg)
            return;
        if (auto *print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
            print_tab->update_dirty();
        if (wxGetApp().mainframe)
            wxGetApp().mainframe->on_config_changed(print_cfg);
    };
    auto decode_gradient_ids = [](const std::string &encoded) {
        return MixedFilamentManager::decode_gradient_component_ids(encoded, 0);
    };
    auto encode_gradient_ids = [](const std::vector<unsigned int> &ids) {
        return MixedFilamentManager::encode_gradient_component_ids(ids);
    };
    auto decode_gradient_weights = [](const std::string &encoded, size_t expected_count) {
        std::vector<int> out;
        if (encoded.empty() || expected_count == 0)
            return out;
        std::string token;
        for (const char c : encoded) {
            if (c >= '0' && c <= '9') {
                token.push_back(c);
                continue;
            }
            if (!token.empty()) {
                out.emplace_back(std::max(0, std::atoi(token.c_str())));
                token.clear();
            }
        }
        if (!token.empty())
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
        if (out.size() != expected_count)
            out.clear();
        return out;
    };
    auto normalize_gradient_weights = [](const std::vector<int> &weights, size_t n) {
        std::vector<int> out = weights;
        if (out.size() != n)
            out.assign(n, (n > 0) ? int(100 / n) : 0);
        int sum = 0;
        for (int &v : out) {
            v = std::max(0, v);
            sum += v;
        }
        if (sum <= 0 && n > 0) {
            out.assign(n, 0);
            out[0] = 100;
            return out;
        }
        std::vector<double> rem(n, 0.);
        int assigned = 0;
        for (size_t i = 0; i < n; ++i) {
            const double exact = 100.0 * double(out[i]) / double(sum);
            out[i] = int(std::floor(exact));
            rem[i] = exact - double(out[i]);
            assigned += out[i];
        }
        int missing = std::max(0, 100 - assigned);
        while (missing > 0) {
            size_t best_idx = 0;
            double best_rem = -1.0;
            for (size_t i = 0; i < rem.size(); ++i) {
                if (rem[i] > best_rem) {
                    best_rem = rem[i];
                    best_idx = i;
                }
            }
            ++out[best_idx];
            rem[best_idx] = 0.0;
            --missing;
        }
        return out;
    };
    auto encode_gradient_weights = [](const std::vector<int> &weights) {
        std::ostringstream ss;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (i > 0)
                ss << '/';
            ss << std::max(0, weights[i]);
        }
        return ss.str();
    };
    auto build_weighted_multi_sequence = [normalize_gradient_weights](const std::vector<unsigned int> &ids,
                                                                      const std::vector<int> &weights,
                                                                      size_t max_cycle_limit) {
        if (ids.empty())
            return std::vector<unsigned int>();

        std::vector<unsigned int> filtered_ids;
        std::vector<int> counts;
        filtered_ids.reserve(ids.size());
        counts.reserve(ids.size());

        std::vector<int> normalized = normalize_gradient_weights(weights, ids.size());
        for (size_t i = 0; i < ids.size(); ++i) {
            const int weight = (i < normalized.size()) ? std::max(0, normalized[i]) : 0;
            if (weight <= 0)
                continue;
            filtered_ids.emplace_back(ids[i]);
            counts.emplace_back(weight);
        }
        if (filtered_ids.empty()) {
            filtered_ids = ids;
            counts.assign(ids.size(), 1);
        }

        int g = 0;
        for (const int c : counts)
            g = std::gcd(g, std::max(1, c));
        if (g > 1) {
            for (int &c : counts)
                c = std::max(1, c / g);
        }

        constexpr size_t k_max_cycle = 48;
        const size_t effective_cycle_limit =
            max_cycle_limit > 0 ? std::min(k_max_cycle, std::max<size_t>(1, max_cycle_limit)) : k_max_cycle;
        reduce_weight_counts_to_cycle_limit(counts, effective_cycle_limit);

        std::vector<unsigned int> reduced_ids;
        std::vector<int> reduced_counts;
        reduced_ids.reserve(filtered_ids.size());
        reduced_counts.reserve(counts.size());
        for (size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] <= 0)
                continue;
            reduced_ids.emplace_back(filtered_ids[i]);
            reduced_counts.emplace_back(counts[i]);
        }
        if (reduced_ids.empty())
            return std::vector<unsigned int>();
        filtered_ids = std::move(reduced_ids);
        counts = std::move(reduced_counts);

        const int total = std::accumulate(counts.begin(), counts.end(), 0);
        if (total <= 0)
            return std::vector<unsigned int>(filtered_ids.begin(), filtered_ids.end());

        const size_t cycle = size_t(total);

        std::vector<unsigned int> sequence;
        sequence.reserve(cycle);
        std::vector<int> emitted(counts.size(), 0);
        for (size_t pos = 0; pos < cycle; ++pos) {
            size_t best_idx = 0;
            double best_score = -1e9;
            for (size_t i = 0; i < counts.size(); ++i) {
                const double target = double(pos + 1) * double(counts[i]) / double(total);
                const double score = target - double(emitted[i]);
                if (score > best_score) {
                    best_score = score;
                    best_idx = i;
                }
            }
            ++emitted[best_idx];
            sequence.emplace_back(filtered_ids[best_idx]);
        }
        if (sequence.empty())
            sequence = filtered_ids;
        return sequence;
    };
    auto decode_manual_pattern_ids = [num_physical](const std::string &pattern,
                                                    unsigned int       component_a,
                                                    unsigned int       component_b,
                                                    size_t             wall_loops) {
        return build_grouped_manual_pattern_preview_sequence(pattern, component_a, component_b, num_physical, wall_loops);
    };
    const bool height_weighted_mode = get_mixed_mode(false);
    int   gradient_mode = height_weighted_mode ? 1 : 0;
    float lower_bound   = std::max(0.01f, get_mixed_float("mixed_filament_height_lower_bound", 0.04f));
    float upper_bound   = std::max(lower_bound, get_mixed_float("mixed_filament_height_upper_bound", 0.16f));
    float preferred_local_z_a = std::max(0.f, get_mixed_float("mixed_color_layer_height_a", 0.f));
    float preferred_local_z_b = std::max(0.f, get_mixed_float("mixed_color_layer_height_b", 0.f));
    float nominal_layer_height = 0.2f;
    if (print_cfg && print_cfg->has("layer_height"))
        nominal_layer_height = float(print_cfg->opt_float("layer_height"));
    nominal_layer_height = std::max(0.01f, nominal_layer_height);
    size_t wall_loops = 1;
    if (print_cfg && print_cfg->has("wall_loops"))
        wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    const bool local_z_mode = get_mixed_bool("dithering_local_z_mode", false);
    const bool local_z_direct_multicolor =
        get_mixed_bool("dithering_local_z_direct_multicolor", false) &&
        preferred_local_z_a <= EPSILON &&
        preferred_local_z_b <= EPSILON;
    const bool component_bias_enabled = get_mixed_bool("mixed_filament_component_bias_enabled", false);
    float pointillism_pixel_size = std::max(0.f, get_mixed_float("mixed_filament_pointillism_pixel_size", 0.f));
    float pointillism_line_gap   = std::max(0.f, get_mixed_float("mixed_filament_pointillism_line_gap", 0.f));
    float mixed_surface_indentation = std::clamp(get_mixed_float("mixed_filament_surface_indentation", 0.f), -2.f, 2.f);
    bool  advanced_dithering = get_mixed_bool("mixed_filament_advanced_dithering", false);
    const std::string mixed_definitions = get_mixed_string("mixed_filament_definitions");
    const MixedFilamentPreviewSettings preview_settings {
        nominal_layer_height,
        lower_bound,
        upper_bound,
        preferred_local_z_a,
        preferred_local_z_b,
        local_z_mode,
        local_z_direct_multicolor,
        wall_loops
    };
    const MixedFilamentDisplayContext display_context {
        num_physical,
        physical_colors,
        nozzle_diameters,
        preview_settings,
        component_bias_enabled
    };
    auto summarize_sequence = [num_physical](const std::vector<unsigned int> &sequence) {
        if (sequence.empty() || num_physical == 0)
            return std::string();
        std::vector<size_t> counts(num_physical + 1, size_t(0));
        size_t total = 0;
        for (const unsigned int id : sequence) {
            if (id == 0 || id > num_physical)
                continue;
            ++counts[id];
            ++total;
        }
        if (total == 0)
            return std::string();
        std::ostringstream ss;
        bool first = true;
        for (size_t id = 1; id <= num_physical; ++id) {
            if (counts[id] == 0)
                continue;
            const int pct = int(std::lround(100.0 * double(counts[id]) / double(total)));
            if (!first)
                ss << "  ";
            first = false;
            ss << "F" << id << ":" << pct << "%";
        }
        return ss.str();
    };
    auto compute_entry_display_color = [display_context](const MixedFilament &entry) {
        return compute_mixed_filament_display_color(entry, display_context);
    };

    auto &mixed_mgr = preset_bundle->mixed_filaments;
    mixed_mgr.set_display_context(display_context);
    if (sync_manager) {
        mixed_mgr.auto_generate(physical_colors);
        mixed_mgr.clear_custom_entries();
        mixed_mgr.load_custom_entries(mixed_definitions, physical_colors);
        mixed_mgr.apply_gradient_settings(gradient_mode, lower_bound, upper_bound, advanced_dithering);
    }

    if (component_bias_enabled) {
        for (MixedFilament &entry : mixed_mgr.mixed_filaments()) {
            const float bias_value = mixed_filament_single_surface_offset_value(entry, nozzle_diameters);
            const auto balanced_pair = mixed_filament_single_surface_offset_pair(entry, bias_value, nozzle_diameters);
            entry.component_a_surface_offset = balanced_pair.first;
            entry.component_b_surface_offset = balanced_pair.second;
        }
    }

    // During project load, sidebar may refresh before physical filament combos
    // finish syncing. Avoid overwriting persisted mixed definitions while the
    // physical filament set is incomplete.
    if (num_physical >= 2) {
        set_mixed_mode(height_weighted_mode);
        set_mixed_bool("mixed_filament_component_bias_enabled", component_bias_enabled);
        set_mixed_float("mixed_filament_height_lower_bound", lower_bound);
        set_mixed_float("mixed_filament_height_upper_bound", upper_bound);
        set_mixed_float("mixed_color_layer_height_a", preferred_local_z_a);
        set_mixed_float("mixed_color_layer_height_b", preferred_local_z_b);
        set_mixed_float("mixed_filament_pointillism_pixel_size", pointillism_pixel_size);
        set_mixed_float("mixed_filament_pointillism_line_gap", pointillism_line_gap);
        set_mixed_float("mixed_filament_surface_indentation", mixed_surface_indentation);
        set_mixed_string("mixed_filament_definitions", mixed_mgr.serialize_custom_entries());
    }

    auto &mixed = mixed_mgr.mixed_filaments();
    const std::vector<size_t> ordered_mixed_indices = build_mixed_filament_ui_indices(mixed, m_mixed_filament_ui_order);
    std::vector<uint64_t>       sanitized_mixed_ui_order_ids;
    sanitized_mixed_ui_order_ids.reserve(ordered_mixed_indices.size());
    for (const size_t mixed_id : ordered_mixed_indices) {
        if (mixed_id < mixed.size() && mixed[mixed_id].stable_id != 0)
            sanitized_mixed_ui_order_ids.emplace_back(mixed[mixed_id].stable_id);
    }
    m_mixed_filament_ui_order = std::move(sanitized_mixed_ui_order_ids);

    m_mixed_filament_drag_active = false;
    m_mixed_filament_drag_source_mixed_id = size_t(-1);
    m_mixed_filament_row_bindings.clear();

    const int compact_gap_x   = m_owner->FromDIP(6);
    const int compact_gap_y   = m_owner->FromDIP(4);
    const int compact_row_pad = m_owner->FromDIP(6);
    const bool is_dark = wxGetApp().dark_mode();
    const wxColour mixed_rows_bg = is_dark ? wxColour(45, 45, 49) : wxColour(246, 248, 251);
    const wxColour mixed_row_bg = is_dark ? wxColour(52, 52, 56) : wxColour(255, 255, 255);
    const wxColour mixed_row_hover_bg = is_dark ? wxColour(62, 62, 68) : wxColour(241, 247, 255);
    const wxColour mixed_text_fg = is_dark ? wxColour(232, 232, 232) : wxColour(20, 20, 20);
    const wxColour mixed_summary_fg = is_dark ? wxColour(182, 182, 182) : wxColour(96, 96, 96);
    m_panel_mixed_filaments_content->SetBackgroundColour(mixed_rows_bg);

    // Get the content sizer and clear it
    wxSizer *content_sizer = m_panel_mixed_filaments_content->GetSizer();
    if (content_sizer)
        content_sizer->Clear(true);
    
    // Re-add the top margin spacer that was added in constructor but cleared above
    if (content_sizer)
        content_sizer->AddSpacer(m_owner->FromDIP(SidebarProps::ContentMargin()));

    // Update button states (buttons are now in title bar, created in constructor)
    if (m_btn_add_gradient)
        m_btn_add_gradient->Enable(num_physical >= 2);
    if (m_btn_add_pattern)
        m_btn_add_pattern->Enable(num_physical >= 2);
    if (m_btn_add_color)
        m_btn_add_color->Enable(num_physical >= 2);

    // Mixed Filaments panel is hidden
    m_panel_mixed_filaments_title->Hide();
    m_panel_mixed_filaments_content->Hide();
    m_host.layout_owner();
    refresh_model_canvas_colors();
    wxWeakRef<MixedFilamentsPanel> weakSelf(this);
    wxTheApp->CallAfter([weakSelf]() {
        if (weakSelf) weakSelf->update_color_mix_panel();
    });
    return;

#if 0 // DEAD CODE — Mixed Filaments panel UI, disabled. DO NOT add or modify code here.
      //    All active Mixed Filament operations now live in:
      //      - init_color_mix_panel()  (Color Mixing panel with Add/Delete buttons)
      //      - Filament Management title bar (Batch Match, Flushing volumes, AMS sync, Settings)
    
    // Reset the max size in case it was collapsed
    m_panel_mixed_filaments_content->SetMaxSize({-1, -1});

    auto *rows_scroller = new wxScrolledWindow(m_panel_mixed_filaments_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    rows_scroller->SetScrollRate(0, m_owner->FromDIP(6));
    rows_scroller->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    rows_scroller->SetBackgroundColour(mixed_rows_bg);
    auto *rows_sizer = new wxBoxSizer(wxVERTICAL);
    rows_scroller->SetSizer(rows_sizer);

    if (mixed.empty()) {
        auto *empty_label = new wxStaticText(rows_scroller, wxID_ANY,
                                             _L("No mixed filaments yet. Use Add Gradient, Add Pattern, or Add Color to create one."));
        empty_label->SetForegroundColour(mixed_summary_fg);
        empty_label->SetFont(::Label::Body_13);
        empty_label->Wrap(m_owner->FromDIP(360));
        rows_sizer->Add(empty_label, 0, wxALL | wxEXPAND, m_owner->FromDIP(12));
        rows_scroller->Layout();
        rows_scroller->FitInside();
        const int empty_content_h = empty_label->GetBestSize().GetHeight() + m_owner->FromDIP(28);
        const int empty_rows_h = std::max(m_owner->FromDIP(86), empty_content_h);
        rows_scroller->SetMinSize(wxSize(-1, empty_rows_h));
        rows_scroller->SetMaxSize(wxSize(-1, empty_rows_h));
        if (content_sizer)
            content_sizer->Add(rows_scroller, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, m_owner->FromDIP(SidebarProps::ContentMargin()));
        m_panel_mixed_filaments_content->Layout();
        m_host.layout_owner();
        refresh_model_canvas_colors();
        update_color_mix_panel();
        return;
    }

    auto adjust_rows_scroller_height = [this, rows_scroller]() {
        if (!rows_scroller)
            return;
        const int min_h = m_owner->FromDIP(68);
        const int collapsed_max_h = m_owner->FromDIP(220);
        int two_rows_cap_h = collapsed_max_h;
        const auto &children = rows_scroller->GetChildren();
        if (!children.empty()) {
            std::vector<int> heights;
            heights.reserve(children.GetCount());
            for (wxWindowList::compatibility_iterator it = children.GetFirst(); it; it = it->GetNext()) {
                wxWindow *child = it->GetData();
                wxPanel *panel = dynamic_cast<wxPanel *>(child);
                if (!panel)
                    continue;
                heights.emplace_back(std::max(panel->GetSize().GetHeight(), panel->GetBestSize().GetHeight()));
            }
            if (!heights.empty()) {
                std::sort(heights.begin(), heights.end(), std::greater<int>());
                const size_t keep = std::min<size_t>(2, heights.size());
                int rows_h = 0;
                for (size_t i = 0; i < keep; ++i)
                    rows_h += heights[i];
                if (keep > 1)
                    rows_h += int(keep - 1) * m_owner->FromDIP(2);
                rows_h += m_owner->FromDIP(8);
                two_rows_cap_h = std::max(collapsed_max_h, rows_h);
            }
        }

        const int max_h = m_expanded_mixed_filament_rows.empty() ? collapsed_max_h : two_rows_cap_h;
        const int content_h = std::max(0, rows_scroller->GetVirtualSize().GetHeight());
        const int desired_h = std::clamp(content_h, min_h, max_h);
        rows_scroller->SetMinSize(wxSize(-1, desired_h));
        rows_scroller->SetMaxSize(wxSize(-1, desired_h));
    };

    for (auto it = m_expanded_mixed_filament_rows.begin(); it != m_expanded_mixed_filament_rows.end();) {
        if (*it >= mixed.size() || mixed[*it].deleted)
            it = m_expanded_mixed_filament_rows.erase(it);
        else
            ++it;
    }

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string &hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    auto mixed_summary_text = [decode_gradient_ids](const MixedFilament &entry) {
        const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(entry.manual_pattern);
        if (!entry.custom)
            return wxString::Format("(Filament %u + Filament %u)", unsigned(entry.component_a), unsigned(entry.component_b));
        if (!normalized_pattern.empty())
            return _L("(Pattern)");
        if (decode_gradient_ids(entry.gradient_component_ids).size() >= 3)
            return _L("(Color)");
        return wxString::Format("(F%u + F%u)", unsigned(entry.component_a), unsigned(entry.component_b));
    };

    auto apply_mixed_entry_changes = [this, preset_bundle, print_cfg, num_physical](size_t mixed_id,
                                                                                    const MixedFilament &updated_mf,
                                                                                    bool preserve_enabled = false,
                                                                                    bool rebuild_virtual_id_remap = false) {
        if (!preset_bundle)
            return;

        auto &mgr = preset_bundle->mixed_filaments;
        auto &mfs = mgr.mixed_filaments();
        if (mixed_id >= mfs.size())
            return;

        const std::vector<MixedFilament> old_mixed = rebuild_virtual_id_remap ? mfs : std::vector<MixedFilament>();
        MixedFilament merged = updated_mf;
        if (preserve_enabled)
            merged.enabled = mfs[mixed_id].enabled;
        mfs[mixed_id] = merged;

        const std::string serialized = mgr.serialize_custom_entries();
        if (print_cfg) {
            if (ConfigOptionString *opt = print_cfg->option<ConfigOptionString>("mixed_filament_definitions"))
                opt->value = serialized;
            else
                print_cfg->set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));
        }
        if (ConfigOptionString *opt = preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
            opt->value = serialized;
        else
            preset_bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));

        if (print_cfg) {
            if (auto *print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
                print_tab->update_dirty();
            if (wxGetApp().mainframe)
                wxGetApp().mainframe->on_config_changed(print_cfg);
        }
        if (wxGetApp().plater())
            m_host.plater_update_project_dirty_from_presets();

        if (rebuild_virtual_id_remap)
            preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical);

        int mode = 0;
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
            mode = opt->value ? 1 : 0;
        else if (const ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>("mixed_filament_gradient_mode"))
            mode = opt->value != 0 ? 1 : 0;
        float lo = preset_bundle->project_config.has("mixed_filament_height_lower_bound") ?
            float(preset_bundle->project_config.opt_float("mixed_filament_height_lower_bound")) : 0.04f;
        float hi = preset_bundle->project_config.has("mixed_filament_height_upper_bound") ?
            float(preset_bundle->project_config.opt_float("mixed_filament_height_upper_bound")) : 0.16f;
        bool advanced = false;
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_advanced_dithering"))
            advanced = opt->value;
        mode = std::clamp(mode, 0, 1);
        lo = std::max(0.01f, lo);
        hi = std::max(lo, hi);
        mgr.apply_gradient_settings(mode, lo, hi, advanced);
        update_dynamic_filament_list();

        if (rebuild_virtual_id_remap && wxGetApp().plater()) {
            m_host.set_skip_mixed_filament_sync_once();
            m_host.plater_on_filaments_change(num_physical);
        }
    };

    auto current_mixed_filament_ui_order = [this, &mixed]() {
        std::vector<uint64_t> ordered_ids;
        ordered_ids.reserve(m_mixed_filament_row_bindings.size());
        for (const auto &binding : m_mixed_filament_row_bindings) {
            if (binding.mixed_id < mixed.size() && mixed[binding.mixed_id].stable_id != 0)
                ordered_ids.emplace_back(mixed[binding.mixed_id].stable_id);
        }
        return ordered_ids;
    };

    auto drop_insert_position = [this]() {
        const wxPoint mouse_pos = wxGetMousePosition();
        size_t        visible_idx = 0;
        for (const auto &binding : m_mixed_filament_row_bindings) {
            if (binding.row == nullptr || !binding.row->IsShown())
                continue;

            const wxPoint top_left = binding.row->ClientToScreen(wxPoint(0, 0));
            const int     row_h = std::max(binding.row->GetSize().GetHeight(), binding.row->GetBestSize().GetHeight());
            const int     center_y = top_left.y + row_h / 2;
            if (mouse_pos.y < center_y)
                return visible_idx;

            ++visible_idx;
        }
        return visible_idx;
    };

    for (size_t display_mixed_idx = 0; display_mixed_idx < ordered_mixed_indices.size(); ++display_mixed_idx) {
        const size_t mixed_id = ordered_mixed_indices[display_mixed_idx];
        MixedFilament &mf = mixed[mixed_id];
        const bool auto_row = !mf.custom;

        auto *row = new wxPanel(rows_scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        row->SetBackgroundColour(mixed_row_bg);
        auto *row_sizer = new wxBoxSizer(wxVERTICAL);
        m_mixed_filament_row_bindings.push_back({mixed_id, row});

        auto *header_panel = new wxPanel(row, wxID_ANY);
        header_panel->SetBackgroundColour(mixed_row_bg);
        auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);

        const std::string synced_color = compute_entry_display_color(mf);
        if (mf.display_color != synced_color)
            mf.display_color = synced_color;
        auto *drag_handle = new MixedFilamentDragHandle(header_panel, mixed_summary_fg, mixed_row_bg);
        drag_handle->SetToolTip(_L("Drag to reorder mixed filaments in this panel."));
        header_sizer->Add(drag_handle, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        wxColour swatch_color = parse_mixed_color(mf.display_color);
        auto *swatch = new wxPanel(header_panel, wxID_ANY, wxDefaultPosition, wxSize(m_owner->FromDIP(12), m_owner->FromDIP(12)));
        swatch->SetBackgroundColour(swatch_color);
        swatch->SetMinSize(wxSize(m_owner->FromDIP(12), m_owner->FromDIP(12)));
        header_sizer->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        const int virtual_filament_id = int(num_physical + display_mixed_idx + 1);
        auto *name_label = new wxStaticText(header_panel, wxID_ANY, wxString::Format("Mixed Filament %d", virtual_filament_id));
        name_label->SetForegroundColour(mixed_text_fg);
        header_sizer->Add(name_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        auto *summary_label = new wxStaticText(header_panel, wxID_ANY, mixed_summary_text(mf));
        summary_label->SetForegroundColour(mixed_summary_fg);
        header_sizer->Add(summary_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        header_sizer->AddStretchSpacer(1);

        auto *enabled_chk = new wxCheckBox(header_panel, wxID_ANY, _L("Enabled"));
        enabled_chk->SetValue(mf.enabled);
        enabled_chk->SetForegroundColour(mixed_text_fg);
        header_sizer->Add(enabled_chk, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap_x);
        enabled_chk->Bind(wxEVT_LEFT_UP, [](wxMouseEvent &evt) {
            evt.StopPropagation();
            evt.Skip();
        });
        enabled_chk->Bind(wxEVT_CHECKBOX, [mixed_id, enabled_chk, apply_mixed_entry_changes, preset_bundle](wxCommandEvent &) {
            if (!preset_bundle || !enabled_chk)
                return;
            auto &mgr = preset_bundle->mixed_filaments;
            auto &mfs = mgr.mixed_filaments();
            if (mixed_id >= mfs.size())
                return;
            MixedFilament updated = mfs[mixed_id];
            updated.enabled = enabled_chk->GetValue();
            apply_mixed_entry_changes(mixed_id, updated, false, true);
        });

        auto *del_btn = new ScalableButton(header_panel, wxID_ANY, "cross"); 
        del_btn->SetToolTip(_L("Delete mixed filament"));
        header_sizer->Add(del_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap_x);
        
        del_btn->Bind(wxEVT_BUTTON, [this, mixed_id, num_physical, set_mixed_string, notify_mixed_change](wxCommandEvent&) {
             if (wxGetApp().preset_bundle) {
                 auto &mgr = wxGetApp().preset_bundle->mixed_filaments;
                 auto &mfs = mgr.mixed_filaments();
                 if (mixed_id < mfs.size()) {
                     const std::vector<MixedFilament> old_mixed = mfs;
                     auto canonical_pair = [](unsigned int a, unsigned int b) {
                         return std::make_pair(std::min(a, b), std::max(a, b));
                     };
                     MixedFilament &target = mfs[mixed_id];
                     const auto target_pair = canonical_pair(target.component_a, target.component_b);
                     const bool valid_auto_pair = target_pair.first >= 1 &&
                                                  target_pair.second >= 1 &&
                                                  target_pair.first <= num_physical &&
                                                  target_pair.second <= num_physical &&
                                                  target_pair.first != target_pair.second;
                     if (target.custom && target.origin_auto && valid_auto_pair) {
                         bool tombstoned_existing_auto = false;
                         for (size_t idx = 0; idx < mfs.size(); ++idx) {
                             if (idx == mixed_id)
                                 continue;
                             MixedFilament &candidate = mfs[idx];
                             if (candidate.custom)
                                 continue;
                             if (canonical_pair(candidate.component_a, candidate.component_b) != target_pair)
                                 continue;
                             candidate.deleted = true;
                             candidate.enabled = false;
                             tombstoned_existing_auto = true;
                             break;
                         }

                         if (tombstoned_existing_auto) {
                             mfs.erase(mfs.begin() + mixed_id);
                         } else {
                             target.component_a = target_pair.first;
                             target.component_b = target_pair.second;
                             target.mix_b_percent = 50;
                             target.ratio_a = 1;
                             target.ratio_b = 1;
                             target.manual_pattern.clear();
                             target.gradient_component_ids.clear();
                             target.gradient_component_weights.clear();
                             target.pointillism_all_filaments = false;
                             target.distribution_mode = int(MixedFilament::Simple);
                             target.custom = false;
                             target.origin_auto = true;
                             target.deleted = true;
                             target.enabled = false;
                         }
                     } else if (target.custom) {
                         mfs.erase(mfs.begin() + mixed_id);
                     } else {
                         target.deleted = true;
                         target.enabled = false;
                     }
                     m_expanded_mixed_filament_rows.clear();
                     set_mixed_string("mixed_filament_definitions", mgr.serialize_custom_entries());
                     wxGetApp().preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical, mixed_id);
                     notify_mixed_change();
                     if (wxGetApp().plater())
                         m_host.plater_update_project_dirty_from_presets();
                     if (wxGetApp().plater()) {
                         m_host.set_skip_mixed_filament_sync_once();
                         m_host.plater_on_filaments_change(num_physical);
                     }
                 }
             }
        });

        header_panel->SetSizer(header_sizer);
        row_sizer->Add(header_panel, 0, wxEXPAND | wxALL, 0);

        auto *editor_host = new wxPanel(row, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        editor_host->SetBackgroundColour(mixed_row_bg);
        auto *editor_sizer = new wxBoxSizer(wxVERTICAL);
        editor_host->SetSizer(editor_sizer);
        editor_host->Hide();
        row_sizer->Add(editor_host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, compact_row_pad);

        auto set_row_hover = [row, header_panel, editor_host, drag_handle, mixed_summary_fg, mixed_row_bg, mixed_row_hover_bg](bool hovered) {
            const wxColour bg = hovered ? mixed_row_hover_bg : mixed_row_bg;
            if (row) row->SetBackgroundColour(bg);
            if (header_panel) header_panel->SetBackgroundColour(bg);
            if (editor_host) editor_host->SetBackgroundColour(bg);
            if (drag_handle) drag_handle->set_colors(mixed_summary_fg, bg);
            if (row) row->Refresh();
            if (header_panel) header_panel->Refresh();
            if (editor_host) editor_host->Refresh();
        };

        auto row_contains_mouse = [row]() {
            if (!row)
                return false;
            const wxPoint mouse_pos = wxGetMousePosition();
            const wxPoint local = row->ScreenToClient(mouse_pos);
            return row->GetClientRect().Contains(local);
        };

        auto ensure_editor = [this, mixed_id, num_physical, physical_colors, nozzle_diameters, palette, preview_settings, component_bias_enabled, preset_bundle,
                              editor_host, editor_sizer, swatch, summary_label, header_panel, row,
                              rows_scroller, mixed_summary_text, apply_mixed_entry_changes]() {
            if (!preset_bundle || !editor_sizer || editor_sizer->GetItemCount() > 0)
                return;

            auto &mgr = preset_bundle->mixed_filaments;
            auto &mfs = mgr.mixed_filaments();
            if (mixed_id >= mfs.size())
                return;

            auto *editor = new MixedFilamentConfigPanel(editor_host, mixed_id, mfs[mixed_id], num_physical, physical_colors, nozzle_diameters, palette, preview_settings,
                component_bias_enabled,
                [this, mixed_id, swatch, summary_label, header_panel, row, rows_scroller, mixed_summary_text, apply_mixed_entry_changes](const MixedFilament &updated_mf) {
                    apply_mixed_entry_changes(mixed_id, updated_mf, true);

                    if (swatch) {
                        swatch->SetBackgroundColour(parse_mixed_color(updated_mf.display_color));
                        swatch->Refresh();
                    }
                    if (summary_label) {
                        summary_label->SetLabel(mixed_summary_text(updated_mf));
                    }
                    if (header_panel)
                        header_panel->Layout();
                    if (row)
                        row->Layout();
                    if (rows_scroller) {
                        rows_scroller->Layout();
                        rows_scroller->FitInside();
                    }
                });

            editor_sizer->Add(editor, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, m_owner->FromDIP(4));
            editor_host->Layout();
        };

        auto toggle_editor = [this, mixed_id, editor_host, ensure_editor, rows_scroller, adjust_rows_scroller_height]() {
            if (!editor_host || !rows_scroller)
                return;

            if (editor_host->IsShown()) {
                editor_host->Hide();
                m_expanded_mixed_filament_rows.erase(mixed_id);
            } else {
                ensure_editor();
                editor_host->Show();
                m_expanded_mixed_filament_rows.insert(mixed_id);
            }

            rows_scroller->Layout();
            rows_scroller->FitInside();
            adjust_rows_scroller_height();
            m_panel_mixed_filaments_content->Layout();
            m_host.layout_scrolled();
            m_host.layout_owner();
        };

        auto bind_toggle_target = [&toggle_editor](wxWindow *target) {
            if (!target)
                return;
            target->SetCursor(wxCursor(wxCURSOR_HAND));
            target->Bind(wxEVT_LEFT_UP, [toggle_editor](wxMouseEvent &) {
                toggle_editor();
            });
        };

        auto bind_hover_target = [set_row_hover, row_contains_mouse](wxWindow *target) {
            if (!target)
                return;
            target->Bind(wxEVT_ENTER_WINDOW, [set_row_hover](wxMouseEvent &evt) {
                set_row_hover(true);
                evt.Skip();
            });
            target->Bind(wxEVT_LEAVE_WINDOW, [set_row_hover, row_contains_mouse](wxMouseEvent &evt) {
                set_row_hover(row_contains_mouse());
                evt.Skip();
            });
        };

        auto release_drag_capture = [this]() {
            m_mixed_filament_drag_active = false;
            m_mixed_filament_drag_source_mixed_id = size_t(-1);
        };

        auto bind_drag_target = [this,
                                 mixed_id,
                                 &mixed,
                                 drop_insert_position,
                                 current_mixed_filament_ui_order,
                                 release_drag_capture](wxWindow *target) {
            if (!target)
                return;

            target->Bind(wxEVT_LEFT_DOWN, [this, mixed_id, target](wxMouseEvent &evt) {
                if (!target)
                    return;
                m_mixed_filament_drag_active = true;
                m_mixed_filament_drag_source_mixed_id = mixed_id;
                if (!target->HasCapture())
                    target->CaptureMouse();
                evt.StopPropagation();
            });

            target->Bind(wxEVT_MOTION, [this](wxMouseEvent &evt) {
                if (m_mixed_filament_drag_active)
                    evt.StopPropagation();
            });

            target->Bind(wxEVT_LEFT_UP, [this, &mixed, target, drop_insert_position, current_mixed_filament_ui_order](wxMouseEvent &evt) {
                if (target && target->HasCapture())
                    target->ReleaseMouse();

                if (!m_mixed_filament_drag_active || m_mixed_filament_drag_source_mixed_id >= mixed.size()) {
                    m_mixed_filament_drag_active = false;
                    m_mixed_filament_drag_source_mixed_id = size_t(-1);
                    evt.StopPropagation();
                    return;
                }

                const size_t source_mixed_id = m_mixed_filament_drag_source_mixed_id;
                m_mixed_filament_drag_active = false;
                m_mixed_filament_drag_source_mixed_id = size_t(-1);

                std::vector<size_t> current_mixed_ids;
                current_mixed_ids.reserve(m_mixed_filament_row_bindings.size());
                for (const auto &binding : m_mixed_filament_row_bindings) {
                    if (binding.mixed_id < mixed.size() && !mixed[binding.mixed_id].deleted)
                        current_mixed_ids.emplace_back(binding.mixed_id);
                }

                const auto source_it = std::find(current_mixed_ids.begin(), current_mixed_ids.end(), source_mixed_id);
                if (source_it == current_mixed_ids.end()) {
                    evt.StopPropagation();
                    return;
                }

                const size_t source_pos = size_t(std::distance(current_mixed_ids.begin(), source_it));
                size_t       insert_pos = drop_insert_position();
                insert_pos = std::min(insert_pos, current_mixed_ids.size());

                current_mixed_ids.erase(source_it);
                if (insert_pos > source_pos)
                    --insert_pos;
                insert_pos = std::min(insert_pos, current_mixed_ids.size());
                current_mixed_ids.insert(current_mixed_ids.begin() + ptrdiff_t(insert_pos), source_mixed_id);

                std::vector<uint64_t> reordered_stable_ids;
                reordered_stable_ids.reserve(current_mixed_ids.size());
                for (const size_t row_mixed_id : current_mixed_ids) {
                    if (row_mixed_id < mixed.size() && mixed[row_mixed_id].stable_id != 0)
                        reordered_stable_ids.emplace_back(mixed[row_mixed_id].stable_id);
                }

                if (reordered_stable_ids != current_mixed_filament_ui_order()) {
                    m_mixed_filament_ui_order = std::move(reordered_stable_ids);
                    update_mixed_filament_panel(false);
                }

                evt.StopPropagation();
            });

            target->Bind(wxEVT_MOUSE_CAPTURE_LOST, [release_drag_capture](wxMouseCaptureLostEvent &) {
                release_drag_capture();
            });
        };

        header_panel->SetToolTip(auto_row ?
            _L("Click to edit automatic mixed filament settings (saved as custom).") :
            _L("Click to expand/retract mixed filament settings"));
        bind_toggle_target(row);
        bind_toggle_target(header_panel);
        bind_toggle_target(name_label);
        bind_toggle_target(summary_label);
        bind_toggle_target(swatch);
        bind_hover_target(row);
        bind_hover_target(header_panel);
        bind_hover_target(name_label);
        bind_hover_target(summary_label);
        bind_hover_target(swatch);
        bind_hover_target(drag_handle);
        bind_drag_target(drag_handle);

        del_btn->Bind(wxEVT_LEFT_UP, [](wxMouseEvent &evt) {
            evt.StopPropagation();
            evt.Skip();
        });

        if (m_expanded_mixed_filament_rows.count(mixed_id) != 0) {
            ensure_editor();
            editor_host->Show();
        }

        row->SetSizer(row_sizer);
        rows_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, m_owner->FromDIP(2));
        rows_sizer->AddSpacer(m_owner->FromDIP(2));
    }

    rows_sizer->AddSpacer(m_owner->FromDIP(2));
    rows_scroller->Layout();
    rows_scroller->FitInside();
    adjust_rows_scroller_height();
    if (prev_rows_view_y > 0)
        rows_scroller->Scroll(0, prev_rows_view_y);

    content_sizer->Add(rows_scroller, 0, wxEXPAND | wxLEFT | wxRIGHT, m_owner->FromDIP(2));
    content_sizer->AddSpacer(m_owner->FromDIP(2));
    m_panel_mixed_filaments_content->Layout();
    m_host.layout_scrolled();
    m_host.layout_owner();
    refresh_model_canvas_colors();
    update_color_mix_panel();
#endif // Mixed Filaments panel UI
}

void MixedFilamentsPanel::merge_mixed_filament(size_t from_id, size_t to_id,
                                     bool skip_update, bool skip_serialize)
{
    // Merge a mixed filament into another filament (physical or mixed)
    // This marks the source as deleted and remaps all objects using it
    
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return;
    auto& pb = *preset_bundle;
    const size_t num_physical = pb.filament_presets.size();

    // Validate parameters
    size_t total_filaments = pb.mixed_filaments.total_filaments(num_physical);
    if (from_id >= total_filaments || to_id >= total_filaments) {
        BOOST_LOG_TRIVIAL(error) << "merge_mixed_filament: Invalid filament ID. from_id=" 
                                 << from_id << " to_id=" << to_id << " total=" << total_filaments;
        return;
    }
    
    // Prevent self-merge
    if (from_id == to_id) {
        BOOST_LOG_TRIVIAL(warning) << "merge_mixed_filament: Cannot merge filament into itself: " << from_id;
        return;
    }
    
    // Verify source is actually a mixed filament
    if (!pb.mixed_filaments.is_mixed((unsigned int)(from_id + 1), num_physical)) {
        BOOST_LOG_TRIVIAL(error) << "merge_mixed_filament: Source filament " << from_id 
                                 << " is not a mixed filament";
        return;
    }
    
    // Get source mixed filament index
    int source_mixed_idx = pb.mixed_filaments.mixed_index_from_filament_id((unsigned int)(from_id + 1), num_physical);
    if (source_mixed_idx < 0) {
        BOOST_LOG_TRIVIAL(error) << "merge_mixed_filament: Cannot find mixed filament index for ID " << from_id;
        return;
    }
    
    auto& mfs = pb.mixed_filaments.mixed_filaments();
    if ((size_t)source_mixed_idx >= mfs.size()) {
        BOOST_LOG_TRIVIAL(error) << "merge_mixed_filament: Mixed filament index " << source_mixed_idx 
                                 << " out of range (size=" << mfs.size() << ")";
        return;
    }
    
    BOOST_LOG_TRIVIAL(info) << "Merging mixed filament " << from_id << " into filament " << to_id;
    
    // Build remap table using PresetBundle method
    pb.build_merge_filament_remap(from_id, to_id, total_filaments);
    // The remap is 1-based and also accounts for a mixed target shifting down
    // when it follows the deleted source.
    const std::vector<unsigned int> merge_remap = pb.last_filament_id_remap();
    const int merged_target_id =
        merge_remap.size() > from_id + 1 && merge_remap[from_id + 1] > 0
            ? int(merge_remap[from_id + 1] - 1)
            : -1;
    
    // Mark source mixed filament as deleted
    mfs[source_mixed_idx].deleted = true;
    mfs[source_mixed_idx].enabled = false;
    
    // Persist changes (skip when called from batch cleanup)
    if (!skip_serialize) {
        if (auto* opt = pb.project_config.option<ConfigOptionString>("mixed_filament_definitions"))
            opt->value = pb.mixed_filaments.serialize_custom_entries();
    }
    
    // Save mixed snapshot
    std::vector<unsigned char> is_mixed_snapshot;
    if (auto* opt = pb.project_config.option<ConfigOptionBools>("filament_is_mixed"))
        is_mixed_snapshot = opt->values;
    
    // Pass the remapped target instead of -1 so ObjectList writes the selected
    // physical/mixed target to config-level extruder assignments rather than
    // falling back to filament 1.
    size_t total_after = pb.mixed_filaments.total_filaments(num_physical);
    m_host.plater_on_filaments_delete(total_after, from_id, merged_target_id, is_mixed_snapshot);
    
    BOOST_LOG_TRIVIAL(info) << "Mixed filament merge completed. Total filaments after: " << total_after;
    
    // Update UI
    update_color_mix_panel();
    if (!skip_update) {
        m_host.layout_scrolled();
        m_host.plater_update();
    }
}

} // namespace GUI
} // namespace Slic3r
