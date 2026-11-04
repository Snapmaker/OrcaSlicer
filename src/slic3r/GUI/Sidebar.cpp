#include "Plater.hpp"
#include "app/SlicingController.hpp"   // H2 2.3：组合根服务直调
#include "app/SceneRefreshCoordinator.hpp"   // H2 2.3：组合根服务直调
#include "app/ProjectSession.hpp"   // H2 2.3：组合根服务直调
#include "app/MixingGateKeeper.hpp"   // H2 2.3：组合根服务直调
#include "app/NotificationHub.hpp"   // H2 2.3：组合根服务直调                  // Plater 完整类型（ctor/方法大量使用）
#include "Sidebar.hpp"
#include "GUI_App.hpp"                 // wxGetApp()
#include "PresetComboBoxes.hpp"        // PlaterPresetComboBox
#include "PlaterFilamentComboBox.hpp"  // 阶段 I：上游 2.4.1 的单耗材预设联动组合框
#include "GUI_ObjectList.hpp"          // ObjectList / ObjectDataViewModelNode
#include "GUI_ObjectSettings.hpp"      // ObjectSettings
#include "GUI_ObjectLayers.hpp"        // ObjectLayers
#include "DeviceManager.hpp"           // MachineObject（AMS 同步）
#include "MixedColorMatchHelpers.hpp"  // BatchMatchResult / 混色管理器
#include "MixedFilamentDialog.hpp"        // 混色编辑对话框
#include "MixedFilamentBatchDialog.hpp"
#include "MixedGradientSelector.hpp"       // blend_pair_filament_mixer
#include "MixedColorMatchPanel.hpp"
#include "MixedFilamentBadge.hpp"
#include "MixedFilamentColorMapPanel.hpp"
#include "MainFrame.hpp"                  // MainFrame 完整类型（DPI 对话框基类链）
#include "FlowTypeHelper.hpp"             // FlowType / set_nozzle_volume_types
#include "filamentsync/FilamentData.hpp"  // MixedFilamentPreviewInfo
#include "filamentsync/SyncConfirmDialog.hpp" // SyncRichConfirmDialog
#include "filamentsync/SyncFilamentColorDialog.hpp" // SyncFilamentColorDialog
#include "NotificationManager.hpp"            // CustomNotification / RegularNotificationLevel
#include "CreatePresetsDialog.hpp"        // CreatePrinterPresetDialog / CreatePresetSuccessfulDialog
#include "PhysicalPrinterDialog.hpp"
#include "WipeTowerDialog.hpp"            // WipingDialog
#include "libslic3r/filament_mixer.h"
#include "Search.hpp"                  // Search::SearchObjectDialog（priv 成员 dia）
#include "Tab.hpp"                     // 喷嘴设置/预设联动
#include "wxExtensions.hpp"            // ScalableButton / ConfigOptionsGroup / nozzle_type_key_to_label / 翻译宏
#include "Widgets/ComboBox.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ProgressDialog.hpp"  // 批量物理删除进度（原 Plater.cpp:163）
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/FlushVolCalc.hpp"
#include "../Utils/GatewayDevice.hpp"  // nozzle-sync：GatewayDevice::query_machine_info（main 版打印标题同步，2.8 ①）
#include <atomic>
#include <thread>

#include <wx/bmpcbox.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/tokenzr.h>
#include <wx/control.h>
#include <wx/custombgwin.h>
#include <wx/dcbuffer.h>
#include <wx/popupwin.h>
#include <wx/progdlg.h>
#include <wx/numdlg.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/settings.h>
#ifdef _WIN32
#include <wx/richtooltip.h>
#endif

#include "Mixing/MixingColorMatchDialog.hpp"
#include "Mixing/MixingGradientWeightsDialog.hpp"
#include "Mixing/MixedFilamentConfigPanel.hpp"
#include "Mixing/MixedFilamentDragHandle.hpp"
#include "Mixing/MixingBatchHelpers.hpp"
#include "Mixing/MixedFilamentsPanel.hpp"   // 阶段 B2：混色面板壳类化（Sidebar 只留布局宿主与门面转发）
#include "app/BatchMatchCoordinator.hpp"    // 阶段 B3：批量匹配编排服务
#include "app/FilamentService.hpp"
#include "app/PlateController.hpp"   // H2 收口：partplate_list accessor 供给          // 阶段 G1：耗材/AMS 编排服务（门面转发）

namespace Slic3r {
namespace GUI {

#define PRINTER_THUMBNAIL_SIZE (wxSize(FromDIP(48), FromDIP(48)))
#define PRINTER_THUMBNAIL_SIZE_SMALL (wxSize(FromDIP(32), FromDIP(32)))
#define PRINTER_PANEL_SIZE_SMALL (wxSize(FromDIP(98), FromDIP(68)))
#define PRINTER_PANEL_SIZE_WIDEN (wxSize(FromDIP(136), FromDIP(68)))
#define PRINTER_PANEL_SIZE (wxSize(FromDIP(98), FromDIP(98)))

wxDEFINE_EVENT(EVT_SCHEDULE_BACKGROUND_PROCESS,     SimpleEvent);

namespace {

std::string extract_base_filament_name(const std::string& full_name)
{
    std::string base = full_name;
    size_t at_pos = base.find('@');
    if (at_pos != std::string::npos) {
        base = base.substr(0, at_pos);
        base.erase(0, base.find_first_not_of(" \t\n\r"));
        base.erase(base.find_last_not_of(" \t\n\r") + 1);
    }
    return base;
}

// Resolve a machine filament name to a matching local filament preset.
// Filament presets follow the convention "BaseName @Model nozzle",
// e.g. "Generic PA-CF @U1 0.4 nozzle".  Split by '@' to extract the
// base name, trim, and compare exactly — so "Generic PA" does NOT
// accidentally match "Generic PA-CF".
Preset* resolve_filament_preset(PresetBundle* preset_bundle, 
    const std::string& filament_name, const std::string& filament_type)
{
    if (!preset_bundle || filament_name.empty())
        return nullptr;

    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    for (auto& preset : preset_bundle->filaments) {
        if (!preset.is_compatible || !preset.is_system)
            continue;

        std::string base = extract_base_filament_name(preset.name);
        if (to_lower(base) == to_lower(filament_name)) {
            return &preset;
        }
    }

    const std::string generic_prefix = "Generic ";
    std::string generic_base = generic_prefix + filament_type;
    for (auto& preset : preset_bundle->filaments) {
        if (!preset.is_compatible || !preset.is_system)
            continue;

        std::string base = extract_base_filament_name(preset.name);
        std::string type = preset.config.opt_string("filament_type", static_cast<unsigned int>(0));

        if ((to_lower(type) == to_lower(filament_type)) && 
                (to_lower(base) == to_lower(generic_base))) {
            return &preset;
        }
    }

    return nullptr;
}

void build_design_filament_list(PresetBundle* preset_bundle, std::vector<FilamentData>& out_list)
{
    if (!preset_bundle)
        return;

    const auto& filament_presets = preset_bundle->filament_presets;
    const auto* colors_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    const auto* multiColorsOpt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_multi_colors");
    const auto* colorModeOpt   = preset_bundle->project_config.option<ConfigOptionInts>("filament_colour_mode");

    for (size_t i = 0; i < filament_presets.size(); ++i) {
        FilamentData fd;
        fd.m_index = i;

        Preset* preset = preset_bundle->filaments.find_preset(filament_presets[i]);
         if (preset) {
            fd.m_name = preset->label(false);
            const auto* type_opt = preset->config.option<ConfigOptionStrings>("filament_type");
            if (type_opt && !type_opt->values.empty())
                fd.m_type = type_opt->values[0];
        } else {
            fd.m_name = extract_base_filament_name(filament_presets[i]);
        }

        if (fd.m_type.empty()) {
            const auto* type_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_type");
            if (type_opt && i < type_opt->values.size())
                fd.m_type = type_opt->values[i];
        }

        {
            std::vector<std::string> filamentColors;
            FilamentColorMode mode = FilamentColorMode::Segment;

            if (multiColorsOpt && i < multiColorsOpt->values.size() && !multiColorsOpt->values[i].empty())
                filamentColors = SplitFilamentMultiColors(multiColorsOpt->values[i]);
            else if (colors_opt && i < colors_opt->values.size())
                filamentColors = {colors_opt->values[i]};

            if (colorModeOpt && i < colorModeOpt->values.size())
                mode = FilamentColorModeFromConfig(colorModeOpt->values[i]);

            fd.m_color = FilamentColor::FromColors(filamentColors, mode);
        }

        out_list.push_back(std::move(fd));
    }
}

void build_machine_filament_list(PresetBundle* preset_bundle, std::vector<FilamentData>& out_list)
{
    if (!preset_bundle)
        return;

    for (const auto& info : preset_bundle->m_connect_machine_info_list) {
        FilamentData fd;
        fd.m_index = info.index;
        fd.m_name  = info.filament_info;
        fd.m_type  = info.filament_type;
        
        if (!info.color_info.empty() || !info.multiColors.empty()) {
            fd.m_color = FilamentColor::FromColors(info.multiColors, info.colorMode, info.color_info);
        }

        out_list.push_back(std::move(fd));
    }
}

} // namespace

// Nozzle diameter selection when multiple diameters are reported (e.g. U1 sync).
// diameters_raw: list from device (may have duplicates or fewer than 4). Dedup and full-list logic inside.
namespace {
class NozzleDiameterSelectDialog : public DPIDialog
{
    RadioGroup* m_radio = nullptr;
    std::vector<std::string> m_diameters;

public:
    NozzleDiameterSelectDialog(wxWindow* parent, const wxString& message, const wxString& caption,
                               const std::vector<std::string>& diameters_raw)
        : DPIDialog(parent, wxID_ANY, caption, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    {
        static const std::vector<std::string> full_list = {"0.2", "0.4", "0.6", "0.8"};
        std::set<std::string> returned_set(diameters_raw.begin(), diameters_raw.end());
        std::vector<bool> item_enabled(full_list.size(), true);
        bool any_enabled = false;
        for (size_t i = 0; i < full_list.size(); ++i) {
            bool in = (returned_set.count(full_list[i]) > 0);
            item_enabled[i] = in;
            if (in) any_enabled = true;
        }
        if (!any_enabled)
            item_enabled.assign(full_list.size(), true);
        m_diameters = full_list;

        SetBackgroundColour(*wxWHITE);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        wxStaticText* msg = new wxStaticText(this, wxID_ANY, message);
        msg->Wrap(FromDIP(400));
        sizer->Add(msg, 0, wxALL, FromDIP(10));
        std::vector<wxString> labels;
        for (const auto& d : m_diameters)
            labels.push_back(_L("Nozzle") + ": " + from_u8(d) + "mm");
        m_radio = new RadioGroup(this, labels, wxHORIZONTAL, 2);
        for (size_t i = 0; i < item_enabled.size(); ++i)
            if (!item_enabled[i])
                m_radio->SetItemEnabled((int)i, false);
        int first = 0;
        for (; first < (int)item_enabled.size(); ++first)
            if (item_enabled[first]) break;
        if (first >= (int)item_enabled.size()) first = 0;
        m_radio->SetSelection(first, false);
        sizer->Add(m_radio, 0, wxALL, FromDIP(10));
        auto* btns = new DialogButtons(this, {"OK", "Cancel"});
        btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(btns, 0, wxEXPAND);
        SetSizer(sizer);
        Layout();
        Fit();
        Centre(wxBOTH);
        wxGetApp().UpdateDlgDarkUI(this);
    }
    int GetSelection() const { return m_radio ? m_radio->GetSelection() : -1; }
    std::string GetSelectedDiameter() const {
        int idx = GetSelection();
        return (idx >= 0 && idx < (int)m_diameters.size()) ? m_diameters[idx] : std::string();
    }
    void on_dpi_changed(const wxRect& suggested_rect) override {}
};

} // namespace



int SidebarProps::TitlebarMargin() { return 8; }  // Use as side margins on titlebar. Has less margin on sides to create separation with its content
int SidebarProps::ContentMargin()  { return 12; } // Use as side margins contents of title
int SidebarProps::IconSpacing()    { return 10; } // Use on main elements
int SidebarProps::ElementSpacing() { return 5; }  // Use if elements has relation between them like edit button for combo box etc.

// CustomNotebook.h
#pragma once

#include <wx/wx.h>
#include <vector>

class CustomNotebook : public wxControl
{
public:
    CustomNotebook(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize)
        : wxControl(parent, id, pos, size, wxBORDER_NONE), m_selectedIndex(-1)
    {
        // Figma 27551-61181: each segment is a fixed 70x24 button with 13 px inner
        // padding, centered text and square corners. The outer radius matches the
        // parent nozzle card (StaticBox::SetCornerRadius(8), raw pixels).
        m_tabWidth    = FromDIP(70);
        m_tabHeight   = FromDIP(24);
        m_tabPadding  = FromDIP(13);
        m_roundRadius = 8;

        SetBackgroundStyle(wxBG_STYLE_PAINT);
        UpdateColors();

        Bind(wxEVT_PAINT, &CustomNotebook::OnPaint, this);
        Bind(wxEVT_ERASE_BACKGROUND, &CustomNotebook::OnEraseBackground, this);
        Bind(wxEVT_LEFT_DOWN, &CustomNotebook::OnLeftDown, this);
        Bind(wxEVT_SIZE, &CustomNotebook::OnSize, this);
    }

    void AddPage(wxWindow* page, const wxString& text)
    {
        m_tabs.push_back({text, page});
        if (page) {
            page->Reparent(this);
            page->Hide();
            page->SetBackgroundColour(m_selectedTabColor);
        }

        if (m_selectedIndex == -1) {
            SetSelection(0);
        }

        UpdateLayout();
        Refresh();
    }

    void DeleteAllPages()
    {
        for (auto& tab : m_tabs) {
            if (tab.page) {
                tab.page->Destroy();
            }
        }
        m_tabs.clear();
        m_selectedIndex = -1;
        UpdateLayout();
        InvalidateBestSize();
        Refresh();
    }

    size_t GetPageCount() const { return m_tabs.size(); }

    wxWindow* GetPage(size_t index) const { return (index < m_tabs.size()) ? m_tabs[index].page : nullptr; }

    int GetSelection() const { return m_selectedIndex; }

    void SetSelection(size_t index)
    {
        if (index >= m_tabs.size() || static_cast<int>(index) == m_selectedIndex)
            return;

        if (m_selectedIndex != -1 && m_tabs[m_selectedIndex].page) {
            m_tabs[m_selectedIndex].page->Hide();
        }

        m_selectedIndex = index;

        if (m_selectedIndex != -1 && m_tabs[m_selectedIndex].page) {
            m_tabs[m_selectedIndex].page->Show();
        }

        UpdateLayout();
        Refresh();
    }

protected:
    void OnPaint(wxPaintEvent& event)
    {
        UpdateColors();

        wxPaintDC dc(this);

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_bgColor));
        dc.DrawRectangle(GetClientRect());

        dc.SetPen(wxPen(m_dividerColor, 1));
        dc.SetBrush(wxBrush(m_dividerColor));
        int trackW = GetSize().x;
        int rh     = std::min(m_roundRadius, m_tabHeight / 2); // guard very short strips
        // rounded-top shape: all-corner rounded rect + rectangle patching the bottom half
        dc.DrawRoundedRectangle(0, 0, trackW, 2 * rh, rh);
        dc.DrawRectangle(0, rh, trackW, m_tabHeight - rh);

        wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        font.SetPointSize(m_textSize);
        dc.SetFont(font);

        auto height = dc.GetCharHeight();
        if (height > m_tabHeight - 2) {
            m_tabHeight = height + 2;
            Layout();
        }

        int xPos = 0;
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            bool isSelected = static_cast<int>(i) == m_selectedIndex;

            int textWidth, textHeight;
            dc.GetTextExtent(m_tabs[i].text, &textWidth, &textHeight);
            int tabWidth = GetTabWidth(textWidth);

            if (isSelected) {
                // teal background for the selected tab.
                const wxColour selColor(0x00, 0x96, 0x88); // #009688
                dc.SetPen(wxPen(selColor, 1));
                dc.SetBrush(wxBrush(selColor));
                // Round only the outer top corner(s) that meet the parent card's
                // rounded corner; inner segment corners stay square (Figma).
                bool roundLeft  = (i == 0);
                bool roundRight = (xPos + tabWidth >= trackW - 1);
                if (roundLeft || roundRight) {
                    dc.DrawRoundedRectangle(xPos, 0, tabWidth, 2 * rh, rh);
                    if (!roundLeft)
                        dc.DrawRectangle(xPos, 0, tabWidth - rh, rh);
                    if (!roundRight)
                        dc.DrawRectangle(xPos + rh, 0, tabWidth - rh, rh);
                    dc.DrawRectangle(xPos, rh, tabWidth, m_tabHeight - rh);
                } else {
                    dc.DrawRectangle(xPos, 0, tabWidth, m_tabHeight);
                }
            }

            dc.SetTextForeground(isSelected ? m_selectedTextColor : m_textColor);
            int xText = xPos + (tabWidth - textWidth) / 2;
            dc.DrawText(m_tabs[i].text, xText, (m_tabHeight - textHeight) / 2);

            xPos += tabWidth;
        }

        dc.SetPen(wxPen(m_borderColor, 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(GetClientRect(), m_roundRadius);
    }

    void OnLeftDown(wxMouseEvent& event)
    {
        wxPoint pos = event.GetPosition();
        if (pos.y > m_tabHeight) {
            event.Skip();
            return;
        }

        int tabIndex = HitTest(pos);
        if (tabIndex != -1 && tabIndex != m_selectedIndex) {
            SetSelection(tabIndex);
            Refresh();
        }
    }

    void OnSize(wxSizeEvent& event)
    {
        UpdateLayout();
        Refresh();
        event.Skip();
    }

    void OnEraseBackground(wxEraseEvent& event) {}

private:
    struct TabInfo
    {
        wxString  text;
        wxWindow* page;
    };

    // Figma 27551-61181: segments are 70 px wide; only grow when the
    // localized label (e.g. "Left Nozzle") does not fit.
    int GetTabWidth(int textWidth) const
    {
        return std::max(m_tabWidth, textWidth + 2 * m_tabPadding);
    }

    wxRect GetTabRect(size_t index) const
    {
        if (index >= m_tabs.size())
            return wxRect();

        wxClientDC dc(const_cast<CustomNotebook*>(this));
        wxFont     font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        font.SetPointSize(m_textSize);
        dc.SetFont(font);

        int textWidth, textHeight;
        dc.GetTextExtent(m_tabs[index].text, &textWidth, &textHeight);
        int tabWidth = GetTabWidth(textWidth);

        int x = 0;
        for (size_t i = 0; i < index; ++i) {
            dc.GetTextExtent(m_tabs[i].text, &textWidth, &textHeight);
            x += GetTabWidth(textWidth);
        }

        return wxRect(x, 0, tabWidth, m_tabHeight);
    }

    int HitTest(const wxPoint& pt) const
    {
        if (pt.y > m_tabHeight)
            return -1;

        wxClientDC dc(const_cast<CustomNotebook*>(this));
        wxFont     font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        font.SetPointSize(m_textSize);
        dc.SetFont(font);

        int xPos = 0;
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            int textWidth, textHeight;
            dc.GetTextExtent(m_tabs[i].text, &textWidth, &textHeight);
            int tabWidth = GetTabWidth(textWidth);

            if (pt.x >= xPos && pt.x <= xPos + tabWidth) {
                return i;
            }

            xPos += tabWidth;
        }

        return -1;
    }

