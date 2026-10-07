#include "ImageTraceDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "format.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CodeEmboss.hpp" // create_code_group_id
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/NSVGUtils.hpp"

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace {

const char *TRACE_CFG_SECTION = "image_trace";
const int   TRACE_PREVIEW_PX  = 240;
// Longest side of the image traced for the live preview
const int   TRACE_PREVIEW_SIDE = 480;
// Working resolutions offered to the user
const std::array<int, 5> TRACE_RESOLUTIONS = {256, 512, 1024, 1536, 2048};

wxString trace_filament_label(int index) { return index == 0 ? _L("Default") : wxString::Format(_L("Filament %d"), index); }

bool parse_hex_color(const std::string &hex, std::array<uint8_t, 3> &out)
{
    if (hex.size() < 7 || hex[0] != '#')
        return false;
    for (int c = 0; c < 3; ++c) {
        unsigned v = 0;
        if (std::sscanf(hex.c_str() + 1 + 2 * c, "%2x", &v) != 1)
            return false;
        out[size_t(c)] = uint8_t(v);
    }
    return true;
}

// Light / dark gray checkerboard shows transparency
unsigned char checker(int x, int y) { return ((x / 8 + y / 8) % 2) ? 205 : 235; }

wxImage blank_preview(int px)
{
    wxImage image(px, px);
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x) {
            unsigned char v = checker(x, y);
            image.SetRGB(x, y, v, v, v);
        }
    return image;
}

// RGBA buffer blended over the checkerboard into a square image
wxBitmap rgba_to_bitmap(const unsigned char *rgba, int w, int h, int px)
{
    wxImage image = blank_preview(px);
    int     ox = (px - w) / 2, oy = (px - h) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char *c = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            int                  X = ox + x, Y = oy + y;
            if (X < 0 || Y < 0 || X >= px || Y >= px)
                continue;
            unsigned char bg    = checker(X, Y);
            auto          blend = [a = c[3], bg](unsigned char v) { return (unsigned char) ((v * a + bg * (255 - a)) / 255); };
            image.SetRGB(X, Y, blend(c[0]), blend(c[1]), blend(c[2]));
        }
    return wxBitmap(image);
}

long long to_um(coord_t c) { return std::llround(double(c) * SCALING_FACTOR * 1000.); }

} // namespace

