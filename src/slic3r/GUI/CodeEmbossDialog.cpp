#include "CodeEmbossDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "format.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/NSVGUtils.hpp"

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace {

const char *CFG_SECTION = "code_emboss";
const int   PREVIEW_PX  = 220;

// Order of items in the symbology choice
const std::array<Barcode::Symbology, 5> SYMBOLOGIES = {Barcode::Symbology::QR, Barcode::Symbology::Code128, Barcode::Symbology::EAN13,
                                                       Barcode::Symbology::UPCA, Barcode::Symbology::Code39};

int symbology_index(Barcode::Symbology s)
{
    auto it = std::find(SYMBOLOGIES.begin(), SYMBOLOGIES.end(), s);
    return it == SYMBOLOGIES.end() ? 0 : int(it - SYMBOLOGIES.begin());
}

// Value of "d" attribute of the only path in SVG of code part
std::string path_data(const std::string &svg)
{
    const std::string key   = " d=\"";
    size_t            begin = svg.find(key);
    if (begin == std::string::npos)
        return {};
    begin += key.size();
    size_t end = svg.find('"', begin);
    return end == std::string::npos ? std::string() : svg.substr(begin, end - begin);
}

std::string view_box(const std::string &svg)
{
    const std::string key   = "<svg ";
    size_t            begin = svg.find(key);
    if (begin == std::string::npos)
        return {};
    size_t end = svg.find('>', begin);
    return end == std::string::npos ? std::string() : svg.substr(begin, end - begin + 1);
}

wxString filament_label(int index) { return index == 0 ? _L("Default") : wxString::Format(_L("Filament %d"), index); }

} // namespace

