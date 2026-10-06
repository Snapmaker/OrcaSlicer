#include "ExportLogs.hpp"
#include <memory>
#include <thread>
#include <boost/log/trivial.hpp>
#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/ProgressDialog.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/FlashForge/MultiComHelper.hpp"

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_EXPORT_LOGS_FINISHED, ExportLogsFinishedEvent);
wxDEFINE_EVENT(EVT_UPLOAD_LOG_PROGRESS, wxCommandEvent);

ExportLogsDlg::ExportLogsDlg(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Export log"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxSYSTEM_MENU)
{
    SetBackgroundColour(*wxWHITE);
    SetSize(FromDIP(wxSize(400, 200)));
    SetMinSize(FromDIP(wxSize(400, 200)));
    SetMaxSize(FromDIP(wxSize(400, 200)));

    wxStaticText *msgStatText = new wxStaticText(this, wxID_ANY, _L("Exporting, please wait"));
    msgStatText->SetFont(Label::Body_14);

    wxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer(3);
    sizer->Add(msgStatText, 0, wxALIGN_CENTER);
    sizer->AddStretchSpacer(4);

    SetSizer(sizer);
    Layout();
    CenterOnParent();
}

void ExportLogsDlg::onExportLogsFinished(ExportLogsFinishedEvent &event)
{
    EndModal(wxID_OK);
    if (event.succeed) {
        CallAfter([outputPath = event.outputPath]() {
        MessageDialog dlg(wxGetApp().mainframe, _CTX("Export successful", "Flashforge"), _L("Export log"));
        dlg.ShowModal();
            wxLaunchDefaultApplication(wxFileName(outputPath).GetPath());
        });
    } else {
        CallAfter([outputPath = event.outputPath]() {
        MessageDialog dlg(wxGetApp().mainframe, _L("Export failed, please try again"), _L("Export log"));
        dlg.ShowModal();
            if (wxFileName::FileExists(outputPath)) {
                wxRemoveFile(outputPath);
        }
        });
    }
}

void ExportLogs::exportLocal()
{
    wxString defFileName = wxDateTime::Now().Format("%Y-%m-%d_%H-%M-%S.zip");
    wxFileDialog fileDlg(wxGetApp().mainframe, "", "", defFileName, "*.zip", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fileDlg.ShowModal() != wxID_OK) {
        return;
    }
    flush_logs();
    wxString outputPath = fileDlg.GetPath();
    wxString rootPath = wxString::FromUTF8(data_dir());
    ExportLogsDlg exportDlg(wxGetApp().mainframe);
    exportDlg.Bind(EVT_EXPORT_LOGS_FINISHED, &ExportLogsDlg::onExportLogsFinished, &exportDlg);
    auto thread = std::thread([&]() {
        auto fileInfos = getRootLatestFiles(rootPath, wxDateTime::Now());
        bool succeed = saveZip(outputPath, rootPath, fileInfos);
        wxQueueEvent(&exportDlg, new ExportLogsFinishedEvent(EVT_EXPORT_LOGS_FINISHED, succeed, outputPath));
    });
    exportDlg.ShowModal();
    thread.join();
}

void ExportLogs::uploadLocal() 
{
    wxString     defFileName = wxDateTime::Now().Format("%Y-%m-%d_%H-%M-%S.zip");
    flush_logs();
    wxString      outputPath = wxStandardPaths::Get().GetTempDir() + "/" + defFileName;
    wxString      rootPath   = wxString::FromUTF8(data_dir());
    wxString      email;
    if (!wxGetApp().is_flashforge_login()) {
        EmailInputDialog email_dlg(wxGetApp().mainframe, _L("Upload log"));
        if (email_dlg.ShowModal() == wxID_OK) {
            email = email_dlg.getText();
        } else {
            return;
        }
    }
    auto dlg = wxGetApp().mainframe->createLogProgress();
    dlg->Update(10, _L("Save tmp zip file"));
    dlg->Fit();
    auto fileInfos = getRootLatestFiles(rootPath, wxDateTime::Now());
    bool succeed   = saveZip(outputPath, rootPath, fileInfos);
    dlg->Update(20, _L("Generating local device id"));
    dlg->Fit();
    wxString deviceId = getDeviceHash();
    dlg->Update(30, _L("Upload zip file"));
    dlg->Fit();
    
    auto logCallback = [](long long now, long long total, void* callbackData) {
        wxWindow* inst = (wxWindow*) callbackData;
        const int  now_progress    = 30;
        if (total != 0) {
            int progress = (double) now / total;
            if (inst != nullptr) {
                wxCommandEvent* evt = new wxCommandEvent(EVT_UPLOAD_LOG_PROGRESS);
                evt->SetInt(progress * (100 - now_progress) + now_progress);
                wxQueueEvent(inst, evt);
            }
        }
        return 0;
    };
    auto ret = MultiComHelper::inst()->uploadLogFileCloud(outputPath.utf8_string(), defFileName.utf8_string(), email.utf8_string(), logCallback, wxGetApp().mainframe, ComTimeoutWanB);
    if (ret != COM_OK) {
        dlg->Hide();
        ErrorDialog failed_dlg(wxGetApp().mainframe, _L("Upload failed"), _L("Upload log"));
        failed_dlg.ShowModal();
    } else {
        dlg->Update(100, _L("Upload Complete"));
        MessageDialog success_dlg(wxGetApp().mainframe, _L("Upload Successful"), _L("Upload log"));
        success_dlg.ShowModal();
    }
}

std::vector<std::pair<wxString, std::vector<wxString>>> ExportLogs::getRootLatestFiles(const wxString &rootPath,
    const wxDateTime &now)
{
    std::vector<std::pair<wxString, std::vector<wxString>>> fileInfos;
    fileInfos.emplace_back("log", getDirLatestFiles(rootPath + "/log", "debug_", now));
    fileInfos.emplace_back("FlashNetwork", getDirLatestFiles(rootPath + "/FlashNetwork", "", now));
    return fileInfos;
}

std::vector<wxString> ExportLogs::getDirLatestFiles(const wxString &dirPath, const wxString &prefix,
    const wxDateTime &now)
{
    wxDir dir(dirPath);
    wxString fileName;
    std::vector<wxString> fileNames;
    if (dir.GetFirst(&fileName, wxEmptyString, wxDIR_FILES)) {
        do {
            if (prefix.empty() || fileName.StartsWith(prefix)) {
                wxDateTime fileDateTime = wxFileName(dirPath + '/' + fileName).GetModificationTime();
                if (fileDateTime.IsValid()) {
                    if ((now - fileDateTime).GetHours() <= 7 * 24) {
                        fileNames.push_back(fileName);
                    }
                }
            }
        } while (dir.GetNext(&fileName));
    }
    return fileNames;
}

bool ExportLogs::saveZip(const wxString &outputPath, const wxString &rootPath,
    const std::vector<std::pair<wxString, std::vector<wxString>>> &fileInfos)
{
    mz_zip_archive zipArchive;
    mz_zip_zero_struct(&zipArchive);
    if (!open_zip_writer(&zipArchive, outputPath.utf8_string())) {
        BOOST_LOG_TRIVIAL(error) << mz_zip_get_error_string(mz_zip_get_last_error(&zipArchive));
        return false;
    }
    std::unique_ptr<mz_zip_archive, decltype(&close_zip_writer)> freeCurl(&zipArchive, close_zip_writer);
    for (auto &fileInfo : fileInfos) {
        wxString dirPath = fileInfo.first + '/';
        std::string encodedDirPath = encode_path(dirPath.utf8_string().c_str());
        if (!mz_zip_writer_add_mem(&zipArchive, encodedDirPath.c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION)) {
            BOOST_LOG_TRIVIAL(error) << mz_zip_get_error_string(mz_zip_get_last_error(&zipArchive));
            return false;
        }
        for (auto &fileName : fileInfo.second) {
            std::string dstPath = encode_path((dirPath + fileName).utf8_string().c_str());
            std::string srcPath = encode_path((rootPath + '/' + dirPath + fileName).utf8_string().c_str());
            if (!addZipFile(&zipArchive, dstPath, srcPath)) {
                return false;
            }
        }
    }
    if (!mz_zip_writer_finalize_archive(&zipArchive)) {
        BOOST_LOG_TRIVIAL(error) << mz_zip_get_error_string(mz_zip_get_last_error(&zipArchive));
        return false;
    }
    return true;
}

bool ExportLogs::addZipFile(mz_zip_archive *zipArchive, const std::string &dstPath, const wxString &srcPath)
{
    std::string encodedSrcPath = encode_path(srcPath.utf8_string().c_str());
    if (mz_zip_writer_add_file(zipArchive, dstPath.c_str(), encodedSrcPath.c_str(), nullptr, 0,
        MZ_DEFAULT_COMPRESSION)) {
        return true;
    }
    mz_zip_error error = mz_zip_get_last_error(zipArchive);
    if (error != MZ_ZIP_FILE_OPEN_FAILED) {
        BOOST_LOG_TRIVIAL(error) << mz_zip_get_error_string(error);
        return false;
    }
    wxFile file;
    if (!file.Open(srcPath, wxFile::read)) {
        BOOST_LOG_TRIVIAL(error) << "wxFile::Open error";
        return false;
    }
    wxFileOffset fileSize = file.Length();
    if (fileSize == wxInvalidOffset) {
        BOOST_LOG_TRIVIAL(error) << "wxFile::Length error";
        return false;
    }
    std::vector<char> buf(fileSize);
    if (file.Read(buf.data(), fileSize) != fileSize) {
        BOOST_LOG_TRIVIAL(error) << "wxFile::Read error";
        return false;
    }
    if (!mz_zip_writer_add_mem(zipArchive, dstPath.c_str(), buf.data(), fileSize, MZ_DEFAULT_COMPRESSION)) {
        BOOST_LOG_TRIVIAL(error) << mz_zip_get_error_string(mz_zip_get_last_error(zipArchive));
        return false;
    }
    return true;
}

bool ExportLogs::isVirtualMachine()
{
    wxArrayString output;
#ifdef __WXMSW__
    wxString cmd = "powershell -Command \"Get-WmiObject Win32_ComputerSystem | Select-Object -ExpandProperty Manufacturer\"";
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString manu = output[0].Lower().Trim();
        if (manu.Contains("vmware") || manu.Contains("virtualbox") || manu.Contains("microsoft corporation") || manu.Contains("qemu"))
            return true;
    }
#elif __WXMAC__
    wxString cmd = "system_profiler SPHardwareDataType | grep 'Model Identifier'";
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString model = output[0].Lower().Trim();
        if (model.Contains("vmware") || model.Contains("parallels") || model.Contains("virtual"))
            return true;
    }
