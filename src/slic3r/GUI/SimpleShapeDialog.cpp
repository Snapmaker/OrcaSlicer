#include "SimpleShapeDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/NSVGUtils.hpp"

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r { namespace GUI {

namespace {

const char *SHAPE_CFG_SECTION = "simple_shape";
const int   SHAPE_PREVIEW_PX  = 160;

// Order of items in the shape choice is the order of SimpleShapeType
const std::array<SimpleShapeType, 8> SHAPE_CHOICES = {SimpleShapeType::Circle,  SimpleShapeType::Square,  SimpleShapeType::Triangle,
                                                      SimpleShapeType::Pentagon, SimpleShapeType::Hexagon, SimpleShapeType::Octagon,
                                                      SimpleShapeType::Star,     SimpleShapeType::Ring};

wxString filament_label(int index) { return index == 0 ? _L("Default") : wxString::Format(_L("Filament %d"), index); }

bool has_corners(SimpleShapeType type) { return type != SimpleShapeType::Circle && type != SimpleShapeType::Ring; }

} // namespace

SimpleShapeParams SimpleShapeDialog::load_from_config()
{
    SimpleShapeParams p;
    AppConfig        *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return p;
    auto read_double = [cfg](const char *key, double &out, double min, double max) {
        if (!cfg->has(SHAPE_CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stod(cfg->get(SHAPE_CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    auto read_int = [cfg](const char *key, int &out, int min, int max) {
        if (!cfg->has(SHAPE_CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stoi(cfg->get(SHAPE_CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    int type = int(p.type);
    read_int("type", type, 0, int(SHAPE_CHOICES.size()) - 1);
    p.type = SimpleShapeType(type);
    read_double("size", p.size, 0.5, 500.);
    read_double("depth", p.depth, 0.05, 100.);
    read_int("star_points", p.star_points, 3, 24);
    read_double("inner_ratio", p.inner_ratio, 0.05, 0.95);
    read_double("corner_radius", p.corner_radius, 0., 100.);
    return p;
}

void SimpleShapeDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    cfg->set(SHAPE_CFG_SECTION, "type", std::to_string(int(m_params.type)));
    cfg->set(SHAPE_CFG_SECTION, "size", float_to_string_decimal_point(m_params.size));
    cfg->set(SHAPE_CFG_SECTION, "depth", float_to_string_decimal_point(m_params.depth));
    cfg->set(SHAPE_CFG_SECTION, "star_points", std::to_string(m_params.star_points));
    cfg->set(SHAPE_CFG_SECTION, "inner_ratio", float_to_string_decimal_point(m_params.inner_ratio));
    cfg->set(SHAPE_CFG_SECTION, "corner_radius", float_to_string_decimal_point(m_params.corner_radius));
}

SimpleShapeDialog::SimpleShapeDialog(wxWindow *parent, const SimpleShapeParams &params, const SimpleShapeDialogOptions &options)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                options.is_edit ? _L("Edit shape") : _L("Add shape"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_params(params)
    , m_options(options)
{
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        m_filament_colors = plater->get_extruder_colors_from_plater_config(nullptr, false);

    wxBoxSizer      *root = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer      *cols = new wxBoxSizer(wxHORIZONTAL);
    wxFlexGridSizer *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(1);

    auto row = [this, &grid](const wxString &text, wxWindow *ctrl, std::vector<wxWindow *> *group = nullptr) {
        wxStaticText *l = new wxStaticText(this, wxID_ANY, text);
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        if (group != nullptr) {
            group->push_back(l);
            group->push_back(ctrl);
        }
    };
    auto spin_double = [this](double value, double min, double max, double inc) {
        auto *s = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, min, max,
                                       value, inc);
        s->SetDigits(2);
        return s;
    };
    auto spin_int = [this](int value, int min, int max) {
        return new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, min, max, value);
    };

    wxArrayString types;
    types.Add(_L("Circle"));
    types.Add(_L("Square"));
    types.Add(_L("Triangle"));
    types.Add(_L("Pentagon"));
    types.Add(_L("Hexagon"));
    types.Add(_L("Octagon"));
    types.Add(_L("Star"));
    types.Add(_L("Ring"));
    m_type = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, types);
    m_type->SetSelection(int(m_params.type));
    row(_L("Shape") + ":", m_type);

    m_size = spin_double(m_params.size, 0.5, 500., 1.);
    m_size->SetToolTip(_L("Width of the shape, the height follows its proportions. "
                          "It can be changed later by the size in the SVG tool, also without keeping the ratio."));
    row(_L("Width") + " (mm):", m_size);

    m_depth = spin_double(m_params.depth, 0.05, 100., 0.1);
    m_depth->SetToolTip(_L("Height above the surface, or depth of the cut for negative volume."));
    row(_L("Depth") + " (mm):", m_depth);

    m_star_points = spin_int(m_params.star_points, 3, 24);
    row(_L("Points") + ":", m_star_points, &m_star_controls);

    m_inner_ratio = spin_int(int(std::lround(m_params.inner_ratio * 100.)), 5, 95);
    m_inner_ratio->SetToolTip(_L("Star: size of the inner corners, ring: size of the hole, relative to the outer size."));
    row(_L("Inner size") + " (%):", m_inner_ratio, &m_inner_controls);

    m_corner_radius = spin_double(m_params.corner_radius, 0., 100., 0.5);
    row(_L("Corner radius") + " (mm):", m_corner_radius, &m_corner_controls);

    if (m_options.allow_filament) {
        m_filament  = new wxChoice(this, wxID_ANY);
        int count   = std::max<int>(1, int(m_filament_colors.size()));
        for (int i = 0; i <= count; ++i)
            m_filament->Append(filament_label(i));
        m_filament->SetSelection(std::clamp(m_options.extruder, 0, count));
        row(_L("Filament") + ":", m_filament);
    }

    m_use_surface = new wxCheckBox(this, wxID_ANY, _L("Project onto surface"));
    m_use_surface->SetValue(m_options.allow_use_surface && m_options.use_surface);
    m_use_surface->SetToolTip(_L("Wrap the shape onto the curved surface of the object instead of a flat plate."));
    grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
    grid->Add(m_use_surface, 0, wxALIGN_CENTER_VERTICAL);
    if (!m_options.allow_use_surface || m_options.is_edit)
        m_use_surface->Hide(); // on edit it is controled by the SVG gizmo

    cols->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));

    wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
    wxImage     blank(FromDIP(SHAPE_PREVIEW_PX), FromDIP(SHAPE_PREVIEW_PX));
    blank.SetRGB(wxRect(0, 0, blank.GetWidth(), blank.GetHeight()), 200, 200, 200);
    m_preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(blank));
    right->Add(m_preview, 0);
    m_info = new wxStaticText(this, wxID_ANY, wxEmptyString);
    right->Add(m_info, 0, wxTOP, FromDIP(6));
    cols->Add(right, 0, wxEXPAND | wxALL, FromDIP(10));
    root->Add(cols, 1, wxEXPAND);

    wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
    wxButton               *ok      = new wxButton(this, wxID_OK, m_options.is_edit ? _L("Apply") : _L("Add"));
    buttons->AddButton(ok);
    buttons->AddButton(new wxButton(this, wxID_CANCEL));
    buttons->Realize();
    root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));

    auto changed = [this](wxCommandEvent &evt) {
        update_enabled();
        refresh_preview();
        evt.Skip();
    };
    m_type->Bind(wxEVT_CHOICE, changed);
    if (m_filament != nullptr)
        m_filament->Bind(wxEVT_CHOICE, changed);
    for (wxSpinCtrlDouble *s : {m_size, m_depth, m_corner_radius})
        s->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &evt) {
            refresh_preview();
            evt.Skip();
        });
    for (wxSpinCtrl *s : {m_star_points, m_inner_ratio})
        s->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &evt) {
            refresh_preview();
            evt.Skip();
        });
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        m_params              = collect();
        m_options.use_surface = m_use_surface->IsShown() && m_use_surface->GetValue();
        if (m_filament != nullptr)
            m_options.extruder = std::max(0, m_filament->GetSelection());
        save_to_config();
        EndModal(wxID_OK);
    });

    update_enabled();
    refresh_preview();

    SetSizer(root);
    Layout();
    root->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