CodeEmbossParams CodeEmbossDialog::load_from_config()
{
    CodeEmbossParams p;
    // Defaults which are good for two color flush code on top surface
    p.module_size = 1.;
    p.dark_depth  = 0.6;
    p.light_depth = 0.6;
    p.logo_depth  = 0.6;
    p.text        = "https://";

    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return p;
    auto read_double = [cfg](const char *key, double &out, double min, double max) {
        if (!cfg->has(CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stod(cfg->get(CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    auto read_int = [cfg](const char *key, int &out, int min, int max) {
        if (!cfg->has(CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stoi(cfg->get(CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    int symbology = int(p.symbology), ecc = int(p.ecc), clear = int(p.logo_clear);
    read_int("symbology", symbology, 0, int(SYMBOLOGIES.size()) - 1);
    read_int("ecc", ecc, 0, 3);
    read_int("logo_clear", clear, 0, 2);
    p.symbology  = Barcode::Symbology(symbology);
    p.ecc        = Barcode::QrEcc(ecc);
    p.logo_clear = CodeLogoClear(clear);
    read_double("module_size", p.module_size, 0.1, 20.);
    read_double("bar_height", p.bar_height, 1., 200.);
    read_int("quiet_zone", p.quiet_zone, 0, 20);
    read_double("dark_depth", p.dark_depth, 0.05, 50.);
    read_double("light_depth", p.light_depth, 0.05, 50.);
    read_double("logo_depth", p.logo_depth, 0.05, 50.);
    read_int("logo_margin", p.logo_margin, 0, 5);
    read_double("logo_size", p.logo_size, 0.05, 0.4);
    if (cfg->has(CFG_SECTION, "light_part"))
        p.light_part = cfg->get(CFG_SECTION, "light_part") == "1";
    return p;
}

void CodeEmbossDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    const CodeEmbossParams &p = m_params;
    cfg->set(CFG_SECTION, "symbology", std::to_string(symbology_index(p.symbology)));
    cfg->set(CFG_SECTION, "ecc", std::to_string(int(p.ecc)));
    cfg->set(CFG_SECTION, "logo_clear", std::to_string(int(p.logo_clear)));
    cfg->set(CFG_SECTION, "module_size", float_to_string_decimal_point(p.module_size));
    cfg->set(CFG_SECTION, "bar_height", float_to_string_decimal_point(p.bar_height));
    cfg->set(CFG_SECTION, "quiet_zone", std::to_string(p.quiet_zone));
    cfg->set(CFG_SECTION, "dark_depth", float_to_string_decimal_point(p.dark_depth));
    cfg->set(CFG_SECTION, "light_depth", float_to_string_decimal_point(p.light_depth));
    cfg->set(CFG_SECTION, "logo_depth", float_to_string_decimal_point(p.logo_depth));
    cfg->set(CFG_SECTION, "logo_margin", std::to_string(p.logo_margin));
    cfg->set(CFG_SECTION, "logo_size", float_to_string_decimal_point(p.logo_size));
    if (m_options.allow_light_part)
        cfg->set(CFG_SECTION, "light_part", p.light_part ? "1" : "0");
}

CodeEmbossDialog::CodeEmbossDialog(wxWindow                      *parent,
                                   const CodeEmbossParams        &params,
                                   const CodeEmbossDialogOptions &options,
                                   const ExPolygons              &logo)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                options.is_edit ? _L("Edit QR code / barcode") : _L("Add QR code / barcode"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_params(params)
    , m_options(options)
    , m_logo(logo)
{
    if (!m_logo.empty())
        m_logo_name = _u8L("Current logo");
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        m_filament_colors = plater->get_extruder_colors_from_plater_config(nullptr, false);

    wxBoxSizer       *root = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer       *cols = new wxBoxSizer(wxHORIZONTAL);
    wxFlexGridSizer  *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
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
    auto spin_double = [this](double value, double min, double max, double inc) {
        auto *s = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, min, max,
                                       value, inc);
        s->SetDigits(2);
        return s;
    };
    auto spin_int = [this](int value, int min, int max) {
        return new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, min, max, value);
    };
    auto filament_choice = [this](int selected) {
        auto *c = new wxChoice(this, wxID_ANY);
        int   count = std::max<int>(1, int(m_filament_colors.size()));
        for (int i = 0; i <= count; ++i)
            c->Append(filament_label(i));
        c->SetSelection(std::clamp(selected, 0, count));
        return c;
    };

    // ---- code --------------------------------------------------------------------------------
    wxArrayString symbologies;
    symbologies.Add(_L("QR code"));
    symbologies.Add("Code 128");
    symbologies.Add("EAN-13");
    symbologies.Add("UPC-A");
    symbologies.Add("Code 39");
    m_symbology = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, symbologies);
    m_symbology->SetSelection(symbology_index(m_params.symbology));
    row(_L("Type") + ":", m_symbology);

    m_text = new wxTextCtrl(this, wxID_ANY, from_u8(m_params.text), wxDefaultPosition, wxSize(FromDIP(260), -1));
    m_text->SetToolTip(_L("Text, link or numbers encoded in the code.\n"
                          "QR code accepts any text (UTF-8), Code 128 printable ASCII, "
                          "EAN-13 12 digits (+ check digit), UPC-A 11 digits (+ check digit) and "
                          "Code 39 digits, capital letters, space and - . $ / + %."));
    row(_L("Content") + ":", m_text);

    wxArrayString eccs;
    eccs.Add(_L("Low (7%)"));
    eccs.Add(_L("Medium (15%)"));
    eccs.Add(_L("Quartile (25%)"));
    eccs.Add(_L("High (30%)"));
    m_ecc = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, eccs);
    m_ecc->SetSelection(int(m_params.ecc));
    m_ecc->SetToolTip(_L("How much of a damaged QR code can be restored. Higher level makes the code bigger, "
                         "but it tolerates print defects and it is necessary for a logo in the middle."));
    row(_L("Error correction") + ":", m_ecc, &m_qr_only);

    m_module_size = spin_double(m_params.module_size, 0.1, 20., 0.1);
    m_module_size->SetToolTip(_L("Width of one square of QR code or of the narrowest bar. "
                                 "Keep it at least 2-3 times the nozzle diameter to be readable."));
    row(_L("Module size") + " (mm):", m_module_size);

    m_bar_height = spin_double(m_params.bar_height, 1., 200., 1.);
    row(_L("Bar height") + " (mm):", m_bar_height, &m_linear_only);

    m_quiet_zone = spin_int(m_params.quiet_zone, 0, 20);
    m_quiet_zone->SetToolTip(_L("Light border around the code in modules. Scanners need it to find the code, "
                                "4 modules for QR code and 10 for barcodes are recommended."));
    row(_L("Quiet zone") + ":", m_quiet_zone);

    // ---- parts -------------------------------------------------------------------------------
    m_dark_depth = spin_double(m_params.dark_depth, 0.05, 50., 0.1);
    m_dark_depth->SetToolTip(_L("Height of the dark modules above the surface (depth for negative volume)."));
    row(_L("Dark depth") + " (mm):", m_dark_depth);
    if (m_options.allow_filaments) {
        m_dark_filament = filament_choice(m_options.extruders[size_t(CodePartRole::Dark)]);
        row(_L("Dark filament") + ":", m_dark_filament);
    }

    m_light_part = new wxCheckBox(this, wxID_ANY, _L("Create light part"));
    m_light_part->SetValue(m_params.light_part && m_options.allow_light_part);
    m_light_part->SetToolTip(_L("Separate part for the light modules and the quiet zone.\n"
                                "Same depth as dark part and different filament = flush multi-color code.\n"
                                "Smaller depth than dark part = relief code readable by the shadows, "
                                "e.g. with a single filament."));
    grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
    grid->Add(m_light_part, 0, wxALIGN_CENTER_VERTICAL);
    m_light_depth = spin_double(m_params.light_depth, 0.05, 50., 0.1);
    row(_L("Light depth") + " (mm):", m_light_depth, &m_light_controls);
    if (m_options.allow_filaments) {
        m_light_filament = filament_choice(m_options.extruders[size_t(CodePartRole::Light)]);
        row(_L("Light filament") + ":", m_light_filament, &m_light_controls);
    }
    if (!m_options.allow_light_part)
        m_light_part->Hide();

    m_use_surface = new wxCheckBox(this, wxID_ANY, _L("Project onto surface"));
    m_use_surface->SetValue(m_options.allow_use_surface && m_options.use_surface);
    m_use_surface->SetToolTip(_L("Wrap the code onto the curved surface of the object instead of a flat plate."));
    grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
    grid->Add(m_use_surface, 0, wxALIGN_CENTER_VERTICAL);
    if (!m_options.allow_use_surface || m_options.is_edit)
        m_use_surface->Hide(); // on edit it is controled by the SVG gizmo

    // ---- logo --------------------------------------------------------------------------------
    m_has_logo = new wxCheckBox(this, wxID_ANY, _L("Logo in the middle"));
    m_has_logo->SetValue(m_params.has_logo);
    m_has_logo->SetToolTip(_L("Put a SVG logo into the middle of QR code. Modules under the logo are removed, "
                              "error correction restores them, so use High error correction."));
    grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
    grid->Add(m_has_logo, 0, wxALIGN_CENTER_VERTICAL);
    m_qr_only.push_back(m_has_logo);

    wxBoxSizer *logo_file = new wxBoxSizer(wxHORIZONTAL);
    m_logo_button         = new wxButton(this, wxID_ANY, _L("Choose SVG..."));
    m_logo_label          = new wxStaticText(this, wxID_ANY, m_logo_name.empty() ? _L("(none)") : from_u8(m_logo_name));
    logo_file->Add(m_logo_button, 0, wxRIGHT, FromDIP(8));
    logo_file->Add(m_logo_label, 1, wxALIGN_CENTER_VERTICAL);
    wxStaticText *logo_file_label = label(_L("Logo file") + ":");
    grid->Add(logo_file_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(logo_file, 1, wxEXPAND);
    m_logo_controls.insert(m_logo_controls.end(), {logo_file_label, m_logo_button, m_logo_label});

    m_logo_size = spin_int(int(std::lround(m_params.logo_size * 100.)), 5, 40);
    m_logo_size->SetToolTip(_L("Size of the logo relative to the size of QR code (without quiet zone)."));
    row(_L("Logo size") + " (%):", m_logo_size, &m_logo_controls);

    m_logo_margin = spin_int(m_params.logo_margin, 0, 5);
    m_logo_margin->SetToolTip(_L("Free space around the logo in modules."));
    row(_L("Logo margin") + ":", m_logo_margin, &m_logo_controls);

    wxArrayString clears;
    clears.Add(_L("Follow the outline"));
    clears.Add(_L("Square"));
    clears.Add(_L("Circle"));
    m_logo_clear = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, clears);
    m_logo_clear->SetSelection(int(m_params.logo_clear));
    row(_L("Clear area") + ":", m_logo_clear, &m_logo_controls);

    m_logo_depth = spin_double(m_params.logo_depth, 0.05, 50., 0.1);
    row(_L("Logo depth") + " (mm):", m_logo_depth, &m_logo_controls);
    if (m_options.allow_filaments) {
        m_logo_filament = filament_choice(m_options.extruders[size_t(CodePartRole::Logo)]);
        row(_L("Logo filament") + ":", m_logo_filament, &m_logo_controls);
    }

    cols->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));

    // ---- preview -----------------------------------------------------------------------------
    wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
    right->Add(label(_L("Preview") + ":"), 0, wxBOTTOM, FromDIP(4));
    wxImage blank(FromDIP(PREVIEW_PX), FromDIP(PREVIEW_PX));
    blank.SetRGB(wxRect(0, 0, blank.GetWidth(), blank.GetHeight()), 200, 200, 200);
    m_preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(blank));
    right->Add(m_preview, 0);
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
        refresh_preview();
        evt.Skip();
    };
    auto changed_double = [this](wxSpinDoubleEvent &evt) {
        refresh_preview();
        evt.Skip();
    };
    auto changed_int = [this](wxSpinEvent &evt) {
        refresh_preview();
        evt.Skip();
    };
    m_symbology->Bind(wxEVT_CHOICE, changed);
    m_text->Bind(wxEVT_TEXT, changed);
    m_ecc->Bind(wxEVT_CHOICE, changed);
    m_logo_clear->Bind(wxEVT_CHOICE, changed);
    for (wxChoice *c : {m_dark_filament, m_light_filament, m_logo_filament})
        if (c != nullptr)
            c->Bind(wxEVT_CHOICE, changed);
    for (wxCheckBox *c : {m_light_part, m_use_surface})
        c->Bind(wxEVT_CHECKBOX, changed);
    for (wxSpinCtrlDouble *s : {m_module_size, m_bar_height, m_dark_depth, m_light_depth, m_logo_depth})
        s->Bind(wxEVT_SPINCTRLDOUBLE, changed_double);
    for (wxSpinCtrl *s : {m_quiet_zone, m_logo_size, m_logo_margin})
        s->Bind(wxEVT_SPINCTRL, changed_int);
    m_logo_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        choose_logo();
        evt.Skip();
    });
    m_symbology->Bind(wxEVT_CHOICE, [this](wxCommandEvent &evt) {
        // Recommended quiet zone differs between QR and barcodes
        Barcode::Symbology s = SYMBOLOGIES[size_t(std::max(0, m_symbology->GetSelection()))];
        m_quiet_zone->SetValue(Barcode::recommended_quiet_zone(s));
        refresh_preview();
        evt.Skip();
    });
    m_has_logo->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &evt) {
        // Logo needs as much error correction as possible
        if (m_has_logo->GetValue()) {
            m_ecc->SetSelection(int(Barcode::QrEcc::High));
            if (m_logo.empty())
                choose_logo();
        }
        update_enabled();
        refresh_preview();
        evt.Skip();
    });
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        m_params = collect();
        if (!m_is_valid)
            return;
        m_options.use_surface = m_use_surface->IsShown() && m_use_surface->GetValue();
        auto filament         = [](wxChoice *c, int def) { return c != nullptr ? std::max(0, c->GetSelection()) : def; };
        m_options.extruders   = {filament(m_dark_filament, m_options.extruders[0]), filament(m_light_filament, m_options.extruders[1]),
                                 filament(m_logo_filament, m_options.extruders[2])};
        save_to_config();
        EndModal(wxID_OK);
    });

    update_enabled();
    refresh_preview();

    SetSizer(root);
    Layout();
    root->Fit(this);
    CenterOnParent();
    m_text->SetFocus();
    m_text->SetInsertionPointEnd();
    wxGetApp().UpdateDlgDarkUI(this);
}