    void UpdateColors()
    {
        bool is_dark = wxGetApp().app_config->get("dark_color_mode") == "1";

        if (!is_dark) {
            m_bgColor           = wxColour(255, 255, 255);
            m_borderColor       = wxColour(240, 240, 240);
            m_selectedTabColor  = wxColour(255, 255, 255);
            m_textColor         = wxColour(107, 107, 107); // #6B6B6B: match the process flow toggle's unselected grey
            m_dividerColor      = wxColour(240, 240, 240);
            m_selectedTextColor = wxColour(255, 255, 255); // white text on the teal selected tab
        } else {
            m_bgColor           = wxColour(45, 45, 49);
            m_borderColor       = wxColour(76, 76, 85);
            m_selectedTabColor  = wxColour(45, 45, 49);
            m_textColor         = wxColour(107, 107, 107); // #6B6B6B: match the process flow toggle's unselected grey
            m_dividerColor      = wxColour(51, 51, 55);
            m_selectedTextColor = wxColour(255, 255, 255);
        }
    }

    void UpdateLayout()
    {
        if (m_selectedIndex != -1 && m_tabs[m_selectedIndex].page) {
            wxSize size = GetSize();
            // Clamp to 0: during construction / first OnSize the notebook can still
            // report a zero height, making the page height negative. GTK rejects the
            // size request (assertion) and the page keeps a stale geometry.
            m_tabs[m_selectedIndex].page->SetSize(2, m_tabHeight + 1, wxMax(size.x - 4, 0), wxMax(size.y - m_tabHeight - 4, 0));
            m_tabs[m_selectedIndex].page->Layout();
        }
    }

    // Include page content so the parent sizer reserves the notebook height instead
    // of allowing following sidebar panels to overlap it.
    wxSize DoGetBestSize() const override
    {
        int best_width  = m_tabWidth;
        int best_height = m_tabHeight;
        for (const auto &tab : m_tabs)
        {
            if (tab.page == nullptr)
                continue;

            const wxSize page_size = tab.page->GetBestSize();
            best_width             = std::max(best_width, page_size.x);
            best_height            = std::max(best_height, m_tabHeight + page_size.y + 4);
        }
        return wxSize(best_width, best_height);
    }

private:
    std::vector<TabInfo> m_tabs;
    int                  m_selectedIndex;

    wxColour m_bgColor;
    wxColour m_borderColor;
    wxColour m_selectedTabColor;
    wxColour m_textColor;
    wxColour m_selectedTextColor;
    wxColour m_dividerColor;

    int m_tabWidth;
    int m_tabHeight;
    int m_tabPadding;
    int m_roundRadius;
#ifdef _WIN32
    int m_textSize = 10;
#else
    int m_textSize = 13;
#endif
};

struct NozzleSyncState
{
    std::atomic<bool> alive{true};
    Sidebar*          sidebar{nullptr};
};

struct Sidebar::priv
{
    Plater *plater;

    wxPanel *scrolled;
    PlaterPresetComboBox *combo_print;
    std::vector<PlaterPresetComboBox*> combos_filament;
    int editing_filament = -1;
    wxBoxSizer *sizer_filaments;
    PlaterPresetComboBox *combo_sla_print;
    PlaterPresetComboBox *combo_sla_material;
    PlaterPresetComboBox* combo_printer = nullptr;
    wxBoxSizer *sizer_params;

    // test
    wxStaticBitmap * image_printer = nullptr;
    StaticBox*      panel_printer_preset = nullptr;
    

    //BBS Sidebar widgets
    wxPanel* m_panel_print_title;
    wxStaticText* m_staticText_print_title;
    wxPanel* m_panel_print_content;
    wxComboBox* m_comboBox_print_preset;
    wxStaticLine* m_staticline1;
    StaticBox* m_panel_filament_title;
    ScalableButton* m_filament_config_icon = nullptr;
    wxStaticText* m_staticText_filament_settings;
    ScalableButton *  m_bpButton_add_filament;
    ScalableButton *  m_bpButton_del_filament;
    ScalableButton *  m_bpButton_ams_filament;
    ScalableButton *  m_bpButton_set_filament;
    ScalableButton *  m_bpButton_sync_filament = nullptr;
    std::shared_ptr<NozzleSyncState> nozzle_sync_state = std::make_shared<NozzleSyncState>();   // 2.8 ③：main 版喷嘴同步共享状态
    int                         m_menu_filament_id = -1;
    wxPanel* m_panel_filament_content;
    wxScrolledWindow* m_scrolledWindow_filament_content;

    std::unique_ptr<MixedFilamentsPanel> mixed_panel;
    std::unique_ptr<app::BatchMatchCoordinator> batch_match;
    bool                m_skip_mixed_filament_sync_once = false;  // Local edits already mutated manager in place.
    // Physical filament scrolled window
    wxScrolledWindow* m_scrolled_filaments = nullptr;
    wxPanel*          m_panel_scrolled_filament_content = nullptr;
    StaticBox*      m_panel_physical_filaments_title = nullptr;

    wxStaticLine* m_staticline2;
    wxPanel* m_panel_project_title;
    ScalableButton* m_filament_icon = nullptr;
    Button * m_flushing_volume_btn = nullptr;
    Button*  m_btn_batch_match      = nullptr;              // Batch color-match mapping
    TextInput* m_search_item = nullptr;
    StaticBox* m_search_bar = nullptr;
    Search::SearchObjectDialog* dia = nullptr;

    // BBS printer config
    StaticBox* m_panel_printer_title = nullptr;
    ScalableButton* m_printer_icon = nullptr;
    ScalableButton* m_printerinfo_syncbtn = nullptr;
    ScalableButton* m_printer_setting = nullptr;
    wxStaticText* m_text_printer_settings = nullptr;
    wxPanel* m_panel_printer_content = nullptr;

    // nozzle notebook  and related controls
    CustomNotebook*                  m_nozzle_notebook{nullptr};
    std::vector<ComboBox*>       m_nozzle_diameter_lists;
    std::vector<ComboBox*>       m_nozzle_flow_lists;
    std::vector<ScalableButton*> m_nozzle_edit_btns;

    ObjectList          *m_object_list{ nullptr };
    ObjectSettings      *object_settings{ nullptr };
    ObjectLayers        *object_layers{ nullptr };

    wxButton *btn_export_gcode;
    wxButton *btn_reslice;
    ScalableButton *btn_send_gcode;
    //ScalableButton *btn_eject_device;
    ScalableButton* btn_export_gcode_removable; //exports to removable drives (appears only if removable drive is connected)

    Search::OptionsSearcher     searcher;

    priv(Plater *plater) : plater(plater) {}
    ~priv();

    void show_preset_comboboxes();
    void jump_to_object(ObjectDataViewModelNode* item);
    void can_search();

#ifdef _WIN32
    wxString btn_reslice_tip;
    void show_rich_tip(const wxString& tooltip, wxButton* btn);
    void hide_rich_tip(wxButton* btn);
#endif
};

Sidebar::priv::~priv()
{
    // BBS
    //delete object_manipulation;
    delete object_settings;
    // BBS
#if 0
    delete frequently_changed_parameters;
#endif
}

void Sidebar::priv::show_preset_comboboxes()
{
    const bool showSLA = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA;

//BBS
#if 0
    for (size_t i = 0; i < 4; ++i)
        sizer_presets->Show(i, !showSLA);

    for (size_t i = 4; i < 8; ++i) {
        if (sizer_presets->IsShown(i) != showSLA)
            sizer_presets->Show(i, showSLA);
    }

    frequently_changed_parameters->Show(!showSLA);
#endif

    scrolled->GetParent()->Layout();
    scrolled->Refresh();
}

void Sidebar::priv::jump_to_object(ObjectDataViewModelNode* item)
{
    m_object_list->selected_object(item);
}

void Sidebar::priv::can_search()
{
    if (m_search_bar->IsShown()) {
        m_search_item->SetFocus();
    }
}

#ifdef _WIN32
using wxRichToolTipPopup = wxCustomBackgroundWindow<wxPopupTransientWindow>;
static wxRichToolTipPopup* get_rtt_popup(wxButton* btn)
{
    auto children = btn->GetChildren();
    for (auto child : children)
        if (child->IsShown())
            return dynamic_cast<wxRichToolTipPopup*>(child);
    return nullptr;
}

void Sidebar::priv::show_rich_tip(const wxString& tooltip, wxButton* btn)
{
    if (tooltip.IsEmpty())
        return;
    wxRichToolTip tip(tooltip, "");
    tip.SetIcon(wxICON_NONE);
    tip.SetTipKind(wxTipKind_BottomRight);
    tip.SetTitleFont(wxGetApp().normal_font());
    tip.SetBackgroundColour(wxGetApp().get_window_default_clr());

    tip.ShowFor(btn);
    // Every call of the ShowFor() creates new RichToolTip and show it.
    // Every one else are hidden.
    // So, set a text color just for the shown rich tooltip
    if (wxRichToolTipPopup* popup = get_rtt_popup(btn)) {
        auto children = popup->GetChildren();
        for (auto child : children) {
            child->SetForegroundColour(wxGetApp().get_label_clr_default());
            // we neen just first text line for out rich tooltip
            return;
        }
    }
}

void Sidebar::priv::hide_rich_tip(wxButton* btn)
{
    if (wxRichToolTipPopup* popup = get_rtt_popup(btn))
        popup->Dismiss();
}
#endif

std::vector<int> get_min_flush_volumes(const DynamicPrintConfig& full_config)
{
    std::vector<int>extra_flush_volumes;
    //const auto& full_config = wxGetApp().preset_bundle->full_config();
    //auto& printer_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;

    const ConfigOption* nozzle_volume_opt = full_config.option("nozzle_volume");
    int nozzle_volume_val = nozzle_volume_opt ? (int)nozzle_volume_opt->getFloat() : 0;

    const ConfigOptionInt* enable_long_retraction_when_cut_opt = full_config.option<ConfigOptionInt>("enable_long_retraction_when_cut");
    int machine_enabled_level = 0;
    if (enable_long_retraction_when_cut_opt) {
        machine_enabled_level = enable_long_retraction_when_cut_opt->value;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get enable_long_retraction_when_cut from config, value=%1%")%machine_enabled_level;
    }
    const ConfigOptionBools* long_retractions_when_cut_opt = full_config.option<ConfigOptionBools>("long_retractions_when_cut");
    bool machine_activated = false;
    if (long_retractions_when_cut_opt) {
        machine_activated = long_retractions_when_cut_opt->values[0] == 1;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get long_retractions_when_cut from config, value=%1%, activated=%2%")%long_retractions_when_cut_opt->values[0] %machine_activated;
    }

    size_t filament_size = full_config.option<ConfigOptionFloats>("filament_diameter")->values.size();
    std::vector<double> filament_retraction_distance_when_cut(filament_size, 18.0f), printer_retraction_distance_when_cut(filament_size, 18.0f);
    std::vector<unsigned char> filament_long_retractions_when_cut(filament_size, 0);
    const ConfigOptionFloats* filament_retraction_distances_when_cut_opt = full_config.option<ConfigOptionFloats>("filament_retraction_distances_when_cut");
    if (filament_retraction_distances_when_cut_opt) {
        filament_retraction_distance_when_cut = filament_retraction_distances_when_cut_opt->values;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get filament_retraction_distance_when_cut from config, size=%1%, values=%2%")%filament_retraction_distance_when_cut.size() %filament_retraction_distances_when_cut_opt->serialize();
    }

    const ConfigOptionFloats* printer_retraction_distance_when_cut_opt = full_config.option<ConfigOptionFloats>("retraction_distances_when_cut");
    if (printer_retraction_distance_when_cut_opt) {
        printer_retraction_distance_when_cut = printer_retraction_distance_when_cut_opt->values;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get retraction_distances_when_cut from config, size=%1%, values=%2%")%printer_retraction_distance_when_cut.size() %printer_retraction_distance_when_cut_opt->serialize();
    }

    const ConfigOptionBools* filament_long_retractions_when_cut_opt = full_config.option<ConfigOptionBools>("filament_long_retractions_when_cut");
    if (filament_long_retractions_when_cut_opt) {
        filament_long_retractions_when_cut = filament_long_retractions_when_cut_opt->values;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get filament_long_retractions_when_cut from config, size=%1%, values=%2%")%filament_long_retractions_when_cut.size() %filament_long_retractions_when_cut_opt->serialize();
    }

    for (size_t idx = 0; idx < filament_size; ++idx) {
        int extra_flush_volume = nozzle_volume_val;
        int retract_length = machine_enabled_level && machine_activated ? printer_retraction_distance_when_cut[0] : 0;

        unsigned char filament_activated = filament_long_retractions_when_cut[idx];
        double filament_retract_length = filament_retraction_distance_when_cut[idx];

        if (filament_activated == 0)
            retract_length = 0;
        else if (filament_activated == 1 && machine_enabled_level == LongRectrationLevel::EnableFilament) {
            if (!std::isnan(filament_retract_length))
                retract_length = (int)filament_retraction_distance_when_cut[idx];
            else
                retract_length = printer_retraction_distance_when_cut[0];
        }

        extra_flush_volume -= PI * 1.75 * 1.75 / 4 * retract_length;
        extra_flush_volumes.emplace_back(extra_flush_volume);
    }
    return extra_flush_volumes;
}


// Sidebar / public

struct DynamicFilamentList : DynamicList
{
    std::vector<std::pair<wxString, wxBitmap *>> items;

    void apply_on(Choice *c) override
    {
        if (items.empty())
            update(true);
        auto cb = dynamic_cast<ComboBox *>(c->window);
        auto n  = cb->GetSelection();
        cb->Clear();
        cb->Append(_L("Default"));
        for (auto i : items) {
            cb->Append(i.first, *i.second);
        }
        if (n < cb->GetCount())
            cb->SetSelection(n);
    }
    wxString get_value(int index) override
    {
        wxString str;
        str << index;
        return str;
    }
    int index_of(wxString value) override
    {
        long n = 0;
        return (value.ToLong(&n) && n <= items.size()) ? int(n) : -1;
    }
    void update(bool force = false)
    {
        items.clear();
        if (!force && m_choices.empty())
            return;
        auto icons = get_extruder_color_icons(true);
        auto presets = wxGetApp().preset_bundle->filament_presets;
        for (int i = 0; i < presets.size(); ++i) {
            wxString str;
            std::string type;
            wxGetApp().preset_bundle->filaments.find_preset(presets[i])->get_filament_type(type);
            str << type;
            items.push_back({str, icons[i]});
        }
        DynamicList::update();
    }
};

struct DynamicFilamentList1Based : DynamicFilamentList
{
    void apply_on(Choice *c) override
    {
        if (items.empty())
            update(true);
        auto cb = dynamic_cast<ComboBox *>(c->window);
        auto n  = cb->GetSelection();
        cb->Clear();
        for (auto i : items) {
            cb->Append(i.first, *i.second);
        }
        if (n < cb->GetCount())
            cb->SetSelection(n);
    }
    wxString get_value(int index) override
    {
        wxString str;
        str << index+1;
        return str;
    }
    int index_of(wxString value) override
    {
        long n = 0;
        if(!value.ToLong(&n))
            return -1;
        --n;
        return (n >= 0 && n <= items.size()) ? int(n) : -1;
    }
    void update(bool force = false)
    {
        items.clear();
        if (!force && m_choices.empty())
            return;
        auto icons = get_extruder_color_icons(true);
        auto presets = wxGetApp().preset_bundle->filament_presets;
        for (int i = 0; i < presets.size(); ++i) {
            wxString str;
            std::string type;
            wxGetApp().preset_bundle->filaments.find_preset(presets[i])->get_filament_type(type);
            str << type;
            items.push_back({str, icons[i]});
        }
        DynamicList::update();
    }

};


static DynamicFilamentList dynamic_filament_list;
static DynamicFilamentList1Based dynamic_filament_list_1_based;

// nozzle_type_key_to_label: moved to wxExtensions.{hpp,cpp} (phase A, shared with Plater.cpp)

Sidebar::Sidebar(Plater *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(42 * wxGetApp().em_unit(), -1)), p(new priv(parent))
{
    p->nozzle_sync_state->sidebar = this;   // 2.8 ③：alive 守卫回指（main bac6fd5:2152 逐字）
    Choice::register_dynamic_list("support_filament", &dynamic_filament_list);
    Choice::register_dynamic_list("support_interface_filament", &dynamic_filament_list);
    Choice::register_dynamic_list("wall_filament", &dynamic_filament_list_1_based);
    Choice::register_dynamic_list("sparse_infill_filament", &dynamic_filament_list_1_based);
    Choice::register_dynamic_list("solid_infill_filament", &dynamic_filament_list_1_based);
    Choice::register_dynamic_list("wipe_tower_filament", &dynamic_filament_list);

    p->scrolled = new wxPanel(this);
    p->scrolled->SetBackgroundColour(*wxWHITE);


    SetFont(wxGetApp().normal_font());
#ifndef __APPLE__
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDarkUI(p->scrolled);
#else
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif
#endif

    int em = wxGetApp().em_unit();
    //BBS refine layout and styles
    // Sizer in the scrolled area
    auto* scrolled_sizer = m_scrolled_sizer = new wxBoxSizer(wxVERTICAL);
    p->scrolled->SetSizer(scrolled_sizer);

    {
        MixedFilamentsPanelHost host;
        host.filament_count = [this]() { return p->combos_filament.size(); };
        host.set_add_filament_enabled = [this](bool enabled) {
            if (p->m_bpButton_add_filament)
                p->m_bpButton_add_filament->Enable(enabled);
        };
        host.layout_scrolled = [this]() { if (m_scrolled_sizer) m_scrolled_sizer->Layout(); };
        host.layout_owner = [this]() { Layout(); };
        host.change_filament = [this](size_t from_id, size_t to_id) { change_filament(from_id, to_id); };
        host.set_skip_mixed_filament_sync_once = [this]() { p->m_skip_mixed_filament_sync_once = true; };
        host.plater_post_slice_state_change_update = []() {
            if (Plater* plater = wxGetApp().plater())
                wxGetApp().slicing().post_slice_state_change_update();
        };
        host.plater_on_filaments_change = [](size_t count) {
            if (Plater* plater = wxGetApp().plater())
                wxGetApp().filament_svc().on_filaments_change(count);
        };
        host.plater_on_filaments_delete = [](size_t extruders_count, size_t filament_id,
                                             int replace_filament_id,
                                             const std::vector<unsigned char>& is_mixed_snapshot) {
            if (Plater* plater = wxGetApp().plater())
                wxGetApp().filament_svc().on_filaments_delete(extruders_count, filament_id, replace_filament_id, is_mixed_snapshot);
        };
        host.plater_update = []() {
            if (Plater* plater = wxGetApp().plater())
                wxGetApp().refresh().update_gate(false, false);
        };
        host.plater_update_project_dirty_from_presets = []() {
            if (Plater* plater = wxGetApp().plater())
                wxGetApp().session().update_project_dirty_from_presets();
        };
        host.refresh_model_canvas_colors = []() {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr)
                return;
            auto refresh_canvas = [](GLCanvas3D* canvas) {
                if (canvas == nullptr || !canvas->is_initialized())
                    return;
                canvas->update_volumes_colors_by_extruder();
                canvas->render();
            };
            refresh_canvas(plater->get_view3D_canvas3D());
            refresh_canvas(plater->get_assmeble_canvas3D());
        };
        host.next_color_for_filament = []() { return Plater::get_next_color_for_filament(); };
        p->mixed_panel = std::make_unique<MixedFilamentsPanel>(this, std::move(host));
    }
    p->batch_match = std::make_unique<app::BatchMatchCoordinator>(*this);   // 阶段 B3：编排服务（构造注入 Sidebar&，cpp-only）

