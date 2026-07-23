#include "TimelapseDownloadDialog.hpp"

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/dirdlg.h>
#include <wx/event.h>

#include <boost/format.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"

namespace Slic3r {
namespace GUI {

TimelapseDownloadDialog::TimelapseDownloadDialog(const std::string& file_url,
                                                   const std::string& file_name,
                                                   const std::string& sn,
                                                   int file_count,
                                                   wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY, _L("Download"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_file_url(file_url)
    , m_file_name(file_name)
    , m_sn(sn)
    , m_file_count(file_count)
{
    std::string icon_path = (boost::format("%1%/images/Snapmaker_OrcaTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    SetBackgroundColour(*wxWHITE);
    setup_ui();

    wxGetApp().UpdateDlgDarkUI(this);
}

TimelapseDownloadDialog::~TimelapseDownloadDialog()
{
}

void TimelapseDownloadDialog::setup_ui()
{
    wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);

    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);

    wxPanel* panel_path = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    panel_path->SetSize(wxSize(FromDIP(480), FromDIP(130)));
    panel_path->SetMinSize(wxSize(FromDIP(480), FromDIP(130)));

    wxBoxSizer* sizer_path = new wxBoxSizer(wxVERTICAL);

    // File name row
    wxBoxSizer* sizer_file = new wxBoxSizer(wxHORIZONTAL);

    wxString label_text;
    wxString name_text;
    if (m_file_count > 1) {
        label_text = _L("Time-lapse Files:");
        name_text = wxString::Format(_L("%d files to download"), m_file_count);
    } else {
        label_text = _L("Time-lapse File:");
        name_text = wxString::FromUTF8(m_file_name.c_str());
    }

    auto* label_file = new wxStaticText(panel_path, wxID_ANY, label_text);
    label_file->SetForegroundColour(*wxBLACK);
    sizer_file->Add(label_file, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    auto* file_name_text = new wxStaticText(panel_path, wxID_ANY, name_text);
    file_name_text->SetForegroundColour(*wxBLACK);
    sizer_file->Add(file_name_text, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    sizer_path->Add(sizer_file, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    sizer_path->AddSpacer(FromDIP(8));

    // Save path row
    wxBoxSizer* sizer_save = new wxBoxSizer(wxHORIZONTAL);
    auto* label_save = new wxStaticText(panel_path, wxID_ANY, _L("Save Path:"));
    label_save->SetForegroundColour(*wxBLACK);
    sizer_save->Add(label_save, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_download_path = wxGetApp().app_config->get("download_path");
    m_path_text = new wxTextCtrl(panel_path, wxID_ANY, wxString::FromUTF8(m_download_path.c_str()),
                                  wxDefaultPosition, wxSize(FromDIP(280), -1),
                                  wxTE_READONLY);
    m_path_text->SetForegroundColour(*wxBLACK);
    m_path_text->SetBackgroundColour(wxColour(245, 245, 245));
    sizer_save->Add(m_path_text, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    auto* browse_button = new Button(panel_path, _L("Browse..."));
    browse_button->SetSize(wxSize(FromDIP(70), FromDIP(24)));
    browse_button->SetMinSize(wxSize(FromDIP(70), FromDIP(24)));
    browse_button->SetBackgroundColour(*wxWHITE);
    browse_button->SetCornerRadius(FromDIP(4));
    browse_button->SetCursor(wxCURSOR_HAND);
    browse_button->Bind(wxEVT_BUTTON, &TimelapseDownloadDialog::on_browse_clicked, this);
    sizer_save->Add(browse_button, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    sizer_path->Add(sizer_save, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    sizer_path->AddSpacer(FromDIP(20));

    // Bottom buttons: Cancel / Download
    wxBoxSizer* sizer_buttons_path = new wxBoxSizer(wxHORIZONTAL);
    sizer_buttons_path->AddStretchSpacer();

    StateColor btn_cancel_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(231, 231, 231), StateColor::Normal));
    StateColor btn_cancel_bd(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
    StateColor btn_cancel_txt(std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(36, 36, 36), StateColor::Normal));

    auto* btn_cancel_path = new Button(panel_path, _L("Cancel"));
    btn_cancel_path->SetSize(wxSize(FromDIP(80), FromDIP(28)));
    btn_cancel_path->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
    btn_cancel_path->SetBackgroundColour(*wxWHITE);
    btn_cancel_path->SetBackgroundColor(btn_cancel_bg);
    btn_cancel_path->SetBorderColor(btn_cancel_bd);
    btn_cancel_path->SetTextColor(btn_cancel_txt);
    btn_cancel_path->SetCornerRadius(FromDIP(12));
    btn_cancel_path->SetCursor(wxCURSOR_HAND);
    btn_cancel_path->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    sizer_buttons_path->Add(btn_cancel_path, 0, wxALL, FromDIP(5));

    sizer_buttons_path->AddSpacer(FromDIP(12));

    StateColor btn_dl_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(23, 99, 226), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(23, 99, 226), StateColor::Normal));
    StateColor btn_dl_bd(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(23, 99, 226), StateColor::Enabled));
    StateColor btn_dl_txt(std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));

    auto* download_button = new Button(panel_path, _L("Download"));
    download_button->SetSize(wxSize(FromDIP(80), FromDIP(28)));
    download_button->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
    download_button->SetBackgroundColour(*wxWHITE);
    download_button->SetBackgroundColor(btn_dl_bg);
    download_button->SetBorderColor(btn_dl_bd);
    download_button->SetTextColor(btn_dl_txt);
    download_button->SetCornerRadius(FromDIP(12));
    download_button->SetCursor(wxCURSOR_HAND);
    download_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    sizer_buttons_path->Add(download_button, 0, wxALL, FromDIP(5));

    sizer_path->Add(sizer_buttons_path, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    panel_path->SetSizer(sizer_path);
    panel_path->Layout();
    sizer_path->Fit(panel_path);

    m_sizer_main->Add(panel_path, 0, wxALL, FromDIP(16));

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    CentreOnParent();
}

int TimelapseDownloadDialog::ShowModal()
{
    return DPIDialog::ShowModal();
}

void TimelapseDownloadDialog::on_browse_clicked(wxCommandEvent& event)
{
    wxDirDialog dlg(this, _L("Choose Download Directory"), wxString::FromUTF8(m_download_path.c_str()),
                     wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_download_path = dlg.GetPath().ToUTF8().data();
        m_path_text->SetValue(wxString::FromUTF8(m_download_path.c_str()));
    }
}

void TimelapseDownloadDialog::on_dpi_changed(const wxRect& suggested_rect)
{
}

}} // namespace Slic3r::GUI