SimpleShapeParams SimpleShapeDialog::collect() const
{
    SimpleShapeParams p = m_params;
    p.type              = SHAPE_CHOICES[size_t(std::max(0, m_type->GetSelection()))];
    p.size              = m_size->GetValue();
    p.depth             = m_depth->GetValue();
    p.star_points       = m_star_points->GetValue();
    p.inner_ratio       = m_inner_ratio->GetValue() / 100.;
    p.corner_radius     = has_corners(p.type) ? m_corner_radius->GetValue() : 0.;
    return p;
}

void SimpleShapeDialog::update_enabled()
{
    SimpleShapeType type = SHAPE_CHOICES[size_t(std::max(0, m_type->GetSelection()))];
    for (wxWindow *w : m_star_controls)
        w->Show(type == SimpleShapeType::Star);
    for (wxWindow *w : m_inner_controls)
        w->Show(type == SimpleShapeType::Star || type == SimpleShapeType::Ring);
    for (wxWindow *w : m_corner_controls)
        w->Show(has_corners(type));
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void SimpleShapeDialog::refresh_preview()
{
    SimpleShapeParams p   = collect();
    std::string       svg = create_simple_shape_svg(p);

    // color of the selected filament
    std::string color = "#4A78B4";
    int         index = m_filament != nullptr ? m_filament->GetSelection() : 0;
    if (index > 0 && size_t(index) <= m_filament_colors.size() && !m_filament_colors[size_t(index - 1)].empty())
        color = m_filament_colors[size_t(index - 1)];
    if (size_t pos = svg.find("fill=\"#808080\""); pos != std::string::npos)
        svg.replace(pos, 14, "fill=\"" + color + "\"");

    int     px = FromDIP(SHAPE_PREVIEW_PX);
    wxImage image(px, px);
    image.SetRGB(wxRect(0, 0, px, px), 235, 235, 235);
    NSVGimage_ptr nsvg = Slic3r::nsvgParse(svg, "px");
    if (nsvg != nullptr && nsvg->width > 0.f && nsvg->height > 0.f) {
        int   margin = px / 10;
        float scale  = float(px - 2 * margin) / std::max(nsvg->width, nsvg->height);
        int   w      = std::max(1, int(nsvg->width * scale));
        int   h      = std::max(1, int(nsvg->height * scale));
        std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4, 0);
        if (NSVGrasterizer *rast = nsvgCreateRasterizer(); rast != nullptr) {
            nsvgRasterize(rast, nsvg.get(), 0, 0, scale, rgba.data(), w, h, w * 4);
            nsvgDeleteRasterizer(rast);
            int ox = (px - w) / 2, oy = (px - h) / 2;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const unsigned char *c = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
                    auto blend = [a = c[3]](unsigned char v) { return (unsigned char) ((v * a + 235 * (255 - a)) / 255); };
                    image.SetRGB(ox + x, oy + y, blend(c[0]), blend(c[1]), blend(c[2]));
                }
        }
    }
    m_preview->SetBitmap(wxBitmap(image));

    // real size of the shape is written in the svg header
    wxString info;
    size_t   wpos = svg.find("width=\""), hpos = svg.find("height=\"");
    if (wpos != std::string::npos && hpos != std::string::npos) {
        std::string width  = svg.substr(wpos + 7, svg.find('"', wpos + 7) - wpos - 7);
        std::string height = svg.substr(hpos + 8, svg.find('"', hpos + 8) - hpos - 8);
        info = wxString::Format(_L("Size: %s x %s"), from_u8(width), from_u8(height));
    }
    m_info->SetLabel(info);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void SimpleShapeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