    wxColour title_bg = wxColour(248, 248, 248);
    wxColour inactive_text = wxColour(86, 86, 86);
    wxColour active_text = wxColour(0, 0, 0);
    wxColour static_line_col = wxColour(166, 169, 170);

#ifdef __WINDOWS__
    p->scrolled->SetDoubleBuffered(true);
#endif //__WINDOWS__

    // add printer
    {
        /***************** 1. create printer title bar    **************/
        // 1.1 create title bar resources
        p->m_panel_printer_title = new StaticBox(p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
        p->m_panel_printer_title->SetBackgroundColor(title_bg);
        p->m_panel_printer_title->SetBackgroundColor2(0xF1F1F1);

        p->m_printer_icon = new ScalableButton(p->m_panel_printer_title, wxID_ANY, "printer");
        p->m_text_printer_settings = new Label(p->m_panel_printer_title, _L("Printer"), LB_PROPAGATE_MOUSE_EVENT);

        // Use ams_fila_sync icon (sync_nozzle_info.svg does not exist in resources)
        p->m_printerinfo_syncbtn = new ScalableButton(p->m_panel_printer_title, wxID_ANY, "nozzle_sync");
        p->m_printerinfo_syncbtn->SetCursor(wxCURSOR_HAND);
        p->m_printerinfo_syncbtn->SetToolTip(_L("Synchronize nozzle information"));
        p->m_printerinfo_syncbtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
            // The gateway snapshot fetch is a blocking HTTP call (up to ~5s on a slow link), so run
            // it on a worker thread and marshal the result back instead of freezing the main UI.
            if (!p->m_printerinfo_syncbtn->IsEnabled())
                return;
            p->m_printerinfo_syncbtn->Disable();

            const auto gateway = wxGetApp().gateway_service();
            // wxWeakRef mutates wxTrackable state and must not be copied or destroyed on this worker.
            const auto sync_state = p->nozzle_sync_state;
            std::thread([sync_state, gateway]() {
                std::string              machine_type = "";
                std::vector<std::string> nozzle_diameters;
                std::vector<std::string> nozzle_volume_types;
                std::string              device_name = "";
                // The gateway device snapshot doubles as the connected-guard: a successful
                // GET /api/cache/all proves a live device channel (avoids probe + query serial waits).
                const bool got_machine_info = Gateway::GatewayDevice::query_machine_info(gateway, machine_type, nozzle_diameters, nozzle_volume_types,
                                                                                          device_name);

                wxTheApp->CallAfter([sync_state, got_machine_info, machine_type = std::move(machine_type),
                                     nozzle_diameters = std::move(nozzle_diameters),
                                     nozzle_volume_types = std::move(nozzle_volume_types),
                                     device_name = std::move(device_name)]() {
                    if (!sync_state->alive.load(std::memory_order_acquire))
                        return;
                    Sidebar *self = sync_state->sidebar;
                    if (self->p->m_printerinfo_syncbtn)
                        self->p->m_printerinfo_syncbtn->Enable();
                    self->apply_nozzle_sync_result(got_machine_info, std::move(machine_type), std::move(nozzle_diameters),
                                                   std::move(nozzle_volume_types), std::move(device_name));
                });
            }).detach();
        });
        
        p->m_printer_setting = new ScalableButton(p->m_panel_printer_title, wxID_ANY, "settings");
        p->m_printer_setting->SetToolTip(_L("settings"));
        p->m_printer_setting->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
            wxGetApp().run_wizard(ConfigWizard::RR_USER, ConfigWizard::SP_PRINTERS);
            });

        wxBoxSizer* h_sizer_title = new wxBoxSizer(wxHORIZONTAL);
        h_sizer_title->Add(p->m_printer_icon, 0, wxALIGN_CENTRE | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
        h_sizer_title->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));
        h_sizer_title->Add(p->m_text_printer_settings, 0, wxALIGN_CENTER);
        h_sizer_title->AddStretchSpacer();
        h_sizer_title->Add(p->m_printerinfo_syncbtn, 0, wxALIGN_CENTER);
        h_sizer_title->wxSizer::AddSpacer(FromDIP(10));
        h_sizer_title->Add(p->m_printer_setting, 0, wxALIGN_CENTER);
        h_sizer_title->AddSpacer(FromDIP(SidebarProps::TitlebarMargin()));
        h_sizer_title->SetMinSize(-1, 3 * em);

        p->m_panel_printer_title->SetSizer(h_sizer_title);
        p->m_panel_printer_title->Layout();

        // add printer title
        scrolled_sizer->Add(p->m_panel_printer_title, 0, wxEXPAND | wxALL, 0);
        p->m_panel_printer_title->Bind(wxEVT_LEFT_UP, [this] (auto & e)
        {
            const bool expanded = p->m_panel_printer_content->IsShown();
            p->m_panel_printer_content->Show(!expanded);
            p->m_panel_printer_content->SetMaxSize(expanded ? wxSize(-1, 0) : wxSize(-1, -1));
            m_scrolled_sizer->Layout();
        });

        // add spliter 2
        auto spliter_2 = new ::StaticLine(p->scrolled);
        spliter_2->SetLineColour("#CECECE");
        scrolled_sizer->Add(spliter_2, 0, wxEXPAND);


        /*************************** 2. add printer content ************************/
        p->m_panel_printer_content = new wxPanel(p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
        p->m_panel_printer_content->SetBackgroundColour(wxColour(255, 255, 255));

        StateColor panel_bd_col(std::pair<wxColour, int>(wxColour(0x00AE42), StateColor::Pressed),
                                std::pair<wxColour, int>(wxColour(0x00AE42), StateColor::Hovered),
                                std::pair<wxColour, int>(wxColour(0xEEEEEE), StateColor::Normal));

        p->panel_printer_preset = new StaticBox(p->m_panel_printer_content);
        p->panel_printer_preset->SetCornerRadius(8);
        p->panel_printer_preset->SetBorderColor(panel_bd_col);
        p->panel_printer_preset->SetMinSize(PRINTER_PANEL_SIZE_SMALL);
        p->panel_printer_preset->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { p->combo_printer->wxEvtHandler::ProcessEvent(evt); });

        PlaterPresetComboBox* combo_printer = new PlaterPresetComboBox(p->panel_printer_preset, Preset::TYPE_PRINTER);
        combo_printer->SetWindowStyle(combo_printer->GetWindowStyle() & ~wxALIGN_MASK | wxALIGN_LEFT);
        combo_printer->SetBorderWidth(0);
        
        ScalableBitmap bitmap_printer(p->panel_printer_preset, "printer_placeholder", 48);
        p->image_printer = new wxStaticBitmap(p->panel_printer_preset, wxID_ANY, bitmap_printer.bmp(), wxDefaultPosition,
                                                                 PRINTER_THUMBNAIL_SIZE, 0);
        p->image_printer->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { p->combo_printer->wxEvtHandler::ProcessEvent(evt); });
        
        p->combo_printer = combo_printer;

        combo_printer->bind_edit_button_handler([this, combo_printer]() {
                p->editing_filament = -1;
                if (combo_printer->switch_to_tab())
                    p->editing_filament = 0;
            });
        
        combo_printer->bind_connection_button_handler([this]() {
                wxGetApp().sm_disconnect_current_machine();
                PhysicalPrinterDialog dlg(this->GetParent());
                dlg.ShowModal();
            });

        combo_printer->bind_machine_connecting_button_handler([this]() {
        });
        
        combo_printer->set_show_edit_button(true);
        
        combo_printer->set_connection_tooltip(_L("Connect to printer"));
        combo_printer->set_machine_connecting_tooltip(_L("The machine has been connected and is currently in working mode"));

        connection_btn = nullptr;
        machine_connecting_btn = nullptr;

        wxBoxSizer* vsizer_printer = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* hsizer_printer = new wxBoxSizer(wxHORIZONTAL);

        wxBoxSizer* vsizer = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* hsizer = new wxBoxSizer(wxHORIZONTAL);

        combo_printer->SetWindowStyle(combo_printer->GetWindowStyle() & ~wxALIGN_MASK | wxALIGN_LEFT);

        hsizer->Add(p->image_printer, 0, wxLEFT | wxALIGN_CENTER, FromDIP(4));
        hsizer->Add(combo_printer, 1, wxALIGN_CENTRE | wxLEFT | wxRIGHT, FromDIP(6));
        hsizer->AddSpacer(FromDIP(10));
        p->panel_printer_preset->SetSizer(hsizer);

        hsizer_printer->Add(p->panel_printer_preset, 1, wxEXPAND, 0);
        vsizer_printer->AddSpacer(FromDIP(4));
        vsizer_printer->Add(hsizer_printer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
        vsizer_printer->AddSpacer(FromDIP(10));

        /*vsizer_printer->AddSpacer(FromDIP(16));
        hsizer_printer->Add(p->image_printer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
        hsizer_printer->Add(combo_printer, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
        hsizer_printer->Add(edit_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
        hsizer_printer->Add(FromDIP(8), 0, 0, 0, 0);
        hsizer_printer->Add(connection_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
        hsizer_printer->Add(machine_connecting_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
        hsizer_printer->Add(FromDIP(8), 0, 0, 0, 0);

        vsizer_printer->Add(hsizer_printer, 0, wxEXPAND, 0);*/

        // Bed type selection
        p->panel_printer_preset = new StaticBox(p->m_panel_printer_content, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                                wxTAB_TRAVERSAL | wxBORDER_NONE);
        p->panel_printer_preset->SetCornerRadius(8);
        StateColor panel_bd_col1(std::pair<wxColour, int>(wxColour(0x00AE42), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(0x00AE42), StateColor::Hovered),
                            std::pair<wxColour, int>(wxColour(0xEEEEEE), StateColor::Normal));
        // p->panel_printer_preset->SetBorderColor(panel_bd_col1);
        // p->panel_printer_preset->SetMinSize(PRINTER_PANEL_SIZE_SMALL);

        wxBoxSizer* bed_type_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText* bed_type_title = new wxStaticText(p->panel_printer_preset, wxID_ANY, _L("Bed type"));
        bed_type_title->Wrap(-1);
        bed_type_title->SetFont(Label::Body_14);
        m_bed_type_list = new ComboBox(p->panel_printer_preset, wxID_ANY, wxString(""), wxDefaultPosition, {-1, FromDIP(30)}, 0, nullptr, wxCB_READONLY);
        const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
        if (bed_type_def && bed_type_def->enum_keys_map) {
            for (const auto& item : bed_type_def->enum_labels)
                m_bed_type_list->AppendString(_L(item));
            for (const auto& v : bed_type_def->enum_values)
                m_bed_type_combo_enum_values.push_back(v);
        }

        bed_type_title->Bind(wxEVT_ENTER_WINDOW, [bed_type_title, this](wxMouseEvent &e) {
            e.Skip();
            auto font = bed_type_title->GetFont();
            font.SetUnderlined(true);
            bed_type_title->SetFont(font);
            SetCursor(wxCURSOR_HAND);
        });
        bed_type_title->Bind(wxEVT_LEAVE_WINDOW, [bed_type_title, this](wxMouseEvent &e) {
            e.Skip();
            auto font = bed_type_title->GetFont();
            font.SetUnderlined(false);
            bed_type_title->SetFont(font);
            SetCursor(wxCURSOR_ARROW);
        });
        bed_type_title->Bind(wxEVT_LEFT_UP, [bed_type_title, this](wxMouseEvent &e) {
            wxLaunchDefaultBrowser("https://github.com/SoftFever/OrcaSlicer/wiki/bed-types");
        });

        AppConfig *app_config = wxGetApp().app_config;
        std::string str_bed_type = app_config->get("curr_bed_type");
        int bed_type_value = atoi(str_bed_type.c_str());
        // hotfix: btDefault is added as the first one in BedType, and app_config should not be btDefault
        if (bed_type_value == 0) {
            app_config->set("curr_bed_type", "1");
            bed_type_value = 1;
        }

        int bed_type_idx = bed_type_value - 1;
        m_bed_type_list->Select(bed_type_idx);

        bed_type_sizer->Add(bed_type_title, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
        bed_type_sizer->Add(m_bed_type_list, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(0));
        p->panel_printer_preset->SetSizer(bed_type_sizer);

        vsizer_printer->Add(p->panel_printer_preset, 0, wxEXPAND | wxALL, FromDIP(4));
        vsizer_printer->AddSpacer(FromDIP(8));

        auto& project_config = wxGetApp().preset_bundle->project_config;
        BedType bed_type = (BedType)bed_type_value;
        project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));

        p->m_panel_printer_content->SetSizer(vsizer_printer);
        p->m_panel_printer_content->Layout();
        scrolled_sizer->Add(p->m_panel_printer_content, 0, wxEXPAND, 0);

        StaticBox* nozzle_container = new StaticBox(p->m_panel_printer_content, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                                    wxTAB_TRAVERSAL | wxBORDER_NONE);
        nozzle_container->SetCornerRadius(8);
        // nozzle_container->SetBorderColor(panel_bd_col);

        p->m_nozzle_notebook = new CustomNotebook(nozzle_container, wxID_ANY);

        wxBoxSizer* nozzle_sizer = new wxBoxSizer(wxVERTICAL);
        nozzle_sizer->Add(p->m_nozzle_notebook, 1, wxEXPAND | wxALL, FromDIP(0));
        nozzle_container->SetSizer(nozzle_sizer);
        nozzle_container->SetMinSize(wxSize(-1, FromDIP(80)));

        vsizer_printer->Add(nozzle_container, 0, wxEXPAND | wxALL, FromDIP(4));

        // Initialize nozzle settings
        update_nozzle_settings();
    }

    {
    // add filament title
    p->m_panel_filament_title = new StaticBox(p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    p->m_panel_filament_title->SetBackgroundColor(title_bg);
    p->m_panel_filament_title->SetBackgroundColor2(0xF1F1F1);
    p->m_panel_filament_title->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &e)
    {
        if (e.GetPosition().x > (p->m_flushing_volume_btn->IsShown()
                ? p->m_flushing_volume_btn->GetPosition().x : (p->m_bpButton_ams_filament->GetPosition().x - FromDIP(30))))
            return;
        const bool expanded = p->m_panel_filament_content->IsShown();
        p->m_panel_filament_content->Show(!expanded);
        p->m_panel_filament_content->SetMaxSize(expanded ? wxSize(-1, 0) : wxSize(-1, -1));
        m_scrolled_sizer->Layout();
    });

    wxBoxSizer* bSizer39;
    bSizer39 = new wxBoxSizer( wxHORIZONTAL );
    p->m_filament_config_icon = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "filament_group");
    bSizer39->Add(p->m_filament_config_icon, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
    p->m_staticText_filament_settings = new Label(p->m_panel_filament_title, _L("Filament Management"), LB_PROPAGATE_MOUSE_EVENT);
    bSizer39->Add( p->m_staticText_filament_settings, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
    bSizer39->Add(FromDIP(10), 0, 0, 0, 0);
    bSizer39->SetMinSize(-1, FromDIP(30));

    p->m_panel_filament_title->SetSizer( bSizer39 );
    p->m_panel_filament_title->Layout();
    auto spliter_1 = new ::StaticLine(p->scrolled);
    spliter_1->SetLineColour("#A6A9AA");
    scrolled_sizer->Add(spliter_1, 0, wxEXPAND);
    scrolled_sizer->Add(p->m_panel_filament_title, 0, wxEXPAND | wxALL, 0);
    auto spliter_2 = new ::StaticLine(p->scrolled);
    spliter_2->SetLineColour("#CECECE");
    scrolled_sizer->Add(spliter_2, 0, wxEXPAND);

    bSizer39->AddStretchSpacer(1);

    // BBS
    // add wiping dialog
    //wiping_dialog_button->SetFont(wxGetApp().normal_font());
    p->m_flushing_volume_btn = new Button(p->m_panel_filament_title, _L("Flushing volumes"));
    p->m_flushing_volume_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    p->m_flushing_volume_btn->SetId(wxID_RESET);
    p->m_flushing_volume_btn->Bind(wxEVT_BUTTON, ([parent](wxCommandEvent &e)
        {
            auto& project_config = wxGetApp().preset_bundle->project_config;
            const std::vector<double>& init_matrix = (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values;
            const std::vector<double>& init_extruders = (project_config.option<ConfigOptionFloats>("flush_volumes_vector"))->values;
            ConfigOptionFloat* flush_multi_opt = project_config.option<ConfigOptionFloat>("flush_multiplier");
            float flush_multiplier = flush_multi_opt ? flush_multi_opt->getFloat() : 1.f;

            const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false);
            const auto& full_config = wxGetApp().preset_bundle->full_config();
            const auto& extra_flush_volumes = get_min_flush_volumes(full_config);
            WipingDialog dlg(parent, cast<float>(init_matrix), cast<float>(init_extruders), extruder_colours, extra_flush_volumes, flush_multiplier);
            if (dlg.ShowModal() == wxID_OK) {
                std::vector<float> matrix = dlg.get_matrix();
                std::vector<float> extruders = dlg.get_extruders();
                (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values = std::vector<double>(matrix.begin(), matrix.end());
                (project_config.option<ConfigOptionFloats>("flush_volumes_vector"))->values = std::vector<double>(extruders.begin(), extruders.end());
                (project_config.option<ConfigOptionFloat>("flush_multiplier"))->set(new ConfigOptionFloat(dlg.get_flush_multiplier()));

                wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

                wxGetApp().session().update_project_dirty_from_presets();
                wxPostEvent(parent, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, parent));
            }
        }));

    bSizer39->Add(p->m_flushing_volume_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
    bSizer39->Hide(p->m_flushing_volume_btn);

    // Batch color-match mapping button
    p->m_btn_batch_match = new Button(p->m_panel_filament_title, _L("Color Mixing Match"));
    p->m_btn_batch_match->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    p->m_btn_batch_match->SetToolTip(_L("Automatically calculate the color mixing scheme that best matches the original model colors and complete color mapping.\n"
                                        "Note:\n"
                                        "1. Color mixing match is based on the official recommended filaments. The matched colors may differ from the original model.\n"
                                        "2. The order of the Color Mapping list may differ from that of the Color Mixing list."));
    p->m_btn_batch_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        p->batch_match->run_batch_match();   // 阶段 B3：唯一实现归属 app::BatchMatchCoordinator
    });
    bSizer39->Add(p->m_btn_batch_match, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    ams_btn = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "ams_fila_sync", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                                 wxBU_EXACTFIT | wxNO_BORDER, false, 16); // ORCA match icon size with other icons as 16x16
    ams_btn->SetToolTip(_L("Synchronize filament list from AMS"));
    ams_btn->Bind(wxEVT_BUTTON, [this, scrolled_sizer](wxCommandEvent &e) {
        sync_ams_list();
    });
    p->m_bpButton_ams_filament = ams_btn;

    bSizer39->Add(ams_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    //bSizer39->Add(FromDIP(10), 0, 0, 0, 0 );

    ScalableButton* set_btn = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "settings");
    set_btn->SetToolTip(_L("Set filaments to use"));
    set_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        p->editing_filament = -1;
        // wxGetApp().params_dialog()->Popup();
        // wxGetApp().get_tab(Preset::TYPE_FILAMENT)->restore_last_select_item();
        wxGetApp().run_wizard(ConfigWizard::RR_USER, ConfigWizard::SP_FILAMENTS);
        });
    p->m_bpButton_set_filament = set_btn;

    bSizer39->Add(set_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    bSizer39->AddSpacer(FromDIP(SidebarProps::TitlebarMargin()));

    // add filament content
    p->m_panel_filament_content = new wxPanel( p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL );
    p->m_panel_filament_content->SetBackgroundColour( wxColour( 255, 255, 255 ) );

    //wxBoxSizer* bSizer_filament_content;
    //bSizer_filament_content = new wxBoxSizer( wxHORIZONTAL );

    // BBS:  filament double columns, wrapped in scrolled window (max 3 rows)
    p->sizer_filaments = new wxBoxSizer(wxHORIZONTAL);
    p->sizer_filaments->Add(new wxBoxSizer(wxVERTICAL), 1, wxEXPAND);
    p->sizer_filaments->Add(new wxBoxSizer(wxVERTICAL), 1, wxEXPAND);

    p->m_scrolled_filaments = new wxScrolledWindow(p->m_panel_filament_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    p->m_scrolled_filaments->SetScrollRate(0, 5);
    p->m_scrolled_filaments->SetBackgroundColour(*wxWHITE);
    p->m_panel_scrolled_filament_content = new wxPanel(p->m_scrolled_filaments, wxID_ANY);
    p->m_panel_scrolled_filament_content->SetBackgroundColour(*wxWHITE);
    p->m_panel_scrolled_filament_content->SetSizer(p->sizer_filaments);
    auto* scrolled_fila_sizer = new wxBoxSizer(wxVERTICAL);
    scrolled_fila_sizer->Add(p->m_panel_scrolled_filament_content, 0, wxEXPAND);
    p->m_scrolled_filaments->SetSizer(scrolled_fila_sizer);

    p->combos_filament.push_back(nullptr);

    /* first filament item */
    // init_filament_combo(&p->combos_filament[0], 0);

    p->combos_filament[0] = new PlaterFilamentComboBox(p->m_panel_scrolled_filament_content, Preset::TYPE_FILAMENT);
    auto combo_and_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    // BBS:  filament double columns
    combo_and_btn_sizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));
    if (p->combos_filament[0]->clr_picker) {
        p->combos_filament[0]->clr_picker->SetLabel("1");
        combo_and_btn_sizer->Add(p->combos_filament[0]->clr_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2)); // ElementSpacing - 2 (from combo box))
    }
    combo_and_btn_sizer->Add(p->combos_filament[0], 1, wxALL | wxEXPAND, FromDIP(2))->SetMinSize({-1, FromDIP(30) });

    /*ScalableButton* edit_btn = new ScalableButton(p->m_panel_filament_content, wxID_ANY, "edit");
    edit_btn->SetBackgroundColour(wxColour(255, 255, 255));
    edit_btn->SetToolTip(_L("Click to edit preset"));*/

    ScalableButton* edit_btn = new ScalableButton(p->m_panel_scrolled_filament_content, wxID_ANY, "menu_filament");
    edit_btn->SetToolTip(_L("Click to edit preset"));

    PlaterPresetComboBox* combobox = p->combos_filament[0];
    edit_btn->Bind(wxEVT_BUTTON, [this, edit_btn](wxCommandEvent) {
        auto    menu = p->plater->filament_action_menu(0);
        wxPoint pt{0, edit_btn->GetSize().GetHeight() + 10};
        pt                    = edit_btn->ClientToScreen(pt);
        pt                    = wxGetApp().mainframe->ScreenToClient(pt);
        p->m_menu_filament_id = 0;
        p->plater->PopupMenu(menu, (int) pt.x, pt.y);
    });
    combobox->edit_btn = edit_btn;

    combo_and_btn_sizer->Add(edit_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2)); // ElementSpacing - 2 (from combo box))
    combo_and_btn_sizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));

    p->combos_filament[0]->set_filament_idx(0);
    p->sizer_filaments->GetItem((size_t)0)->GetSizer()->Add(combo_and_btn_sizer, 1, wxEXPAND);

    //bSizer_filament_content->Add(p->sizer_filaments, 1, wxALIGN_CENTER | wxALL);
    wxSizer *sizer_filaments2 = new wxBoxSizer(wxVERTICAL);
    // --- Filaments title bar (same level as Color Mix title) ---
    p->m_panel_physical_filaments_title = new StaticBox(p->m_panel_filament_content, wxID_ANY, wxDefaultPosition,
                                                        wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    p->m_panel_physical_filaments_title->SetBackgroundColor(title_bg);
    p->m_panel_physical_filaments_title->SetBackgroundColor2(0xF1F1F1);
    p->m_panel_physical_filaments_title->SetMinSize(wxSize(-1, FromDIP(30)));
    p->m_panel_physical_filaments_title->SetMaxSize(wxSize(-1, FromDIP(30)));
    p->m_filament_icon = new ScalableButton(p->m_panel_physical_filaments_title, wxID_ANY, "filament");
    auto* physical_label = new Label(p->m_panel_physical_filaments_title, _L("Filaments"), LB_PROPAGATE_MOUSE_EVENT);
    auto* h_physical_title = new wxBoxSizer(wxHORIZONTAL);
    auto* white_left_f = new wxPanel(p->m_panel_physical_filaments_title, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(SidebarProps::ContentMargin()), -1));
    white_left_f->SetBackgroundColour(*wxWHITE);
    h_physical_title->Add(white_left_f, 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
    h_physical_title->Add(p->m_filament_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
    h_physical_title->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));
    h_physical_title->Add(physical_label, 0, wxALIGN_CENTER_VERTICAL);
    h_physical_title->AddStretchSpacer();

    // Sync filament button
    ScalableButton* sync_filament_btn = new ScalableButton(p->m_panel_physical_filaments_title, wxID_ANY, "sync_filament");
    sync_filament_btn->SetToolTip(_L("Sync Filament Information"));
    sync_filament_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        show_sync_filament_dialog();
    });
    p->m_bpButton_sync_filament = sync_filament_btn;

    // Delete filament button — delegates to delete_filament for consistent remap behavior
    ScalableButton* del_btn = new ScalableButton(p->m_panel_physical_filaments_title, wxID_ANY, "delete_filament");
    del_btn->SetToolTip(_L("Remove last filament"));
    del_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        if (p->combos_filament.size() <= 1)
            return;
        delete_filament(size_t(-1), -1);
    });
    p->m_bpButton_del_filament = del_btn;

    // Add filament button
    ScalableButton* add_btn = new ScalableButton(p->m_panel_physical_filaments_title, wxID_ANY, "add_filament");
    add_btn->SetToolTip(_L("Add one filament"));
    add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e){
        if (p->combos_filament.size() >= MAXIMUM_EXTRUDER_NUMBER)
            return;
        PresetBundle* pb = wxGetApp().preset_bundle;
        if (!pb || pb->mixed_filaments.total_filaments(p->combos_filament.size()) >= MAXIMUM_FILAMENT_NUMBER)
            return;
        int filament_count = p->combos_filament.size() + 1;
        wxGetApp().plater()->confirm_auto_generated_gradients(filament_count);
        wxColour new_col = Plater::get_next_color_for_filament();
        std::string new_color = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        pb->set_num_filaments(filament_count, new_color);
        wxGetApp().filament_svc().on_filaments_change(filament_count);
        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
        pb->export_selections(*wxGetApp().app_config);
        auto_calc_flushing_volumes(filament_count - 1);
    });
    p->m_bpButton_add_filament = add_btn;

    h_physical_title->Add(sync_filament_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    h_physical_title->Add(del_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    h_physical_title->Add(add_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* white_right_f = new wxPanel(p->m_panel_physical_filaments_title, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(SidebarProps::ContentMargin()), -1));
    white_right_f->SetBackgroundColour(*wxWHITE);
    h_physical_title->Add(white_right_f, 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
    p->m_panel_physical_filaments_title->SetSizer(h_physical_title);
    p->m_panel_physical_filaments_title->Layout();

    if (p->combos_filament.size() <= 1)
        h_physical_title->Hide(p->m_bpButton_del_filament);

    sizer_filaments2->AddSpacer(FromDIP(8));
    sizer_filaments2->Add(p->m_panel_physical_filaments_title, 0, wxEXPAND, 0);
    sizer_filaments2->AddSpacer(FromDIP(8));
    sizer_filaments2->Add(p->m_scrolled_filaments, 0, wxEXPAND, 0);
    sizer_filaments2->AddSpacer(FromDIP(8));
    // --- Color Mix Panel (inside filament content, same level as filaments) ---
    p->mixed_panel->init_color_mix_panel(p->m_panel_filament_content, sizer_filaments2);
    p->m_panel_filament_content->SetSizer(sizer_filaments2);
    p->m_panel_filament_content->Layout();
    scrolled_sizer->Add(p->m_panel_filament_content, 0, wxEXPAND, 0);
    }

    p->mixed_panel->init_mixed_filaments_panel(p->scrolled, scrolled_sizer, title_bg);

    {
    //add project title
    auto params_panel = ((MainFrame*)parent->GetParent())->m_param_panel;
    if (params_panel) {
        params_panel->get_top_panel()->Reparent(p->scrolled);
        auto spliter_1 = new ::StaticLine(p->scrolled);
        spliter_1->SetLineColour("#A6A9AA");
        scrolled_sizer->Add(spliter_1, 0, wxEXPAND);
        scrolled_sizer->Add(params_panel->get_top_panel(), 0, wxEXPAND);
        auto spliter_2 = new ::StaticLine(p->scrolled);
        spliter_2->SetLineColour("#CECECE");
        scrolled_sizer->Add(spliter_2, 0, wxEXPAND);
    }

    //add project content
    p->sizer_params = new wxBoxSizer(wxVERTICAL);

    // ORCA: Update search box to modern style
    p->m_search_bar = new StaticBox(p->scrolled);
    p->m_search_bar->SetCornerRadius(0);
    p->m_search_bar->SetBorderColor(wxColour("#CECECE"));

    p->m_search_item = new TextInput(p->m_search_bar, wxEmptyString, wxEmptyString, "", wxDefaultPosition, wxDefaultSize, 0 | wxBORDER_NONE);
    p->m_search_item->SetIcon(*BitmapCache().load_svg("search", FromDIP(16), FromDIP(16))); // ORCA: Add search icon to search box

    wxTextCtrl* text_ctrl = p->m_search_item->GetTextCtrl();
    text_ctrl->SetHint(_L("Search plate, object and part."));
    text_ctrl->SetForegroundColour(wxColour("#262E30"));
    text_ctrl->SetFont(Label::Body_13);
    text_ctrl->SetSize(wxSize(-1, FromDIP(16))); // Centers text vertically

    text_ctrl->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        if (p->dia->IsShown()) {
            e.Skip();
            return;
        }
        p->m_search_bar->SetBorderColor(wxColour("#009688"));
        wxPoint pos = this->p->m_search_bar->ClientToScreen(wxPoint(0, 0));
#ifndef __WXGTK__
        pos.y += this->p->m_search_bar->GetRect().height;
#else
        this->p->m_search_item->Enable(false);
#endif
        p->dia->SetPosition(pos);
        p->dia->Popup();
        e.Skip(); // required to show caret
    });

    auto search_sizer = new wxBoxSizer(wxHORIZONTAL);
    search_sizer->Add(new wxWindow(p->m_search_bar, wxID_ANY, wxDefaultPosition, wxSize(0, 0)), 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(1));
    search_sizer->Add(p->m_search_item, 1, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    p->m_search_bar->SetSizer(search_sizer);
    p->m_search_bar->Layout();
    search_sizer->Fit(p->m_search_bar);

    p->m_object_list = new ObjectList(p->scrolled);
    p->m_object_list->Bind(wxCUSTOMEVT_EXIT_SEARCH, [this](wxCommandEvent&) {
#ifdef __WXGTK__
        this->p->m_search_item->Enable(true);
#endif
        this->p->m_search_bar->SetBorderColor(wxColour("#CECECE"));
        this->p->m_search_item->GetTextCtrl()->SetValue(""); // reset value when close
    });

    p->sizer_params->Add(p->m_search_bar, 0, wxALL | wxEXPAND, 0);
    p->sizer_params->Add(p->m_object_list, 1, wxEXPAND | wxTOP, 0);
    scrolled_sizer->Add(p->sizer_params, 2, wxEXPAND | wxLEFT, 0);
    p->m_object_list->Hide();
    p->m_search_bar->Hide();
    // Frequently Object Settings
    p->object_settings = new ObjectSettings(p->scrolled);

    p->dia = new Search::SearchObjectDialog(p->m_object_list, p->scrolled->GetParent(), p->m_search_item);
#if !NEW_OBJECT_SETTING
    p->object_settings->Hide();
    p->sizer_params->Add(p->object_settings->get_sizer(), 0, wxEXPAND | wxTOP, 5 * em / 10);
#else
    if (params_panel) {
        params_panel->Reparent(p->scrolled);
        scrolled_sizer->Add(params_panel, 3, wxEXPAND);
    }
#endif
    }

    p->object_layers = new ObjectLayers(p->scrolled);
    p->object_layers->Hide();
    p->sizer_params->Add(p->object_layers->get_sizer(), 0, wxEXPAND | wxTOP, 0);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(p->scrolled, 1, wxEXPAND);
    SetSizer(sizer);
}