#endif
    return false;
}

wxString ExportLogs::getDeviceHash() 
{ 
    std::vector<wxString> identifiers;
    wxString              cmd;
    wxArrayString         output;
#ifdef __WXMSW__
    cmd = "powershell -Command \"Get-WmiObject Win32_BaseBoard | Select-Object -ExpandProperty SerialNumber\"";
    // 1. MB ID
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString sn = output[0].Trim().Trim(false);
        if (!sn.IsEmpty() && sn != "To be filled by O.E.M." && sn != "None")
            identifiers.push_back(sn);
    }
    output.clear();

    // 2. BIOS UUID
    cmd = "powershell -Command \"Get-WmiObject Win32_ComputerSystemProduct | Select-Object -ExpandProperty UUID\"";
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString uuid = output[0].Trim().Trim(false);
        if (!uuid.IsEmpty() && uuid != "00000000-0000-0000-0000-000000000000")
            identifiers.push_back(uuid);
    }
    output.clear();

    // 3. DISK ID
    cmd = "powershell -Command \"Get-WmiObject Win32_DiskDrive | Select-Object -ExpandProperty SerialNumber | Select-Object -First 1\"";
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString diskSn = output[0].Trim().Trim(false);
        if (!diskSn.IsEmpty() && diskSn != "Unknown")
            identifiers.push_back(diskSn);
    }
    output.clear();

