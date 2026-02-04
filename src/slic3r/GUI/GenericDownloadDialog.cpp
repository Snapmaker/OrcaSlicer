#include "GenericDownloadDialog.hpp"

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/hyperlink.h>
#include <wx/textctrl.h>
#include <wx/scrolwin.h>
#include <wx/event.h>
#include <wx/dcgraph.h>

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "wxExtensions.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "GUI_App.hpp"
#include "slic3r/GUI/DownloadManager.hpp"

namespace Slic3r {
namespace GUI {

GenericDownloadDialog::GenericDownloadDialog(wxString title,
                                             const std::string& file_url,
                                             const std::string& file_name,
                                             const std::string& dest_path,
                                             wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), 
                wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_title(title)
    , m_file_url(file_url)
    , m_file_name(file_name)
    , m_dest_path(dest_path)
{
    std::string icon_path = (boost::format("%1%/images/Snapmaker_OrcaTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    SetBackgroundColour(*wxWHITE);
    setup_ui();

    Bind(wxEVT_CLOSE_WINDOW, &GenericDownloadDialog::on_close, this);
    wxGetApp().UpdateDlgDarkUI(this);
   
}

GenericDownloadDialog::~GenericDownloadDialog()
{
    m_is_destroying = true;
    m_task_id = 0;
}

void GenericDownloadDialog::setup_ui()
{
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);

    m_simplebook_status = new wxSimplebook(this);
    m_simplebook_status->SetSize(wxSize(FromDIP(420), FromDIP(100)));
    m_simplebook_status->SetMinSize(wxSize(FromDIP(420), FromDIP(100)));
    m_simplebook_status->SetMaxSize(wxSize(FromDIP(420), FromDIP(250)));

    // Progress page
    m_status_bar = std::make_shared<BBLStatusBarSend>(m_simplebook_status);
    m_panel_download = m_status_bar->get_panel();
    m_panel_download->SetSize(wxSize(FromDIP(400), FromDIP(70)));
    m_panel_download->SetMinSize(wxSize(FromDIP(400), FromDIP(70)));
    m_panel_download->SetMaxSize(wxSize(FromDIP(400), FromDIP(70)));

    // Complete page
    m_panel_complete = new wxPanel(m_simplebook_status, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    wxBoxSizer* sizer_complete = new wxBoxSizer(wxVERTICAL);
    
    m_complete_text = new wxStaticText(m_panel_complete, wxID_ANY, _L("Download completed successfully!"), 
                                       wxDefaultPosition, wxDefaultSize, 0);
    m_complete_text->SetForegroundColour(*wxBLACK);
    m_complete_text->Wrap(FromDIP(360));
    sizer_complete->Add(m_complete_text, 0, wxALIGN_CENTER | wxALL, 5);
    
    StateColor btn_close_bg(std::pair<wxColour, int>(wxColour(0x90, 0x90, 0x90), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(231, 231, 231), StateColor::Normal));
    
    StateColor btn_close_bd(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
    
    StateColor btn_close_txt(std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(36, 36, 36), StateColor::Normal));
    
    m_close_button = new Button(m_panel_complete, _L("Close"));
    m_close_button->SetSize(wxSize(FromDIP(80), FromDIP(28)));
    m_close_button->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
    m_close_button->SetMaxSize(wxSize(FromDIP(80), FromDIP(28)));

    m_close_button->SetBackgroundColour(*wxWHITE);
    m_close_button->SetBackgroundColor(btn_close_bg);
    m_close_button->SetBorderColor(btn_close_bd);
    m_close_button->SetTextColor(btn_close_txt);
    m_close_button->SetCornerRadius(FromDIP(12));
    m_close_button->SetCursor(wxCURSOR_HAND);
    m_close_button->Bind(wxEVT_BUTTON, &GenericDownloadDialog::on_close_clicked, this);

    sizer_complete->Add(m_close_button, 0, wxALIGN_CENTER | wxALL, 5);
    
    m_panel_complete->SetSizer(sizer_complete);
    m_panel_complete->Layout();
    sizer_complete->Fit(m_panel_complete);

    // Error page
    m_panel_error = new wxPanel(m_simplebook_status, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    wxBoxSizer* sizer_error = new wxBoxSizer(wxVERTICAL);
    
    // Simple label for error message display
    m_error_text = new wxStaticText(m_panel_error, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(FromDIP(380), -1),
                                     wxALIGN_LEFT | wxST_ELLIPSIZE_END);
    m_error_text->SetForegroundColour(*wxBLACK);
    m_error_text->Wrap(FromDIP(380));
    sizer_error->Add(m_error_text, 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, FromDIP(10));
    
    // Button sizer aligned to right
    wxBoxSizer* sizer_buttons = new wxBoxSizer(wxHORIZONTAL);
    sizer_buttons->AddStretchSpacer(); 
    
    StateColor btn_retry_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(23, 99, 226), StateColor::Hovered),  // Same as Normal
        std::pair<wxColour, int>(wxColour(23, 99, 226), StateColor::Normal));
    
    // Set border color to same as background to avoid corner color issues
    StateColor btn_retry_bd(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(23, 99, 226), StateColor::Enabled));  // Same as background
    
    StateColor btn_retry_txt(std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));
    
