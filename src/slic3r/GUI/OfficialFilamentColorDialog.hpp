#pragma once

#include "GUI_Utils.hpp"
#include "OfficialFilamentColorLibrary.hpp"

#include <string>
#include <vector>

class wxStaticBitmap;
class wxStaticText;

namespace Slic3r { namespace GUI {

struct OfficialFilamentColorSelection
{
    std::string name;
    std::string sku;
    int mode { 0 };
    std::string primary_color;
    std::vector<std::string> colors;
    bool is_custom { false };
};

class OfficialFilamentColorDialog : public DPIDialog
{
public:
    OfficialFilamentColorDialog(wxWindow* parent,
        const OfficialFilamentMaterial& material,
        const std::string& current_sku,
        const std::string& current_multi_colors,
        int current_mode,
        const std::string& fallback_color);

    const OfficialFilamentColorSelection& selection() const { return m_selection; }

private:
    void build_ui();
    void select_official_color(const OfficialFilamentColor& color);
    void select_custom_color(const std::string& color);
    void update_preview();
    void update_swatch_selection();
    void on_more_color(wxCommandEvent& event);

private:
    OfficialFilamentMaterial m_material;
    OfficialFilamentColorSelection m_selection;
    std::vector<std::pair<wxWindow*, std::string>> m_swatch_by_sku;
    wxStaticBitmap* m_preview_bitmap { nullptr };
    wxStaticText* m_name_label { nullptr };
    wxStaticText* m_sku_label { nullptr };
};

}} // namespace Slic3r::GUI