Sidebar::~Sidebar() { p->nozzle_sync_state->alive.store(false, std::memory_order_release); }   // 2.8 ③：悬空 CallAfter 熔断（main bac6fd5:3403 逐字）

void Sidebar::create_printer_preset()
{
    CreatePrinterPresetDialog dlg(wxGetApp().mainframe);
    int                       res = dlg.ShowModal();
    if (wxID_OK == res) {
        wxGetApp().mainframe->update_side_preset_ui();
        update_ui_from_settings();
        update_all_preset_comboboxes();
        wxGetApp().load_current_presets();
        CreatePresetSuccessfulDialog success_dlg(wxGetApp().mainframe, SuccessType::PRINTER);
        int                          res = success_dlg.ShowModal();
        if (res == wxID_OK) {
            p->editing_filament = -1;
            if (p->combo_printer->switch_to_tab()) p->editing_filament = 0;
        }
    }
}

void Sidebar::init_filament_combo(PlaterPresetComboBox **combo, const int filament_idx)
{
    *combo = new PlaterFilamentComboBox(p->m_panel_scrolled_filament_content, Slic3r::Preset::TYPE_FILAMENT);
    (*combo)->set_filament_idx(filament_idx);

    auto combo_and_btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    // BBS:  filament double columns

    // int em = wxGetApp().em_unit();
    if ((filament_idx % 2) == 0) // Dont add right column item. this one create equal spacing on left, right & middle
        combo_and_btn_sizer->AddSpacer(FromDIP((filament_idx % 2) == 0 ? 12 : 3)); // Content Margin

    (*combo)->clr_picker->SetLabel(wxString::Format("%d", filament_idx + 1));
    combo_and_btn_sizer->Add((*combo)->clr_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2)); // ElementSpacing - 2 (from combo box))
    combo_and_btn_sizer->Add(*combo, 1, wxALL | wxEXPAND, FromDIP(2))->SetMinSize({-1, FromDIP(30)});

    /* BBS hide del_btn
    ScalableButton* del_btn = new ScalableButton(p->m_panel_filament_content, wxID_ANY, "delete_filament");
    del_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e){
        int extruder_count = std::max(1, (int)p->combos_filament.size() - 1);

        update_objects_list_filament_column(std::max(1, extruder_count - 1));
        on_filaments_change(extruder_count);
        wxGetApp().preset_bundle->printers.get_edited_preset().set_num_extruders(extruder_count);
        wxGetApp().preset_bundle->update_multi_material_filament_presets();
    });

    combo_and_btn_sizer->Add(32 * em / 10, 0, 0, 0, 0);
    combo_and_btn_sizer->Add(del_btn, 0, wxALIGN_CENTER_VERTICAL, 5 * em / 10);
    */
    ScalableButton* edit_btn = new ScalableButton(p->m_panel_scrolled_filament_content, wxID_ANY, "menu_filament");
    edit_btn->SetToolTip(_L("Click to edit preset"));

    PlaterPresetComboBox* combobox = (*combo);
    //edit_btn->Bind(wxEVT_BUTTON, [this, combobox, filament_idx](wxCommandEvent)
    //{
    //    p->editing_filament = -1;
    //    if (combobox->switch_to_tab())
    //        p->editing_filament = filament_idx; // sync with TabPresetComboxBox's m_filament_idx
    //});

    edit_btn->Bind(wxEVT_BUTTON, [this, edit_btn, filament_idx](wxCommandEvent) {
        auto menu = p->plater->filament_action_menu(filament_idx);
        wxPoint pt { 0, edit_btn->GetSize().GetHeight() + 10 };
        pt = edit_btn->ClientToScreen(pt);
        pt = wxGetApp().mainframe->ScreenToClient(pt);
        p->m_menu_filament_id = filament_idx;
        p->plater->PopupMenu(menu, (int) pt.x, pt.y);
    });


    combobox->edit_btn = edit_btn;

    combo_and_btn_sizer->Add(edit_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2)); // ElementSpacing - 2 (from combo box))

    combo_and_btn_sizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));

    // BBS:  filament double columns
    auto side = filament_idx % 2;
    auto /***/sizer_filaments = this->p->sizer_filaments->GetItem(side)->GetSizer();
    if (side == 1 && filament_idx > 1) sizer_filaments->Remove(filament_idx / 2);
    sizer_filaments->Add(combo_and_btn_sizer, 1, wxEXPAND);
    if (side == 0) {
        sizer_filaments = this->p->sizer_filaments->GetItem(1)->GetSizer();
        sizer_filaments->AddStretchSpacer(1);
    }
}

void Sidebar::remove_unused_filament_combos(const size_t current_extruder_count)
{
    if (current_extruder_count >= p->combos_filament.size())
        return;
    while (p->combos_filament.size() > current_extruder_count) {
        const int last = p->combos_filament.size() - 1;
        auto sizer_filaments = this->p->sizer_filaments->GetItem(last % 2)->GetSizer();
        sizer_filaments->Remove(last / 2);
        (*p->combos_filament[last]).Destroy();
        p->combos_filament.pop_back();
    }
    // BBS:  filament double columns
    auto sizer_filaments0 = this->p->sizer_filaments->GetItem((size_t)0)->GetSizer();
    auto sizer_filaments1 = this->p->sizer_filaments->GetItem(1)->GetSizer();
    if (current_extruder_count < 2) {
        sizer_filaments1->Clear();
    } else {
        size_t c0 = sizer_filaments0->GetChildren().GetCount();
        size_t c1 = sizer_filaments1->GetChildren().GetCount();
        if (c0 < c1)
            sizer_filaments1->Remove(c1 - 1);
        else if (c0 > c1)
            sizer_filaments1->AddStretchSpacer(1);
    }
}