#elif __WXMAC__
    // 1. IOPlatformUUID
    cmd = "ioreg -d2 -c IOPlatformExpertDevice | awk -F\\\" '/IOPlatformUUID/{print $4}'";
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString uuid = output[0].Trim().Trim(false);
        if (!uuid.IsEmpty())
            identifiers.push_back(uuid);
    }
    output.clear();

    // 2. MB ID
    cmd = "system_profiler SPHardwareDataType | grep 'Serial Number (system)' | awk '{print $4}'";
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString sn = output[0].Trim().Trim(false);
        if (!sn.IsEmpty())
            identifiers.push_back(sn);
    }
    output.clear();

    // 3. MAC ADDR
    cmd = "ifconfig en0 | grep ether | awk '{print $2}'";
    if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
        wxString mac = output[0].Trim().Trim(false);
        if (!mac.IsEmpty())
            identifiers.push_back(mac);
    }
    output.clear();

#endif

    if (isVirtualMachine()) {
#ifdef __WXMSW__
        // 1. Hyper-V/VMware ID
        cmd = "powershell -Command \"Get-WmiObject Win32_ComputerSystem | Select-Object -ExpandProperty Name\"";
        if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
            wxString vmName = output[0].Trim().Trim(false);
            if (!vmName.IsEmpty())
                identifiers.push_back("VM_" + vmName);
        }
        output.clear();

        // 2. Virtual Disk Path
        cmd = "powershell -Command \"Get-WmiObject Win32_LogicalDisk | Select-Object -ExpandProperty DeviceID | Select-Object -First 1\"";
        if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
            wxString diskPath = output[0].Trim().Trim(false);
            if (!diskPath.IsEmpty())
                identifiers.push_back(diskPath);
        }
        output.clear();