ImageTraceParams ImageTraceDialog::load_from_config()
{
    ImageTraceParams p;
    AppConfig       *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return p;
    auto read_double = [cfg](const char *key, double &out, double min, double max) {
        if (!cfg->has(TRACE_CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stod(cfg->get(TRACE_CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    auto read_int = [cfg](const char *key, int &out, int min, int max) {
        if (!cfg->has(TRACE_CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stoi(cfg->get(TRACE_CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    auto read_bool = [cfg](const char *key, bool &out) {
        if (cfg->has(TRACE_CFG_SECTION, key))
            out = cfg->get(TRACE_CFG_SECTION, key) == "1";
    };
    read_int("colors", p.colors, 1, IMAGE_TRACE_MAX_COLORS);
    read_double("blur", p.blur, 0., 5.);
    read_double("despeckle", p.despeckle, 0., 10.);
    read_double("detail", p.detail, 0.1, 3.);
    read_int("max_side", p.max_side, 256, 2048);
    read_double("width", p.width, 1., 1000.);
    read_double("depth", p.depth, 0.05, 50.);
    read_double("depth_step", p.depth_step, 0.05, 20.);
    read_bool("remove_background", p.remove_background);
    read_bool("keep_source", p.keep_source);
    int layout = int(p.layout);
    read_int("layout", layout, 0, 1);
    p.layout = TraceLayout(layout);
    return p;
}

void ImageTraceDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr || m_options.is_edit)
        return;
    const ImageTraceParams &p = m_params;
    cfg->set(TRACE_CFG_SECTION, "colors", std::to_string(p.colors));
    cfg->set(TRACE_CFG_SECTION, "blur", float_to_string_decimal_point(p.blur));
    cfg->set(TRACE_CFG_SECTION, "despeckle", float_to_string_decimal_point(p.despeckle));
    cfg->set(TRACE_CFG_SECTION, "detail", float_to_string_decimal_point(p.detail));
    cfg->set(TRACE_CFG_SECTION, "max_side", std::to_string(p.max_side));
    cfg->set(TRACE_CFG_SECTION, "width", float_to_string_decimal_point(p.width));
    cfg->set(TRACE_CFG_SECTION, "depth", float_to_string_decimal_point(p.depth));
    cfg->set(TRACE_CFG_SECTION, "depth_step", float_to_string_decimal_point(p.depth_step));
    cfg->set(TRACE_CFG_SECTION, "remove_background", p.remove_background ? "1" : "0");
    cfg->set(TRACE_CFG_SECTION, "keep_source", p.keep_source ? "1" : "0");
    cfg->set(TRACE_CFG_SECTION, "layout", std::to_string(int(p.layout)));
}

wxString ImageTraceDialog::choose_image_file(wxWindow *parent)
{
    wxFileDialog dialog(parent, _L("Choose an image to trace"), wxEmptyString, wxEmptyString,
                        _L("Images") + " (*.png;*.jpg;*.jpeg)|*.png;*.PNG;*.jpg;*.JPG;*.jpeg;*.JPEG",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return {};
    return dialog.GetPath();
}

bool ImageTraceDialog::load_image_file(wxWindow *parent, const wxString &path, TraceImage &image, std::string &source_png, std::string &name)
{
    bool                         too_large = false;
    std::unique_ptr<std::string> data      = read_from_disk(into_u8(path), IMAGE_TRACE_MAX_FILE_SIZE, &too_large);
    std::string                  error;
    if (data == nullptr)
        error = too_large ? _u8L("The image file is too large.") : _u8L("The image file could not be read.");
    else if (!decode_trace_image(*data, image, &error) && error.empty())
        error = _u8L("The image could not be decoded.");
    if (!error.empty()) {
        show_error(parent, GUI::format(_L("Image \"%1%\" can not be traced:\n%2%"), wxFileName(path).GetFullName(),
                                       from_u8(error)));
        return false;
    }
    {
        wxBusyCursor busy;
        source_png = encode_trace_source(image);
    }
    name = into_u8(wxFileName(path).GetName());
    return true;
}

ImageTraceDialog::ImageTraceDialog(wxWindow                      *parent,
                                   const TraceImage              &image,
                                   const std::string             &source_png,
                                   const std::string             &image_name,
                                   const ImageTraceParams        &params,
                                   const ImageTraceDialogOptions &options)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                options.is_edit ? _L("Edit image trace") : _L("Import image"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_image(image)
    , m_preview_image(downscale_trace_image(image, TRACE_PREVIEW_SIDE))
    , m_source_png(source_png)
    , m_image_name(image_name)
    , m_params(params)
    , m_options(options)
    , m_timer(this)
{
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        m_filament_colors = plater->get_extruder_colors_from_plater_config(nullptr, false);
    m_user_filament.assign(IMAGE_TRACE_MAX_COLORS, false);
    for (size_t i = 0; i < m_options.extruders.size() && i < m_user_filament.size(); ++i)
        m_user_filament[i] = m_options.is_edit;
    m_options.extruders.resize(IMAGE_TRACE_MAX_COLORS, 0);

    wxBoxSizer      *root = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer      *cols = new wxBoxSizer(wxHORIZONTAL);
    wxFlexGridSizer *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(1);

    auto label = [this](const wxString &text) { return new wxStaticText(this, wxID_ANY, text); };
    auto row   = [&grid, &label](const wxString &text, wxWindow *ctrl, std::vector<wxWindow *> *group = nullptr) {
        wxStaticText *l = label(text);
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        if (group != nullptr) {
            group->push_back(l);
            group->push_back(ctrl);
        }
    };
    auto check_row = [this, &grid](wxCheckBox *check, std::vector<wxWindow *> *group = nullptr) {
        wxStaticText *empty = new wxStaticText(this, wxID_ANY, wxEmptyString);
        grid->Add(empty);
        grid->Add(check, 0, wxALIGN_CENTER_VERTICAL);
        if (group != nullptr) {
            group->push_back(empty);
            group->push_back(check);
        }
    };
    auto spin_double = [this](double value, double min, double max, double inc, int digits = 2) {
        auto *s = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, min, max,
                                       value, inc);
        s->SetDigits(digits);
        return s;
    };

    // ---- image -------------------------------------------------------------------------------
    wxBoxSizer *image_row = new wxBoxSizer(wxHORIZONTAL);
    m_image_label         = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_change_image        = new wxButton(this, wxID_ANY, _L("Change image") + dots);
    image_row->Add(m_image_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    image_row->Add(m_change_image, 0);
    grid->Add(label(_L("Image") + ":"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(image_row, 1, wxEXPAND);

    // ---- colours -----------------------------------------------------------------------------
    m_colors = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, 1,
                              IMAGE_TRACE_MAX_COLORS, m_params.colors);
    m_colors->SetToolTip(_L("Number of colours (parts). Every colour of the image becomes its own part, "
                            "which can be printed with its own filament.\n"
                            "1 = one shape from the transparency or the brightness of the image."));
    row(_L("Colours") + ":", m_colors);

    wxArrayString froms;
    froms.Add(_L("Automatic"));
    froms.Add(_L("Transparency"));
    froms.Add(_L("Brightness"));
    m_shape_from = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, froms);
    m_shape_from->SetSelection(int(m_params.shape_from));
    m_shape_from->SetToolTip(_L("Automatic uses the transparency of an image with transparent background, "
                                "otherwise the brightness."));
    row(_L("Shape from") + ":", m_shape_from, &m_one_color);

    wxBoxSizer *threshold_row = new wxBoxSizer(wxHORIZONTAL);
    m_threshold = new wxSlider(this, wxID_ANY, m_params.threshold, 0, 255, wxDefaultPosition, wxSize(FromDIP(160), -1),
                               wxSL_HORIZONTAL | wxSL_LABELS);
    m_threshold->SetToolTip(_L("Pixels with brightness up to this value become the shape."));
    m_auto_threshold = new wxCheckBox(this, wxID_ANY, _L("Auto"));
    m_auto_threshold->SetValue(m_params.auto_threshold);
    m_auto_threshold->SetToolTip(_L("Find the threshold which separates the dark and the light part best (Otsu)."));
    threshold_row->Add(m_threshold, 1, wxALIGN_CENTER_VERTICAL);
    threshold_row->Add(m_auto_threshold, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    wxStaticText *threshold_label = label(_L("Threshold") + ":");
    grid->Add(threshold_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(threshold_row, 1, wxEXPAND);
    m_brightness.insert(m_brightness.end(), {threshold_label, m_threshold, m_auto_threshold});

    m_invert = new wxCheckBox(this, wxID_ANY, _L("Invert"));
    m_invert->SetValue(m_params.invert);
    check_row(m_invert);

    m_remove_bg = new wxCheckBox(this, wxID_ANY, _L("Leave out the background"));
    m_remove_bg->SetValue(m_params.remove_background);
    m_remove_bg->SetToolTip(_L("The colour which covers most of the image border is not traced (e.g. white paper behind a logo)."));
    check_row(m_remove_bg, &m_multi_color);

    // ---- cleanup -----------------------------------------------------------------------------
    m_blur = spin_double(m_params.blur, 0., 5., 0.25);
    m_blur->SetToolTip(_L("Blur the image before tracing [in pixels of the resolution below]. "
                          "Smooths jagged and noisy edges, removes thin details."));
    row(_L("Smoothing") + " (px):", m_blur);

    m_despeckle = spin_double(m_params.despeckle, 0., 10., 0.05);
    m_despeckle->SetToolTip(_L("Islands and holes smaller than this area are removed."));
    // The unit is spelled as UTF-8 bytes: a plain char* is converted with the system code page.
    row(_L("Remove specks") + wxString::FromUTF8(" (mm\xC2\xB2):"), m_despeckle);

    m_detail = spin_double(m_params.detail, 0.1, 3., 0.1);
    m_detail->SetToolTip(_L("Allowed deviation of the outline [in pixels]. Bigger value = fewer points, smaller file."));
    row(_L("Simplify") + " (px):", m_detail);

    wxArrayString resolutions;
    int           resolution_sel = 2;
    for (size_t i = 0; i < TRACE_RESOLUTIONS.size(); ++i) {
        resolutions.Add(wxString::Format("%d px", TRACE_RESOLUTIONS[i]));
        if (std::abs(TRACE_RESOLUTIONS[i] - m_params.max_side) < std::abs(TRACE_RESOLUTIONS[size_t(resolution_sel)] - m_params.max_side))
            resolution_sel = int(i);
    }
    m_resolution = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, resolutions);
    m_resolution->SetSelection(resolution_sel);
    m_resolution->SetToolTip(_L("The image is downscaled so its longer side has at most this many pixels before tracing."));
    row(_L("Resolution") + ":", m_resolution);

    // ---- size --------------------------------------------------------------------------------
    wxBoxSizer *size_row = new wxBoxSizer(wxHORIZONTAL);
    m_width              = spin_double(m_params.width, 1., 1000., 1., 1);
    m_height_label       = new wxStaticText(this, wxID_ANY, wxEmptyString);
    size_row->Add(m_width, 0, wxALIGN_CENTER_VERTICAL);
    size_row->Add(m_height_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    grid->Add(label(_L("Width") + " (mm):"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(size_row, 1, wxEXPAND);

    m_depth = spin_double(m_params.depth, 0.05, 50., 0.1);
    m_depth->SetToolTip(_L("Height of the shapes above the surface (depth for negative volume)."));
    row(_L("Depth") + " (mm):", m_depth);

    m_stacked = new wxCheckBox(this, wxID_ANY, _L("Stepped relief"));
    m_stacked->SetValue(m_params.layout == TraceLayout::Stacked);
    m_stacked->SetToolTip(_L("Colours are stacked like steps instead of side by side: every level covers the levels above it and "
                             "is higher by the step. The lightest colour is on the top."));
    check_row(m_stacked, &m_multi_color);
    m_depth_step = spin_double(m_params.depth_step, 0.05, 20., 0.1);
    row(_L("Step") + " (mm):", m_depth_step, &m_stacked_controls);

    m_use_surface = new wxCheckBox(this, wxID_ANY, _L("Project onto surface"));
    m_use_surface->SetValue(m_options.allow_use_surface && m_options.use_surface);
    m_use_surface->SetToolTip(_L("Wrap the shapes onto the curved surface of the object instead of a flat plate."));
    check_row(m_use_surface);
    if (!m_options.allow_use_surface || m_options.is_edit)
        m_use_surface->Hide(); // on edit it is controled by the SVG gizmo

    m_keep_source = new wxCheckBox(this, wxID_ANY, _L("Keep the image in the project"));
    m_keep_source->SetValue(m_params.keep_source);
    m_keep_source->SetToolTip(_L("Store a reduced copy of the image (at most 1024 px) inside of the project, "
                                 "so the trace can be changed later by \"Edit trace\". "
                                 "Untick it to store only the traced shapes. The file path is never stored."));
    check_row(m_keep_source);

    // ---- filaments of colour levels ----------------------------------------------------------
    for (int i = 0; i < IMAGE_TRACE_MAX_COLORS; ++i) {
        wxBoxSizer *level = new wxBoxSizer(wxHORIZONTAL);
        wxImage     swatch(FromDIP(22), FromDIP(14));
        swatch.SetRGB(wxRect(0, 0, swatch.GetWidth(), swatch.GetHeight()), 128, 128, 128);
        m_level_swatch[size_t(i)] = new wxStaticBitmap(this, wxID_ANY, wxBitmap(swatch));
        m_level_label[size_t(i)]  = new wxStaticText(this, wxID_ANY, wxString::Format(_L("Colour %d"), i + 1) + ":");
        level->Add(m_level_swatch[size_t(i)], 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        level->Add(m_level_label[size_t(i)], 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(level, 0, wxALIGN_CENTER_VERTICAL);
        if (m_options.allow_filaments) {
            auto *c     = new wxChoice(this, wxID_ANY);
            int   count = std::max<int>(1, int(m_filament_colors.size()));
            for (int f = 0; f <= count; ++f)
                c->Append(trace_filament_label(f));
            c->SetSelection(std::clamp(m_options.extruders[size_t(i)], 0, count));
            c->SetToolTip(_L("Filament of this colour. By default the loaded filament with the nearest colour."));
            m_level_filament[size_t(i)] = c;
            grid->Add(c, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        } else {
            grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
        }
    }

    cols->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));

    // ---- preview -----------------------------------------------------------------------------
    wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
    int         px    = FromDIP(TRACE_PREVIEW_PX);
    right->Add(label(_L("Image") + ":"), 0, wxBOTTOM, FromDIP(4));
    m_source_preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(blank_preview(px)));
    right->Add(m_source_preview, 0, wxBOTTOM, FromDIP(8));
    right->Add(label(_L("Shapes") + ":"), 0, wxBOTTOM, FromDIP(4));
    m_preview_bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(blank_preview(px)));
    right->Add(m_preview_bitmap, 0);
    m_info = new wxStaticText(this, wxID_ANY, wxEmptyString);
    right->Add(m_info, 0, wxTOP, FromDIP(6));
    m_warning = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_warning->SetForegroundColour(wxColour("#D9534F"));
    right->Add(m_warning, 0, wxTOP, FromDIP(6));
    cols->Add(right, 0, wxEXPAND | wxALL, FromDIP(10));
    root->Add(cols, 1, wxEXPAND);

    wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
    m_ok = new wxButton(this, wxID_OK, m_options.is_edit ? _L("Apply") : _L("Add"));
    buttons->AddButton(m_ok);
    buttons->AddButton(new wxButton(this, wxID_CANCEL));
    buttons->Realize();
    root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- events ------------------------------------------------------------------------------
    auto changed = [this](wxCommandEvent &evt) {
        update_enabled();
        schedule_preview();
        evt.Skip();
    };
    auto changed_double = [this](wxSpinDoubleEvent &evt) {
        schedule_preview();
        evt.Skip();
    };
    m_colors->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &evt) {
        update_enabled();
        schedule_preview();
        evt.Skip();
    });
    m_colors->Bind(wxEVT_TEXT, changed);
    for (wxChoice *c : {m_shape_from, m_resolution})
        c->Bind(wxEVT_CHOICE, changed);
    for (wxCheckBox *c : {m_auto_threshold, m_invert, m_remove_bg, m_stacked, m_keep_source})
        c->Bind(wxEVT_CHECKBOX, changed);
    for (wxSpinCtrlDouble *s : {m_blur, m_despeckle, m_detail, m_depth, m_depth_step})
        s->Bind(wxEVT_SPINCTRLDOUBLE, changed_double);
    m_width->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &evt) {
        update_enabled(); // height label
        schedule_preview();
        evt.Skip();
    });
    m_threshold->Bind(wxEVT_SLIDER, [this](wxCommandEvent &evt) {
        // moving the slider means a manual threshold
        m_auto_threshold->SetValue(false);
        update_enabled();
        schedule_preview();
        evt.Skip();
    });
    for (int i = 0; i < IMAGE_TRACE_MAX_COLORS; ++i)
        if (wxChoice *c = m_level_filament[size_t(i)]; c != nullptr)
            c->Bind(wxEVT_CHOICE, [this, i](wxCommandEvent &evt) {
                m_user_filament[size_t(i)] = true;
                schedule_preview();
                evt.Skip();
            });
    m_change_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        change_image();
        evt.Skip();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_preview(); });
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (finish())
            EndModal(wxID_OK);
    });

    refresh_source_preview();
    update_enabled();
    refresh_preview();

    SetSizer(root);
    Layout();
    root->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

ImageTraceDialog::~ImageTraceDialog() { m_timer.Stop(); }

ImageTraceParams ImageTraceDialog::collect() const
{
    ImageTraceParams p  = m_params;
    p.colors            = std::clamp(m_colors->GetValue(), 1, IMAGE_TRACE_MAX_COLORS);
    p.shape_from        = TraceShapeFrom(std::max(0, m_shape_from->GetSelection()));
    p.threshold         = m_threshold->GetValue();
    p.auto_threshold    = m_auto_threshold->GetValue();
    p.invert            = m_invert->GetValue();
    p.remove_background = m_remove_bg->GetValue();
    p.blur              = m_blur->GetValue();
    p.despeckle         = m_despeckle->GetValue();
    p.detail            = m_detail->GetValue();
    p.max_side          = TRACE_RESOLUTIONS[size_t(std::clamp(m_resolution->GetSelection(), 0, int(TRACE_RESOLUTIONS.size()) - 1))];
    p.width             = m_width->GetValue();
    p.depth             = m_depth->GetValue();
    p.layout            = m_stacked->GetValue() ? TraceLayout::Stacked : TraceLayout::Flat;
    p.depth_step        = m_depth_step->GetValue();
    p.keep_source       = m_keep_source->GetValue() && !m_source_png.empty();
    return p;
}

void ImageTraceDialog::update_enabled()
{
    int  colors     = m_colors->GetValue();
    bool one        = colors <= 1;
    bool has_alpha  = m_image.has_alpha();
    int  from       = m_shape_from->GetSelection();
    bool brightness = one && (from == int(TraceShapeFrom::Brightness) || (from == int(TraceShapeFrom::Auto) && !has_alpha));
    bool stacked    = !one && m_stacked->GetValue();
    for (wxWindow *w : m_one_color)
        w->Show(one);
    for (wxWindow *w : m_brightness)
        w->Show(brightness);
    for (wxWindow *w : m_multi_color)
        w->Show(!one);
    for (wxWindow *w : m_stacked_controls)
        w->Show(stacked);
    // background of image with transparency is the transparent part
    m_remove_bg->Enable(!has_alpha);
    m_invert->Show(one || stacked);
    m_invert->SetLabel(one ? _L("Invert (trace the light part)") : _L("Darkest colour on top"));
    m_keep_source->Enable(!m_source_png.empty());

    double height = m_image.width > 0 ? m_width->GetValue() * m_image.height / m_image.width : 0.;
    m_height_label->SetLabel(wxString::Format(_L("Height %.1f mm"), height));

    update_filament_rows();
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void ImageTraceDialog::update_filament_rows()
{
    size_t count = m_preview.layers.size();
    for (size_t i = 0; i < size_t(IMAGE_TRACE_MAX_COLORS); ++i) {
        bool show = i < count;
        m_level_swatch[i]->Show(show);
        m_level_label[i]->Show(show);
        if (m_level_filament[i] != nullptr)
            m_level_filament[i]->Show(show);
        if (!show)
            continue;
        const std::array<uint8_t, 3> &c = m_preview.layers[i].color;
        wxImage swatch(FromDIP(22), FromDIP(14));
        swatch.SetRGB(wxRect(0, 0, swatch.GetWidth(), swatch.GetHeight()), c[0], c[1], c[2]);
        m_level_swatch[i]->SetBitmap(wxBitmap(swatch));
        if (m_level_filament[i] != nullptr && !m_user_filament[i])
            m_level_filament[i]->SetSelection(nearest_filament(c));
    }
}

int ImageTraceDialog::nearest_filament(const std::array<uint8_t, 3> &color) const
{
    int    best   = 0;
    double best_d = std::numeric_limits<double>::max();
    for (size_t i = 0; i < m_filament_colors.size(); ++i) {
        std::array<uint8_t, 3> f;
        if (!parse_hex_color(m_filament_colors[i], f))
            continue;
        double d = 0.;
        for (int c = 0; c < 3; ++c)
            d += (double(f[c]) - color[c]) * (double(f[c]) - color[c]);
        if (d < best_d) {
            best_d = d;
            best   = int(i) + 1;
        }
    }
    return best;
}

std::string ImageTraceDialog::level_color(size_t level) const
{
    if (level < m_level_filament.size() && m_level_filament[level] != nullptr) {
        int index = m_level_filament[level]->GetSelection();
        if (index > 0 && size_t(index) <= m_filament_colors.size() && !m_filament_colors[size_t(index - 1)].empty())
            return m_filament_colors[size_t(index - 1)];
    }
    if (level < m_preview.layers.size())
        return image_trace_color_to_hex(m_preview.layers[level].color);
    return "#000000";
}

void ImageTraceDialog::schedule_preview()
{
    // debounce: spin controls and the slider send many events
    m_timer.StartOnce(150);
}

void ImageTraceDialog::refresh_source_preview()
{
    m_image_label->SetLabel(wxString::Format("%s  (%d x %d px)", from_u8(m_image_name.empty() ? _u8L("Image") : m_image_name), m_image.width,
                                             m_image.height));
    int        px    = FromDIP(TRACE_PREVIEW_PX);
    TraceImage thumb = downscale_trace_image(m_preview_image, px);
    if (thumb.empty())
        m_source_preview->SetBitmap(wxBitmap(blank_preview(px)));
    else
        m_source_preview->SetBitmap(rgba_to_bitmap(thumb.rgba.data(), thumb.width, thumb.height, px));
}

void ImageTraceDialog::refresh_preview()
{
    m_timer.Stop();
    ImageTraceParams p = collect();
    p.max_side         = std::min(p.max_side, TRACE_PREVIEW_SIDE);
    m_preview          = trace_image(m_preview_image, p, {}, false);

    int px = FromDIP(TRACE_PREVIEW_PX);
    if (m_preview.is_valid()) {
        if (p.auto_threshold && m_preview.threshold >= 0)
            m_threshold->SetValue(m_preview.threshold);
        update_filament_rows();
        m_preview_bitmap->SetBitmap(render_preview(m_preview, px));
        wxString info = wxString::Format(_L("Size: %.1f x %.1f mm"), m_preview.width, m_preview.height);
        // pixel size of the final trace
        int    longest    = std::max(1, std::max(m_image.width, m_image.height));
        double final_long = std::min(longest, collect().max_side);
        double mm_per_px  = m_preview.width / std::max(1., m_image.width * final_long / longest);
        info += "\n" + wxString::Format(_L("1 pixel = %.3f mm"), mm_per_px);
        if (collect().colors > 1)
            info += "\n" + wxString::Format(_L("Colours found: %d"), int(m_preview.layers.size()));
        info += "\n" + wxString::Format(_L("Preview: %d outlines, %d points"), int(m_preview.contours), int(m_preview.points));
        m_info->SetLabel(info);
        wxString warnings;
        for (const std::string &w : m_preview.warnings)
            warnings += (warnings.empty() ? "" : "\n") + from_u8(w);
        if (mm_per_px * 2. > 0.4 && m_preview.layers.size() > 0)
            warnings += (warnings.empty() ? "" : "\n") + _L("Pixels are large compared to a nozzle, raise the resolution for finer details.");
        m_warning->SetLabel(warnings);
        m_ok->Enable(true);
    } else {
        update_filament_rows();
        m_preview_bitmap->SetBitmap(wxBitmap(blank_preview(px)));
        m_info->SetLabel(wxEmptyString);
        m_warning->SetLabel(from_u8(m_preview.error));
        m_ok->Enable(false);
    }
    m_info->Wrap(px);
    m_warning->Wrap(px);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

wxBitmap ImageTraceDialog::render_preview(const ImageTraceResult &result, int max_px) const
{
    long long         w = std::max(1ll, std::llround(result.width * 1000.));
    long long         h = std::max(1ll, std::llround(result.height * 1000.));
    std::stringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << " " << h << "\">";
    for (size_t i = 0; i < result.layers.size(); ++i) {
        svg << "<path fill=\"" << level_color(i) << "\" fill-rule=\"evenodd\" d=\"";
        auto write_polygon = [&svg](const Slic3r::Polygon &polygon) {
            for (size_t k = 0; k < polygon.points.size(); ++k)
                svg << (k == 0 ? "M" : (k == 1 ? "L" : " ")) << to_um(polygon.points[k].x()) << " " << to_um(polygon.points[k].y());
            svg << "Z";
        };
        for (const ExPolygon &e : result.layers[i].shape) {
            write_polygon(e.contour);
            for (const Slic3r::Polygon &hole : e.holes)
                write_polygon(hole);
        }
        svg << "\"/>";
    }
    svg << "</svg>";

    NSVGimage_ptr nsvg = Slic3r::nsvgParse(svg.str(), "px");
    if (nsvg == nullptr || nsvg->width <= 0.f || nsvg->height <= 0.f)
        return wxBitmap(blank_preview(max_px));
    float scale = float(max_px) / std::max(nsvg->width, nsvg->height);
    int   iw    = std::max(1, int(nsvg->width * scale));
    int   ih    = std::max(1, int(nsvg->height * scale));
    std::vector<unsigned char> rgba(size_t(iw) * size_t(ih) * 4, 0);
    NSVGrasterizer            *rast = nsvgCreateRasterizer();
    if (rast == nullptr)
        return wxBitmap(blank_preview(max_px));
    nsvgRasterize(rast, nsvg.get(), 0, 0, scale, rgba.data(), iw, ih, iw * 4);
    nsvgDeleteRasterizer(rast);
    return rgba_to_bitmap(rgba.data(), iw, ih, max_px);
}

void ImageTraceDialog::change_image()
{
    wxString path = choose_image_file(this);
    if (path.empty())
        return;
    TraceImage  image;
    std::string source_png, name;
    if (!load_image_file(this, path, image, source_png, name))
        return;
    m_image         = std::move(image);
    m_preview_image = downscale_trace_image(m_image, TRACE_PREVIEW_SIDE);
    m_source_png    = std::move(source_png);
    m_image_name    = name;
    if (!m_source_png.empty() && !m_options.is_edit)
        m_keep_source->SetValue(m_params.keep_source);
    refresh_source_preview();
    update_enabled();
    refresh_preview();
}

bool ImageTraceDialog::finish()
{
    m_timer.Stop();
    m_params = collect();
    if (m_params.group_id.empty())
        m_params.group_id = create_code_group_id();
    {
        wxBusyCursor busy;
        m_result = trace_image(m_image, m_params, m_params.keep_source ? m_source_png : std::string(), true);
    }
    if (!m_result.is_valid()) {
        m_warning->SetLabel(from_u8(m_result.error));
        m_warning->Wrap(FromDIP(TRACE_PREVIEW_PX));
        Layout();
        if (GetSizer() != nullptr)
            GetSizer()->Fit(this);
        return false;
    }
    m_options.use_surface = m_use_surface->IsShown() && m_use_surface->GetValue();
    std::vector<int> extruders;
    for (size_t i = 0; i < m_result.layers.size(); ++i) {
        int extruder = 0;
        if (i < m_level_filament.size() && m_level_filament[i] != nullptr && i < m_preview.layers.size())
            extruder = std::max(0, m_level_filament[i]->GetSelection());
        else if (m_options.allow_filaments)
            extruder = nearest_filament(m_result.layers[i].color);
        extruders.push_back(extruder);
    }
    m_options.extruders = std::move(extruders);
    save_to_config();
    return true;
}

void ImageTraceDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

ImageDropAction ask_image_drop_action(wxWindow *parent, const wxString &file_name, bool can_color_fill)
{
    wxDialog dialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Image dropped"), wxDefaultPosition,
                    wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    wxBoxSizer *root = new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(&dialog, wxID_ANY, GUI::format_wxstr(_L("What should be done with \"%1%\"?"), file_name)), 0,
              wxALL, dialog.FromDIP(12));

    struct Choice
    {
        ImageDropAction action;
        wxString        label;
        wxString        tooltip;
        bool            enabled;
    };
    const Choice choices[] = {
        {ImageDropAction::Trace, _L("Trace to shapes") + dots,
         _L("Trace the image into shapes (one part per colour) which are embossed like an SVG."), true},
        {ImageDropAction::Relief, _L("Relief (coming soon)"), _L("Height map relief of the image is not available yet."), false},
        {ImageDropAction::ColorFill, _L("Colour fill (Image Fill)") + dots,
         can_color_fill ? _L("Paint the image onto the object with the loaded filaments.") :
                          _L("Drop the image onto an object (or select one) to paint it with the image."),
         can_color_fill},
    };
    ImageDropAction result = ImageDropAction::Cancel;
    for (const Choice &c : choices) {
        wxButton *button = new wxButton(&dialog, wxID_ANY, c.label);
        button->SetToolTip(c.tooltip);
        button->Enable(c.enabled);
        button->Bind(wxEVT_BUTTON, [&dialog, &result, action = c.action](wxCommandEvent &) {
            result = action;
            dialog.EndModal(wxID_OK);
        });
        root->Add(button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
    buttons->AddButton(new wxButton(&dialog, wxID_CANCEL));
    buttons->Realize();
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    dialog.SetSizerAndFit(root);
    dialog.CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(&dialog);
    if (dialog.ShowModal() != wxID_OK)
        return ImageDropAction::Cancel;
    return result;
}

}} // namespace Slic3r::GUI