void Sidebar::update_all_preset_comboboxes(bool reload_printer_view)
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    const auto print_tech = preset_bundle.printers.get_edited_preset().printer_technology();

    bool is_bbl_vendor = preset_bundle.is_bbl_vendor();

    auto p_mainframe = wxGetApp().mainframe;
    auto cfg = preset_bundle.printers.get_edited_preset().config;

    const auto& appconfig = wxGetApp().app_config;

    bool use_new_connection = appconfig->get("use_new_connect") == "true";

    auto printer_config     = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto printer_model_opt  = printer_config.option<ConfigOptionString>("printer_model");
    bool is_snapmaker_u1    = false;
    if (printer_model_opt) {
        std::string printer_model = printer_model_opt->value;
        is_snapmaker_u1           = boost::icontains(printer_model, "Snapmaker") && boost::icontains(printer_model, "U1");
    }

    p->combo_printer->set_show_machine_connecting_button(false);
    p->combo_printer->set_show_connection_button(false);

    if (preset_bundle.use_bbl_network()) {
        ams_btn->Show();
        p_mainframe->set_print_button_to_default(MainFrame::PrintSelectType::ePrintPlate);
    } else {
        ams_btn->Hide();
        auto print_btn_type = MainFrame::PrintSelectType::eExportGcode;

        const auto& edit_preset = preset_bundle.printers.get_edited_preset();

        const bool showing_u1_device = p_mainframe->m_printer_view && p_mainframe->m_printer_view->is_u1_device_page();
        // Non-U1: always leave path=2 for missing_connection / print_host. Keep skipping
        // when use_new_connect already sits on the GIF (connect flow loads path=2 afterwards).
        const bool load_non_u1_page = !is_snapmaker_u1 && reload_printer_view && (!use_new_connection || showing_u1_device);

        if (load_non_u1_page) {

            p->combo_printer->set_show_connection_button(true);
            wxString url = cfg.opt_string("print_host_webui").empty() ? cfg.opt_string("print_host") : cfg.opt_string("print_host_webui");
            wxString apikey;
            if (url.empty()) {
                std::string base_url = LOCALHOST_URL + std::to_string(wxGetApp().m_page_http_server.get_port());
                url                  = wxString::Format("%s/web/orca/missing_connection.html", from_u8(base_url));
            }
            else {
                if (!url.Lower().starts_with("http"))
                    url = wxString::Format("http://%s", url);
                const auto host_type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type")->value;
                if (cfg.has("printhost_apikey") && (host_type != htSimplyPrint))
                    apikey = cfg.opt_string("printhost_apikey");
                print_btn_type = preset_bundle.is_bbl_vendor() ? MainFrame::PrintSelectType::ePrintPlate :
                                                                 MainFrame::PrintSelectType::eSendGcode;

                if (url.find("127.0.0.1") != std::string::npos) {
                    url = wxGetApp().gateway_web_url("device_control");
                }
            }
            
            p_mainframe->load_printer_url(url, apikey);

            p_mainframe->set_print_button_to_default(print_btn_type);
        } else {
            print_btn_type = preset_bundle.is_bbl_vendor() ? MainFrame::PrintSelectType::ePrintPlate :
                                                             MainFrame::PrintSelectType::eSendGcode;
            p_mainframe->set_print_button_to_default(print_btn_type);

            if (is_snapmaker_u1) {

                auto        devices     = wxGetApp().app_config->get_devices();
                bool hasOnlineMachine = false;
                for (const auto& device : devices) {
                    if (device.connected) {
                        hasOnlineMachine = true;
                        break;
                    }
                }

                if(hasOnlineMachine)
                    p->combo_printer->set_show_machine_connecting_button(true);
    
                wxString url = wxGetApp().gateway_web_url("device_control");

                if (reload_printer_view && !showing_u1_device) {
                    wxGetApp().mainframe->load_printer_url(url);
                }
            }

            if (!p->combo_printer->get_show_machine_connecting_button() && !is_snapmaker_u1) {
                p->combo_printer->set_show_connection_button(true);
            }
        }
    }

    if (cfg.opt_bool("pellet_modded_printer")) {
		p->m_staticText_filament_settings->SetLabel(_L("Pellet Configuration"));
        p->m_filament_icon->SetBitmap_("pellets");
        p->m_filament_config_icon->SetBitmap_("filament_group");
    } else {
		p->m_staticText_filament_settings->SetLabel(_L("Filament Management"));
        p->m_filament_icon->SetBitmap_("filament");
        p->m_filament_config_icon->SetBitmap_("filament_group");
    }

    show_SEMM_buttons(/*cfg.opt_bool("single_extruder_multi_material")*/true);

    bool support_multi_bed_types = cfg.opt_bool("support_multi_bed_types");
    const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
    const t_config_enum_values* keys_map = bed_type_def ? bed_type_def->enum_keys_map : nullptr;

    m_bed_type_list->Clear();
    m_bed_type_combo_enum_values.clear();
    if (bed_type_def && keys_map) {
        if (is_snapmaker_u1 && !support_multi_bed_types) {
            for (const auto& item : bed_type_def->enum_labels_u1)
                m_bed_type_list->AppendString(_L(item));
            for (const auto& v : bed_type_def->enum_values_u1)
                m_bed_type_combo_enum_values.push_back(v);
        } else if (is_snapmaker_u1 && support_multi_bed_types) {
            for (const auto& item : bed_type_def->enum_labels_ex)
                m_bed_type_list->AppendString(_L(item));
            for (const auto& v : bed_type_def->enum_values_ex)
                m_bed_type_combo_enum_values.push_back(v);
        } else {
            for (const auto& item : bed_type_def->enum_labels)
                m_bed_type_list->AppendString(_L(item));
            for (const auto& v : bed_type_def->enum_values)
                m_bed_type_combo_enum_values.push_back(v);
        }
    }

    auto get_key_for_bed_type = [keys_map](BedType bt) -> std::string {
        if (!keys_map) return {};
        for (const auto& item : *keys_map)
            if ((BedType)item.second == bt) return item.first;
        return {};
    };
    auto get_selection_index = [&]() -> int {
        BedType curr = wxGetApp().preset_bundle->project_config.opt_enum<BedType>("curr_bed_type");
        std::string key = get_key_for_bed_type(curr);
        for (size_t i = 0; i < m_bed_type_combo_enum_values.size(); ++i)
            if (m_bed_type_combo_enum_values[i] == key) return (int)i;
        return 0;
    };

    if (is_bbl_vendor || support_multi_bed_types || is_snapmaker_u1) {
        m_bed_type_list->Enable();
        // Orca: don't update bed type if loading project
        if (!p->plater->is_loading_project()) {
            std::string printer_name = wxGetApp().preset_bundle->printers.get_selected_preset_name();
            auto str_bed_type = wxGetApp().app_config->get_printer_setting(printer_name, "curr_bed_type");
            
            BedType bed_type_to_use;
            bool is_first_time = str_bed_type.empty();
            
            if (!is_first_time) {
                // This printer has saved bed type configuration
                int bed_type_value = atoi(str_bed_type.c_str());
                if (bed_type_value == 0)
                    bed_type_value = 1;
                bed_type_to_use = (BedType)bed_type_value;
            } else {
                // Orca: First time using this printer, get default bed type
                bed_type_to_use = preset_bundle.printers.get_edited_preset().get_default_bed_type(&preset_bundle);
                
                // Orca: Save to app_config immediately to prevent bed type inheritance
                wxGetApp().app_config->set("curr_bed_type", std::to_string(int(bed_type_to_use)));
                wxGetApp().app_config->set_printer_setting(printer_name, "curr_bed_type", std::to_string(int(bed_type_to_use)));
            }
            
            // Orca: Update proj_config directly to avoid callback context issues
            if (is_snapmaker_u1 && !support_multi_bed_types) {
                if (bed_type_to_use != btPTE && bed_type_to_use != btPEI && bed_type_to_use != btGESP) {
                    bed_type_to_use = btPTE;
                    wxGetApp().app_config->set("curr_bed_type", std::to_string(int(bed_type_to_use)));
                    wxGetApp().app_config->set_printer_setting(printer_name, "curr_bed_type", std::to_string(int(bed_type_to_use)));
                }
            }

            wxGetApp().preset_bundle->project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type_to_use));
            int sel_idx = get_selection_index();
            m_bed_type_list->SetSelection(sel_idx);
        } else {
            if (is_snapmaker_u1 && !support_multi_bed_types) {
                BedType curr = wxGetApp().preset_bundle->project_config.opt_enum<BedType>("curr_bed_type");
                if (curr != btPTE && curr != btPEI && curr != btGESP) {
                    wxGetApp().preset_bundle->project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPTE));
                    m_bed_type_list->SetSelection(0);
                } else
                    m_bed_type_list->SetSelection(get_selection_index());
            } else
                m_bed_type_list->SetSelection(get_selection_index());
        }
    } else {
        BedType default_bed_type = preset_bundle.printers.get_edited_preset().get_default_bed_type(&preset_bundle);
        wxGetApp().preset_bundle->project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(default_bed_type));
        m_bed_type_list->SetSelection(get_selection_index());
        m_bed_type_list->Disable();
    }

    if (print_tech == ptFFF) {
        for (PlaterPresetComboBox* cb : p->combos_filament)
            cb->update();
    }

    if (p->combo_printer){
        p->combo_printer->update();
        update_printer_thumbnail();
    }
        
    p_mainframe->show_device(preset_bundle.use_bbl_device_tab() && !use_new_connection);
    p_mainframe->m_tabpanel->SetSelection(p_mainframe->m_tabpanel->GetSelection());
}

void Sidebar::update_presets(Preset::Type preset_type)
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    const auto print_tech = preset_bundle.printers.get_edited_preset().printer_technology();

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": enter, preset_type %1%")%preset_type;
    switch (preset_type) {
    case Preset::TYPE_FILAMENT:
    {
        // BBS
#if 0
        const size_t extruder_cnt = print_tech != ptFFF ? 1 :
                                dynamic_cast<ConfigOptionFloats*>(preset_bundle.printers.get_edited_preset().config.option("nozzle_diameter"))->values.size();
        const size_t filament_cnt = p->combos_filament.size() > extruder_cnt ? extruder_cnt : p->combos_filament.size();
#else
        const size_t filament_cnt = p->combos_filament.size();
#endif
        const std::string &name = preset_bundle.filaments.get_selected_preset_name();
        if (p->editing_filament >= 0) {
            preset_bundle.set_filament_preset(p->editing_filament, name);
        } else if (filament_cnt == 1) {
            // Single filament printer, synchronize the filament presets.
            Preset *preset = preset_bundle.filaments.find_preset(name, false);
            if (preset) {
                if (preset->is_compatible) preset_bundle.set_filament_preset(0, name);
            }

        }

        for (size_t i = 0; i < filament_cnt; i++)
            p->combos_filament[i]->update();

        update_dynamic_filament_list();

        p->plater->notify_filament_usage_changed();

        break;
    }

    case Preset::TYPE_PRINT:
        //wxGetApp().mainframe->m_param_panel;
        //p->combo_print->update();
        {
        Tab* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (print_tab) {
            print_tab->get_combo_box()->update();
        }
        break;
        }
    case Preset::TYPE_SLA_PRINT:
        ;// p->combo_sla_print->update();
        break;

    case Preset::TYPE_SLA_MATERIAL:
        ;// p->combo_sla_material->update();
        break;

    case Preset::TYPE_PRINTER:
    {
        // update_nozzle_settings();
        auto machineName = wxGetApp().preset_bundle->printers.get_selected_preset_name();

        auto printer_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        auto        printer_model_opt = printer_config.option<ConfigOptionString>("printer_model");
        if (printer_model_opt)
        {
            std::string printer_model   = printer_model_opt->value;
            bool        is_snapmaker_u1 = boost::icontains(printer_model, "Snapmaker") && boost::icontains(printer_model, "U1");

            if (is_snapmaker_u1)
            {
                p->m_printerinfo_syncbtn->Show();
            } 
            else 
            {
                p->m_printerinfo_syncbtn->Hide();
            }
        }

        update_all_preset_comboboxes();
        p->show_preset_comboboxes();

        /* update bed shape */
        Tab* printer_tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
        if (printer_tab) {
            printer_tab->update();
            printer_tab->on_preset_loaded();
        }

        Preset& printer_preset = wxGetApp().preset_bundle->printers.get_edited_preset();
        GLCanvas3D* canvas = wxGetApp().plater()->get_current_canvas3D();
        if (canvas) {
            if (auto printer_structure_opt = printer_preset.config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure")) {
                canvas->get_arrange_settings().align_to_y_axis = (printer_structure_opt->value == PrinterStructure::psI3);
            }
            else
                canvas->get_arrange_settings().align_to_y_axis = false;
        }

        break;
    }

    default: break;
    }

    // Synchronize config.ini with the current selections.
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": exit.");

}

//BBS
void Sidebar::update_presets_from_to(Slic3r::Preset::Type preset_type, std::string from, std::string to)
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": enter, preset_type %1%, from %2% to %3%")%preset_type %from %to;

    switch (preset_type) {
    case Preset::TYPE_FILAMENT:
    {
        const size_t filament_cnt = p->combos_filament.size();
        for (auto it = preset_bundle.filament_presets.begin(); it != preset_bundle.filament_presets.end(); it++)
        {
            if ((*it).compare(from) == 0) {
                (*it) = to;
            }
        }
        for (size_t i = 0; i < filament_cnt; i++)
            p->combos_filament[i]->update();
        break;
    }

    default: break;
    }

    // Synchronize config.ini with the current selections.
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": exit!");
}

void Sidebar::change_top_border_for_mode_sizer(bool increase_border)
{
    // BBS
#if 0
    if (p->mode_sizer) {
        p->mode_sizer->set_items_flag(increase_border ? wxTOP : 0);
        p->mode_sizer->set_items_border(increase_border ? int(0.5 * wxGetApp().em_unit()) : 0);
    }
#endif
}

void Sidebar::msw_rescale()
{
    SetMinSize(wxSize(42 * wxGetApp().em_unit(), -1));
    p->m_panel_printer_title->GetSizer()->SetMinSize(-1, 3 * wxGetApp().em_unit());
    p->m_panel_filament_title->GetSizer()
        ->SetMinSize(-1, 3 * wxGetApp().em_unit());
    p->m_printer_icon->msw_rescale();
    p->m_printerinfo_syncbtn->msw_rescale();
    p->m_printer_setting->msw_rescale();
    p->m_filament_icon->msw_rescale();
    p->mixed_panel->msw_rescale();
    p->m_bpButton_add_filament->msw_rescale();
    p->m_bpButton_del_filament->msw_rescale();
    p->m_bpButton_ams_filament->msw_rescale();
    p->m_bpButton_set_filament->msw_rescale();
    p->m_flushing_volume_btn->Rescale();
    // Batch-match button caches its text extent (messureSize) at creation DPI; without
    // Rescale() the cached size goes stale after a per-monitor DPI change (e.g. moving
    // the window to a 150% 4K screen), so render() centers the auto-rescaled font with
    // the old-DPI metrics and the label drifts/clips.
    p->m_btn_batch_match->Rescale();
    //BBS
    m_bed_type_list->Rescale();
    m_bed_type_list->SetMinSize({-1, 3 * wxGetApp().em_unit()});
#if 0
    if (p->mode_sizer)
        p->mode_sizer->msw_rescale();
#endif

    //for (PlaterPresetComboBox* combo : std::vector<PlaterPresetComboBox*> { p->combo_print,
    //                                                            //p->combo_sla_print,
    //                                                            //p->combo_sla_material,
    //                                                            //p->combo_printer
    //                                                            } )
    //    combo->msw_rescale();
    p->combo_printer->msw_rescale();
    for (PlaterPresetComboBox* combo : p->combos_filament)
        combo->msw_rescale();

    // BBS
    //p->frequently_changed_parameters->msw_rescale();
    //obj_list()->msw_rescale();
    // BBS TODO: add msw_rescale for newly added windows
    // BBS
    //p->object_manipulation->msw_rescale();
    p->object_settings->msw_rescale();
    p->m_search_item->Rescale();
    p->m_search_item->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(16)));
    p->m_search_bar->Layout();

    // BBS
#if 0
    p->object_info->msw_rescale();

    p->btn_send_gcode->msw_rescale();
//    p->btn_eject_device->msw_rescale();
    p->btn_export_gcode_removable->msw_rescale();
#ifdef _WIN32
    const int scaled_height = p->btn_export_gcode_removable->GetBitmapHeight();
#else
    const int scaled_height = p->btn_export_gcode_removable->GetBitmapHeight() + 4;
#endif
    p->btn_export_gcode->SetMinSize(wxSize(-1, scaled_height));
    p->btn_reslice     ->SetMinSize(wxSize(-1, scaled_height));
#endif
    p->scrolled->Layout();

    p->searcher.dlg_msw_rescale();
}

void Sidebar::sys_color_changed()
{
    wxWindowUpdateLocker noUpdates(this);

#if 0
    for (wxWindow* win : std::vector<wxWindow*>{ this, p->sliced_info->GetStaticBox(), p->object_info->GetStaticBox(), p->btn_reslice, p->btn_export_gcode })
        wxGetApp().UpdateDarkUI(win);
    p->object_info->msw_rescale();

    for (wxWindow* win : std::vector<wxWindow*>{ p->scrolled, p->presets_panel })
        wxGetApp().UpdateAllStaticTextDarkUI(win);
#endif
    //for (wxWindow* btn : std::vector<wxWindow*>{ p->btn_reslice, p->btn_export_gcode })
    //    wxGetApp().UpdateDarkUI(btn, true);
    p->m_printer_icon->msw_rescale();
    p->m_printerinfo_syncbtn->msw_rescale();
    p->m_printer_setting->msw_rescale();
    p->m_filament_icon->msw_rescale();
    p->mixed_panel->msw_rescale();
    p->m_bpButton_add_filament->msw_rescale();
    p->m_bpButton_del_filament->msw_rescale();
    p->m_bpButton_ams_filament->msw_rescale();
    p->m_bpButton_set_filament->msw_rescale();
    p->m_flushing_volume_btn->Rescale();
    // Keep the cached text extent in sync with the new DPI, same as above.
    p->m_btn_batch_match->Rescale();

    // BBS
#if 0
    if (p->mode_sizer)
        p->mode_sizer->msw_rescale();
    p->frequently_changed_parameters->sys_color_changed();
#endif
    p->object_settings->sys_color_changed();

    //BBS: remove print related combos
#if 0
    for (PlaterPresetComboBox* combo : std::vector<PlaterPresetComboBox*>{  p->combo_print,
                                                                p->combo_sla_print,
                                                                p->combo_sla_material,
                                                                p->combo_printer })
        combo->sys_color_changed();
#endif
    p->combo_printer->sys_color_changed();
    for (PlaterPresetComboBox* combo : p->combos_filament)
        combo->sys_color_changed();

    // BBS
    obj_list()->sys_color_changed();
    obj_layers()->sys_color_changed();
    // BBS
    //p->object_manipulation->sys_color_changed();

    // btn...->msw_rescale() updates icon on button, so use it
    //p->btn_send_gcode->msw_rescale();
//    p->btn_eject_device->msw_rescale();
    //p->btn_export_gcode_removable->msw_rescale();

    p->scrolled->Layout();

    p->searcher.dlg_sys_color_changed();
}

void Sidebar::search()
{
    p->searcher.search();
}

void Sidebar::jump_to_option(const std::string& opt_key, Preset::Type type, const std::wstring& category)
{
    //const Search::Option& opt = p->searcher.get_option(opt_key, type);
    if (type == Preset::TYPE_PRINT) {
        auto tab = dynamic_cast<TabPrintModel*>(wxGetApp().params_panel()->get_current_tab());
        if (tab && tab->has_key(opt_key)) {
            tab->activate_option(opt_key, category);
            return;
        }
        wxGetApp().params_panel()->switch_to_global();
    }
    wxGetApp().get_tab(type)->activate_option(opt_key, category);
}

void Sidebar::jump_to_option(size_t selected)
{
    const Search::Option& opt = p->searcher.get_option(selected);
    jump_to_option(opt.opt_key(), opt.type, opt.category);

    // Switch to the Settings NotePad
//    wxGetApp().mainframe->select_tab();
}

