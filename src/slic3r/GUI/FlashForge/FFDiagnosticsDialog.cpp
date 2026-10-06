#include "FFDiagnosticsDialog.hpp"

#include <algorithm>
#include <memory>

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "slic3r/GUI/FlashForge/FlashNetworkIntfc.h"
#include "slic3r/GUI/FlashForge/FreeInDestructor.h"
#include "slic3r/GUI/FlashForge/MultiComDef.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"

namespace Slic3r { namespace GUI {

namespace {

std::string safe_str(const char *value)
{
    return value == nullptr ? std::string() : std::string(value);
}

// The folder a user will actually find. Desktop first because that is where a support request
// tells them to look; Downloads and then the documents folder as fallbacks on systems where the
// desktop is redirected or missing.
wxString diagnostics_output_dir()
{
    const wxString home = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Desktop);
    if (!home.empty() && wxFileName::DirExists(home))
        return home;
    const wxString downloads = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads);
    if (!downloads.empty() && wxFileName::DirExists(downloads))
        return downloads;
    return wxStandardPaths::Get().GetDocumentsDir();
}

// Every file in `dir`, newest first, capped at `limit`. The zip is meant to be mailed, so a user
// with months of logs must not end up with a 200MB attachment.
std::vector<std::string> latest_files(const wxString &dir, const wxString &prefix, size_t limit)
{
    std::vector<std::pair<wxDateTime, wxString>> found;
    if (!wxFileName::DirExists(dir))
        return {};

    wxDir  d(dir);
    wxString name;
    if (d.IsOpened() && d.GetFirst(&name, wxEmptyString, wxDIR_FILES)) {
        do {
            if (!prefix.empty() && !name.StartsWith(prefix))
                continue;
            wxFileName fn(dir + "/" + name);
            wxDateTime mtime;
            if (fn.FileExists() && (mtime = fn.GetModificationTime()).IsValid())
                found.emplace_back(mtime, fn.GetFullPath());
        } while (d.GetNext(&name));
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    std::vector<std::string> paths;
    for (size_t i = 0; i < found.size() && i < limit; ++i)
        paths.push_back(found[i].second.utf8_string());
    return paths;
}

} // namespace

FFDiagnosticsDialog::FFDiagnosticsDialog(wxWindow          *parent,
                                         const std::string &serial_number,
                                         const std::string &ip,
                                         const std::string &check_code,
                                         unsigned short     port)
    : wxDialog(parent, wxID_ANY, _L("FlashForge connection test"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_port(port == 0 ? 8898 : port)
{
    SetBackgroundColour(*wxWHITE);

    auto *grid = new wxFlexGridSizer(3, 2, FromDIP(8), FromDIP(10));
    grid->AddGrowableCol(1, 1);

    auto add_row = [this, grid](const wxString &label, ::TextInput *&field, const std::string &value) {
        auto *text = new wxStaticText(this, wxID_ANY, label);
        text->SetFont(Label::Body_14);
        grid->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        field = new ::TextInput(this, wxString::FromUTF8(value.c_str()));
        field->SetMinSize(wxSize(FromDIP(280), FromDIP(28)));
        grid->Add(field, 1, wxEXPAND);
    };

    add_row(_L("Serial number"), m_serial, serial_number);
    add_row(_L("IP address"), m_ip, ip);
    add_row(_L("Check code"), m_code, check_code);

    m_status = new wxStaticText(this, wxID_ANY, _L("Enter the printer's details and press Test connection."));
    m_status->SetFont(Label::Body_14);

    m_detail = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              FromDIP(wxSize(520, 200)), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);

    m_test_btn = new wxButton(this, wxID_ANY, _L("Test connection"));
    m_diag_btn = new wxButton(this, wxID_ANY, _L("Diagnostics"));
    m_open_btn = new wxButton(this, wxID_ANY, _L("Open folder"));
    m_open_btn->Enable(false);

    m_test_btn->SetToolTip(_L("Asks the printer to identify itself. No print job is started."));
    m_diag_btn->SetToolTip(_L("Turns on detailed logging, runs the test, and writes one zip file "
                              "you can send for support. The check code is not included."));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(m_test_btn, 0, wxRIGHT, FromDIP(8));
    buttons->Add(m_diag_btn, 0, wxRIGHT, FromDIP(8));
    buttons->Add(m_open_btn, 0);
    buttons->AddStretchSpacer(1);
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Close")), 0);

    // FlashForge's library is theirs; say so where a user of this dialog will see it.
    auto *notice = new wxStaticText(
        this, wxID_ANY,
        _L("FlashForge connectivity uses FlashForge's FlashNetwork library, redistributed "
           "unmodified from the Orca-Flashforge / Flash Studio release it was taken from."));
    notice->SetFont(Label::Body_12);
    notice->SetForegroundColour(wxColour(107, 107, 107));
    notice->Wrap(FromDIP(520));

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(12));
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    sizer->Add(m_detail, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(notice, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(12));

    SetSizerAndFit(sizer);
    CentreOnParent();

    m_test_btn->Bind(wxEVT_BUTTON, &FFDiagnosticsDialog::on_test, this);
    m_diag_btn->Bind(wxEVT_BUTTON, &FFDiagnosticsDialog::on_diagnostics, this);
    m_open_btn->Bind(wxEVT_BUTTON, &FFDiagnosticsDialog::on_open_folder, this);

    wxGetApp().UpdateDlgDarkUI(this);
}

FFConnectionTestResult FFDiagnosticsDialog::run_test()
{
    FFConnectionTestResult result;
    result.serial_number = m_serial->GetTextCtrl()->GetValue().utf8_string();
    result.ip            = m_ip->GetTextCtrl()->GetValue().utf8_string();
    result.port          = m_port;

    const std::string check_code = m_code->GetTextCtrl()->GetValue().utf8_string();

    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        const std::string &why = wxGetApp().flashnetwork_error();
        result.unavailable_reason = why.empty() ? "FlashNetwork is not loaded" : why;
        return result;
    }
    if (result.ip.empty() || result.serial_number.empty()) {
        result.unavailable_reason = "enter the printer's serial number and IP address first";
        return result;
    }

    // Read-only calls: identify the printer and read its state. Neither starts or changes a job.
    fnet_dev_product_t *product = nullptr;
    result.product_attempted = true;
    result.product_code = intfc->getLanDevProduct(result.ip.c_str(), result.port,
                                                  result.serial_number.c_str(), check_code.c_str(),
                                                  &product, ComTimeoutLanA);
    fnet::FreeInDestructor freeProduct(product, intfc->freeDevProduct);

    fnet_dev_detail_t *detail = nullptr;
    result.detail_attempted = true;
    result.detail_code = intfc->getLanDevDetail(result.ip.c_str(), result.port,
                                                result.serial_number.c_str(), check_code.c_str(),
                                                &detail, ComTimeoutLanA);
    fnet::FreeInDestructor freeDetail(detail, intfc->freeDevDetail);

    if (fnet_succeeded(result.detail_code) && detail != nullptr) {
        result.product_name     = safe_str(detail->name);
        result.machine_type     = safe_str(detail->nozzleModel);
        if (result.machine_type.empty())
            result.machine_type = safe_str(detail->measure);
        result.firmware_version = safe_str(detail->firmwareVersion);
        result.mac_address      = safe_str(detail->macAddr);
        result.status           = safe_str(detail->status);
    }
    return result;
}

void FFDiagnosticsDialog::show_result(const std::string &summary, const std::string &detail, bool ok)
{
    m_status->SetLabel(wxString::FromUTF8(summary.c_str()));
    m_status->SetForegroundColour(ok ? wxColour(0x1F, 0x8A, 0x3C) : wxColour(0xEA, 0x35, 0x22));
    m_detail->SetValue(wxString::FromUTF8(detail.c_str()));
    Layout();
}

void FFDiagnosticsDialog::on_test(wxCommandEvent &)
{
    wxBusyCursor busy;
    m_test_btn->Enable(false);
    const FFConnectionTestResult result = run_test();
    m_test_btn->Enable(true);
    show_result(ff_test_summary(result), ff_test_report(result), ff_test_fully_ok(result));
}

wxString FFDiagnosticsDialog::write_diagnostics_zip(const FFConnectionTestResult &test, wxString &error)
{
    FFDiagnosticsInput input;
    input.app_version           = SLIC3R_VERSION;
    input.os_description        = wxGetOsDescription().utf8_string();
    input.flashnetwork_loaded   = MultiComMgr::inst()->networkIntfc() != nullptr;
    input.flashnetwork_version  = MultiComMgr::inst()->libraryVersion();
    input.flashnetwork_dll_path = wxGetApp().flashnetwork_path();
    input.test                  = test;

    const wxString data_root = wxString::FromUTF8(data_dir());
    input.flashnetwork_logs = latest_files(data_root + "/FlashNetwork", wxEmptyString, 6);
    input.app_logs          = latest_files(data_root + "/log", "debug_", 2);

    const auto entries = ff_diagnostics_manifest(input);

    // Belt and braces: the input type has no field for a check code, but the user typed one into
    // this dialog, so assert here too that it did not find its way into the archive.
    const std::string check_code = m_code->GetTextCtrl()->GetValue().utf8_string();
    if (!ff_diagnostics_manifest_is_free_of(entries, check_code)) {
        error = _L("Internal error: the diagnostics archive would have contained the check code.");
        return wxEmptyString;
    }

    const std::string date = wxDateTime::Now().Format("%Y-%m-%d").utf8_string();
    const wxString    dir  = diagnostics_output_dir();
    // ff_diagnostics_file_name() already ends in .zip, so build the full name and let wxFileName
    // split it - setting a name and an extension separately would produce "....zip.zip".
    wxFileName out(dir, wxString::FromUTF8(ff_diagnostics_file_name(date).c_str()));
    // Do not silently overwrite an archive from earlier the same day.
    for (int suffix = 2; out.FileExists() && suffix < 100; ++suffix) {
        out.Assign(dir, wxString::FromUTF8(
                            ff_diagnostics_file_name(date + "_" + std::to_string(suffix)).c_str()));
    }
    const wxString output_path = out.GetFullPath();

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_writer(&zip, output_path.utf8_string())) {
        error = _L("Could not create the diagnostics archive.");
        return wxEmptyString;
    }
    std::unique_ptr<mz_zip_archive, decltype(&close_zip_writer)> closer(&zip, close_zip_writer);