CodeEmbossParams CodeEmbossDialog::collect() const
{
    CodeEmbossParams p = m_params;
    p.symbology        = SYMBOLOGIES[size_t(std::max(0, m_symbology->GetSelection()))];
    p.text             = into_u8(m_text->GetValue());
    p.ecc              = Barcode::QrEcc(std::max(0, m_ecc->GetSelection()));
    p.module_size      = m_module_size->GetValue();
    p.bar_height       = m_bar_height->GetValue();
    p.quiet_zone       = m_quiet_zone->GetValue();
    p.dark_depth       = m_dark_depth->GetValue();
    p.light_part       = m_options.allow_light_part && m_light_part->GetValue();
    p.light_depth      = m_light_depth->GetValue();
    p.has_logo         = p.symbology == Barcode::Symbology::QR && m_has_logo->GetValue() && !m_logo.empty();
    p.logo_size        = m_logo_size->GetValue() / 100.;
    p.logo_margin      = m_logo_margin->GetValue();
    p.logo_clear       = CodeLogoClear(std::max(0, m_logo_clear->GetSelection()));
    p.logo_depth       = m_logo_depth->GetValue();
    return p;
}

void CodeEmbossDialog::update_enabled()
{
    bool is_qr = SYMBOLOGIES[size_t(std::max(0, m_symbology->GetSelection()))] == Barcode::Symbology::QR;
    for (wxWindow *w : m_qr_only)
        w->Show(is_qr);
    for (wxWindow *w : m_linear_only)
        w->Show(!is_qr);
    bool logo = is_qr && m_has_logo->GetValue();
    for (wxWindow *w : m_logo_controls)
        w->Show(logo);
    bool light = m_options.allow_light_part && m_light_part->GetValue();
    for (wxWindow *w : m_light_controls)
        w->Show(m_options.allow_light_part);
    for (wxWindow *w : m_light_controls)
        w->Enable(light);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void CodeEmbossDialog::choose_logo()
{
    wxFileDialog dialog(this, _L("Choose SVG logo:"), wxEmptyString, wxEmptyString, file_wildcards(FT_SVG), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    std::string path = into_u8(dialog.GetPath());
    std::unique_ptr<std::string> data = read_from_disk(path);
    ExPolygons logo = data != nullptr ? load_code_logo(*data) : ExPolygons{};
    if (logo.empty()) {
        show_error(this, GUI::format(_L("SVG file does NOT contain a single path to be embossed (%1%)."), path));
        return;
    }
    m_logo      = std::move(logo);
    m_logo_name = into_u8(dialog.GetFilename());
    m_logo_label->SetLabel(from_u8(m_logo_name));
    m_has_logo->SetValue(true);
    update_enabled();
    refresh_preview();
}

void CodeEmbossDialog::refresh_preview()
{
    CodeEmbossParams p = collect();
    if (p.group_id.empty())
        p.group_id = "preview";
    CodeEmbossResult result = create_code_emboss(p, m_logo.empty() ? nullptr : &m_logo);
    m_is_valid              = result.is_valid();
    m_ok->Enable(m_is_valid);

    int px = FromDIP(PREVIEW_PX);
    if (m_is_valid) {
        m_preview->SetBitmap(render_preview(result, px));
        wxString info = wxString::Format(_L("Size: %.1f x %.1f mm"), result.width, result.height);
        if (result.code.qr_version > 0)
            info += "\n" + wxString::Format(_L("QR version %d (%d x %d), error correction %s"), result.code.qr_version,
                                            result.code.matrix.width, result.code.matrix.height, Barcode::to_string(result.code.qr_ecc));
        else if (result.code.encoded_text != p.text)
            info += "\n" + wxString::Format(_L("Encoded: %s"), from_u8(result.code.encoded_text));
        m_info->SetLabel(info);
        wxString warnings;
        for (const std::string &w : result.warnings)
            warnings += (warnings.empty() ? "" : "\n") + from_u8(w);
        if (p.module_size < 0.8)
            warnings += (warnings.empty() ? "" : "\n") + _L("Modules smaller than 0.8 mm may not print sharply enough to be scanned.");
        m_warning->SetLabel(warnings);
    } else {
        wxImage blank(px, px);
        blank.SetRGB(wxRect(0, 0, px, px), 200, 200, 200);
        m_preview->SetBitmap(wxBitmap(blank));
        m_info->SetLabel(wxEmptyString);
        m_warning->SetLabel(from_u8(result.error));
    }
    m_info->Wrap(px);
    m_warning->Wrap(px);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

wxBitmap CodeEmbossDialog::render_preview(const CodeEmbossResult &result, int max_px) const
{
    auto color = [this](wxChoice *choice, const char *def) -> std::string {
        int index = choice != nullptr ? choice->GetSelection() : 0;
        if (index > 0 && size_t(index) <= m_filament_colors.size() && !m_filament_colors[size_t(index - 1)].empty())
            return m_filament_colors[size_t(index - 1)];
        return def;
    };

    // All parts into one SVG, each part colored by its filament
    std::stringstream svg;
    svg << view_box(result.parts.front().svg);
    for (const CodeEmbossPart &part : result.parts) {
        std::string fill;
        switch (part.role) {
        case CodePartRole::Dark: fill = color(m_dark_filament, "#000000"); break;
        case CodePartRole::Light: fill = color(m_light_filament, "#FFFFFF"); break;
        case CodePartRole::Logo: fill = color(m_logo_filament, "#1E6FD9"); break;
        }
        svg << "<path fill=\"" << fill << "\" fill-rule=\"evenodd\" d=\"" << path_data(part.svg) << "\"/>";
    }
    svg << "</svg>";

    wxImage image(max_px, max_px);
    image.SetRGB(wxRect(0, 0, max_px, max_px), 200, 200, 200);
    NSVGimage_ptr nsvg = Slic3r::nsvgParse(svg.str(), "px");
    if (nsvg == nullptr || nsvg->width <= 0.f || nsvg->height <= 0.f)
        return wxBitmap(image);

    float scale = float(max_px) / std::max(nsvg->width, nsvg->height);
    int   w     = std::max(1, int(nsvg->width * scale));
    int   h     = std::max(1, int(nsvg->height * scale));
    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4, 0);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (rast == nullptr)
        return wxBitmap(image);
    nsvgRasterize(rast, nsvg.get(), 0, 0, scale, rgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);

    int ox = (max_px - w) / 2, oy = (max_px - h) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char *c = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            // blend over gray background
            auto blend = [a = c[3]](unsigned char v) { return (unsigned char) ((v * a + 200 * (255 - a)) / 255); };
            image.SetRGB(ox + x, oy + y, blend(c[0]), blend(c[1]), blend(c[2]));
        }
    return wxBitmap(image);
}

void CodeEmbossDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