// BBS. Move logic from Plater::on_extruders_change() to Sidebar::on_filaments_change().
void Sidebar::on_filaments_change(size_t num_filaments)
{
    auto& choices = combos_filament();

    if (num_filaments == choices.size()) {
        // Project load may keep the same physical filament count while mixed
        // definitions changed. Refresh mixed panel even without count changes.
        const bool sync_manager = !p->m_skip_mixed_filament_sync_once;
        p->m_skip_mixed_filament_sync_once = false;
        update_ui_from_settings();
        update_dynamic_filament_list();
        p->mixed_panel->update_mixed_filament_panel(sync_manager);

        // Recalc scrolled filament window height (max 3 rows, matches color mix)
        if (p->m_scrolled_filaments && p->m_panel_scrolled_filament_content) {
            p->m_panel_scrolled_filament_content->Layout();
            const wxSize content_best = p->m_panel_scrolled_filament_content->GetBestSize();
            const int row_count = ((int)num_filaments + 1) / 2; // 2-column grid
            const int desired_h = row_count > 3
                ? (content_best.GetHeight() * 3) / std::max(1, row_count)
                : content_best.GetHeight();
            p->m_scrolled_filaments->SetMinSize({-1, desired_h});
            p->m_scrolled_filaments->SetMaxSize({-1, desired_h});
        }
        Layout();
        return;
    }

    p->m_skip_mixed_filament_sync_once = false;

    if (choices.size() == 1 || num_filaments == 1)
        choices[0]->GetDropDown().Invalidate();

    wxWindowUpdateLocker noUpdates_scrolled_panel(this);

    size_t i = choices.size();
    while (i < num_filaments)
    {
        PlaterPresetComboBox* choice/*{ nullptr }*/;
        init_filament_combo(&choice, i);
        choices.push_back(choice);

        // initialize selection
        choice->update();
        ++i;
    }

    // remove unused choices if any
    remove_unused_filament_combos(num_filaments);

    auto sizer = p->m_panel_filament_title->GetSizer();
    if (p->m_flushing_volume_btn != nullptr && sizer != nullptr) {
        if (num_filaments > 1) {
            sizer->Show(p->m_flushing_volume_btn);
        } else {
            sizer->Hide(p->m_flushing_volume_btn);
        }
    }
    if (p->m_bpButton_del_filament != nullptr && p->m_panel_physical_filaments_title != nullptr) {
        auto* inner_sizer = p->m_panel_physical_filaments_title->GetSizer();
        if (inner_sizer) {
            if (num_filaments > 1)
                inner_sizer->Show(p->m_bpButton_del_filament);
            else
                inner_sizer->Hide(p->m_bpButton_del_filament);
        }
    }

    // Recalc scrolled filament window height (max 3 rows, matches color mix)
    if (p->m_scrolled_filaments && p->m_panel_scrolled_filament_content) {
        p->m_panel_scrolled_filament_content->Layout();
        const wxSize content_best = p->m_panel_scrolled_filament_content->GetBestSize();
        const int row_count = ((int)num_filaments + 1) / 2; // 2-column grid
        const int desired_h = row_count > 3
            ? (content_best.GetHeight() / std::max(1, row_count)) * 3
            : content_best.GetHeight();
        p->m_scrolled_filaments->SetMinSize({-1, desired_h});
        p->m_scrolled_filaments->SetMaxSize({-1, desired_h});
    }

    Layout();
    wxWeakRef<Sidebar> weak_this(this);
    wxTheApp->CallAfter([weak_this]() {
        Sidebar* sidebar = weak_this.get();
        if (sidebar && sidebar->p && sidebar->p->m_scrolled_filaments) {
            int vh = sidebar->p->m_scrolled_filaments->GetVirtualSize().y;
            int ch = sidebar->p->m_scrolled_filaments->GetClientSize().y;
            sidebar->p->m_scrolled_filaments->Scroll(0, std::max(0, vh - ch));
        }
    });
    p->m_panel_filament_title->Refresh();
    update_ui_from_settings();
    update_dynamic_filament_list();
    p->mixed_panel->update_mixed_filament_panel();
    p->mixed_panel->update_color_mix_panel();

    // Disable add buttons when combined filament limit reached
    if (PresetBundle *pb = wxGetApp().preset_bundle) {
        const bool can_add = pb->mixed_filaments.total_filaments(combos_filament().size()) < MAXIMUM_FILAMENT_NUMBER;
        if (p->m_bpButton_add_filament)
            p->m_bpButton_add_filament->Enable(can_add);
        p->mixed_panel->set_add_color_mix_enabled(can_add);
    }
}



void Sidebar::init_color_mix_panel(wxWindow* parent, wxSizer* sizer)
{
    p->mixed_panel->init_color_mix_panel(parent, sizer);
}

void Sidebar::update_color_mix_panel()
{
    p->mixed_panel->update_color_mix_panel();
}

void Sidebar::update_mixed_filament_panel(bool sync_manager)
{
    p->mixed_panel->update_mixed_filament_panel(sync_manager);
}

std::vector<unsigned int> Sidebar::get_ui_ordered_filament_ids() const
{
    const size_t num_physical = static_cast<size_t>(std::max(wxGetApp().filaments_cnt(), 0));
    std::vector<unsigned int> ordered_filament_ids;
    ordered_filament_ids.reserve(num_physical);
    for (size_t idx = 0; idx < num_physical; ++idx)
        ordered_filament_ids.emplace_back(unsigned(idx + 1));

    if (wxGetApp().preset_bundle == nullptr)
        return ordered_filament_ids;

    const auto &mixed = wxGetApp().preset_bundle->mixed_filaments.mixed_filaments();
    if (mixed.empty())
        return ordered_filament_ids;

    const std::vector<size_t> ordered_mixed_indices = build_mixed_filament_ui_indices(mixed, p->mixed_panel->mixed_filament_ui_order());
    std::vector<unsigned int> actual_filament_id_by_mixed_idx(mixed.size(), 0);
    unsigned int              next_filament_id = unsigned(num_physical + 1);
    for (size_t mixed_idx = 0; mixed_idx < mixed.size(); ++mixed_idx) {
        if (!mixed[mixed_idx].enabled || mixed[mixed_idx].deleted)
            continue;
        actual_filament_id_by_mixed_idx[mixed_idx] = next_filament_id++;
    }

    ordered_filament_ids.reserve(size_t(next_filament_id - 1));
    for (const size_t mixed_idx : ordered_mixed_indices) {
        if (mixed_idx >= actual_filament_id_by_mixed_idx.size())
            continue;
        const unsigned int actual_filament_id = actual_filament_id_by_mixed_idx[mixed_idx];
        if (actual_filament_id != 0)
            ordered_filament_ids.emplace_back(actual_filament_id);
    }

    return ordered_filament_ids;
}

void Sidebar::add_filament() {
    if (p->combos_filament.size() >= MAXIMUM_EXTRUDER_NUMBER) return;
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb || pb->mixed_filaments.total_filaments(p->combos_filament.size()) >= MAXIMUM_FILAMENT_NUMBER) return;
    wxColour    new_col        = Plater::get_next_color_for_filament();
    add_custom_filament(new_col);
}

void Sidebar::on_filaments_delete(size_t filament_id)
{
    auto& choices = combos_filament();

    p->m_skip_mixed_filament_sync_once = false;

    if (filament_id >= choices.size())
        return;

    if (choices.size() <= 2)
        choices[0]->GetDropDown().Invalidate();

    wxWindowUpdateLocker noUpdates_scrolled_panel(this);

    // delete UI item
    if (filament_id < p->combos_filament.size()) {
        const int last            = p->combos_filament.size() - 1;
        auto      sizer_filaments = this->p->sizer_filaments->GetItem(last % 2)->GetSizer();
        sizer_filaments->Remove(last / 2);

        PlaterPresetComboBox* to_delete_combox = p->combos_filament[filament_id];
        (*p->combos_filament[last]).Destroy();
        p->combos_filament.pop_back();

        // BBS:  filament double columns
        auto sizer_filaments0 = this->p->sizer_filaments->GetItem((size_t) 0)->GetSizer();
        auto sizer_filaments1 = this->p->sizer_filaments->GetItem(1)->GetSizer();
        if (p->combos_filament.size() < 2) {
            sizer_filaments1->Clear();
        } else {
            size_t c0 = sizer_filaments0->GetChildren().GetCount();
            size_t c1 = sizer_filaments1->GetChildren().GetCount();
            if (c0 < c1)
                sizer_filaments1->Remove(c1 - 1);
            else if (c0 > c1)
                sizer_filaments1->AddStretchSpacer(1);
        }
    }

    auto sizer = p->m_panel_filament_title->GetSizer();
    if (p->m_flushing_volume_btn != nullptr && sizer != nullptr) {
        if (p->combos_filament.size() > 1)
            sizer->Show(p->m_flushing_volume_btn);
        else
            sizer->Hide(p->m_flushing_volume_btn);
    }

    if (p->m_bpButton_del_filament != nullptr && p->m_panel_physical_filaments_title != nullptr) {
        auto* inner_sizer = p->m_panel_physical_filaments_title->GetSizer();
        if (inner_sizer) {
            if (p->combos_filament.size() > 1)
                inner_sizer->Show(p->m_bpButton_del_filament);
            else
                inner_sizer->Hide(p->m_bpButton_del_filament);
        }
    }

    for (size_t idx = filament_id; idx < p->combos_filament.size(); ++idx) {
        p->combos_filament[idx]->update();
    }

    // Recalc scrolled filament window height (max 3 rows)
    if (p->m_scrolled_filaments && p->m_panel_scrolled_filament_content) {
        p->m_panel_scrolled_filament_content->Layout();
        const wxSize content_best = p->m_panel_scrolled_filament_content->GetBestSize();
        const int row_count = ((int)p->combos_filament.size() + 1) / 2;
        const int desired_h = row_count > 3
            ? (content_best.GetHeight() / std::max(1, row_count)) * 3
            : content_best.GetHeight();
        p->m_scrolled_filaments->SetMinSize({-1, desired_h});
        p->m_scrolled_filaments->SetMaxSize({-1, desired_h});
    }

    Layout();
    wxWeakRef<Sidebar> weak_this(this);
    wxTheApp->CallAfter([weak_this]() {
        Sidebar* sidebar = weak_this.get();
        if (sidebar && sidebar->p && sidebar->p->m_scrolled_filaments) {
            int vh = sidebar->p->m_scrolled_filaments->GetVirtualSize().y;
            int ch = sidebar->p->m_scrolled_filaments->GetClientSize().y;
            sidebar->p->m_scrolled_filaments->Scroll(0, std::max(0, vh - ch));
        }
    });
    p->m_panel_filament_title->Refresh();
    update_ui_from_settings();
    update_dynamic_filament_list();
    // During batch physical deletion, skip the expensive panel rebuilds —
    // they run once after the loop finishes (in cleanup_unused_filaments_after_batch_match).
    // Query the single Plater-owned flag (no Sidebar-side duplicate).
    if (wxGetApp().filament_svc().batch_physical_deletion() == 0) {
        p->mixed_panel->update_mixed_filament_panel();
        p->mixed_panel->update_color_mix_panel();
    }

    if (PresetBundle *pb = wxGetApp().preset_bundle) {
        const bool can_add = pb->mixed_filaments.total_filaments(p->combos_filament.size()) < MAXIMUM_FILAMENT_NUMBER;
        if (p->m_bpButton_add_filament)
            p->m_bpButton_add_filament->Enable(can_add);
        p->mixed_panel->set_add_color_mix_enabled(can_add);
    }
}

void Sidebar::edit_filament() {
    p->editing_filament = -1;
    if (p->m_menu_filament_id >= 0 && p->m_menu_filament_id < p->combos_filament.size() &&
        p->combos_filament[p->m_menu_filament_id]->switch_to_tab())
        p->editing_filament = p->m_menu_filament_id; // sync with TabPresetComboxBox's m_filament_idx
}

// Helper function: Check if target mixed filament depends on source physical filament

void Sidebar::change_filament(size_t from_id, size_t to_id)
{
    // 1. Parameter preprocessing
    if (from_id == size_t(-2))
        from_id = p->m_menu_filament_id;
    if (from_id == size_t(-1))
        from_id = p->combos_filament.size() - 1;
    if (from_id == to_id)
        return;

    auto& pb = *wxGetApp().preset_bundle;
    const size_t num_physical = pb.filament_presets.size();

    // 2. Determine source and target types
    // Note: filament IDs here are 0-based, but is_mixed expects 1-based
    bool from_is_mixed = pb.mixed_filaments.is_mixed((unsigned int)(from_id + 1), num_physical);
    bool to_is_mixed = pb.mixed_filaments.is_mixed((unsigned int)(to_id + 1), num_physical);

    // 3. Dependency check: physical → mixed
    if (!from_is_mixed && to_is_mixed) {
        const MixedFilament* target_mf = pb.mixed_filaments.mixed_filament_from_id((unsigned int)(to_id + 1), num_physical);
        unsigned int from_1based = (unsigned int)(from_id + 1);
        
        if (mixed_filament_uses_physical(target_mf, from_1based)) {
            MessageDialog dlg(wxGetApp().plater(),
                _L("The target mixed filament uses this physical filament as a component. "
                   "Merging will remove this physical filament and may invalidate the mixed filament. Continue?"),
                _L("Warning"), wxOK | wxCANCEL | wxICON_WARNING);
            int ret = dlg.ShowModal();
            if (ret != wxID_OK)
                return;
        }
    }

    // 3b. Check for mixed filaments that depend on the source physical filament
    if (!from_is_mixed) {
        unsigned int from_1based = (unsigned int)(from_id + 1);
        std::vector<size_t> dependent_mixed_indices = pb.mixed_filaments.mixed_filaments_using_physical(from_1based);
        
        // If there are dependent mixed filaments, show warning dialog
        if (!dependent_mixed_indices.empty()) {
            wxString msg = _L("This filament is used in the following mixed filament configurations:\n\n");
            
            const auto& mfs = pb.mixed_filaments.mixed_filaments();
            size_t visible_idx = 0;
            for (size_t j = 0; j < mfs.size(); ++j) {
                if (mfs[j].deleted) continue;
                
                // Check if this is one of the dependent mixed filaments
                bool is_dependent = std::find(dependent_mixed_indices.begin(), dependent_mixed_indices.end(), j) 
                                   != dependent_mixed_indices.end();
                
                if (is_dependent) {
                    const int virtual_id = static_cast<int>(num_physical) + visible_idx + 1;
                    msg += wxString::Format(_L("• Mixed Filament %d\n"), virtual_id);
                }
                
                visible_idx++;
            }
            
            msg += _L("\nMerging this filament will invalidate these mixed filament configurations. Continue?");

            MessageDialog dlg(wxGetApp().plater(), msg, _L("Warning"), wxOK | wxCANCEL | wxICON_WARNING);
            int ret = dlg.ShowModal();
            if (ret != wxID_OK)
                return;
        }
    }

    // 4. Execute merge based on scenario
    if (from_is_mixed) {
        // Mixed → Physical or Mixed → Mixed
        p->mixed_panel->merge_mixed_filament(from_id, to_id);
    } else {
        // Physical → Physical or Physical → Mixed
        delete_filament(from_id, int(to_id));
    }
}

void Sidebar::merge_mixed_filament(size_t from_id, size_t to_id,
                              bool skip_update, bool skip_serialize)
{
    p->mixed_panel->merge_mixed_filament(from_id, to_id, skip_update, skip_serialize);
}