    for (const FFDiagnosticsEntry &entry : entries) {
        bool added = false;
        if (entry.is_inline) {
            added = mz_zip_writer_add_mem(&zip, entry.zip_path.c_str(), entry.inline_text.data(),
                                          entry.inline_text.size(), MZ_DEFAULT_COMPRESSION);
        } else {
            added = mz_zip_writer_add_file(&zip, entry.zip_path.c_str(),
                                           encode_path(entry.source_path.c_str()).c_str(),
                                           nullptr, 0, MZ_DEFAULT_COMPRESSION);
            // A log that vanished or is locked must not sink the whole archive - the summary and
            // the connection test are the parts that matter most.
            if (!added) {
                BOOST_LOG_TRIVIAL(warning) << "diagnostics: skipping " << entry.source_path;
                added = true;
            }
        }
        if (!added) {
            error = _L("Could not write the diagnostics archive.");
            return wxEmptyString;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        error = _L("Could not finish the diagnostics archive.");
        return wxEmptyString;
    }
    return output_path;
}

void FFDiagnosticsDialog::on_diagnostics(wxCommandEvent &)
{
    wxBusyCursor busy;
    m_diag_btn->Enable(false);

    // Raise FlashNetwork's log level for the session. It can only be set at initialise time, so
    // this tears the stack down and brings it back up; the test right afterwards is then the first
    // thing written to the fresh debug log, which is exactly what we want in the zip.
    const bool debug_on = MultiComMgr::inst()->setDebugLogging(true);
    if (!debug_on)
        BOOST_LOG_TRIVIAL(warning) << "diagnostics: could not raise the FlashNetwork log level";

    flush_logs();
    const FFConnectionTestResult result = run_test();
    flush_logs();

    wxString error;
    const wxString path = write_diagnostics_zip(result, error);
    m_diag_btn->Enable(true);

    if (path.empty()) {
        show_result(error.utf8_string(), ff_test_report(result), false);
        return;
    }

    m_last_zip_path = path;
    m_open_btn->Enable(true);

    std::string detail = ff_test_report(result);
    detail += "\nDiagnostics written to:\n  " + path.utf8_string() + "\n";
    if (!debug_on)
        detail += "\nNote: detailed FlashNetwork logging could not be enabled for this session.\n";
    show_result((_L("Diagnostics saved to ") + path).utf8_string(), detail, true);
}

void FFDiagnosticsDialog::on_open_folder(wxCommandEvent &)
{
    if (m_last_zip_path.empty())
        return;
    wxLaunchDefaultApplication(wxFileName(m_last_zip_path).GetPath());
}

}} // namespace Slic3r::GUI