    m_retry_button = new Button(m_panel_error, _L("Retry"));
    m_retry_button->SetSize(wxSize(FromDIP(80), FromDIP(28)));
    m_retry_button->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
    m_retry_button->SetMaxSize(wxSize(FromDIP(80), FromDIP(28)));
    // Set window background color to white to ensure rounded corners are white
    m_retry_button->SetBackgroundColour(*wxWHITE);
    m_retry_button->SetBackgroundColor(btn_retry_bg);
    m_retry_button->SetBorderColor(btn_retry_bg);
    m_retry_button->SetTextColor(btn_retry_txt);
    m_retry_button->SetCornerRadius(FromDIP(12));
    m_retry_button->SetCursor(wxCURSOR_HAND);
    m_retry_button->Bind(wxEVT_BUTTON, &GenericDownloadDialog::on_retry_clicked, this);

    // Setup StateColor for determine button (gray style)
    StateColor btn_determine_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(231, 231, 231), StateColor::Normal));
    
    StateColor btn_determine_bd(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
    
    StateColor btn_determine_txt(std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(36, 36, 36), StateColor::Normal));
    
    m_error_close_button = new Button(m_panel_error, _L("Determine"));
    m_error_close_button->SetSize(wxSize(FromDIP(80), FromDIP(28)));
    m_error_close_button->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
    m_error_close_button->SetMaxSize(wxSize(FromDIP(80), FromDIP(28)));
    // Set window background color to white to ensure rounded corners are white
    m_error_close_button->SetBackgroundColour(*wxWHITE);
    m_error_close_button->SetBackgroundColor(btn_determine_bg);
    m_error_close_button->SetBorderColor(btn_determine_bg);
    m_error_close_button->SetTextColor(btn_determine_txt);
    m_error_close_button->SetCornerRadius(FromDIP(12));
    m_error_close_button->SetCursor(wxCURSOR_HAND);
    m_error_close_button->Bind(wxEVT_BUTTON, &GenericDownloadDialog::on_close_clicked, this);
    sizer_buttons->Add(m_error_close_button, 0, 0);
    sizer_buttons->AddSpacer(FromDIP(6));
    sizer_buttons->Add(m_retry_button, 0, 0);
        
    sizer_error->AddSpacer(FromDIP(40));
    sizer_error->Add(sizer_buttons, 0, wxALIGN_RIGHT | wxRIGHT, FromDIP(24));
    
    
    m_panel_error->SetSizer(sizer_error);
    m_panel_error->Layout();
    sizer_error->Fit(m_panel_error);


    m_sizer_main->Add(m_simplebook_status, 0, wxALL, FromDIP(16));

    m_simplebook_status->AddPage(m_panel_download, wxEmptyString, true);
    m_simplebook_status->AddPage(m_panel_complete, wxEmptyString, false);
    m_simplebook_status->AddPage(m_panel_error, wxEmptyString, false);

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    CentreOnParent();
}

wxString GenericDownloadDialog::format_text(wxStaticText* st, wxString str, int warp)
{
    if (wxGetApp().app_config->get("language") != "zh_CN") { 
        return str; 
    }

    wxString out_txt = str;
    wxString count_txt = "";
    
    for (int i = 0; i < str.length(); i++) {
        auto text_size = st->GetTextExtent(count_txt);
        if (text_size.x < warp) {
            count_txt += str[i];
        } else {
            out_txt.insert(i - 1, '\n');
            count_txt = "";
        }
    }
    return out_txt;
}

void GenericDownloadDialog::start_download()
{
    show_progress_page();
    m_status_bar->set_progress(0);
    m_status_bar->set_status_text(_L("Preparing download..."));
    m_status_bar->change_button_label(_L("Cancel"));
    m_status_bar->set_cancel_callback_fina([this]() {
        if (m_task_id > 0) {
            DownloadManager::getInstance().cancel_download(m_task_id);
            m_task_id = 0;  // Mark as canceled to prevent duplicate cancellation
        }
    });

    // Create download callbacks
    DownloadCallbacks callbacks;
    callbacks.on_progress = [this](size_t task_id, int percent, size_t downloaded, size_t total) {
        on_download_progress(task_id, percent, downloaded, total);
    };
    callbacks.on_complete = [this](size_t task_id, const std::string& file_path) {
        on_download_complete(task_id, file_path);
    };
    callbacks.on_error = [this](size_t task_id, const std::string& error) {
        on_download_error(task_id, error);
    };

    // Start download
    if (m_dest_path.empty()) {
        m_task_id = DownloadManager::getInstance().start_internal_download(
            m_file_url, m_file_name, std::move(callbacks));
    } else {
        m_task_id = DownloadManager::getInstance().start_internal_download(
            m_file_url, m_file_name, m_dest_path, std::move(callbacks));
    }
}