#elif __WXMAC__
        // Parallels/VMware Fusion UUID
        cmd = "defaults read /Library/Preferences/Parallels/parallels.desktop.plist 'Virtual Machines' 2>/dev/null | grep uuid | "
                       "head -n1 | awk '{print $3}' | tr -d '\"'";
        if (wxExecute(cmd, output, wxEXEC_SYNC) == 0) {
            wxString vmUUID = output[0].Trim().Trim(false);
            if (!vmUUID.IsEmpty())
                identifiers.push_back("VMUUID_" + vmUUID);
        }
        output.clear();

#endif
    }
    identifiers.push_back("orca-flashforge");

    auto simpleHash64 = [](const wxString& str) {
        uint64_t      hash = 0xCBF29CE484222325LL;
        const wxChar* data = str.wc_str();
        while (*data != '\0') {
            hash ^= (wxUint64) *data;
            hash *= 0x100000001B3LL;
            data++;
        }
        return hash;
    };
    wxString combined;
    for (const auto& id : identifiers) {
        if (!id.IsEmpty())
            combined += id + "|";
    }
    wxString finalID = wxString::Format("%016llX", (unsigned long long)simpleHash64(combined)).Upper();
    return finalID;
}

EmailInputDialog::EmailInputDialog(wxWindow* parent, const wxString& title) : 
    wxDialog(parent, wxID_ANY, title) 
{
    SetBackgroundColour(*wxWHITE);
    auto sizer = new wxBoxSizer(wxVERTICAL);
    auto text_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto label      = new Label(this, _L("Email Address"));
    m_text          = new wxTextCtrl(this, wxID_ANY, "");
    m_text->SetMinSize(wxSize(FromDIP(200), FromDIP(25)));
    text_sizer->Add(label, 0, wxLEFT | wxALIGN_CENTER_HORIZONTAL, FromDIP(10));
    text_sizer->AddSpacer(FromDIP(10));
    text_sizer->Add(m_text, 0, wxRIGHT | wxALIGN_CENTER_HORIZONTAL, FromDIP(10));
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto btn = new Button(this, _L("Ok"));
    btn->SetMinSize(wxSize(FromDIP(60), FromDIP(22)));
    StateColor bg_color(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
                               std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                               std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    btn->SetBackgroundColor(bg_color);
    StateColor text_color = StateColor(std::pair{wxColour(255, 255, 255), (int) StateColor::Normal});
    btn->SetTextColor(text_color);
    btn->SetBorderWidth(0);
    btn->SetCornerRadius(FromDIP(8));
    auto w = new wxPanel(this);
    w->SetBackgroundColour(*wxWHITE);
    w->SetMinSize(wxSize(FromDIP(50), -1));
    btn_sizer->AddStretchSpacer(1);
    btn_sizer->Add(w, 0, wxEXPAND | wxALIGN_RIGHT, 0);
    btn_sizer->Add(btn, 0, wxALIGN_RIGHT | wxRIGHT, FromDIP(5));
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(text_sizer, 0, wxALL, 0);
    sizer->AddSpacer(FromDIP(15));
    sizer->Add(btn_sizer, 0, wxALL | wxALIGN_RIGHT, 0);
    sizer->AddSpacer(FromDIP(10));
    SetSizer(sizer);
    sizer->Fit(this);
    Center();
    Layout();

    btn->Bind(wxEVT_BUTTON, [=](wxCommandEvent& evt) {
        if (m_text->GetValue().empty()) {
            GUI::show_error(this, _L("Email cannot be empty."));
            return;
        }
        if (this->IsModal()) {
            EndModal(wxID_OK);
        } else {
            Close();
        }
    });
}

wxString EmailInputDialog::getText() { return this->m_text->GetValue(); }

} // namespace GUI
} // namespace Slic3r