void Sidebar::delete_filament(size_t filament_id, int replace_filament_id,
                               bool skip_dependency_check,
                               bool skip_update)
{
    if (p->combos_filament.size() <= 1) return;
    wxBusyCursor busy;
    size_t filament_count = p->combos_filament.size() - 1;

    if (filament_id == size_t(-2)) {
        filament_id = p->m_menu_filament_id;
    }
    if (filament_id == size_t(-1)) {
        filament_id = filament_count;
    }

    size_t total_filaments = wxGetApp().preset_bundle->filament_presets.size();
    if (filament_id > filament_count && filament_id >= total_filaments)
        return;

    bool is_mixed = (filament_id >= p->combos_filament.size());

    // Check for mixed filaments that depend on the source physical filament (before deletion)
    // Skip this dialog when called from change_filament() (merge path), since change_filament()
    // already showed its own confirmation dialog for the same dependent mixed filaments.
    if (!is_mixed && replace_filament_id < 0 && !skip_dependency_check) {
        auto& pb = *wxGetApp().preset_bundle;
        const size_t num_physical = pb.filament_presets.size();
        unsigned int filament_1based = (unsigned int)(filament_id + 1);
        std::vector<size_t> dependent_mixed_indices = pb.mixed_filaments.mixed_filaments_using_physical(filament_1based);

        // If there are dependent mixed filaments, show warning dialog
        if (!dependent_mixed_indices.empty()) {
            wxString msg = _L("This filament is used in the following mixed filament configurations:\n\n");

            const auto& mfs = pb.mixed_filaments.mixed_filaments();
            size_t visible_idx = 0;
            for (size_t j = 0; j < mfs.size(); ++j) {
                if (mfs[j].deleted) continue;

                // Check if this is one of the dependent mixed filaments
                bool is_dependent = std::find(dependent_mixed_indices.begin(), dependent_mixed_indices.end(), j)
                                   != dependent_mixed_indices.end();

                if (is_dependent) {
                    const int virtual_id = static_cast<int>(num_physical) + visible_idx + 1;
                    msg += wxString::Format(_L("• Mixed Filament %d\n"), virtual_id);
                }

                visible_idx++;
            }

            msg += _L("\nDeleting this filament will invalidate these mixed filament configurations. Continue?");

            MessageDialog dlg(wxGetApp().plater(), msg, _L("Warning"), wxOK | wxCANCEL | wxICON_WARNING);
            int ret = dlg.ShowModal();
            if (ret != wxID_OK)
                return;
        }
    }

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return;

    if (!is_mixed) {
        if (preset_bundle->is_the_only_edited_filament(filament_id) || (filament_id == 0)) {
            wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(preset_bundle->filament_presets[0], false, "", true);
        }
        if (p->editing_filament == filament_id || p->editing_filament >= filament_count) {
            p->editing_filament = -1;
        }
    }

    std::vector<unsigned char> is_mixed_snapshot;
    if (auto* opt = preset_bundle->project_config.option<ConfigOptionBools>("filament_is_mixed"))
        is_mixed_snapshot = opt->values;

    auto& pb = *preset_bundle;
    size_t old_num_physical = pb.filament_presets.size();
    size_t old_total_filaments = pb.mixed_filaments.total_filaments(old_num_physical);
    
    // Check if this is a physical filament being merged into a mixed filament
    bool is_physical_to_mixed_merge = !is_mixed && replace_filament_id >= (int)old_num_physical;
    
    // Check if target mixed filament depends on source physical filament
    bool target_depends_on_source = false;
    
    if (is_physical_to_mixed_merge) {
        // Get target mixed filament
        const MixedFilament* target_mf = pb.mixed_filaments.mixed_filament_from_id(
            (unsigned int)(replace_filament_id + 1), old_num_physical);
        unsigned int source_1based = (unsigned int)(filament_id + 1);
        
        // Use helper function to check dependency
        target_depends_on_source = mixed_filament_uses_physical(target_mf, source_1based);
        
        BOOST_LOG_TRIVIAL(info) << "Physical to mixed merge: source=" << filament_id 
                                << " target=" << replace_filament_id
                                << " target_depends_on_source=" << target_depends_on_source;
    }
    
    // Case 2: If target mixed doesn't depend on source physical, use remap mechanism
    // This is similar to merge_mixed_filament() - we use a remap table to redirect objects
    // to the target mixed filament, then delete the physical filament
    if (is_physical_to_mixed_merge && !target_depends_on_source) {
        // Build custom remap table that accounts for physical filament deletion
        // When a physical filament is deleted, mixed filament virtual IDs change
        pb.build_merge_filament_remap(filament_id, replace_filament_id, old_total_filaments, old_num_physical);
        
        BOOST_LOG_TRIVIAL(info) << "Built custom remap for physical to mixed merge (accounts for virtual ID changes)";
        
        // Preserve the custom merge target for config-level object/volume extruder
        // assignments. The count update below replaces PresetBundle's transient
        // remap with its generic deletion remap, so restore this merge-specific
        // table before on_filaments_delete() consumes it.
        const std::vector<unsigned int> physical_to_mixed_remap = pb.last_filament_id_remap();
        const int merged_target_id =
            physical_to_mixed_remap.size() > filament_id + 1 &&
            physical_to_mixed_remap[filament_id + 1] > 0
                ? int(physical_to_mixed_remap[filament_id + 1] - 1)
                : -1;

        // Update PresetBundle before refreshing the sidebar. Sidebar::on_filaments_delete()
        // first reduces the UI physical count and then reloads custom mixed definitions.
        // If it runs while the bundle still contains old physical IDs, a row such as
        // old (3, 5) is temporarily invalid against four UI slots and is discarded.
        // Updating first renumbers that row to (3, 4) and keeps the merge target alive.
        pb.update_num_filaments(filament_id);
        (void)pb.consume_last_filament_id_remap();
        pb.set_filament_id_remap(physical_to_mixed_remap);

        const size_t total_after_delete =
            pb.mixed_filaments.total_filaments(pb.filament_presets.size());
        wxGetApp().plates().partplate_list().on_filament_deleted(
            total_after_delete, filament_id);

        // Pass the post-deletion mixed target so painted states and config-level
        // object/volume extruder assignments follow the same remap.
        wxGetApp().filament_svc().on_filaments_delete(
            total_after_delete, filament_id, merged_target_id, is_mixed_snapshot);

        // Resynchronize filament_colour from the post-deletion project config;
        // GLCanvas3D reads this config when updating GLVolume colors.
        wxGetApp().filament_svc().update_filament_colors_in_full_config();

        // Refresh controls that may still hold the pre-deletion filament list.
        for (size_t idx = filament_id; idx < p->combos_filament.size(); ++idx) {
            if (p->combos_filament[idx])
                p->combos_filament[idx]->update();
        }
        obj_list()->update_objects_list_filament_column(pb.filament_presets.size());
        update_dynamic_filament_list();
        update_mixed_filament_panel(false);
        update_color_mix_panel();
        Layout();

        BOOST_LOG_TRIVIAL(info) << "Physical to mixed merge completed using custom remap mechanism";

        // Update UI
        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
        wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
        if (!skip_update)
            wxGetApp().refresh().update_gate(false, false);
        return;
    }
    // Case 1: If target mixed depends on source physical, delete the physical filament
    // Let on_filaments_delete use default behavior (reset to default)
    else if (is_physical_to_mixed_merge && target_depends_on_source) {
        BOOST_LOG_TRIVIAL(info) << "Physical to mixed merge: target depends on source, will delete physical and reset to default";
    }

    // Delete the physical filament (this also removes mixed filaments that depend on it)
    pb.update_num_filaments(filament_id);
    size_t new_num_physical = pb.filament_presets.size();

    size_t total_after_delete = pb.mixed_filaments.total_filaments(new_num_physical);
    wxGetApp().plates().partplate_list().on_filament_deleted(total_after_delete, filament_id);

    int final_replace_id;
    if (is_physical_to_mixed_merge && target_depends_on_source) {
        // Case 1: target mixed was also deleted by remove_physical_filament
        final_replace_id = -1;
    } else {
        int adjusted = replace_filament_id;
        if (replace_filament_id >= (int)old_num_physical)
            adjusted = int(new_num_physical + (size_t(replace_filament_id) - old_num_physical));
        else if (replace_filament_id > (int)filament_id)
            adjusted = replace_filament_id - 1;
        final_replace_id = adjusted;
    }

    wxGetApp().filament_svc().on_filaments_delete(total_after_delete, filament_id,
                                             final_replace_id,
                                             is_mixed_snapshot);

    wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    if (!skip_update)
        wxGetApp().refresh().update_gate(false, false);
}

// Helper to extract kept-physical and kept-mixed id sets from a batch match result
// so compute_redundant_filaments can decide which filaments are redundant.

void Sidebar::cleanup_unused_filaments_after_batch_match(const BatchMatchResult &match_result,
                                                          std::function<void(int, int)> on_progress)
{
    p->batch_match->cleanup_unused_filaments_after_batch_match(match_result, on_progress);
}

void Sidebar::add_custom_filament(wxColour new_col) {
    if (p->combos_filament.size() >= MAXIMUM_EXTRUDER_NUMBER) return;
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb || pb->mixed_filaments.total_filaments(p->combos_filament.size()) >= MAXIMUM_FILAMENT_NUMBER) return;

    int         filament_count = p->combos_filament.size() + 1;
    wxGetApp().plater()->confirm_auto_generated_gradients(filament_count);
    std::string new_color      = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    pb->set_num_filaments(filament_count, new_color);
    wxGetApp().filament_svc().on_filaments_change(filament_count);
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    pb->export_selections(*wxGetApp().app_config);
    auto_calc_flushing_volumes(filament_count - 1);
}

void Sidebar::on_bed_type_change(BedType bed_type)
{
    // btDefault option is not included in global bed type setting
    int sel_idx = (int)bed_type - 1;
    if (m_bed_type_list != nullptr)
        m_bed_type_list->SetSelection(sel_idx);
}

std::map<int, DynamicPrintConfig> Sidebar::build_filament_ams_list(MachineObject* obj)
{
    return wxGetApp().plater()->filament_service().build_filament_ams_list(obj);
}

void Sidebar::load_ams_list(std::string const &device, MachineObject* obj)
{
    wxGetApp().plater()->filament_service().load_ams_list(device, obj);
}

void Sidebar::sync_ams_list()
{
    wxGetApp().plater()->filament_service().sync_ams_list();
}

void Sidebar::apply_nozzle_sync_result(bool got_machine_info, std::string machine_type,
                                       std::vector<std::string> nozzle_diameters,
                                       std::vector<std::string> nozzle_volume_types,
                                       std::string device_name)
{
    if (!got_machine_info) {
        MessageDialog dlg(wxGetApp().mainframe,
                          _L("Printer not connected. Please go to the home page or the device page to connect the printer."),
                          _L("Note"), wxOK);
        dlg.ShowModal();
        return;
    }

    MachineInfo machine_info;
    machine_info.model                = SSWCPProtocol::normalize_machine_model(machine_type);
    machine_info.device_name          = device_name;
    machine_info.nozzle_diameters     = nozzle_diameters;
    machine_info.nozzle_volume_types = nozzle_volume_types;
    const auto& sync_nozzle_slots = wxGetApp().preset_bundle->m_connect_machine_info_list;
    if (!sync_nozzle_slots.empty()) {
        std::vector<std::pair<std::string, std::string>> cached_slots;
        for (const auto& slot : sync_nozzle_slots)
            cached_slots.emplace_back(slot.nozzle_info, slot.nozzle_volume_type);
        SSWCPProtocol::select_complete_cached_nozzle_info(cached_slots, nozzle_diameters, machine_info.nozzle_volume_types);
    }
    if (machine_info.model != "Snapmaker U1")
        return;

    if (nozzle_diameters.empty()) {
        MessageDialog dlg(wxGetApp().mainframe,
                          _L("No nozzle information detected. Please go to the printer settings to configure the nozzle."),
                          _L("Note"), wxOK);
        dlg.ShowModal();
        return;
    }

    auto apply_nozzle_preset = [this](const std::string& diameter) {
        auto preset = wxGetApp().preset_bundle->get_similar_printer_preset({}, diameter);
        if (preset == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "get the similar printer preset fail";
            return false;
        }
        preset->is_visible = true;
        for (size_t i = 0; i < p->m_nozzle_diameter_lists.size(); ++i)
            p->m_nozzle_diameter_lists[i]->SetValue(diameter + "mm");
        wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(preset->name);
        wxGetApp().plater()->sidebar().update_all_preset_comboboxes(true);
        return true;
    };

    const std::string& first_diameter = nozzle_diameters.front();
    const bool         mixed_diameters = std::any_of(nozzle_diameters.begin() + 1,
                                                     nozzle_diameters.end(),
                                                     [&](const std::string& diameter) { return diameter != first_diameter; });
    if (mixed_diameters) {
        NozzleDiameterSelectDialog dlg(wxGetApp().mainframe,
                                       _L("Note: Inconsistent nozzle diameters. Current version does not support mixed diameter printing. Please select one nozzle for this print."),
                                       _L("Set Nozzle Diameter"),
                                       nozzle_diameters);
        if (dlg.ShowModal() == wxID_OK) {
            std::string selected_diameter = dlg.GetSelectedDiameter();
            if (!selected_diameter.empty())
                apply_nozzle_preset(selected_diameter);
        }
    } else {
        std::string diameter = first_diameter;
        boost::algorithm::trim(diameter);
        if (diameter.size() > 2 && boost::iends_with(diameter, "mm")) {
            diameter.resize(diameter.size() - 2);
            boost::algorithm::trim(diameter);
        }
        if (!apply_nozzle_preset(diameter))
            return;
    }

    if (!machine_info.nozzle_volume_types.empty())
        GUI::FlowType::set_nozzle_volume_types(machine_info.nozzle_volume_types);
    wxGetApp().plater()->sidebar().update_nozzle_settings(true);
    if (!mixed_diameters)
        wxTheApp->CallAfter([this]() {
            MessageDialog dlg(wxGetApp().mainframe, _L("Nozzle settings synchronized successfully"), _L("Note"), wxOK);
            dlg.ShowModal();
        });
}

void Sidebar::show_sync_filament_dialog()
{
    if (!wxGetApp().plater())
        return;

    if (p->m_bpButton_sync_filament == nullptr || !p->m_bpButton_sync_filament->IsEnabled())
        return;

    p->m_bpButton_sync_filament->Disable();
    const auto      gateway    = wxGetApp().gateway_service();
    const auto      sync_state = p->nozzle_sync_state;
    ScalableButton* button      = p->m_bpButton_sync_filament;
    std::thread([gateway, sync_state, button]() {
        std::string              machine_type;
        std::string              device_name;
        std::vector<std::string> nozzle_diameters;
        std::vector<std::string> nozzle_volume_types;
        const bool got_machine_info = Gateway::GatewayDevice::query_machine_info(gateway, machine_type, nozzle_diameters, nozzle_volume_types, device_name);

        wxTheApp->CallAfter([sync_state, button, got_machine_info, machine_type = std::move(machine_type)]() {
            if (!sync_state->alive.load(std::memory_order_acquire))
                return;
            if (button != nullptr)
                button->Enable();
            sync_state->sidebar->continue_sync_filament_dialog(got_machine_info, machine_type);
        });
    }).detach();
}