int GenericDownloadDialog::ShowModal()
{
    start_download();
    return DPIDialog::ShowModal();
}

void GenericDownloadDialog::on_download_progress(size_t task_id, int percent, size_t downloaded, size_t total)
{
    wxGetApp().CallAfter([this, percent, downloaded, total]() {
        // Check if dialog is being destroyed
        if (m_is_destroying || IsBeingDeleted()) {
            return;
        }
        
        update_progress(percent);
        
        // Format status text
        wxString status_text;
        if (total > 0) {
            double downloaded_mb = downloaded / (1024.0 * 1024.0);
            double total_mb = total / (1024.0 * 1024.0);
            status_text = wxString::Format(_L("Downloading: %.1f MB / %.1f MB (%d%%)"), 
                                          downloaded_mb, total_mb, percent);
        } else {
            double downloaded_mb = downloaded / (1024.0 * 1024.0);
            status_text = wxString::Format(_L("Downloading: %.1f MB..."), downloaded_mb);
        }
        m_status_bar->set_status_text(status_text);
        
        // Call user callback if set
        if (m_on_progress) {
            m_on_progress(m_task_id, percent, downloaded, total);
        }
    });
}

void GenericDownloadDialog::on_download_complete(size_t task_id, const std::string& file_path)
{
    wxGetApp().CallAfter([this, file_path]() {
        // Check if dialog is being destroyed
        if (m_is_destroying || IsBeingDeleted()) {
            return;
        }
        
        m_download_success = true;
        m_file_path = file_path;
        show_complete_page();
        
        // Mark task as completed - no need to cancel it
        m_task_id = 0;
        
        // Call user callback if set
        if (m_on_complete) {
            m_on_complete(m_task_id, file_path);
        }
    });
}

void GenericDownloadDialog::on_download_error(size_t task_id, const std::string& error)
{
    // Log detailed error information for debugging
    BOOST_LOG_TRIVIAL(error) << boost::format("GenericDownloadDialog: Download failed for file '%1%' from URL '%2%'. Error: %3%")
        % m_file_name % m_file_url % error;
    
    wxGetApp().CallAfter([this, error]() {
        // Check if dialog is being destroyed
        if (m_is_destroying || IsBeingDeleted()) {
            return;
        }
        
        m_download_success = false;
        m_error_message = error;
        show_error_page(error);
        
        // Mark task as completed (failed) - no need to cancel it
        m_task_id = 0;
        
        // Call user callback if set
        if (m_on_error) {
            m_on_error(m_task_id, error);
        }
    });
}

void GenericDownloadDialog::on_retry_clicked(wxCommandEvent& event)
{
    if (m_on_retry) {
        m_on_retry();
    }
    start_download();
    event.Skip();
}

void GenericDownloadDialog::on_close_clicked(wxCommandEvent& event)
{
    if (m_task_id > 0) {
        DownloadManager::getInstance().cancel_download(m_task_id);
        m_task_id = 0;
    }
    EndModal(m_download_success ? wxID_OK : wxID_CANCEL);
    event.Skip();
}

void GenericDownloadDialog::on_close(wxCloseEvent& event)
{
    if (m_task_id > 0) {
        DownloadManager::getInstance().cancel_download(m_task_id);
        m_task_id = 0;
    }
    event.Skip();
}

void GenericDownloadDialog::show_progress_page()
{
    m_simplebook_status->SetSelection(0);
    m_status_bar->set_progress(0);
    m_status_bar->show_cancel_button();
}

void GenericDownloadDialog::show_complete_page()
{
    //m_simplebook_status->SetSelection(1);
    //m_status_bar->hide_cancel_button();
    EndModal(wxID_OK);
}

void GenericDownloadDialog::show_error_page(const std::string& error_msg)
{
    m_simplebook_status->SetSelection(2);
    
    // Display simple error message: filename + "Download failed"
    wxString filename = wxString::FromUTF8(m_file_name.c_str());
    wxString error_text = filename + " - Download failed";
    
    // Set error text in simple label
    m_error_text->SetLabel(error_text);
    m_error_text->Wrap(FromDIP(380));
    
    m_panel_error->Layout();
    m_simplebook_status->Layout();
    Layout();
    Fit();
    m_status_bar->hide_cancel_button();
}

void GenericDownloadDialog::update_progress(int percent, const wxString& status_text)
{
    m_status_bar->set_progress(percent);
    if (!status_text.IsEmpty()) {
        m_status_bar->set_status_text(status_text);
    }
}

void GenericDownloadDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    // Handle DPI changes if needed
}

}} // namespace Slic3r::GUI

