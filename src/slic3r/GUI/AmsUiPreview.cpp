#include "AmsUiPreview.hpp"

#include "AMSDryCtrlDialog.hpp"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "Widgets/AMSControl.hpp"
#include "Widgets/AMSDualView.hpp"
#include "Widgets/StateColor.hpp"

#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <boost/log/trivial.hpp>

#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r { namespace GUI {

namespace {

// A made-up H2D push: an AMS 2 Pro and an AMS on the right (main) extruder, an AMS HT drying on
// the left (deputy) one, both external spools, the left extruder loaded from the HT and the right
// one from A2. Only fields the parser reads.
const char *k_h2d_push = R"({"print":{
  "command":"push_status","msg":0,"sequence_id":"1",
  "cfg":"0","fun":"0","aux":"0","stat":"0","fun2":"20",
  "device":{"extruder":{"state":2,"info":[
     {"id":0,"info":2,"temp":14418140,"snow":1,"spre":65535,"star":65535,"hnow":0,"htar":0,"stat":0},
     {"id":1,"info":2,"temp":14418140,"snow":32768,"spre":65535,"star":65535,"hnow":1,"htar":1,"stat":0}]}},
  "ams":{"ams_exist_bits":"13","tray_exist_bits":"100FE","tray_is_bbl_bits":"100FE","tray_read_done_bits":"100FE",
    "tray_now":"1","tray_tar":"255","version":3,
    "ams":[
      {"id":"0","info":"3","humidity":"4","humidity_raw":"20","temp":"25.5","dry_time":0,"tray":[
        {"id":"0"},
        {"id":"1","tray_type":"PLA","tray_info_idx":"GFA00","tray_color":"00AE42FF","tag_uid":"1234567890ABCDEF","remain":80},
        {"id":"2","tray_type":"PLA","tray_info_idx":"GFA00","tray_color":"56B7E6FF","tag_uid":"1234567890ABCDEF","remain":60},
        {"id":"3","tray_type":"PLA","tray_info_idx":"GFA00","tray_color":"F4EE2AFF","tag_uid":"1234567890ABCDEF","remain":40}]},
      {"id":"1","info":"1","humidity":"3","temp":"24.0","dry_time":0,"tray":[
        {"id":"0","tray_type":"PETG","tray_info_idx":"GFG00","tray_color":"000000FF","tag_uid":"0000000000000000"},
        {"id":"1","tray_type":"PETG","tray_info_idx":"GFG00","tray_color":"FFFFFFFF","tag_uid":"0000000000000000"},
        {"id":"2","tray_type":"ABS","tray_info_idx":"GFB00","tray_color":"C12E1FFF","tag_uid":"0000000000000000"},
        {"id":"3","tray_type":"TPU","tray_info_idx":"GFU01","tray_color":"8E9089FF","tag_uid":"0000000000000000"}]},
      {"id":"128","info":"840124","humidity":"4","humidity_raw":"21","temp":"44.0","dry_time":615,
       "dry_setting":{"dry_filament":"PLA","dry_temperature":45,"dry_duration":24},"dry_sf_reason":[6],"tray":[
        {"id":"0","tray_type":"PLA","tray_info_idx":"GFA00","tray_color":"E0201DFF","tag_uid":"1234567890ABCDEF"}]}
    ]},
  "vir_slot":[
    {"id":"254","tray_type":"","tray_color":"00000000"},
    {"id":"255","tray_type":"PETG","tray_info_idx":"GFG00","tray_color":"FFFFFFFF"}]
}})";

#ifdef _WIN32
bool looks_blank(const wxBitmap &bmp)
{
    const wxImage img = bmp.ConvertToImage();
    const unsigned char *d = img.GetData();
    const size_t n = size_t(img.GetWidth()) * img.GetHeight();
    size_t non_white = 0;
    for (size_t i = 0; i < n; ++i)
        if (d[3 * i] < 250 || d[3 * i + 1] < 250 || d[3 * i + 2] < 250)
            ++non_white;
    return non_white < n / 200;
}