void Sidebar::continue_sync_filament_dialog(bool got_machine_info, const std::string& machine_type)
{
    if (!got_machine_info) {
        SyncRichConfirmDialog dlg(this,
            _L("No printer is connected. Please connect your U1 from the Device page before syncing."),
            wxYES_NO);
        dlg.SetYesNoLabels(_L("Connect Now"), _L("Later"));
        dlg.CentreOnScreen();
        if (dlg.ShowModal() == wxID_YES) {
            dlg.navigateToTab(MainFrame::tpMonitor);
        }
        return;
    }

    {
        static const std::set<std::string> white_list_machine_types = {
            "Snapmaker U1"
        };
        MachineInfo machine_info;
        machine_info.model            = SSWCPProtocol::normalize_machine_model(machine_type);

        bool is_white_listed_type = white_list_machine_types.find(machine_info.model) != white_list_machine_types.end();

        if (!machine_info.model.empty() && !is_white_listed_type) {
            SyncRichConfirmDialog dlg(this,
                _L("The connected printer is not U1. Unable to sync filament information. Please switch to U1 and try again."),
                wxYES_NO);
            dlg.SetYesNoLabels(_L("Connect Now"), _L("Later"));
            dlg.CentreOnScreen();
            if (dlg.ShowModal() == wxID_YES) {
                dlg.navigateToTab(MainFrame::tpMonitor);
            }
            return;
        }
    }

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return;

    std::vector<FilamentData> machineFilamentList;
    build_machine_filament_list(preset_bundle, machineFilamentList);
    auto nonEmptyFilaments = [](const std::vector<FilamentData>& filamentDatas) {
        for (const auto& filament : filamentDatas) {
            if (!is_none_filament(filament))
                return true;
        }
        return false;
    };
    if (machineFilamentList.empty() || !nonEmptyFilaments(machineFilamentList)) {
        SyncRichConfirmDialog dlg(this,
            _L("No filaments detected on the printer. Please preload the filaments before syncing."),
            wxOK);
        dlg.SetOKLabel(_L("Got it"));
        dlg.CentreOnScreen();
        dlg.ShowModal();
        return;
    }

    std::vector<FilamentData> designFilamentList;
    build_design_filament_list(preset_bundle, designFilamentList);

    std::vector<MixedFilamentPreviewInfo> mixedInfos;
    int oldPhysicalCount = designFilamentList.size();
    const auto& mixedFilaments = preset_bundle->mixed_filaments.mixed_filaments();
    int enabledMixedIdx = 0;
    for (size_t i = 0; i < mixedFilaments.size(); ++i) {
        const auto& mf = mixedFilaments[i];
        if (mf.deleted || !mf.enabled)
            continue;

        MixedFilamentPreviewInfo info;
        info.m_virtual_filament_id = oldPhysicalCount + enabledMixedIdx;
        info.m_config = mf;
        mixedInfos.push_back(info);
        ++enabledMixedIdx;
    }

    wxGetApp().plater()->update_all_plate_thumbnails(true);
    SyncFilamentColorDialog dlg(this, designFilamentList, machineFilamentList);
    dlg.setHasMixedFilaments(preset_bundle->mixed_filaments.enabled_count() > 0);
    dlg.setMixedFilamentInfos(mixedInfos);
    {
        if (wxGetApp().plater()->model().objects.empty()) {
            dlg.setOverwriteMode();
        } else {
            dlg.CentreOnScreen();
            if (dlg.ShowModal() != wxID_OK)
                return;
        }
        std::vector<FilamentData> syncedData = dlg.getSyncDataList();

        size_t effective_size = syncedData.size();
        // The number of filaments cannot be reduced to zero.
        if (effective_size == 0)
            return;

        size_t combo_Size = p->combos_filament.size();
        if (effective_size != combo_Size) {
            if (effective_size > combo_Size &&
                    (effective_size > MAXIMUM_EXTRUDER_NUMBER ||
                        preset_bundle->mixed_filaments.total_filaments(effective_size) >= MAXIMUM_FILAMENT_NUMBER)) {
                effective_size = combo_Size;
            }
            if (effective_size != combo_Size) {
                wxColour    new_col   = Plater::get_next_color_for_filament();
                std::string new_color = into_u8(new_col.GetAsString(wxC2S_HTML_SYNTAX));
                preset_bundle->set_num_filaments(effective_size, new_color);

                const auto& dlgRemap = dlg.getFilamentIdRemap();
                if (!dlgRemap.empty())
                    preset_bundle->set_filament_id_remap(dlgRemap);
            }
        }

        ConfigOptionStrings* co = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        ConfigOptionStrings* mco = preset_bundle->project_config.option<ConfigOptionStrings>("filament_multi_colors");
        ConfigOptionInts*    cmo = preset_bundle->project_config.option<ConfigOptionInts>("filament_colour_mode");

        for (size_t i = 0; i < effective_size; ++i) {
            Preset* matched = resolve_filament_preset(preset_bundle, syncedData[i].m_name, syncedData[i].m_type);
            if (matched) {
                preset_bundle->set_filament_preset(i, matched->name);
            }

            if (co && i < co->values.size()) {
                wxColour c = getMainColor(syncedData[i].m_color);
                co->values[i] = into_u8(c.GetAsString(wxC2S_HTML_SYNTAX));
            }
            if (mco && i < mco->values.size()) {
                mco->values[i] = syncedData[i].m_color.ToMultiColorsString();
            }
            if (cmo && i < cmo->values.size()) {
                cmo->values[i] = FilamentColorModeToConfig(syncedData[i].m_color.NormalizedMode());
            }
        }

        if (dlg.shouldDeleteMixedFilaments()) {
            auto& mixedList = preset_bundle->mixed_filaments.mixed_filaments();
            for (auto& mf : mixedList) {
                if (mf.enabled && !mf.deleted) {
                    mf.deleted = true;
                    mf.enabled = false;
                }
            }
            if (auto* opt = preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                opt->value = preset_bundle->mixed_filaments.serialize_custom_entries();
        }

        const size_t num_filaments = effective_size;
        wxGetApp().plater()->on_filaments_change(num_filaments);

        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
        preset_bundle->export_selections(*wxGetApp().app_config);

        for (auto* combo : p->combos_filament) {
            if (combo)
                combo->update();
        }

        for (size_t i = 0; i < effective_size; ++i) {
            auto_calc_flushing_volumes(i);
        }

        wxGetApp().plater()->get_notification_manager()->push_notification(
            NotificationType::CustomNotification,
            NotificationManager::NotificationLevel::RegularNotificationLevel,
            _u8L("Filament types and colors have been successfully synced from the printer."));
    }
}

void Sidebar::show_SEMM_buttons(bool bshow)
{
    if(p->m_bpButton_add_filament)
        p->m_bpButton_add_filament->Show(bshow);
    if (p->m_bpButton_del_filament && p->combos_filament.size() > 1) // ORCA add filament count as condition to prevent showing Flushing volumes and Del Filament icon visible while only 1 filament exist
        p->m_bpButton_del_filament->Show(bshow);
    if (p->m_flushing_volume_btn && p->combos_filament.size() > 1) // ORCA add filament count as condition to prevent showing Flushing volumes and Del Filament icon visible while only 1 filament exist
        p->m_flushing_volume_btn->Show(bshow);
    Layout();
}

void Sidebar::update_dynamic_filament_list()
{
    dynamic_filament_list.update();
    dynamic_filament_list_1_based.update();
}

void Sidebar::update_nozzle_settings(bool switch_machine)
{
    if (!p->m_nozzle_notebook)
        return;

    const DynamicPrintConfig& printer_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;

    // Get new nozzle count
    auto* nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(printer_config.option("nozzle_diameter"));
    size_t new_nozzle_count = nozzle_diameter ? nozzle_diameter->values.size() : 1;

    std::string diam_str = "";
    if (const auto* pv = printer_config.option<ConfigOptionString>("printer_variant")) // absent in bare configs
        diam_str = pv->value;

    // Visible presets for this printer_model (system + user).
    auto diameters = wxGetApp().preset_bundle->printers.diameters_of_selected_printer();

    // Record focus before DeleteAllPages destroys the focused control.
    bool focus_was_in_notebook = false;
    if (wxWindow* focus = wxWindow::FindFocus())
        focus_was_in_notebook = p->m_nozzle_notebook->IsDescendant(focus);

    wxWindowUpdateLocker noUpdates(p->m_nozzle_notebook);

    // Rebuild in one visual step: freeze the notebook so the tab strip does not
    // flash through the empty / partially-rebuilt states — DeleteAllPages() and
    // each AddPage() refresh, and those intermediate repaints coalesce into a
    // single paint on thaw.
    p->m_nozzle_notebook->Freeze();

    // Clear existing pages and controls
    p->m_nozzle_notebook->DeleteAllPages();
    p->m_nozzle_diameter_lists.clear();
    p->m_nozzle_edit_btns.clear();
    p->m_nozzle_flow_lists.clear();

    // Recreate pages for new nozzle count
    // Create tabs for each nozzle
    for (size_t i = 0; i < new_nozzle_count; i++) {
        wxPanel* nozzle_panel = new wxPanel(p->m_nozzle_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                            wxTAB_TRAVERSAL | wxBORDER_NONE);
        // nozzle_panel->SetBackgroundColour(wxColour(255, 255, 255));

        wxBoxSizer* tab_sizer = new wxBoxSizer(wxHORIZONTAL);

        // Add diameter label and combobox
        wxBoxSizer*   diameter_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText* diameter_label = new wxStaticText(nozzle_panel, wxID_ANY, _L("Diameter"));
        bool          is_dark        = wxGetApp().app_config->get("dark_color_mode") == "1";
        if (!is_dark) {
            diameter_label->SetForegroundColour(wxColor(0, 0, 0));
        }
        else {
            diameter_label->SetForegroundColour(wxColor(194, 194, 194));
        }
        
        diameter_label->SetFont(Label::Body_14);

        ComboBox* diameter_combo = new ComboBox(nozzle_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, {-1, FromDIP(32)}, 0,
                                                nullptr, wxCB_READONLY);
        

        for (auto& diameter : diameters) {
            diameter_combo->AppendString(wxString(diameter) + "mm");
        }
        if (diameter_combo->GetCount() == 0 && !diam_str.empty()) {
            diameter_combo->AppendString(wxString(diam_str) + "mm");
        }
        if (diameters.size() < 2) {
            diameter_combo->Enable(false);
        }

        diameter_combo->Bind(wxEVT_COMBOBOX, [this, diameter_combo, i](wxCommandEvent& event) {

            //auto* pNotice = p->plater->get_notification_manager();
            //if (pNotice)
            //{
            //    pNotice->close_notification_of_type(NotificationType::CustomNotification);
            //    pNotice->push_notification(_u8L("Note: Printing PLA Silk on the hot end of 0.6mm hardened steel is not recommended. 0.4mm or smaller specifications are suggested."), 0); 
            //    pNotice->set_slicing_progress_hidden();            
            //}

            auto printer_config    = wxGetApp().preset_bundle->printers.get_edited_preset().config;
            auto printer_model_opt = printer_config.option<ConfigOptionString>("printer_model");
            if (printer_model_opt) {
                std::string printer_model   = printer_model_opt->value;
                bool        is_snapmaker_u1 = boost::icontains(printer_model, "Snapmaker") && boost::icontains(printer_model, "U1");

                if (is_snapmaker_u1)
                {
                    //check the config has flags to tips switch nozzle and all nozzle will be changed to the same type
                    auto  notShow = wxGetApp().app_config->get("app", "sync_diameter_flags");
                    if (notShow != "true")
                    {
                        RichMessageDialog dlg(static_cast<wxWindow*>(wxGetApp().mainframe),
                                              _L("Note: Changing this will sync all other nozzles to the same diameter."),
                                              _L("Set Nozzle Diameter"), 
                                               wxOK);
                        dlg.ShowCheckBox(_L("Don't show this again"), false);
                        auto res = dlg.ShowModal();
                        bool isCheckBox = dlg.IsCheckBoxChecked();

                        if (wxID_OK == res)
                            wxGetApp().app_config->set("app", "sync_diameter_flags", isCheckBox);     
                    }
                }
            }

            auto diameter = diameter_combo->GetValue().substr(0, 3);
            auto preset          = wxGetApp().preset_bundle->get_similar_printer_preset({}, diameter.ToStdString());
            if (preset == nullptr) {
                BOOST_LOG_TRIVIAL(error) << "get the similar printer preset fail";
                return;
            }
            preset->is_visible = true; // force visible

            for (size_t i = 0; i < p->m_nozzle_diameter_lists.size(); ++i) {
                //set all nozzle use the diameter
                p->m_nozzle_diameter_lists[i]->SetValue(diameter + "mm");
            }

            wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(preset->name);
            // Tab::select_preset does NOT rebuild the nozzle notebook (only load_current_presets and
            // the device-sync paths do), so rebuild it explicitly. Deferred with CallAfter because a
            // synchronous rebuild would destroy this combo while its event is still being processed.
            wxTheApp->CallAfter([]() {
                if (wxGetApp().plater() != nullptr)
                    wxGetApp().plater()->sidebar().update_nozzle_settings(true);
            });
            // Do not event.Skip(): the sidebar-level handler would treat this combo as the bed-type combo.
        });
        
        diameter_combo->SetValue(diam_str.empty() ? wxString() : wxString(diam_str) + "mm");

        p->m_nozzle_diameter_lists.push_back(diameter_combo);

        diameter_sizer->AddSpacer(15);
        diameter_sizer->Add(diameter_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        diameter_sizer->AddSpacer(10);
        diameter_sizer->Add(diameter_combo, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(15));

        // Snapmaker: per-nozzle flow type (standard / high flow), requirement 7.1
        wxStaticText* flow_label = new wxStaticText(nozzle_panel, wxID_ANY, _L("Flow"));
        flow_label->SetForegroundColour(is_dark ? wxColor(194, 194, 194) : wxColor(0, 0, 0));
        flow_label->SetFont(Label::Body_14);

        ComboBox* flow_combo = new ComboBox(nozzle_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, {-1, FromDIP(32)}, 0,
                                            nullptr, wxCB_READONLY);
        const bool supports_high_flow = GUI::FlowType::printer_supports_high_flow();
        flow_combo->AppendString(_L("Standard"));
        if (supports_high_flow)
            flow_combo->AppendString(_L("High Flow"));
        else
            flow_combo->Enable(false);
        const std::vector<std::string> nozzle_flows = GUI::FlowType::nozzle_volume_types();
        flow_combo->SetSelection(i < nozzle_flows.size() && nozzle_flows[i] == FLOW_MODE_HIGH_FLOW ? 1 : 0);
        flow_combo->Bind(wxEVT_COMBOBOX, [flow_combo, i](wxCommandEvent&) {
            GUI::FlowType::set_nozzle_volume_type(i, flow_combo->GetSelection() == 1 ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD);
            // Do not event.Skip(): the sidebar-level wxEVT_COMBOBOX handler would treat this
            // combo as the bed-type combo (same reason as the diameter combo above).
        });
        p->m_nozzle_flow_lists.push_back(flow_combo);

        diameter_sizer->AddSpacer(10);
        diameter_sizer->Add(flow_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        diameter_sizer->AddSpacer(10);
        diameter_sizer->Add(flow_combo, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(15));

        tab_sizer->Add(diameter_sizer, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

        nozzle_panel->SetSizer(tab_sizer);

        // Add tab
        wxString tab_name = "";
        switch (new_nozzle_count)
        {
        case 1:
        {
            tab_name = _L("Nozzle");
            break;
        }
        case 2:
        {
            if (i == 0)
                tab_name = _L("Left Nozzle");
            else
                tab_name = _L("Right Nozzle");

            break;
        }
        default:
        {
            tab_name = wxString(_L("Nozzle")) + wxString::Format(" %d", i + 1);
        }
            
        }
        p->m_nozzle_notebook->AddPage(nozzle_panel, tab_name);
    }

    p->m_nozzle_notebook->Layout();
    p->m_nozzle_notebook->Thaw();
    p->m_nozzle_notebook->InvalidateBestSize();
    if (p->m_nozzle_notebook->GetParent() != nullptr)
    {
        p->m_nozzle_notebook->GetParent()->InvalidateBestSize();
    }
    p->m_panel_printer_content->InvalidateBestSize();
    p->scrolled->InvalidateBestSize();
    p->m_panel_printer_content->Layout();
    m_scrolled_sizer->Layout();

    if (switch_machine) {
        p->combo_printer->SetFocus();
    } else if (focus_was_in_notebook) {
        // The focused control was destroyed by the rebuild.
        p->combo_printer->GetParent()->SetFocus();
    }
}

ObjectList* Sidebar::obj_list()
{
    // BBS
    //return obj_list();
    return p->m_object_list;
}

ObjectSettings* Sidebar::obj_settings()
{
    return p->object_settings;
}

ObjectLayers* Sidebar::obj_layers()
{
    return p->object_layers;
}

wxPanel* Sidebar::scrolled_panel()
{
    return p->scrolled;
}

wxPanel* Sidebar::print_panel()
{
    return p->m_panel_print_content;
}

wxPanel* Sidebar::filament_panel()
{
    return p->m_panel_filament_content;
}

ConfigOptionsGroup* Sidebar::og_freq_chng_params(const bool is_fff)
{
    // BBS
#if 0
    return p->frequently_changed_parameters->get_og(is_fff);
#endif
    return NULL;
}

wxButton* Sidebar::get_wiping_dialog_button()
{
#if 0
    return p->frequently_changed_parameters->get_wiping_dialog_button();
#endif
    return NULL;
}

void Sidebar::enable_buttons(bool enable)
{
#if 0
    p->btn_reslice->Enable(enable);
    p->btn_export_gcode->Enable(enable);
    p->btn_send_gcode->Enable(enable);
//    p->btn_eject_device->Enable(enable);
    p->btn_export_gcode_removable->Enable(enable);
#endif
}

bool Sidebar::show_reslice(bool show)          const { return p->btn_reslice->Show(show); }
bool Sidebar::show_export(bool show)           const { return p->btn_export_gcode->Show(show); }
bool Sidebar::show_send(bool show)             const { return p->btn_send_gcode->Show(show); }
bool Sidebar::show_export_removable(bool show) const { return p->btn_export_gcode_removable->Show(show); }
//bool Sidebar::show_eject(bool show)            const { return p->btn_eject_device->Show(show); }
//bool Sidebar::get_eject_shown()                const { return p->btn_eject_device->IsShown(); }

bool Sidebar::is_multifilament()
{
    return p->combos_filament.size() > 1;
}

static std::vector<Search::InputInfo> get_search_inputs(ConfigOptionMode mode)
{
    std::vector<Search::InputInfo> ret {};

    auto& tabs_list = wxGetApp().tabs_list;
    auto print_tech = wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology();
    for (auto tab : tabs_list)
        if (tab->supports_printer_technology(print_tech))
            ret.emplace_back(Search::InputInfo {tab->get_config(), tab->type(), mode});

    return ret;
}

void Sidebar::update_searcher()
{
    p->searcher.init(get_search_inputs(m_mode));
}

void Sidebar::update_mode()
{
    m_mode = wxGetApp().get_mode();

    //BBS: remove print related combos
    update_searcher();

    wxWindowUpdateLocker noUpdates(this);

    // BBS
    //obj_list()->get_sizer()->Show(m_mode > comSimple);

    obj_list()->unselect_objects();
    obj_list()->update_selections();
//    obj_list()->update_object_menu();

    Layout();
}

bool Sidebar::is_collapsed() { return p->plater->is_sidebar_collapsed(); }

void Sidebar::collapse(bool collapse) { p->plater->collapse_sidebar(collapse); }

#ifdef _MSW_DARK_MODE
void Sidebar::show_mode_sizer(bool show)
{
    //p->mode_sizer->Show(show);
}
#endif

void Sidebar::update_ui_from_settings()
{
    // BBS
    //p->object_manipulation->update_ui_from_settings();
    // update Cut gizmo, if it's open
    GLCanvas3D* canvas = p->plater->canvas3D();
    if (canvas) {
        canvas->update_gizmos_on_off_state();
        p->plater->set_current_canvas_as_dirty();        
        p->plater->get_current_canvas3D()->request_extra_frame();
    }
#if 0
    p->object_list->apply_volumes_order();
#endif
}

bool Sidebar::show_object_list(bool show) const
{
    p->m_search_bar->Show(show);
    if (!p->m_object_list->Show(show))
        return false;
    if (!show)
        p->object_layers->Show(false);
    else
        p->m_object_list->part_selection_changed();
    p->scrolled->Layout();
    return true;
}

void Sidebar::finish_param_edit() { p->editing_filament = -1; }

std::vector<PlaterPresetComboBox*>& Sidebar::combos_filament()
{
    return p->combos_filament;
}

Search::OptionsSearcher& Sidebar::get_searcher()
{
    return p->searcher;
}

std::string& Sidebar::get_search_line()
{
    return p->searcher.search_string();
}

void Sidebar::update_printer_thumbnail()
{
    auto& preset_bundle = wxGetApp().preset_bundle;
    Preset & selected_preset = preset_bundle->printers.get_edited_preset();

    auto        inherit    = selected_preset.inherits();
    std::string model_name = inherit == "" ? selected_preset.name : inherit;
    std::string png_name   = "";
    std::string vendor     = model_name.substr(0, model_name.find_first_of(" "));
    if (model_name.find("Snapmaker") != std::string::npos) {
        png_name = model_name.substr(0, model_name.find_last_of("(") - 1);
    } else {
        png_name = "printer_placeholder";
    }
    png_name += "_cover.png";

    boost::filesystem::path(resources_dir()) / "profile" / vendor / png_name;
    std::string printer_type    = selected_preset.get_current_printer_type(preset_bundle);

    try {
        p->image_printer->SetBitmap(create_scaled_bitmap(png_name, this, 48));
    }
    catch (std::exception& e) {
        p->image_printer->SetBitmap(create_scaled_bitmap("printer_placeholder", this, 48));
    }
    

    /*if (printer_thumbnails.find(printer_type) != printer_thumbnails.end())
        p->image_printer->SetBitmap(create_scaled_bitmap(, this, 48));
    else
        p->image_printer->SetBitmap(create_scaled_bitmap("printer_placeholder", this, 48));*/
}

void Sidebar::auto_calc_flushing_volumes(const int modify_id)
{
    auto& preset_bundle = wxGetApp().preset_bundle;
    auto& project_config = preset_bundle->project_config;
    auto& printer_config = preset_bundle->printers.get_edited_preset().config;
    const auto& full_config = wxGetApp().preset_bundle->full_config();
    auto& ams_multi_color_filament = preset_bundle->ams_multi_color_filment;

    const std::vector<double>& init_matrix = (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values;
    const std::vector<double>& init_extruders = (project_config.option<ConfigOptionFloats>("flush_volumes_vector"))->values;

    const std::vector<int>&   min_flush_volumes= get_min_flush_volumes(full_config);

    ConfigOptionFloat* flush_multi_opt = project_config.option<ConfigOptionFloat>("flush_multiplier");
    float flush_multiplier = flush_multi_opt ? flush_multi_opt->getFloat() : 1.f;
    std::vector<double> matrix = init_matrix;
    int m_max_flush_volume = Slic3r::g_max_flush_volume;
    unsigned int m_number_of_extruders = (int)(sqrt(init_matrix.size()) + 0.001);

    const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false);
    std::vector<std::vector<wxColour>> multi_colours;

    // Support for multi-color filament
    for (int i = 0; i < extruder_colours.size(); ++i) {
        std::vector<wxColour> single_filament;
        if (i < ams_multi_color_filament.size()) {
            if (!ams_multi_color_filament[i].empty()) {
                std::vector<std::string> colors = ams_multi_color_filament[i];
                for (int j = 0; j < colors.size(); ++j) {
                    single_filament.push_back(wxColour(colors[j]));
                }
                multi_colours.push_back(single_filament);
                continue;
            }
        }

        single_filament.push_back(wxColour(extruder_colours[i]));
        multi_colours.push_back(single_filament);
    }

    const size_t expectedMatrixSize = multi_colours.size() * multi_colours.size();
    if (matrix.size() != expectedMatrixSize) {
        BOOST_LOG_TRIVIAL(error) << "Invalid flushing volume matrix: modify_id=" << modify_id
                                 << ", filament_count=" << multi_colours.size()
                                 << ", matrix_size=" << matrix.size()
                                 << ", expected_size=" << expectedMatrixSize;
    }

    if (modify_id >= 0 && modify_id < multi_colours.size()) {
        for (int i = 0; i < multi_colours.size(); ++i) {
            // from to modify
            int from_idx = i;
            if (from_idx != modify_id) {
                Slic3r::FlushVolCalculator calculator(min_flush_volumes[from_idx], m_max_flush_volume);
                int flushing_volume = 0;
                bool is_from_support = is_support_filament(from_idx);
                bool is_to_support = is_support_filament(modify_id);
                if (is_to_support) {
                    flushing_volume = Slic3r::g_flush_volume_to_support;
                }
                else {
                    for (int j = 0; j < multi_colours[from_idx].size(); ++j) {
                        const wxColour& from = multi_colours[from_idx][j];
                        for (int k = 0; k < multi_colours[modify_id].size(); ++k) {
                            const wxColour& to = multi_colours[modify_id][k];
                            int volume = calculator.calc_flush_vol(from.Alpha(), from.Red(), from.Green(), from.Blue(), to.Alpha(), to.Red(), to.Green(), to.Blue());
                            flushing_volume = std::max(flushing_volume, volume);
                        }
                    }
                    if (is_from_support)
                        flushing_volume = std::max(flushing_volume, Slic3r::g_min_flush_volume_from_support);
                }
                matrix[m_number_of_extruders * from_idx + modify_id] = flushing_volume;
            }

            // modify to to
            int to_idx = i;
            if (to_idx != modify_id) {
                Slic3r::FlushVolCalculator calculator(min_flush_volumes[modify_id], m_max_flush_volume);
                bool is_from_support = is_support_filament(modify_id);
                bool is_to_support = is_support_filament(to_idx);
                int flushing_volume = 0;
                if (is_to_support) {
                    flushing_volume = Slic3r::g_flush_volume_to_support;
                }
                else {
                    for (int j = 0; j < multi_colours[modify_id].size(); ++j) {
                        const wxColour& from = multi_colours[modify_id][j];
                        for (int k = 0; k < multi_colours[to_idx].size(); ++k) {
                            const wxColour& to = multi_colours[to_idx][k];
                            int volume = calculator.calc_flush_vol(from.Alpha(), from.Red(), from.Green(), from.Blue(), to.Alpha(), to.Red(), to.Green(), to.Blue());
                            flushing_volume = std::max(flushing_volume, volume);
                        }
                    }
                    if (is_from_support)
                        flushing_volume = std::max(flushing_volume, Slic3r::g_min_flush_volume_from_support);

                    matrix[m_number_of_extruders * modify_id + to_idx] = flushing_volume;
                }
            }
        }
    }
    (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values = std::vector<double>(matrix.begin(), matrix.end());

    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    wxGetApp().session().update_project_dirty_from_presets();
    wxPostEvent(this, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, this));
}

void Sidebar::jump_to_object(ObjectDataViewModelNode* item)
{
    p->jump_to_object(item);
}

void Sidebar::can_search()
{
    p->can_search();
}



void Sidebar::set_btn_label(const ActionButtonType btn_type, const wxString& label) const
{
    switch (btn_type)
    {
        case ActionButtonType::abReslice:   p->btn_reslice->SetLabelText(label);        break;
        case ActionButtonType::abExport:    p->btn_export_gcode->SetLabelText(label);   break;
        case ActionButtonType::abSendGCode: /*p->btn_send_gcode->SetLabelText(label);*/     break;
    }
}

} // namespace Slic3r
} // namespace GUI