bool print_window(wxWindow *top, wxBitmap &bmp, unsigned flags)
{
    using DwmFlushFn = HRESULT(WINAPI *)();
    static DwmFlushFn dwm_flush = reinterpret_cast<DwmFlushFn>(::GetProcAddress(::LoadLibraryW(L"dwmapi.dll"), "DwmFlush"));
    ::RedrawWindow((HWND) top->GetHWND(), nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (dwm_flush)
        dwm_flush();
    const wxSize sz = top->GetSize();
    bmp.Create(sz.x, sz.y, 24);
    wxMemoryDC mdc(bmp);
    mdc.SetBackground(*wxWHITE_BRUSH);
    mdc.Clear();
    const bool ok = ::PrintWindow((HWND) top->GetHWND(), (HDC) mdc.GetHDC(), flags) != 0;
    mdc.SelectObject(wxNullBitmap);
    return ok;
}
#endif

// Copy a top-level window into a PNG. First from off-screen; if DWM left that blank, from an
// on-screen position with the window made click-through and 1/255 opaque, i.e. invisible, for the
// moment it takes. It is never activated either way.
bool capture(wxWindow *top, const wxString &path)
{
#ifdef _WIN32
    wxBitmap bmp;
    // 0: WM_PRINT, drawn by the windows themselves; 2: PW_RENDERFULLCONTENT, read back from DWM.
    bool ok = print_window(top, bmp, 0) && !looks_blank(bmp);
    if (!ok)
        ok = print_window(top, bmp, 2) && !looks_blank(bmp);
    if (!ok) {
        HWND       hwnd = (HWND) top->GetHWND();
        const LONG ex   = ::GetWindowLongW(hwnd, GWL_EXSTYLE);
        ::SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        ::SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
        const wxPoint old = top->GetPosition();
        ::SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ok = print_window(top, bmp, 2) && !looks_blank(bmp);
        ::SetWindowPos(hwnd, HWND_BOTTOM, old.x, old.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ::SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    }
    return ok && bmp.SaveFile(path, wxBITMAP_TYPE_PNG);
#else
    (void) top;
    (void) path;
    return false;
#endif
}

// The painted part of the dual view, drawn straight into a bitmap (no window system involved),
// with the child buttons outlined where they sit.
bool render_canvas(AMSDualView *view, const wxString &path)
{
    const wxSize sz = view->GetSize();
    wxBitmap     bmp(sz.x, sz.y, 24);
    {
        wxMemoryDC mdc(bmp);
        wxGCDC     dc(mdc);
        view->PaintTo(dc);
        for (wxWindow *child : view->GetChildren()) {
            if (!child->IsShown())
                continue;
            const wxRect r = child->GetRect();
            dc.SetPen(wxPen(wxColour(0, 150, 136), 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(r, r.height / 2);
            const wxString label = child->GetLabel();
            if (!label.empty()) {
                dc.SetFont(view->GetFont());
                dc.SetTextForeground(wxColour(0, 150, 136));
                const wxSize te = dc.GetTextExtent(label);
                dc.DrawText(label, r.x + (r.width - te.x) / 2, r.y + (r.height - te.y) / 2);
            }
        }
    }
    return bmp.SaveFile(path, wxBITMAP_TYPE_PNG);
}

} // namespace

bool run_ams_ui_preview_if_asked()
{
    wxString dir;
    if (!wxGetEnv("EDGESLICER_TEST_AMS_PREVIEW", &dir) || dir.IsEmpty())
        return false;
    BOOST_LOG_TRIVIAL(warning) << "EDGESLICER_TEST_AMS_PREVIEW is set: rendering the AMS preview into " << dir.ToUTF8().data();
    wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    auto out = [&dir](const wxString &name) {
        wxFileName f(dir, name);
        return f.GetFullPath();
    };
    auto log_saved = [](bool ok, const wxString &path) {
        BOOST_LOG_TRIVIAL(warning) << "AMS preview: " << (ok ? "wrote " : "FAILED ") << path.ToUTF8().data();
    };

    // No agent, not registered with the DeviceManager: this printer cannot send anything.
    auto obj = std::make_unique<MachineObject>(nullptr, "preview-h2d", "PREVIEW00000000", "0.0.0.0");
    obj->printer_type = "O1D";
    try {
        obj->parse_json(k_h2d_push);
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "AMS preview: parse_json threw";
    }
    BOOST_LOG_TRIVIAL(warning) << "AMS preview: parsed ams=" << obj->amsList.size() << " vir_slots=" << obj->vir_slots.size()
                               << " extruders=" << obj->m_extder_data.total_extder_count << " remote_dry=" << obj->is_support_remote_dry
                               << " multi=" << obj->is_multi_extruders();
    for (const auto &kv : obj->amsList)
        if (kv.second)
            BOOST_LOG_TRIVIAL(warning) << "AMS preview: ams " << kv.first << " type=" << kv.second->type << " extruder=" << kv.second->nozzle
                                       << " trays=" << kv.second->trayList.size() << " exists=" << kv.second->is_exists
                                       << " dry_status=" << kv.second->dry.status << " sub=" << kv.second->dry.sub_status;

    const bool dark_before = StateColor::IsDarkMode();

    auto *frame = new wxFrame(nullptr, wxID_ANY, "AMS preview", wxPoint(-20000, -20000), wxDefaultSize,
                              wxFRAME_NO_TASKBAR | wxFRAME_TOOL_WINDOW | wxBORDER_NONE);
    frame->SetBackgroundColour(*wxWHITE);
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    auto *ctrl  = new AMSControl(frame);
    sizer->Add(ctrl, 0, wxALL, frame->FromDIP(10));
    frame->SetSizer(sizer);
    ctrl->SetMachine(obj.get());
    ctrl->SetDualMode(obj->is_multi_extruders());
    ctrl->UpdateDual(obj.get());
    sizer->Fit(frame);
    frame->SetPosition(wxPoint(-20000, -20000));
    frame->ShowWithoutActivating();
    log_saved(capture(frame, out("ams_dual_light.png")), out("ams_dual_light.png"));
    log_saved(render_canvas(ctrl->GetDualView(), out("ams_dual_canvas_light.png")), out("ams_dual_canvas_light.png"));

    auto *dlg = new AMSDryCtrlDialog(frame);
    dlg->set_ams_id("128");
    dlg->update(obj.get());
    dlg->SetPosition(wxPoint(-20000, -18000));
    dlg->ShowWithoutActivating();
    log_saved(capture(dlg, out("ams_dry_dialog.png")), out("ams_dry_dialog.png"));

    // The same dialog for the idle AMS 2 Pro, with Start and the limits shown.
    dlg->set_ams_id("0");
    dlg->update(obj.get());
    log_saved(capture(dlg, out("ams_dry_dialog_idle.png")), out("ams_dry_dialog_idle.png"));
    dlg->Hide();
    frame->Hide();

    // Dark: switch the app's theme for this process only (the preview never saves the config and
    // ends right after), then build fresh windows so the theme is applied as it is at start-up.
    wxGetApp().app_config->set("dark_color_mode", "1");
    wxGetApp().Update_dark_mode_flag();
    StateColor::SetDarkMode(true);

    auto *dframe = new wxFrame(nullptr, wxID_ANY, "AMS preview dark", wxPoint(-20000, -20000), wxDefaultSize,
                               wxFRAME_NO_TASKBAR | wxFRAME_TOOL_WINDOW | wxBORDER_NONE);
    dframe->SetBackgroundColour(*wxWHITE);
    auto *dsizer = new wxBoxSizer(wxVERTICAL);
    auto *dctrl  = new AMSControl(dframe);
    dsizer->Add(dctrl, 0, wxALL, dframe->FromDIP(10));
    dframe->SetSizer(dsizer);
    dctrl->SetMachine(obj.get());
    dctrl->SetDualMode(obj->is_multi_extruders());
    dctrl->UpdateDual(obj.get());
    wxGetApp().UpdateDarkUIWin(dframe);
    dsizer->Fit(dframe);
    dframe->SetPosition(wxPoint(-20000, -20000));
    dframe->ShowWithoutActivating();
    log_saved(capture(dframe, out("ams_dual_dark.png")), out("ams_dual_dark.png"));
    log_saved(render_canvas(dctrl->GetDualView(), out("ams_dual_canvas_dark.png")), out("ams_dual_canvas_dark.png"));

    auto *ddlg = new AMSDryCtrlDialog(dframe);
    ddlg->set_ams_id("128");
    ddlg->update(obj.get());
    ddlg->SetPosition(wxPoint(-20000, -18000));
    ddlg->ShowWithoutActivating();
    log_saved(capture(ddlg, out("ams_dry_dialog_dark.png")), out("ams_dry_dialog_dark.png"));
    ddlg->set_ams_id("0");
    ddlg->update(obj.get());
    log_saved(capture(ddlg, out("ams_dry_dialog_idle_dark.png")), out("ams_dry_dialog_idle_dark.png"));
    ddlg->Hide();
    dframe->Hide();
    (void) dark_before;
    // The caller ends the process next; the windows still point at the made-up printer, so keep it.
    obj.release();
    return true;
}

}} // namespace Slic3r::GUI
