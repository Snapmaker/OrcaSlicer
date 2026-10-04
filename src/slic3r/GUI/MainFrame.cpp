#include "MainFrame.hpp"
#include "PrintSelectKeys.hpp"

#include <wx/panel.h>
#include <wx/notebook.h>
#include <wx/listbook.h>
#include <wx/simplebook.h>
#include <wx/icon.h>
#include <wx/sizer.h>
#include <wx/menu.h>
#include <wx/progdlg.h>
#include <wx/textentry.h>
#include <wx/tooltip.h>
//#include <wx/glcanvas.h>
#include <wx/filename.h>
#include <wx/debug.h>
#include <wx/utils.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/StartupProfile.hpp"
#include "libslic3r/Zipper.hpp"

#include "Tab.hpp"
#include "ProgressStatusBar.hpp"
#include "3DScene.hpp"
#include "ParamsDialog.hpp"
#include "PrintHostDialogs.hpp"
#include "../Utils/PrintHostDevices.hpp"
#include "wxExtensions.hpp"
#include "GUI_ObjectList.hpp"
#include "Mouse3DController.hpp"
//#include "RemovableDriveManager.hpp"
#include "InstanceCheck.hpp"
#include "I18N.hpp"
#include "GLCanvas3D.hpp"
#include "Plater.hpp"
#include "RemoteAccess.hpp"
#include "FlashForge/FFDeviceTab.hpp"
#include "WebViewDialog.hpp"
#include "HomePanel.hpp"
#include "../Utils/Process.hpp"
#include "format.hpp"
// BBS
#include "PartPlate.hpp"
#include "Preferences.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "BindDialog.hpp"
#include "../Utils/MacDarkMode.hpp"

#include <fstream>
#include <string_view>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <string>
#include <vector>
#include <ctime>

#include "GUI_App.hpp"
#include "DualNozzleState.hpp"
#include "FlowTypeHelper.hpp"
#include "SliceModePopup.hpp"
#include "UnsavedChangesDialog.hpp"
#include "MsgDialog.hpp"
#include "Notebook.hpp"
#include "Theme.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "NotificationManager.hpp"
#include "MarkdownTip.hpp"
#include "NetworkTestDialog.hpp"
#include "FirewallCheckDialog.hpp"
#include "BambuSetupNoticeDialog.hpp"
#include "ConfigWizard.hpp"
#include "Widgets/WebView.hpp"
#include "DailyTips.hpp"
#include "SliceCompare/SliceCompareFrame.hpp"

#ifdef _WIN32
#include <dbt.h>
#include <shlobj.h>
#include <shellapi.h>
#endif // _WIN32
#include "../Utils/MQTT.hpp"
#include "../Utils/MqttReconnectPolicy.hpp"
#include <slic3r/GUI/CreatePresetsDialog.hpp>
#include "sentry_wrapper/SentryWrapper.hpp"
#include "GenericDownloadDialog.hpp"

#define UPDATE_BUSER    true
#define UPDATE_BUAUTO   false

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_SELECT_TAB, wxCommandEvent);
wxDEFINE_EVENT(EVT_HTTP_ERROR, wxCommandEvent);
wxDEFINE_EVENT(EVT_SHOW_IP_DIALOG, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_MACHINE_LIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_PRESET_CB, SimpleEvent);
wxDEFINE_EVENT(EVT_NETWORK_TEST_LOG_UPDATE, wxCommandEvent);



// BBS: backup
wxDEFINE_EVENT(EVT_BACKUP_POST, wxCommandEvent);
wxDEFINE_EVENT(EVT_LOAD_URL, wxCommandEvent);
wxDEFINE_EVENT(EVT_LOAD_PRINTER_URL, LoadPrinterViewEvent);

/**
 * @brief Checks whether a wxWidgets text input currently owns the focus.
 * @return True when keyboard input belongs to a text control.
 */
static bool IsTextEntryFocused()
{
    for (wxWindow* window = wxWindow::FindFocus(); window != nullptr; window = window->GetParent())
    {
        if (dynamic_cast<wxTextEntry*>(window) != nullptr)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Checks whether the Fit Camera shortcut must be passed through.
 * @param plater Current plater instance.
 * @return True when the shortcut must not trigger Fit Camera.
 */
static bool ShouldSkipFitCameraShortcut(Plater* plater)
{
    if (plater == nullptr || IsTextEntryFocused())
    {
        return true;
    }

    ImGuiWrapper* const imgui = wxGetApp().imgui();
    if (imgui != nullptr && (imgui->want_keyboard() || imgui->want_text_input()))
    {
        return true;
    }

    GLCanvas3D* const currentCanvas = plater->get_current_canvas3D();
    if (currentCanvas == nullptr)
    {
        return true;
    }

    GLCanvas3D* const viewCanvas = plater->get_view3D_canvas3D();
    GLCanvas3D* const assembleCanvas = plater->get_assmeble_canvas3D();
    return (viewCanvas != nullptr && viewCanvas->get_gizmos_manager().is_running()) ||
           (assembleCanvas != nullptr && assembleCanvas->get_gizmos_manager().is_running()) ||
           currentCanvas->get_gizmos_manager().is_running();
}

enum class ERescaleTarget
{
    Mainframe,
    SettingsDialog
};

#ifdef __APPLE__
class Snapmaker_OrcaTaskBarIcon : public wxTaskBarIcon
{
public:
    Snapmaker_OrcaTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        if (wxGetApp().app_config->get("single_instance") == "false") {
            // Only allow opening a new PrusaSlicer instance on OSX if "single_instance" is disabled,
            // as starting new instances would interfere with the locking mechanism of "single_instance" support.
            append_menu_item(menu, wxID_ANY, _L("New Window"), _L("Open a new window"),
            [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr);
        }
//        append_menu_item(menu, wxID_ANY, _L("G-code Viewer") + dots, _L("Open G-code Viewer"),
//            [](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};
/*class GCodeViewerTaskBarIcon : public wxTaskBarIcon
{
public:
    GCodeViewerTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        append_menu_item(menu, wxID_ANY, _L("Open PrusaSlicer"), _L("Open a new PrusaSlicer"),
            [](wxCommandEvent&) { start_new_slicer(nullptr, true); }, "", nullptr);
        //append_menu_item(menu, wxID_ANY, _L("G-code Viewer") + dots, _L("Open new G-code Viewer"),
        //    [](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};*/
#endif // __APPLE__

// Load the icon either from the exe, or from the ico file.
static wxIcon main_frame_icon(GUI_App::EAppMode app_mode)
{
#if _WIN32
    std::wstring path(size_t(MAX_PATH), wchar_t(0));
    int len = int(::GetModuleFileName(nullptr, path.data(), MAX_PATH));
    if (len > 0 && len < MAX_PATH) {
        path.erase(path.begin() + len, path.end());
        //BBS: remove GCodeViewer as seperate APP logic
        /*if (app_mode == GUI_App::EAppMode::GCodeViewer) {
            // Only in case the slicer was started with --gcodeviewer parameter try to load the icon from prusa-gcodeviewer.exe
            // Otherwise load it from the exe.
            for (const std::wstring_view exe_name : { std::wstring_view(L"prusa-slicer.exe"), std::wstring_view(L"prusa-slicer-console.exe") })
                if (boost::iends_with(path, exe_name)) {
                    path.erase(path.end() - exe_name.size(), path.end());
                    path += L"prusa-gcodeviewer.exe";
                    break;
                }
        }*/
    }
    return wxIcon(path, wxBITMAP_TYPE_ICO);
#else // _WIN32
    return wxIcon(Slic3r::var("Snapmaker_Orca_128px.png"), wxBITMAP_TYPE_PNG);
#endif // _WIN32
}

// BBS
#ifndef __APPLE__
#define BORDERLESS_FRAME_STYLE (wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX)
#else
#define BORDERLESS_FRAME_STYLE (wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX)
#endif

wxDEFINE_EVENT(EVT_SYNC_CLOUD_PRESET,     SimpleEvent);

#ifdef __APPLE__
static const wxString ctrl = ("Ctrl+");
// FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
static const std::string ctrl_t = u8"\u2318+"; // "⌘" (Mac Command)
#else
static const wxString ctrl = _L("Ctrl+");
// FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
static const wxString ctrl_t = ctrl;
#endif
static const wxString shift = _L("Shift+");

MainFrame::MainFrame() :
DPIFrame(NULL, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, BORDERLESS_FRAME_STYLE, "mainframe")
    , m_printhost_queue_dlg(new PrintHostQueueDialog(this))
    // BBS
    , m_recent_projects(18)
    , m_settings_dialog(this)
    , diff_dialog(this)
{
#ifdef __WXOSX__
    set_miniaturizable(GetHandle());
#endif

    if (!wxGetApp().app_config->has("user_mode")) {
        wxGetApp().app_config->set("user_mode", "simple");
        wxGetApp().app_config->set_bool("developer_mode", false);
        wxGetApp().app_config->save();
    }

    wxGetApp().app_config->set_bool("internal_developer_mode", false);

    wxString max_recent_count_str = wxGetApp().app_config->get("max_recent_count");
    long max_recent_count = 18;
    if (max_recent_count_str.ToLong(&max_recent_count))
        set_max_recent_count((int)max_recent_count);

    //reset log level
    auto loglevel = wxGetApp().app_config->get("log_severity_level");
    Slic3r::set_logging_level(Slic3r::level_string_to_boost(loglevel));

    // BBS
    m_recent_projects.SetMenuPathStyle(wxFH_PATH_SHOW_ALWAYS);
    MarkdownTip::Recreate(this);

    // Fonts were created by the DPIFrame constructor for the monitor, on which the window opened.
    wxGetApp().update_fonts(this);

#ifndef __APPLE__
    m_topbar         = new BBLTopbar(this);
#else
    auto panel_topbar = new wxPanel(this, wxID_ANY);
    panel_topbar->SetBackgroundColour(Theme::colour("titlebar_bg", wxColour(38, 46, 48)));
    m_mac_topbar_panel = panel_topbar;
    auto sizer_tobar = new wxBoxSizer(wxVERTICAL);
    panel_topbar->SetSizer(sizer_tobar);
    panel_topbar->Layout();
#endif

    //wxAuiToolBar* toolbar = new wxAuiToolBar();
/*
#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
    this->SetFont(this->normal_font());
#endif
    // Font is already set in DPIFrame constructor
*/

#ifdef __APPLE__
	m_reset_title_text_colour_timer = new wxTimer();
	m_reset_title_text_colour_timer->SetOwner(this);
	Bind(wxEVT_TIMER, [this](auto& e) {
		set_title_colour_after_set_title(GetHandle());
		m_reset_title_text_colour_timer->Stop();
	});
	this->Bind(wxEVT_FULLSCREEN, [this](wxFullScreenEvent& e) {
		set_tag_when_enter_full_screen(e.IsFullScreen());
		if (!e.IsFullScreen()) {
            if (m_reset_title_text_colour_timer) {
                m_reset_title_text_colour_timer->Stop();
                m_reset_title_text_colour_timer->Start(500);
            }
		}
		e.Skip();
	});
#endif
#ifdef __APPLE__
    // Initialize the docker task bar icon.
    switch (wxGetApp().get_app_mode()) {
    default:
    case GUI_App::EAppMode::Editor:
        m_taskbar_icon = std::make_unique<Snapmaker_OrcaTaskBarIcon>(wxTBI_DOCK);
        m_taskbar_icon->SetIcon(wxIcon(Slic3r::var("EdgeSlicer-mac_256px.ico"), wxBITMAP_TYPE_ICO), "EdgeSlicer");
        break;
    case GUI_App::EAppMode::GCodeViewer:
        break;
    }
#endif // __APPLE__

    // Load the icon either from the exe, or from the ico file.
    SetIcon(main_frame_icon(wxGetApp().get_app_mode()));

#ifdef __WXGTK__
    // GTK/X11: set minimum size before any layout so gtk_widget_set_size_request and
    // gtk_window_resize never see 0 or negative dimensions (avoids assertion failures).
    SetMinSize(wxGetApp().get_min_size());
#endif

    // initialize tabpanel and menubar
    {
        Slic3r::StartupScopedTimer t("MainFrame ctor step=init_tabpanel");
        init_tabpanel();
    }
    {
        Slic3r::StartupScopedTimer t("MainFrame ctor step=init_menubar");
        if (wxGetApp().is_gcode_viewer())
            init_menubar_as_gcodeviewer();
        else
            init_menubar_as_editor();
    }

    // BBS
#if 0
    // This is needed on Windows to fake the CTRL+# of the window menu when using the numpad
    wxAcceleratorEntry entries[6];
    entries[0].Set(wxACCEL_CTRL, WXK_NUMPAD1, wxID_HIGHEST + 1);
    entries[1].Set(wxACCEL_CTRL, WXK_NUMPAD2, wxID_HIGHEST + 2);
    entries[2].Set(wxACCEL_CTRL, WXK_NUMPAD3, wxID_HIGHEST + 3);
    entries[3].Set(wxACCEL_CTRL, WXK_NUMPAD4, wxID_HIGHEST + 4);
    entries[4].Set(wxACCEL_CTRL, WXK_NUMPAD5, wxID_HIGHEST + 5);
    entries[5].Set(wxACCEL_CTRL, WXK_NUMPAD6, wxID_HIGHEST + 6);
    wxAcceleratorTable accel(6, entries);
    SetAcceleratorTable(accel);
#endif // _WIN32

    // BBS
    //wxAcceleratorEntry entries[13];
    //int index = 0;
    //entries[index++].Set(wxACCEL_CTRL, (int)'N', wxID_HIGHEST + wxID_NEW);
    //entries[index++].Set(wxACCEL_CTRL, (int)'O', wxID_HIGHEST + wxID_OPEN);
    //entries[index++].Set(wxACCEL_CTRL, (int)'S', wxID_HIGHEST + wxID_SAVE);
    //entries[index++].Set(wxACCEL_CTRL | wxACCEL_SHIFT, (int)'S', wxID_HIGHEST + wxID_SAVEAS);
    //entries[index++].Set(wxACCEL_CTRL, (int)'X', wxID_HIGHEST + wxID_CUT);
    ////entries[index++].Set(wxACCEL_CTRL, (int)'I', wxID_HIGHEST + wxID_ADD);
    //entries[index++].Set(wxACCEL_CTRL, (int)'A', wxID_HIGHEST + wxID_SELECTALL);
    //entries[index++].Set(wxACCEL_NORMAL, (int)27 /* escape */, wxID_HIGHEST + wxID_CANCEL);
    //entries[index++].Set(wxACCEL_CTRL, (int)'Z', wxID_HIGHEST + wxID_UNDO);
    //entries[index++].Set(wxACCEL_CTRL, (int)'Y', wxID_HIGHEST + wxID_REDO);
    //entries[index++].Set(wxACCEL_CTRL, (int)'C', wxID_HIGHEST + wxID_COPY);
    //entries[index++].Set(wxACCEL_CTRL, (int)'V', wxID_HIGHEST + wxID_PASTE);
    //entries[index++].Set(wxACCEL_CTRL, (int)'P', wxID_HIGHEST + wxID_PREFERENCES);
    //entries[index++].Set(wxACCEL_CTRL, (int)'I', wxID_HIGHEST + wxID_FILE6);
    //wxAcceleratorTable accel(sizeof(entries) / sizeof(entries[0]), entries);
    //SetAcceleratorTable(accel);

    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->new_project(); }, wxID_HIGHEST + wxID_NEW);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->load_project(); }, wxID_HIGHEST + wxID_OPEN);
    //// BBS: close save project
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(); }, wxID_HIGHEST + wxID_SAVE);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(true); }, wxID_HIGHEST + wxID_SAVEAS);
    ////Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (m_plater) m_plater->add_model(); }, wxID_HIGHEST + wxID_ADD);
    ////Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->remove_selected(); }, wxID_HIGHEST + wxID_DELETE);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    //        if (!can_add_models())
    //            return;
    //        if (m_plater) {
    //            m_plater->add_model();
    //        }
    //    }, wxID_HIGHEST + wxID_FILE6);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->select_all(); }, wxID_HIGHEST + wxID_SELECTALL);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->deselect_all(); }, wxID_HIGHEST + wxID_CANCEL);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    //    if (m_plater->is_view3D_shown())
    //        m_plater->undo();
    //    }, wxID_HIGHEST + wxID_UNDO);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    //    if (m_plater->is_view3D_shown())
    //        m_plater->redo();
    //    }, wxID_HIGHEST + wxID_REDO);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->copy_selection_to_clipboard(); }, wxID_HIGHEST + wxID_COPY);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->paste_from_clipboard(); }, wxID_HIGHEST + wxID_PASTE);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->cut_selection_to_clipboard(); }, wxID_HIGHEST + wxID_CUT);
    Bind(wxEVT_SIZE, [this](wxSizeEvent&) {
            BOOST_LOG_TRIVIAL(trace) << "mainframe: size changed, is maximized = " << this->IsMaximized();
#ifndef __APPLE__
            if (this->IsMaximized()) {
                m_topbar->SetWindowSize();
            } else {
                m_topbar->SetMaximizedSize();
            }
#endif
        Refresh();
        Layout();
        });

    //BBS
    Bind(EVT_SELECT_TAB, [this](wxCommandEvent&evt) {
        TabPosition pos = (TabPosition)evt.GetInt();
        m_tabpanel->SetSelection(pos);
    });

    // Profiling aid for the lazily built tabs: with ORCA_STARTUP_PROFILE_OPEN_TABS set to
    // a comma-separated list of TabPosition indices, select each of them once the frame is
    // up. The deferred construction then shows up in the same "[StartupProfile] ... lazy="
    // marks as the rest of the startup, which is how the win from deferring them is
    // measured. Inert unless the variable is set.
    if (const char* tabs = std::getenv("ORCA_STARTUP_PROFILE_OPEN_TABS"); tabs && *tabs) {
        std::vector<int>  positions;
        std::stringstream ss{std::string(tabs)};
        std::string       piece;
        while (std::getline(ss, piece, ',')) {
            try { positions.push_back(std::stoi(piece)); } catch (...) {}
        }
        // Run once the app has settled, so the marks measure the tab build and not the
        // tail of startup still running around it.
        wxTimer* open_tabs_timer = new wxTimer(this);
        Bind(wxEVT_TIMER, [this, positions](wxTimerEvent& e) {
            for (int pos : positions) {
                if (pos < 0 || pos >= (int) m_tabpanel->GetPageCount())
                    continue;
                Slic3r::startup_profile_log("open_tab begin index=" + std::to_string(pos));
                m_tabpanel->SetSelection(pos);
                Slic3r::startup_profile_log("open_tab end index=" + std::to_string(pos));
            }
        }, open_tabs_timer->GetId());
        open_tabs_timer->StartOnce(8000);
    }

    Bind(EVT_SYNC_CLOUD_PRESET, &MainFrame::on_select_default_preset, this);
    Bind(EVT_NETWORK_TEST_LOG_UPDATE, [](wxCommandEvent& evt) {
        BOOST_LOG_TRIVIAL(warning) << "[NetworkTest] " << into_u8(evt.GetString());
    });

//    Bind(wxEVT_MENU,
//        [this](wxCommandEvent&)
//        {
//            PreferencesDialog dlg(this);
//            dlg.ShowModal();
//#if ENABLE_GCODE_LINES_ID_IN_H_SLIDER
//            if (dlg.seq_top_layer_only_changed() || dlg.seq_seq_top_gcode_indices_changed())
//#else
//            if (dlg.seq_top_layer_only_changed())
//#endif // ENABLE_GCODE_LINES_ID_IN_H_SLIDER
//                plater()->refresh_print();
//        }, wxID_HIGHEST + wxID_PREFERENCES);


    // set default tooltip timer in msec
    // SetAutoPop supposedly accepts long integers but some bug doesn't allow for larger values
    // (SetAutoPop is not available on GTK.)
    wxToolTip::SetAutoPop(32767);

    m_loaded = true;

    // initialize layout
    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    wxSizer* sizer = new wxBoxSizer(wxVERTICAL);
#ifndef __APPLE__
     sizer->Add(m_topbar, 0, wxEXPAND);
#else
     sizer->Add(panel_topbar, 0, wxEXPAND);
#endif // __WINDOWS__


    sizer->Add(m_main_sizer, 1, wxEXPAND);
    SetSizerAndFit(sizer);
    // initialize layout from config
    update_layout();
    sizer->SetSizeHints(this);

#ifdef WIN32
    // SetMaximize causes the window to overlap the taskbar, due to the fact this window has wxMAXIMIZE_BOX off
    // https://forums.wxwidgets.org/viewtopic.php?t=50634
    // Fix it here
    this->Bind(wxEVT_MAXIMIZE, [this](auto &e) {
        wxDisplay display(this);
        auto      size = display.GetClientArea().GetSize();
        auto      pos  = display.GetClientArea().GetPosition();
        HWND      hWnd = GetHandle();
        RECT      borderThickness;
        SetRectEmpty(&borderThickness);
        AdjustWindowRectEx(&borderThickness, GetWindowLongPtr(hWnd, GWL_STYLE), FALSE, 0);
        const auto max_size = size + wxSize{-borderThickness.left + borderThickness.right, -borderThickness.top + borderThickness.bottom};
        const auto current_size = GetSize();
        SetSize({std::min(max_size.x, current_size.x), std::min(max_size.y, current_size.y)});
        Move(pos + wxPoint{borderThickness.left, borderThickness.top});
        e.Skip();
    });
#endif // WIN32
    // BBS
    Fit();

    const wxSize min_size = wxGetApp().get_min_size(); //wxSize(76*wxGetApp().em_unit(), 49*wxGetApp().em_unit());

    SetMinSize(min_size/*wxSize(760, 490)*/);
    SetSize(wxSize(FromDIP(1200), FromDIP(800)));

    Layout();

    update_title();

    // declare events
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": mainframe received close_widow event";
        // Ultra: a hub-managed instance closes to the hub's tray menu, not to the grave. Only
        // request_quit() (tray, hub page, POST /api/quit, File > Quit) really ends it.
        const bool quit_requested = m_quit_requested;
        m_quit_requested = false;
        if (wxGetApp().is_hub_managed() && !quit_requested && event.CanVeto()) {
            event.Veto();
            Hide();
            RemoteAccess::get().set_hidden(true);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": hub-managed instance: the close hid the window, still running";
            return;
        }
        if (event.CanVeto() && m_plater->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(true)) {
            // prevents to open the save dirty project dialog
            event.Veto();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< "cancelled by gizmo in editing";
            return;
        }

        //BBS:
        //if (event.CanVeto() && !wxGetApp().check_and_save_current_preset_changes(_L("Application is closing"), _L("Closing Application while some presets are modified."))) {
        //    event.Veto();
        //    return;
        //}
        // Ultra: never prompt about modified presets on exit - the project 3mf
        // carries the live settings, so unsaved preset edits are simply dropped.
        auto check = [](bool /*yes_or_no*/) { return true; };

        // BBS: close save project
        int result;
        if (event.CanVeto() && ((result = m_plater->close_with_confirm(check)) == wxID_CANCEL)) {
            event.Veto();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< "cancelled by close_with_confirm selection";
            return;
        }
        if (event.CanVeto() && !wxGetApp().check_print_host_queue()) {
            event.Veto();
            return;
        }

    #if 0 // BBS
        //if (m_plater != nullptr) {
        //    int saved_project = m_plater->save_project_if_dirty(_L("Closing Application. Current project is modified."));
        //    if (saved_project == wxID_CANCEL) {
        //        event.Veto();
        //        return;
        //    }
        //    // check unsaved changes only if project wasn't saved
        //    else if (plater()->is_project_dirty() && saved_project == wxID_NO && event.CanVeto() &&
        //             (plater()->is_presets_dirty() && !wxGetApp().check_and_save_current_preset_changes(_L("Application is closing"), _L("Closing Application while some presets are modified.")))) {
        //        event.Veto();
        //        return;
        //    }
        //}
    #endif

        MarkdownTip::ExitTip();

        m_plater->reset();
        this->shutdown();
        // propagate event

        wxGetApp().remove_mall_system_dialog();
        event.Skip();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": mainframe finished process close_widow event";
    });

    //FIXME it seems this method is not called on application start-up, at least not on Windows. Why?
    // The same applies to wxEVT_CREATE, it is not being called on startup on Windows.
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (m_plater != nullptr && event.GetActive())
            m_plater->on_activate();
        event.Skip();
    });

// OSX specific issue:
// When we move application between Retina and non-Retina displays, The legend on a canvas doesn't redraw
// So, redraw explicitly canvas, when application is moved
//FIXME maybe this is useful for __WXGTK3__ as well?
#if __APPLE__
    Bind(wxEVT_MOVE, [](wxMoveEvent& event) {
        GLCanvas3D* canvas = wxGetApp().plater()->get_current_canvas3D();
        if (canvas) {
            canvas->set_as_dirty();
            canvas->request_extra_frame();
        }
        event.Skip();
    });
#endif

    update_ui_from_settings();    // FIXME (?)

    if (m_plater != nullptr) {
        // BBS
        update_slice_print_status(eEventSliceUpdate, true, true);

        // BBS: backup project
        if (wxGetApp().app_config->get("backup_switch") == "true") {
            std::string backup_interval;
            if (!wxGetApp().app_config->get("app", "backup_interval", backup_interval))
                backup_interval = "10";
            Slic3r::set_backup_interval(boost::lexical_cast<long>(backup_interval));
        } else {
            Slic3r::set_backup_interval(0);
        }
        Slic3r::set_backup_callback([this](int action) {
            if (action == 0) {
                wxPostEvent(this, wxCommandEvent(EVT_BACKUP_POST));
            }
            else if (action == 1) {
                if (!m_plater->up_to_date(false, true)) {
                    m_plater->export_3mf(m_plater->model().get_backup_path() + "/.3mf", SaveStrategy::Backup);
                    m_plater->up_to_date(true, true);
                }
            }
         });
        Bind(EVT_BACKUP_POST, [](wxCommandEvent& e) {
            Slic3r::run_backup_ui_tasks();
            });

        // BBS: auto-save the project to its own file, in addition to auto-backup
        m_autosave_timer = new wxTimer(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent& e) {
            if (m_plater == nullptr)
                return;
            // Only save projects that already have a file on disk; never pop a Save-As dialog.
            if (m_plater->get_project_filename(".3mf").IsEmpty())
                return;
            if (!m_plater->is_project_dirty())
                return;
            if (m_plater->save_project(false) != wxID_YES) {
                // The failed save already showed an error dialog; stop retrying every interval.
                m_autosave_timer->Stop();
            }
        }, m_autosave_timer->GetId());
        update_autosave_timer();
;    }
    this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &evt) {
#ifdef __APPLE__
        if (evt.CmdDown() && (evt.GetKeyCode() == 'H')) {
            //call parent_menu hide behavior
            return;}
        if (evt.CmdDown() && (!evt.ShiftDown()) && (evt.GetKeyCode() == 'M')) {
            this->Iconize();
            return;
        }
        if (evt.CmdDown() && evt.GetKeyCode() == 'Q') { wxPostEvent(this, wxCloseEvent(wxEVT_CLOSE_WINDOW)); return;}
        if (evt.CmdDown() && evt.RawControlDown() && evt.GetKeyCode() == 'F') {
            EnableFullScreenView(true);
            if (IsFullScreen()) {
                ShowFullScreen(false);
            } else {
                ShowFullScreen(true);
            }
            return;}
#endif
        if (evt.CmdDown() && evt.GetKeyCode() == 'R')
        {
            if (m_slice_enable)
            {
                wxGetApp().plater()->update(true, true);
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
            }
            return;
        }
        if (evt.CmdDown() && evt.ShiftDown() && evt.GetKeyCode() == 'G') {
            m_plater->apply_background_progress();
            m_print_enable = get_enable_print_status();
            m_print_btn->Enable(m_print_enable);
            if (m_print_enable) {
                if (wxGetApp().preset_bundle->use_bbl_network())
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_PLATE));
                else
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_GCODE));
            }
            evt.Skip();
            return;
        }
        else if (evt.CmdDown() && evt.GetKeyCode() == 'G') { if (can_export_gcode()) { wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE)); } evt.Skip(); return; }
        if (evt.CmdDown() && evt.GetKeyCode() == 'J') { m_printhost_queue_dlg->Show(); return; }
        if (evt.CmdDown() && evt.GetKeyCode() == 'N') { m_plater->new_project(); return;}
        if (evt.CmdDown() && evt.GetKeyCode() == 'O') { m_plater->load_project(); return;}
        if (evt.CmdDown() && evt.ShiftDown() && evt.GetKeyCode() == 'S') { if (can_save_as()) m_plater->save_project(true); return;}
        else if (evt.CmdDown() && evt.GetKeyCode() == 'S') { if (can_save()) m_plater->save_project(); return;}
        if (evt.CmdDown() && evt.GetKeyCode() == 'F') {
            if (m_plater && (m_tabpanel->GetSelection() == TabPosition::tp3DEditor || m_tabpanel->GetSelection() == TabPosition::tpPreview)) {
                m_plater->sidebar().can_search();
            }
        }
#ifdef __APPLE__
        if (evt.CmdDown() && evt.GetKeyCode() == ',')
#else
        if (evt.CmdDown() && evt.GetKeyCode() == 'P')
#endif
        {
            // Orca: Use GUI_App::open_preferences instead of direct call so windows associations are updated on exit
            wxGetApp().open_preferences();
            plater()->get_current_canvas3D()->force_set_focus();
            return;
        }

        if (evt.CmdDown() && evt.GetKeyCode() == 'I') {
            if (!can_add_models()) return;
            if (m_plater) { m_plater->add_file(); }
            return;
        }

        const bool isFitCameraShortcut = !evt.HasAnyModifiers() &&
                                         (evt.GetKeyCode() == 'Z' || evt.GetKeyCode() == 'z');
        if (isFitCameraShortcut)
        {
            if (ShouldSkipFitCameraShortcut(m_plater) || !can_change_view())
            {
                evt.Skip();
            }
            else
            {
                ZoomCameraToFit();
            }

            return;
        }

        evt.Skip();
    });

#ifdef _MSW_DARK_MODE
    wxGetApp().UpdateDarkUIWin(this);
#endif // _MSW_DARK_MODE

    wxGetApp().persist_window_geometry(this, true);
    wxGetApp().persist_window_geometry(&m_settings_dialog, true);
    // bind events from DiffDlg

    bind_diff_dialog();
}

void MainFrame::bind_diff_dialog()
{
    auto get_tab = [](Preset::Type type) {
        Tab* null_tab = nullptr;
        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->type() == type)
                return tab;
        return null_tab;
    };

    auto transfer = [this, get_tab](Preset::Type type) {
        get_tab(type)->transfer_options(diff_dialog.get_left_preset_name(type),
                                        diff_dialog.get_right_preset_name(type),
                                        diff_dialog.get_selected_options(type));
    };

    auto process_options = [this](std::function<void(Preset::Type)> process) {
        const Preset::Type diff_dlg_type = diff_dialog.view_type();
        if (diff_dlg_type == Preset::TYPE_INVALID) {
            for (const Preset::Type& type : diff_dialog.types_list() )
                process(type);
        }
        else
            process(diff_dlg_type);
    };

    diff_dialog.Bind(EVT_DIFF_DIALOG_TRANSFER,      [process_options, transfer](SimpleEvent&)         { process_options(transfer); });
}


#ifdef __WIN32__

// Orca: Fix maximized window overlaps taskbar when taskbar auto hide is enabled (#8085)
// Adopted from https://gist.github.com/MortenChristiansen/6463580
static void AdjustWorkingAreaForAutoHide(const HWND hWnd, MINMAXINFO* mmi)
{
    const auto taskbarHwnd = FindWindowA("Shell_TrayWnd", nullptr);
    if (!taskbarHwnd) {
        return;
    }
    const auto monitorContainingApplication = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONULL);
    const auto monitorWithTaskbarOnIt = MonitorFromWindow(taskbarHwnd, MONITOR_DEFAULTTONULL);
    if (monitorContainingApplication != monitorWithTaskbarOnIt) {
        return;
    }
    APPBARDATA abd;
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd   = taskbarHwnd;

    // Find if task bar has auto-hide enabled
    const auto uState = (UINT) SHAppBarMessage(ABM_GETSTATE, &abd);
    if ((uState & ABS_AUTOHIDE) != ABS_AUTOHIDE) {
        return;
    }

    RECT borderThickness;
    SetRectEmpty(&borderThickness);
    AdjustWindowRectEx(&borderThickness, GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION, FALSE, 0);

    // Determine taskbar position
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    const auto& rc = abd.rc;
    if (rc.top == rc.left && rc.bottom > rc.right) {
        // Left
        const auto offset = borderThickness.left + 2;
        mmi->ptMaxPosition.x += offset;
        mmi->ptMaxTrackSize.x -= offset;
        mmi->ptMaxSize.x -= offset;
    } else if (rc.top == rc.left && rc.bottom < rc.right) {
        // Top
        const auto offset = borderThickness.top + 2;
        mmi->ptMaxPosition.y += offset;
        mmi->ptMaxTrackSize.y -= offset;
        mmi->ptMaxSize.y -= offset;
    } else if (rc.top > rc.left) {
        // Bottom
        const auto offset = borderThickness.bottom + 2;
        mmi->ptMaxSize.y -= offset;
        mmi->ptMaxTrackSize.y -= offset;
    } else {
        // Right
        const auto offset = borderThickness.right + 2;
        mmi->ptMaxSize.x -= offset;
        mmi->ptMaxTrackSize.x -= offset;
    }
}

WXLRESULT MainFrame::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    HWND hWnd = GetHandle();
    /* When we have a custom titlebar in the window, we don't need the non-client area of a normal window
     * to be painted. In order to achieve this, we handle the "WM_NCCALCSIZE" which is responsible for the
     * size of non-client area of a window and set the return value to 0. Also we have to tell the
     * application to not paint this area on activate and deactivation events so we also handle
     * "WM_NCACTIVATE" message. */
    switch (nMsg) {
    case WM_NCACTIVATE: {
        /* Returning 0 from this message disable the window from receiving activate events which is not
        desirable. However When a visual style is not active (?) for this window, "lParam" is a handle to an
        optional update region for the nonclient area of the window. If this parameter is set to -1,
        DefWindowProc does not repaint the nonclient area to reflect the state change. */
        lParam = -1;
        break;
    }
    /* To remove the standard window frame, you must handle the WM_NCCALCSIZE message, specifically when
    its wParam value is TRUE and the return value is 0 */
    case WM_NCCALCSIZE:
        if (wParam) {
            /* Detect whether window is maximized or not. We don't need to change the resize border when win is
             *  maximized because all resize borders are gone automatically */
            WINDOWPLACEMENT wPos;
            // GetWindowPlacement fail if this member is not set correctly.
            wPos.length = sizeof(wPos);
            GetWindowPlacement(hWnd, &wPos);
            if (wPos.showCmd != SW_SHOWMAXIMIZED) {
                RECT borderThickness;
                SetRectEmpty(&borderThickness);
                AdjustWindowRectEx(&borderThickness, GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION, FALSE, NULL);
                borderThickness.left *= -1;
                borderThickness.top *= -1;
                NCCALCSIZE_PARAMS *sz = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
                // Add 1 pixel to the top border to make the window resizable from the top border
                sz->rgrc[0].top += 1; // borderThickness.top;
                sz->rgrc[0].left += borderThickness.left;
                sz->rgrc[0].right -= borderThickness.right;
                sz->rgrc[0].bottom -= borderThickness.bottom;
                return 0;
            }
        }
        break;

    case WM_GETMINMAXINFO: {
        auto mmi = (MINMAXINFO*) lParam;
        HandleGetMinMaxInfo(mmi);
        AdjustWorkingAreaForAutoHide(hWnd, mmi);
        return 0;
    }

    /* The PC woke up. Handled here rather than through wxEVT_POWER_RESUME because Windows only sends
     * PBT_APMRESUMESUSPEND (the one wx maps) when a person woke the machine; PBT_APMRESUMEAUTOMATIC is
     * the one that always arrives. Both may arrive for one wake; on_system_resume() coalesces them.
     * The message still falls through to wx so its own power events keep working. */
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
            on_system_resume();
        break;
    }
    return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
}

#endif

void MainFrame::on_system_resume()
{
    // Not straight away: the wake message arrives before Wi-Fi / Ethernet is back, and a connect
    // attempt made now would only burn one retry. The one-shot timer also folds the two resume
    // messages of a single wake into one bounce.
    if (m_resume_reconnect_timer == nullptr) {
        m_resume_reconnect_timer = new wxTimer(this);
        Bind(wxEVT_TIMER, [](wxTimerEvent&) {
            const size_t asked = MqttClient::reconnect_all_live("system resume");
            BOOST_LOG_TRIVIAL(info) << "[MainFrame] system resume: " << asked << " MQTT client(s) asked to reconnect";
        }, m_resume_reconnect_timer->GetId());
    }
    BOOST_LOG_TRIVIAL(info) << "[MainFrame] system resume; MQTT sessions will be bounced in "
                            << MqttReconnectPolicy::RESUME_SETTLE_MS << " ms";
    m_resume_reconnect_timer->StartOnce(int(MqttReconnectPolicy::RESUME_SETTLE_MS));
}

void  MainFrame::show_log_window()
{
    m_log_window = new wxLogWindow(this, _L("Logging"), true, false);
    m_log_window->Show();
}

//BBS GUI refactor: remove unused layout new/dlg
void MainFrame::update_layout()
{
    auto restore_to_creation = [this]() {
        auto clean_sizer = [](wxSizer* sizer) {
            while (!sizer->GetChildren().IsEmpty()) {
                sizer->Detach(0);
            }
        };

        // On Linux m_plater needs to be removed from m_tabpanel before to reparent it
        int plater_page_id = m_tabpanel->FindPage(m_plater);
        if (plater_page_id != wxNOT_FOUND)
            m_tabpanel->RemovePage(plater_page_id);

        if (m_plater->GetParent() != this)
            m_plater->Reparent(this);

        if (m_tabpanel->GetParent() != this)
            m_tabpanel->Reparent(this);

        plater_page_id = (m_plater_page != nullptr) ? m_tabpanel->FindPage(m_plater_page) : wxNOT_FOUND;
        if (plater_page_id != wxNOT_FOUND) {
            m_tabpanel->DeletePage(plater_page_id);
            m_plater_page = nullptr;
        }

        clean_sizer(m_main_sizer);
        clean_sizer(m_settings_dialog.GetSizer());

        if (m_settings_dialog.IsShown())
            m_settings_dialog.Close();

        m_tabpanel->Hide();
        m_plater->Hide();

        Layout();
    };

    //BBS GUI refactor: remove unused layout new/dlg
    //ESettingsLayout layout = wxGetApp().is_gcode_viewer() ? ESettingsLayout::GCodeViewer : ESettingsLayout::Old;
    ESettingsLayout layout =  ESettingsLayout::Old;

    if (m_layout == layout)
        return;

    wxBusyCursor busy;

    Freeze();

    // Remove old settings
    if (m_layout != ESettingsLayout::Unknown)
        restore_to_creation();

    ESettingsLayout old_layout = m_layout;
    m_layout = layout;

    // From the very beginning the Print settings should be selected
    //m_last_selected_tab = m_layout == ESettingsLayout::Dlg ? 0 : 1;
    m_last_selected_tab = 1;

    // Set new settings
    switch (m_layout)
    {
    case ESettingsLayout::Old:
    {
        m_plater->Reparent(m_tabpanel);
        m_tabpanel->InsertPage(tp3DEditor, m_plater, _L("Prepare"), std::string("tab_3d_active"), std::string("tab_3d_active"), false);
        m_tabpanel->InsertPage(tpPreview, m_plater, _L("Preview"), std::string("tab_preview_active"), std::string("tab_preview_active"), false);
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxTOP, 0);

        m_tabpanel->Bind(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, [this](wxCommandEvent& evt)
        {
            // jump to 3deditor under preview_only mode
            if (evt.GetId() == tp3DEditor){
                m_plater->update(true);

                if (!preview_only_hint())
                    return;
            }
            evt.Skip();
        });

        m_plater->Show();
        m_tabpanel->Show();

        break;
    }
    case ESettingsLayout::GCodeViewer:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        //BBS: add bed exclude area
        m_plater->set_bed_shape({ { 0.0, 0.0 }, { 200.0, 0.0 }, { 200.0, 200.0 }, { 0.0, 200.0 } }, {}, 0.0, {}, {}, true);
        m_plater->get_collapse_toolbar().set_enabled(false);
        m_plater->enable_sidebar(false);
        m_plater->Show();
        break;
    }
    default:
        break;
    }

    //BBS GUI refactor: remove unused layout new/dlg
//#ifdef __APPLE__
//    // Using SetMinSize() on Mac messes up the window position in some cases
//    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
//    // So, if we haven't possibility to set MinSize() for the MainFrame,
//    // set the MinSize() as a half of regular  for the m_plater and m_tabpanel, when settings layout is in slNew mode
//    // Otherwise, MainFrame will be maximized by height
//    if (m_layout == ESettingsLayout::New) {
//        wxSize size = wxGetApp().get_min_size();
//        size.SetHeight(int(0.5 * size.GetHeight()));
//        m_plater->SetMinSize(size);
//        m_tabpanel->SetMinSize(size);
//    }
//#endif

#ifdef __APPLE__
    m_plater->sidebar().change_top_border_for_mode_sizer(m_layout != ESettingsLayout::Old);
#endif

    Layout();
    Thaw();
}

// BBS: start/stop the project auto-save timer from the current app config
void MainFrame::update_autosave_timer()
{
    if (m_autosave_timer == nullptr)
        return;
    long interval = 0;
    if (wxGetApp().app_config->get("autosave_switch") == "true") {
        std::string autosave_interval;
        if (!wxGetApp().app_config->get("app", "autosave_interval", autosave_interval))
            autosave_interval = "300";
        try {
            interval = boost::lexical_cast<long>(autosave_interval);
        } catch (...) {
            interval = 0;
        }
    }
    if (interval > 0)
        m_autosave_timer->Start(int(interval) * 1000);
    else
        m_autosave_timer->Stop();
}

// Called when closing the application and when switching the application language.
bool MainFrame::request_quit(bool discard)
{
    m_quit_requested = true;
    return Close(discard); // discard -> CanVeto() == false -> no prompts at all; false: the user cancelled
}

void MainFrame::shutdown(bool isRecreate)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "MainFrame::shutdown enter";
    // First of all: no phone/agent request, send or control job may queue work for the GUI thread
    // any more, and what is already queued is dropped. The Plater is torn down below and freed
    // with this frame, and a request that ran after that crashed (c2a7d4de, 2026-09-22). A quit
    // also takes this instance off the hub's list now, so the hub stops routing requests here;
    // a language switch's rebuild reopens the gate once the new frame exists (recreate_GUI).
    RemoteAccess::close_gui_gate(!isRecreate);
    if (!isRecreate)
        RemoteAccess::get().stop();
    // BBS: backup
    Slic3r::set_backup_callback(nullptr);
    if (m_autosave_timer != nullptr)
        m_autosave_timer->Stop();
#ifdef _WIN32
	if (m_hDeviceNotify) {
		::UnregisterDeviceNotification(HDEVNOTIFY(m_hDeviceNotify));
		m_hDeviceNotify = nullptr;
	}
 	if (m_ulSHChangeNotifyRegister) {
        SHChangeNotifyDeregister(m_ulSHChangeNotifyRegister);
        m_ulSHChangeNotifyRegister = 0;
 	}
#endif // _WIN32

    if (m_plater != nullptr) {
        m_plater->get_ui_job_worker().cancel_all();

        // Unbinding of wxWidgets event handling in canvases needs to be done here because on MAC,
        // when closing the application using Command+Q, a mouse event is triggered after this lambda is completed,
        // causing a crash
        m_plater->unbind_canvas_event_handlers();

        // Cleanup of canvases' volumes needs to be done here or a crash may happen on some Linux Debian flavours
        m_plater->reset_canvas_volumes();
    }

    // Weird things happen as the Paint messages are floating around the windows being destructed.
    // Avoid the Paint messages by hiding the main window.
    // Also the application closes much faster without these unnecessary screen refreshes.
    // In addition, there were some crashes due to the Paint events sent to already destructed windows.
    this->Show(false);

    if (m_settings_dialog.IsShown())
        // call Close() to trigger call to lambda defined into GUI_App::persist_window_geometry()
        m_settings_dialog.Close();

    if (m_plater != nullptr) {
        // Stop the background thread (Windows and Linux).
        // Disconnect from a 3DConnextion driver (OSX).
        m_plater->get_mouse3d_controller().shutdown();
        // Store the device parameter database back to appconfig.
        m_plater->get_mouse3d_controller().save_config(*wxGetApp().app_config);
    }

    // stop agent
    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent)
        agent->track_enable(false);

    // Stop the background thread of the removable drive manager, so that no new updates will be sent to the Plater.
    //wxGetApp().removable_drive_manager()->shutdown();
	//stop listening for messages from other instances
	wxGetApp().other_instance_message_handler()->shutdown(this);
    // Save the slic3r.ini.Usually the ini file is saved from "on idle" callback,
    // but in rare cases it may not have been called yet.
    if(wxGetApp().app_config->dirty())
        wxGetApp().app_config->save();
//         if (m_plater)
//             m_plater->print = undef;
//         Slic3r::GUI::deregister_on_request_update_callback();

    // set to null tabs and a plater
    // to avoid any manipulations with them from App->wxEVT_IDLE after of the mainframe closing
    wxGetApp().tabs_list.clear();
    wxGetApp().model_tabs_list.clear();
    wxGetApp().shutdown(isRecreate);
    // BBS: why clear ?
    //wxGetApp().plater_ = nullptr;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "MainFrame::shutdown exit";
}

void MainFrame::update_filament_tab_ui()
{
    wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)->reload_config();
    wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)->update_tab_ui();
}

void MainFrame::update_title()
{
    return;
}

void MainFrame::show_publish_button(bool show)
{
    // m_publish_btn->Show(show);
    // Layout();
}

void MainFrame::update_title_colour_after_set_title()
{
#ifdef __APPLE__
    set_title_colour_after_set_title(GetHandle());
#endif
}

void MainFrame::show_option(bool show)
{
    if (!show) {
        if (m_slice_btn->IsShown()) {
            m_slice_btn->Hide();
            m_print_btn->Hide();
            m_slice_option_btn->Hide();
            m_print_option_btn->Hide();
            Layout();
        }
    } else {
        if (!m_slice_btn->IsShown()) {
            m_slice_btn->Show();
            m_print_btn->Show();
            m_slice_option_btn->Show();
            m_print_option_btn->Show();
            Layout();
        }
    }
}

// Lazy tab construction -------------------------------------------------------------
//
// MainFrame::init_tabpanel used to build MonitorPanel, CalibrationPanel, MultiMachinePage
// and ProjectPanel eagerly; together they were the majority of its cost for tabs the user
// usually never opens in a session. Each now gets a LazyPanelHolder placeholder page at
// its TabPosition slot; the real panel is constructed the first time that page is shown,
// or when one of the getters below is called.
//
// Creating the panel late is safe because none of them subscribes to device/network events
// at construction: they poll a wxTimer and, in their own Show(true), (re)start it and pull
// the currently selected machine. The getters below force construction for callers that
// need the object without the tab being visible.

LazyPanelHolder* MainFrame::make_monitor_holder()
{
    return new LazyPanelHolder(m_tabpanel, "MainFrame lazy=MonitorPanel", [this](wxWindow* parent) -> wxWindow* {
        m_monitor = new MonitorPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_monitor->SetBackgroundColour(*wxWHITE);
        apply_theme_to_lazy_panel(m_monitor);
        return m_monitor;
    });
}

LazyPanelHolder* MainFrame::make_calibration_holder()
{
    return new LazyPanelHolder(m_tabpanel, "MainFrame lazy=CalibrationPanel", [this](wxWindow* parent) -> wxWindow* {
        m_calibration = new CalibrationPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_calibration->SetBackgroundColour(*wxWHITE);
        apply_theme_to_lazy_panel(m_calibration);
        return m_calibration;
    });
}

LazyPanelHolder* MainFrame::make_multi_machine_holder()
{
    return new LazyPanelHolder(m_tabpanel, "MainFrame lazy=MultiMachinePage", [this](wxWindow* parent) -> wxWindow* {
        m_multi_machine = new MultiMachinePage(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_multi_machine->SetBackgroundColour(*wxWHITE);
        apply_theme_to_lazy_panel(m_multi_machine);
        return m_multi_machine;
    });
}

LazyPanelHolder* MainFrame::make_project_holder()
{
    return new LazyPanelHolder(m_tabpanel, "MainFrame lazy=ProjectPanel", [this](wxWindow* parent) -> wxWindow* {
        m_project = new ProjectPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_project->SetBackgroundColour(*wxWHITE);
        apply_theme_to_lazy_panel(m_project);
        // The eager build had GUI_App call this right after the frame was constructed.
        m_project->init_auxiliary();
        return m_project;
    });
}

// A panel built after startup missed the dark-mode pass and the DPI rescale the others
// got, so give it the same treatment at creation time.
void MainFrame::apply_theme_to_lazy_panel(wxWindow* panel)
{
    if (panel == nullptr)
        return;
#ifdef _MSW_DARK_MODE
    wxGetApp().UpdateDarkUIWin(panel);
#endif
}

MonitorPanel* MainFrame::monitor()
{
    if (m_monitor == nullptr && m_monitor_holder != nullptr)
        m_monitor_holder->realize();
    return m_monitor;
}

CalibrationPanel* MainFrame::calibration()
{
    if (m_calibration == nullptr && m_calibration_holder != nullptr)
        m_calibration_holder->realize();
    return m_calibration;
}

MultiMachinePage* MainFrame::multi_machine()
{
    if (m_multi_machine == nullptr && m_multi_machine_holder != nullptr)
        m_multi_machine_holder->realize();
    return m_multi_machine;
}

ProjectPanel* MainFrame::project()
{
    if (m_project == nullptr && m_project_holder != nullptr)
        m_project_holder->realize();
    return m_project;
}

void MainFrame::init_tabpanel() {
    // wxNB_NOPAGETHEME: Disable Windows Vista theme for the Notebook background. The theme performance is terrible on
    // Windows 10 with multiple high resolution displays connected.
    // BBS
    wxBoxSizer *side_tools = create_side_tools();
    m_tabpanel = new Notebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, side_tools,
                              wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_tabpanel->SetBackgroundColour(*wxWHITE);

#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
    m_tabpanel->SetFont(Slic3r::GUI::wxGetApp().normal_font());
#endif
    m_tabpanel->Hide();
    m_settings_dialog.set_tabpanel(m_tabpanel);

#ifdef __WXMSW__
    m_tabpanel->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
#else
    m_tabpanel->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
#endif
        //BBS
        wxWindow* panel = m_tabpanel->GetCurrentPage();
        int sel = m_tabpanel->GetSelection();
        //wxString page_text = m_tabpanel->GetPageText(sel);
        m_last_selected_tab = m_tabpanel->GetSelection();
        if (panel == m_plater) {
            if (sel == tp3DEditor) {
                if (!m_plater || !m_plater->is_view3D_shown())
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLVIEWTOOLBAR_3D));
                m_param_panel->OnActivate();
            }
            else if (sel == tpPreview) {
                if (!m_plater || m_plater->is_view3D_shown())
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLVIEWTOOLBAR_PREVIEW));
                m_param_panel->OnActivate();
            }
        }
        //else if (panel == m_param_panel)
        //    m_param_panel->OnActivate();
        else if (panel == m_monitor_holder) {
            //monitor
        }
#ifndef __APPLE__
        if (sel == tp3DEditor) {
            m_topbar->EnableUndoRedoItems();
        }
        else {
            m_topbar->DisableUndoRedoItems();
        }
#endif

        // Notify WCP page state change subscribers
        static int prev_monitored_tab = -1;

        // Send "inactive" to previous tab if leaving a monitored tab
        if (prev_monitored_tab == tpHome && sel != tpHome) {
            // Leaving Home: the start page (if it is the one showing) hears "inactive" from HomePanel.
            if (m_home)
                m_home->on_tab_changed(false);
        } else if (prev_monitored_tab == tpMonitor && sel != tpMonitor) {
            // Leaving device page (PrinterWebView)
            if (m_printer_view) {
                wxWebView* printer_webview = m_printer_view->get_browser();
                wxGetApp().page_state_notify_webview(printer_webview, "inactive");
            }
        }

        // Send "active" to current tab if entering a monitored tab
        if (sel == tpHome) {
            // Entering Home: the Home page is built (first time) or refreshed; the start page (if it
            // is the one showing) hears "active" from HomePanel.
            if (m_home)
                m_home->on_tab_changed(true);
            prev_monitored_tab = tpHome;
        } else if (sel == tpMonitor) {
            // Entering device page (PrinterWebView)
            if (m_printer_view) {
                wxWebView* printer_webview = m_printer_view->get_browser();
                wxGetApp().page_state_notify_webview(printer_webview, "active");
            }
            prev_monitored_tab = tpMonitor;
        } else {
            // Update prev_monitored_tab only when leaving a monitored tab
            if (prev_monitored_tab != tpHome && prev_monitored_tab != tpMonitor) {
                prev_monitored_tab = -1;
            }
        }

        if (panel)
            panel->SetFocus();

        /*switch (sel) {
        case TabPosition::tpHome:
            show_option(false);
            break;
        case TabPosition::tp3DEditor:
            show_option(true);
            break;
        case TabPosition::tpPreview:
            show_option(true);
            break;
        case TabPosition::tpMonitor:
            show_option(false);
            break;
        default:
            show_option(false);
            break;
        }*/
    });

    if (wxGetApp().is_editor()) {
        {
            // Home shows Recent and Print History. The old start page (m_webview) is built only when
            // asked for (show_start_page / start_page), so nothing loads it at startup any more.
            Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=HomePanel");
            m_home = new HomePanel(m_tabpanel);
        }
        Bind(EVT_LOAD_URL, [this](wxCommandEvent &evt) {
            // A URL for the home page goes to the start page, which is where it always went.
            wxString url = evt.GetString();
            show_start_page();
            if (m_webview)
                m_webview->load_url(url);
        });
        m_tabpanel->AddPage(m_home, "", "tab_home_active", "tab_home_active", false);
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=ParamsPanel");
        m_param_panel = new ParamsPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
      
    }
    // The splash is ticked before the heavy constructors (Plater is the last one left
    // in this function: Monitor/Calibration are deferred to first show). Free after startup.
    wxGetApp().tick_splash_animation();
    {
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=Plater");
        m_plater = new Plater(this, this);
        m_plater->SetBackgroundColour(*wxWHITE);
        m_plater->Hide();
    }

    wxGetApp().plater_ = m_plater;

    {
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=create_preset_tabs");
        create_preset_tabs();
    }

        //BBS add pages
    wxGetApp().tick_splash_animation();
    {
        // Deferred: MonitorPanel was ~620 ms of this constructor and is rarely the tab a
        // user lands on. The holder keeps tpMonitor's slot; the panel is built on the
        // first Show(true), where MonitorPanel::Show() already pulls the current device
        // state (it polls a timer, it is not an event subscriber, so nothing is missed).
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=MonitorPanel(deferred)");
        m_monitor_holder = make_monitor_holder();
        m_tabpanel->AddPage(m_monitor_holder, _L("Device"), std::string("tab_monitor_active"), std::string("tab_monitor_active"), false);
    }

    // Stream tab: grid of LAN camera streams, always right after Device.
    {
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=StreamPanel");
        m_stream = new StreamPanel(m_tabpanel);
        m_stream->SetBackgroundColour(*wxWHITE);
        m_tabpanel->AddPage(m_stream, _L("Stream"), std::string("tab_stream_active"), std::string("tab_stream_active"), false);
    }

    {
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=PrinterWebView");
        m_printer_view = new PrinterWebView(m_tabpanel);
    }
    Bind(EVT_LOAD_PRINTER_URL, [this](LoadPrinterViewEvent &evt) {
        wxString url = evt.GetString();
        wxString key = evt.GetAPIkey();
        m_printer_view->load_url(url, key);
    });

    m_printer_view->Hide();

    if (wxGetApp().is_enable_multi_machine()) {
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=MultiMachinePage(deferred)");
        m_multi_machine_holder = make_multi_machine_holder();
        // TODO: change the bitmap
        m_tabpanel->AddPage(m_multi_machine_holder, _L("Multi-device"), std::string("tab_multi_active"), std::string("tab_multi_active"), false);
    }

    {
        // Deferred: ProjectPanel builds a webview. Its only startup caller was
        // GUI_App's mainframe->m_project->init_auxiliary(), which is now a no-op until
        // the tab is opened (ProjectPanel::Show() reloads the model data anyway).
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=ProjectPanel(deferred)");
        m_project_holder = make_project_holder();
        m_tabpanel->AddPage(m_project_holder, _L("Project"), std::string("tab_auxiliary_active"), std::string("tab_auxiliary_active"), false);
    }

    wxGetApp().tick_splash_animation();
    {
        // Deferred: CalibrationPanel was ~989 ms of this constructor (it builds the whole
        // wizard stack). CalibrationPanel::Show() pulls the selected machine on first
        // show, so creating it late loses nothing.
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=CalibrationPanel(deferred)");
        m_calibration_holder = make_calibration_holder();
        m_tabpanel->AddPage(m_calibration_holder, _L("Calibration"), std::string("tab_calibration_active"), std::string("tab_calibration_active"), false);
    }

    if (m_plater) {
        Slic3r::StartupScopedTimer t("MainFrame::init_tabpanel step=initial_config");
        // load initial config
        auto full_config = wxGetApp().preset_bundle->full_config();
        m_plater->on_config_change(full_config);

        // Show a correct number of filament fields.
        // nozzle_diameter is undefined when SLA printer is selected
        // BBS
        if (full_config.has("filament_colour")) {
            m_plater->on_filaments_change(full_config.option<ConfigOptionStrings>("filament_colour")->values.size());
        }
    }
}

// SoftFever
void MainFrame::show_device(bool bBBLPrinter) {
    auto idx = -1;
    // Ultra: leaving a Flashforge printer - unmount its device tab first
    if (m_ff_device != nullptr && (idx = m_tabpanel->FindPage(m_ff_device)) != wxNOT_FOUND) {
        m_ff_device->Show(false);
        m_tabpanel->RemovePage(idx);
    }
    if (bBBLPrinter) {
        if (m_monitor_holder && m_tabpanel->FindPage(m_monitor_holder) != wxNOT_FOUND)
            return;
        // Remove printer view
        if ((idx = m_tabpanel->FindPage(m_printer_view)) != wxNOT_FOUND) {
            m_printer_view->Show(false);
            m_tabpanel->RemovePage(idx);
        }

        // Create/insert monitor page. The page is the lazy holder, so an already-built
        // MonitorPanel keeps its state across an unmount/remount, and one that was never
        // opened still costs nothing here.
        if (!m_monitor_holder)
            m_monitor_holder = make_monitor_holder();
        m_monitor_holder->Show(false);
        m_tabpanel->InsertPage(tpMonitor, m_monitor_holder, _L("Device"), std::string("tab_monitor_active"), std::string("tab_monitor_active"));

        if (wxGetApp().is_enable_multi_machine()) {
            if (!m_multi_machine_holder)
                m_multi_machine_holder = make_multi_machine_holder();
            // TODO: change the bitmap
            m_multi_machine_holder->Show(false);
            m_tabpanel->InsertPage(tpMultiDevice, m_multi_machine_holder, _L("Multi-device"), std::string("tab_multi_active"),
                                   std::string("tab_multi_active"), false);
        }
        if (!m_calibration_holder)
            m_calibration_holder = make_calibration_holder();
        m_calibration_holder->Show(false);
        // Calibration is always the last page, so don't use InsertPage here. Otherwise, if multi_machine page is not enabled,
        // the calibration tab won't be properly added as well, due to the TabPosition::tpCalibration no longer matches the real tab position.
        m_tabpanel->AddPage(m_calibration_holder, _L("Calibration"), std::string("tab_calibration_active"),
                               std::string("tab_calibration_active"), false);

#ifdef _MSW_DARK_MODE
        wxGetApp().UpdateDarkUIWin(this);
#endif // _MSW_DARK_MODE

    } else {
        if (m_tabpanel->FindPage(m_printer_view) != wxNOT_FOUND)
            return;

        if (m_calibration_holder && (idx = m_tabpanel->FindPage(m_calibration_holder)) != wxNOT_FOUND) {
            m_calibration_holder->Show(false);
            m_tabpanel->RemovePage(idx);
        }
        if (m_multi_machine_holder && (idx = m_tabpanel->FindPage(m_multi_machine_holder)) != wxNOT_FOUND) {
            m_multi_machine_holder->Show(false);
            m_tabpanel->RemovePage(idx);
        }
        if (m_monitor_holder && (idx = m_tabpanel->FindPage(m_monitor_holder)) != wxNOT_FOUND) {
            m_monitor_holder->Show(false);
            m_tabpanel->RemovePage(idx);
        }
        if (m_printer_view == nullptr) {
            m_printer_view = new PrinterWebView(m_tabpanel);
            Bind(EVT_LOAD_PRINTER_URL, [this](LoadPrinterViewEvent& evt) {
                wxString url = evt.GetString();
                wxString key = evt.GetAPIkey();
                m_printer_view->load_url(url, key);
            });
        }
        m_printer_view->Show(false);
        m_tabpanel->InsertPage(tpMonitor, m_printer_view, _L("Device"), std::string("tab_monitor_active"),
                               std::string("tab_monitor_active"));
    }
}

// Ultra: Flashforge device tab (Orca-Flashforge port) - third Device-tab mode
void MainFrame::show_flashforge_device()
{
    if (m_ff_device != nullptr && m_tabpanel->FindPage(m_ff_device) != wxNOT_FOUND)
        return;
    int idx;
    if (m_calibration_holder && (idx = m_tabpanel->FindPage(m_calibration_holder)) != wxNOT_FOUND) {
        m_calibration_holder->Show(false);
        m_tabpanel->RemovePage(idx);
    }
    if (m_multi_machine_holder && (idx = m_tabpanel->FindPage(m_multi_machine_holder)) != wxNOT_FOUND) {
        m_multi_machine_holder->Show(false);
        m_tabpanel->RemovePage(idx);
    }
    if (m_monitor_holder && (idx = m_tabpanel->FindPage(m_monitor_holder)) != wxNOT_FOUND) {
        m_monitor_holder->Show(false);
        m_tabpanel->RemovePage(idx);
    }
    if (m_printer_view && (idx = m_tabpanel->FindPage(m_printer_view)) != wxNOT_FOUND) {
        m_printer_view->Show(false);
        m_tabpanel->RemovePage(idx);
    }
    if (m_ff_device == nullptr)
        m_ff_device = new FFDeviceTab(m_tabpanel);
    m_ff_device->Show(false);
    m_tabpanel->InsertPage(tpMonitor, m_ff_device, _L("Device"), std::string("tab_monitor_active"),
                           std::string("tab_monitor_active"));
    static_cast<FFDeviceTab*>(m_ff_device)->OnActivate();
#ifdef _MSW_DARK_MODE
    wxGetApp().UpdateDarkUIWin(this);
#endif
}

bool MainFrame::preview_only_hint()
{
    if (m_plater && (m_plater->only_gcode_mode() || (m_plater->using_exported_file()))) {
        BOOST_LOG_TRIVIAL(info) << boost::format("skipped tab switch from %1% to %2% in preview mode")%m_tabpanel->GetSelection() %tp3DEditor;

        ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Warning"));
        confirm_dlg.Bind(EVT_SECONDARY_CHECK_CONFIRM, [this](wxCommandEvent& e) {
            preview_only_to_editor = true;
        });
        confirm_dlg.update_btn_label(_L("Yes"), _L("No"));
        auto filename = m_plater->get_preview_only_filename();

        confirm_dlg.update_text(filename + " " + _L("will be closed before creating a new model. Do you want to continue?"));
        confirm_dlg.on_show();
        if (preview_only_to_editor) {
            m_plater->new_project();
            preview_only_to_editor = false;
        }

        return false;
    }

    return true;
}

#ifdef WIN32
void MainFrame::register_win32_callbacks()
{
    //static GUID GUID_DEVINTERFACE_USB_DEVICE  = { 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED };
    //static GUID GUID_DEVINTERFACE_DISK        = { 0x53f56307, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b };
    //static GUID GUID_DEVINTERFACE_VOLUME      = { 0x71a27cdd, 0x812a, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f };
    static GUID GUID_DEVINTERFACE_HID           = { 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 };

    // Register USB HID (Human Interface Devices) notifications to trigger the 3DConnexion enumeration.
    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter = { 0 };
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    m_hDeviceNotify = ::RegisterDeviceNotification(this->GetHWND(), &NotificationFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

// or register for file handle change?
//      DEV_BROADCAST_HANDLE NotificationFilter = { 0 };
//      NotificationFilter.dbch_size = sizeof(DEV_BROADCAST_HANDLE);
//      NotificationFilter.dbch_devicetype = DBT_DEVTYP_HANDLE;

    // Using Win32 Shell API to register for media insert / removal events.
    LPITEMIDLIST ppidl;
    if (SHGetSpecialFolderLocation(this->GetHWND(), CSIDL_DESKTOP, &ppidl) == NOERROR) {
        SHChangeNotifyEntry shCNE;
        shCNE.pidl       = ppidl;
        shCNE.fRecursive = TRUE;
        // Returns a positive integer registration identifier (ID).
        // Returns zero if out of memory or in response to invalid parameters.
        m_ulSHChangeNotifyRegister = SHChangeNotifyRegister(this->GetHWND(),        // Hwnd to receive notification
            SHCNE_DISKEVENTS,                                                       // Event types of interest (sources)
            SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED,
            //SHCNE_UPDATEITEM,                                                     // Events of interest - use SHCNE_ALLEVENTS for all events
            WM_USER_MEDIACHANGED,                                                   // Notification message to be sent upon the event
            1,                                                                      // Number of entries in the pfsne array
            &shCNE);                                                                // Array of SHChangeNotifyEntry structures that
                                                                                    // contain the notifications. This array should
                                                                                    // always be set to one when calling SHChnageNotifyRegister
                                                                                    // or SHChangeNotifyDeregister will not work properly.
        assert(m_ulSHChangeNotifyRegister != 0);    // Shell notification failed
    } else {
        // Failed to get desktop location
        assert(false);
    }

    {
        static constexpr int device_count = 1;
        RAWINPUTDEVICE devices[device_count] = { 0 };
        // multi-axis mouse (SpaceNavigator, etc.)
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage = 0x08;
        if (! RegisterRawInputDevices(devices, device_count, sizeof(RAWINPUTDEVICE)))
            BOOST_LOG_TRIVIAL(error) << "RegisterRawInputDevices failed";
    }
}
#endif // _WIN32

void MainFrame::create_preset_tabs()
{
    wxGetApp().update_label_colours_from_appconfig();

    //BBS: GUI refactor
    //m_param_panel = new ParamsPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    m_param_dialog = new ParamsDialog(m_plater);

#define ORCA_TIMED_TAB(expr, label)                                              \
    do {                                                                         \
        Slic3r::StartupScopedTimer t("MainFrame::create_preset_tabs step=" label); \
        expr;                                                                    \
    } while (0)
    ORCA_TIMED_TAB(add_created_tab(new TabPrint(m_param_panel), "cog"), "TabPrint");
    ORCA_TIMED_TAB(add_created_tab(new TabPrintPlate(m_param_panel), "cog"), "TabPrintPlate");
    ORCA_TIMED_TAB(add_created_tab(new TabPrintObject(m_param_panel), "cog"), "TabPrintObject");
    ORCA_TIMED_TAB(add_created_tab(new TabPrintPart(m_param_panel), "cog"), "TabPrintPart");
    ORCA_TIMED_TAB(add_created_tab(new TabPrintLayer(m_param_panel), "cog"), "TabPrintLayer");
    ORCA_TIMED_TAB(add_created_tab(new TabFilament(m_param_dialog->panel()), "spool"), "TabFilament");
    /* BBS work around to avoid appearance bug */
    //add_created_tab(new TabSLAPrint(m_param_panel));
    //add_created_tab(new TabSLAMaterial(m_param_panel));
    ORCA_TIMED_TAB(add_created_tab(new TabPrinter(m_param_dialog->panel()), "printer"), "TabPrinter");
#undef ORCA_TIMED_TAB

    {
        Slic3r::StartupScopedTimer t("MainFrame::create_preset_tabs step=rebuild_panels");
        m_param_panel->rebuild_panels();
        m_param_dialog->panel()->rebuild_panels();
    }
    //m_tabpanel->AddPage(m_param_panel, "Parameters", "notebook_presets_active");
    //m_tabpanel->InsertPage(tpSettings, m_param_panel, _L("Parameters"), std::string("cog"));
}

void MainFrame::add_created_tab(Tab* panel,  const std::string& bmp_name /*= ""*/)
{
    panel->create_preset_tab();

    if (panel->type() == Preset::TYPE_PLATE) {
        wxGetApp().tabs_list.pop_back();
        wxGetApp().plate_tab = panel;
    }
    // BBS: model config
    if (panel->type() == Preset::TYPE_MODEL) {
        wxGetApp().tabs_list.pop_back();
        wxGetApp().model_tabs_list.push_back(panel);
    }
}

bool MainFrame::is_active_and_shown_tab(wxPanel* panel)
{
    if (panel == m_param_panel)
        panel = m_plater;
    else
        return m_param_dialog->IsShown();

    if (m_tabpanel->GetCurrentPage() != panel)
        return false;
    return true;
}

bool MainFrame::can_start_new_project() const
{
    /*return m_plater && (!m_plater->get_project_filename(".3mf").IsEmpty() ||
                        GetTitle().StartsWith('*')||
                        wxGetApp().has_current_preset_changes() ||
                        !m_plater->model().objects.empty());*/
    return (m_plater && !m_plater->is_background_process_slicing());
}

bool MainFrame::can_open_project() const
{
    return (m_plater && !m_plater->is_background_process_slicing());
}

bool  MainFrame::can_add_models() const
{
    return (m_plater && !m_plater->is_background_process_slicing() && !m_plater->only_gcode_mode() && !m_plater->using_exported_file());
}

bool MainFrame::can_save() const
{
    return (m_plater != nullptr) &&
        !m_plater->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(false) &&
        m_plater->is_project_dirty() && !m_plater->using_exported_file() && !m_plater->only_gcode_mode();
}

bool MainFrame::can_save_as() const
{
    return (m_plater != nullptr) &&
        !m_plater->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(false) && !m_plater->using_exported_file() && !m_plater->only_gcode_mode();
}

void MainFrame::save_project()
{
    save_project_as(m_plater->get_project_filename(".3mf"));
}

bool MainFrame::save_project_as(const wxString& filename)
{
    bool ret = (m_plater != nullptr) ? m_plater->export_3mf(into_path(filename)) : false;
    if (ret) {
//        wxGetApp().update_saved_preset_from_current_preset();
        m_plater->reset_project_dirty_after_save();
    }
    return ret;
}

bool MainFrame::can_upload() const
{
    return true;
}

bool MainFrame::can_export_model() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_export_toolpaths() const
{
    return (m_plater != nullptr) && (m_plater->printer_technology() == ptFFF) && m_plater->is_preview_shown() && m_plater->is_preview_loaded() && m_plater->has_toolpaths_to_export();
}

bool MainFrame::can_export_supports() const
{
    if ((m_plater == nullptr) || (m_plater->printer_technology() != ptSLA) || m_plater->model().objects.empty())
        return false;

    bool can_export = false;
    const PrintObjects& objects = m_plater->sla_print().objects();
    for (const SLAPrintObject* object : objects)
    {
        if (object->has_mesh(slaposPad) || object->has_mesh(slaposSupportTree))
        {
            can_export = true;
            break;
        }
    }
    return can_export;
}

bool MainFrame::can_export_gcode() const
{
    if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters
    PartPlateList &part_plate_list = m_plater->get_partplate_list();
    PartPlate *current_plate = part_plate_list.get_curr_plate();
    if (!current_plate->is_slice_result_ready_for_print())
        return false;

    return true;
}

bool MainFrame::can_export_all_gcode() const
{
    if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters
    PartPlateList& part_plate_list = m_plater->get_partplate_list();
    return part_plate_list.is_all_slice_results_ready_for_print();
}

bool MainFrame::can_print_3mf() const
{
    if (m_plater && !m_plater->model().objects.empty()) {
        if (wxGetApp().preset_bundle->printers.get_edited_preset().is_custom_defined())
            return false;
    }
    return true;
}

bool MainFrame::can_send_gcode() const
{
    if (m_plater && !m_plater->model().objects.empty())
    {
        auto        devices     = wxGetApp().app_config->get_devices();
        const auto& edit_preset = wxGetApp().preset_bundle->printers.get_edited_preset();

        auto printer_config    = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        auto printer_model_opt = printer_config.option<ConfigOptionString>("printer_model");
        const std::string printer_model = printer_model_opt ? printer_model_opt->value : std::string();
        const bool        connect_flow_active = wxGetApp().app_config->get("use_new_connect") == "true";

        // The same question Plater::send_gcode_legacy asks, so the button and the send it triggers
        // cannot disagree. "use_new_connect" alone said yes for every preset once any Snapmaker had
        // connected - including an Elegoo with no address anywhere, whose Print then opened the
        // Snapmaker pre-treat page.
        if (PrintHostDevices::send_flow_for(printer_model, connect_flow_active) ==
            PrintHostDevices::SendFlow::SnapmakerConnect)
            return true;

        // Not "does the preset hold an address" any more: this model may have three printers in
        // its device list and an empty print_host, which is exactly the case the devices
        // feature exists for. PrintHostDevices::can_send_for is the one place that decides.
        return PrintHostDevices::can_send_for(edit_preset);
    }
    return true;
}

bool MainFrame::can_slice() const
{
#ifdef SUPPORT_BACKGROUND_PROCESSING
    bool bg_proc = wxGetApp().app_config->get("background_processing") == "1";
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() && !bg_proc : false;
#else
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() : false;
#endif
}

bool MainFrame::can_change_view() const
{
    switch (m_layout)
    {
    default:                   { return false; }
    //BBS GUI refactor: remove unused layout new/dlg
    case ESettingsLayout::Old: {
        int page_id = m_tabpanel->GetSelection();
        return page_id != wxNOT_FOUND && dynamic_cast<const Slic3r::GUI::Plater*>(m_tabpanel->GetPage((size_t)page_id)) != nullptr;
    }
    case ESettingsLayout::GCodeViewer: { return true; }
    }
}

bool MainFrame::can_clone() const {
    return can_select() && !m_plater->is_selection_empty();
}

bool MainFrame::can_select() const
{
    return (m_plater != nullptr) && (m_tabpanel->GetSelection() == TabPosition::tp3DEditor) && !m_plater->model().objects.empty();
}

bool MainFrame::can_deselect() const
{
    return (m_plater != nullptr) && (m_tabpanel->GetSelection() == TabPosition::tp3DEditor) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete() const
{
    return (m_plater != nullptr) && (m_tabpanel->GetSelection() == TabPosition::tp3DEditor) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete_all() const
{
    return (m_plater != nullptr) && (m_tabpanel->GetSelection() == TabPosition::tp3DEditor) && !m_plater->model().objects.empty();
}

bool MainFrame::can_reslice() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

namespace {
// Static check: PrintSelectKeys integers must stay aligned with PrintSelectType.
static_assert(static_cast<int>(MainFrame::ePrintAll) == PrintSelectKeys::ePrintAll);
static_assert(static_cast<int>(MainFrame::ePrintPlate) == PrintSelectKeys::ePrintPlate);
static_assert(static_cast<int>(MainFrame::eExportSlicedFile) == PrintSelectKeys::eExportSlicedFile);
static_assert(static_cast<int>(MainFrame::eExportGcode) == PrintSelectKeys::eExportGcode);
static_assert(static_cast<int>(MainFrame::eSendGcode) == PrintSelectKeys::eSendGcode);
static_assert(static_cast<int>(MainFrame::eSendToPrinter) == PrintSelectKeys::eSendToPrinter);
static_assert(static_cast<int>(MainFrame::eSendToPrinterAll) == PrintSelectKeys::eSendToPrinterAll);
static_assert(static_cast<int>(MainFrame::eUploadGcode) == PrintSelectKeys::eUploadGcode);
static_assert(static_cast<int>(MainFrame::eExportAllSlicedFile) == PrintSelectKeys::eExportAllSlicedFile);
static_assert(static_cast<int>(MainFrame::ePrintMultiMachine) == PrintSelectKeys::ePrintMultiMachine);

const char *print_select_type_key(MainFrame::PrintSelectType type)
{
    return PrintSelectKeys::key(static_cast<int>(type));
}

// Single source for the print button and print dropdown labels. Keep Edge's strings
// (eSendGcode is _L("Print"), not upstream's _L_CONTEXT("Print","Verb")).
wxString print_select_type_label(MainFrame::PrintSelectType type)
{
    switch (type) {
    case MainFrame::ePrintAll:            return _L("Print all");
    case MainFrame::ePrintPlate:          return _L("Print plate");
    case MainFrame::eExportSlicedFile:    return _L("Export plate sliced file");
    case MainFrame::eExportAllSlicedFile: return _L("Export all sliced file");
    case MainFrame::eExportGcode:         return _L("Export G-code file");
    case MainFrame::eSendGcode:           return _L("Print");
    case MainFrame::eSendToPrinter:       return _L("Send");
    case MainFrame::eSendToPrinterAll:    return _L("Send all");
    case MainFrame::ePrintMultiMachine:   return _L("Send to Multi-device");
    case MainFrame::eUploadGcode:         break; // no dropdown entry
    }
    return _L("Print plate");
}
} // namespace

std::vector<MainFrame::PrintSelectType> MainFrame::available_print_actions() const
{
    std::vector<PrintSelectType> actions;
    const auto                   preset_bundle = wxGetApp().preset_bundle;

    if (preset_bundle && !preset_bundle->is_bbl_vendor()) {
        // Third-party: Print (send gcode), both sliced-file exports, Export G-code.
        // A plate-sliced file ("<name>.gcode.3mf") is not a Bambu-only artefact: the Flashforge
        // Creator 5 and other third-party hosts consume it, and File > Export already offers it
        // via can_export_gcode() (Ctrl+G). Upstream gates the sliced-file item on use_3mf; Edge
        // offers both exports to every vendor.
        actions.push_back(eSendGcode);
        actions.push_back(eExportSlicedFile);
        actions.push_back(eExportAllSlicedFile);
        actions.push_back(eExportGcode);
        return actions;
    }

    // BBL vendor (LAN or cloud): Print plate, optional Multi-device, both sliced-file exports,
    // Export G-code. No Print all / Send / Send all on Edge. The native print flow is ePrintPlate
    // / EVT_GLTOOLBAR_PRINT_PLATE (SelectMachineDialog). eSendGcode is the third-party print-host
    // flow and additionally gates on can_send_gcode(); keep the label and the enum in sync with
    // set_print_button_to_default(ePrintPlate) and the Ctrl+Shift+G shortcut.
    actions.push_back(ePrintPlate);
    if (enable_multi_machine)
        actions.push_back(ePrintMultiMachine);
    actions.push_back(eExportSlicedFile);
    actions.push_back(eExportAllSlicedFile);
    actions.push_back(eExportGcode);
    return actions;
}

void MainFrame::apply_print_select_state(PrintSelectType select_type)
{
    // Dropdown path (and the shared setup for set_print_button_to_default): (1) set the label,
    // (2) set m_print_select to the SAME enum the matching dropdown item uses, and (3)
    // unconditionally recompute m_print_enable from get_enable_print_status() only — the same
    // enable rule a dropdown pick used on main. The extra can_send_gcode() gate for eSendGcode
    // lives only in set_print_button_to_default; export actions never depend on a print host, so
    // "Export G-code file" is not greyed when no print host is configured.
    m_print_btn->SetLabel(print_select_type_label(select_type));
    m_print_select = select_type;
    m_print_enable = get_enable_print_status();
    m_print_btn->Enable(m_print_enable);
    this->Layout();
}

void MainFrame::select_print_action(PrintSelectType select_type)
{
    apply_print_select_state(select_type);
    remember_print_select(select_type);
}

void MainFrame::remember_print_select(PrintSelectType select_type)
{
    if (!wxGetApp().app_config->get_bool("remember_print_action"))
        return;
    // AppConfig is marked dirty here and flushed by the regular autosave.
    wxGetApp().app_config->set("last_print_action", print_select_type_key(select_type));
}

bool MainFrame::get_remembered_print_select(PrintSelectType &out) const
{
    if (!wxGetApp().app_config->get_bool("remember_print_action"))
        return false;
    const auto        offered = available_print_actions();
    std::vector<int>  offered_ints;
    offered_ints.reserve(offered.size());
    for (PrintSelectType type : offered)
        offered_ints.push_back(static_cast<int>(type));
    const int resolved = PrintSelectKeys::resolve_or_default(wxGetApp().app_config->get("last_print_action"),
                                                            offered_ints.data(), offered_ints.size(), -1);
    if (resolved < 0)
        return false;
    // Edge: do not restore send_gcode when this printer cannot send (no host). Upstream
    // would still restore it and leave Print greyed; we fall back to the computed default.
    if (resolved == static_cast<int>(eSendGcode) && !can_send_gcode())
        return false;
    out = static_cast<PrintSelectType>(resolved);
    return true;
}

wxBoxSizer* MainFrame::create_side_tools()
{
    enable_multi_machine = wxGetApp().is_enable_multi_machine();
    int em = em_unit();
    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);

    m_slice_select = eSlicePlate;
    m_print_select = ePrintPlate;

    // m_publish_btn = new Button(this, _L("Upload"), "bar_publish", 0, FromDIP(16));
    m_slice_btn = new SideButton(this, _L("Slice plate"), "");
    m_slice_option_btn = new SideButton(this, "", "sidebutton_dropdown", 0, 14);
    m_print_btn = new SideButton(this, _L("Print plate"), "");
    m_print_option_btn = new SideButton(this, "", "sidebutton_dropdown", 0, 14);

    // Restore the last used print/export action if the user opted to remember it.
    // preset_bundle may not be final here; Plater's printer-switch calls re-validate.
    PrintSelectType remembered_print_select;
    if (get_remembered_print_select(remembered_print_select)) {
        m_print_select = remembered_print_select;
        m_print_btn->SetLabel(print_select_type_label(remembered_print_select));
    }

    update_side_button_style();
    // m_publish_btn->Hide();
    m_slice_option_btn->Enable();
    m_print_option_btn->Enable();
    // sizer->Add(m_publish_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(1));
    // sizer->Add(FromDIP(15), 0, 0, 0, 0);
    sizer->Add(m_slice_option_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    sizer->Add(m_slice_btn       , 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(15));
    sizer->Add(m_print_option_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    sizer->Add(m_print_btn       , 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(19));

    m_slice_mode_popup = new SliceModePopup(this);
    auto try_show_slice_mode_popup = [this](wxMouseEvent &e) {
        e.Skip();
        if (m_slice_enable && GUI::FlowType::distinct_nozzle_flow_type_count() >= 2)
            m_slice_mode_popup->ShowFor({m_slice_btn, m_slice_option_btn}, m_slice_btn);
    };
    m_slice_btn->Bind(wxEVT_ENTER_WINDOW, try_show_slice_mode_popup);
    m_slice_option_btn->Bind(wxEVT_ENTER_WINDOW, try_show_slice_mode_popup);

    sizer->Layout();

    // m_publish_btn->Bind(wxEVT_BUTTON, [this](auto& e) {
    //     CallAfter([this] {
    //         wxGetApp().open_publish_page_dialog();

    //         if (!wxGetApp().getAgent()) {
    //             BOOST_LOG_TRIVIAL(info) << "publish: no agent";
    //             return;
    //         }

    //         // record
    //         json j;
    //         NetworkAgent* agent = GUI::wxGetApp().getAgent();
    //     });
    // });

    m_slice_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
            if (m_slice_mode_popup)
                m_slice_mode_popup->HidePopup();
            start_slice();
        });

    m_print_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
            //this->m_plater->select_view_3D("Preview");
            if (m_print_select == ePrintAll || m_print_select == ePrintPlate || m_print_select == ePrintMultiMachine)
            {
                m_plater->apply_background_progress();
                // check valid of print
                m_print_enable = get_enable_print_status();
                m_print_btn->Enable(m_print_enable);
                if (m_print_enable) {
                    if (m_print_select == ePrintAll)
                        wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_ALL));
                    if (m_print_select == ePrintPlate)
                        wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_PLATE));
                    if(m_print_select == ePrintMultiMachine)
                         wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_MULTI_MACHINE));
                }
            }
            else if (m_print_select == eExportGcode)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_GCODE));
            else if (m_print_select == eSendGcode)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_GCODE));
            else if (m_print_select == eUploadGcode)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_UPLOAD_GCODE));
            else if (m_print_select == eExportSlicedFile)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE));
            else if (m_print_select == eExportAllSlicedFile)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_ALL_SLICED_FILE));
            else if (m_print_select == eSendToPrinter)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_TO_PRINTER));
            else if (m_print_select == eSendToPrinterAll)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_TO_PRINTER_ALL));
            /* else if (m_print_select == ePrintMultiMachine)
                 wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_MULTI_MACHINE));*/
        });

    m_slice_option_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
            if (m_slice_mode_popup)
                m_slice_mode_popup->HidePopup();
            SidePopup* p = new SidePopup(this);
            SideButton* slice_all_btn = new SideButton(p, _L("Slice all"), "");
            slice_all_btn->SetCornerRadius(0);
            SideButton* slice_plate_btn = new SideButton(p, _L("Slice plate"), "");
            slice_plate_btn->SetCornerRadius(0);

            slice_all_btn->Bind(wxEVT_BUTTON, [this, p](wxCommandEvent&) {
                m_slice_btn->SetLabel(_L("Slice all"));
                m_slice_select = eSliceAll;
                m_slice_enable = get_enable_slice_status();
                m_slice_btn->Enable(m_slice_enable);
                this->Layout();
                p->Dismiss();
                });

            slice_plate_btn->Bind(wxEVT_BUTTON, [this, p](wxCommandEvent&) {
                m_slice_btn->SetLabel(_L("Slice plate"));
                m_slice_select = eSlicePlate;
                m_slice_enable = get_enable_slice_status();
                m_slice_btn->Enable(m_slice_enable);
                this->Layout();
                p->Dismiss();
                });
            p->append_button(slice_all_btn);
            p->append_button(slice_plate_btn);
            // Bambu two-extruder printers: change the plate's filament -> extruder arrangement
            // (opens the pre-slice confirmation even when nothing changed, then slices the plate).
            // Not gated on m_slice_enable: that is false once the plate is sliced, which is exactly
            // when the arrangement is wanted again (the Slice button itself stays disabled then).
            // Whenever Slice is available the arrangement is too, whatever the Plater's "slicing" flag
            // says (the owner found the entry missing once a sliced H2C plate had been reset, 2026-09-23).
            PartPlate* arrange_plate = m_plater ? m_plater->get_partplate_list().get_curr_plate() : nullptr;
            if (GUI::DualNozzle::preset_is_dual_nozzle_bambu() && arrange_plate && arrange_plate->has_printable_instances() &&
                (m_slice_enable || !m_plater->is_background_process_slicing()) && !m_plater->only_gcode_mode() &&
                !m_plater->using_exported_file()) {
                SideButton* arrange_btn = new SideButton(p, _L("Filament arrangement..."), "");
                arrange_btn->SetCornerRadius(0);
                arrange_btn->Bind(wxEVT_BUTTON, [this, p](wxCommandEvent&) {
                    p->Dismiss();
                    if (!m_plater)
                        return;
                    GUI::DualNozzle::open_arrangement_and_reslice(m_plater, m_plater->get_partplate_list().get_curr_plate_index());
                });
                p->append_button(arrange_btn);
            }
            p->Popup(m_slice_btn);
        }
    );

    m_print_option_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
        SidePopup* p = new SidePopup(this);
        for (PrintSelectType type : available_print_actions()) {
            SideButton* btn = new SideButton(p, print_select_type_label(type), "");
            btn->SetCornerRadius(0);
            btn->Bind(wxEVT_BUTTON, [this, p, type](wxCommandEvent&) {
                select_print_action(type);
                p->Dismiss();
            });
            p->append_button(btn);
        }
        p->Popup(m_print_btn);
         }
    );

    /*
    Button * aux_btn = new Button(this, _L("Auxiliary"));
    aux_btn->SetBackgroundColour(0x3B4446);
    aux_btn->Bind(wxEVT_BUTTON, [](auto e) {
        wxGetApp().sidebar().show_auxiliary_dialog();
    });
    sizer->Add(aux_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 1 * em / 10);
    */
    sizer->Add(FromDIP(19), 0, 0, 0, 0);

    return sizer;
}

bool MainFrame::get_enable_slice_status()
{
    bool enable = true;

    bool on_slicing = m_plater->is_background_process_slicing();
    if (on_slicing) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": on slicing, return false directly!");
        return false;
    }
    else if  (m_plater->only_gcode_mode() || m_plater->using_exported_file()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": in gcode/exported 3mf mode, return false directly!");
        return false;
    }

    PartPlateList &part_plate_list = m_plater->get_partplate_list();
    PartPlate *current_plate = part_plate_list.get_curr_plate();

    if (m_slice_select == eSliceAll)
    {
        /*if (part_plate_list.is_all_slice_results_valid())
        {
            enable = false;
        }
        else if (!part_plate_list.is_all_plates_ready_for_slice())
        {
            enable = false;
        }*/
        enable = m_plater->has_sliceable_plate_for_slice_all();
    }
    else if (m_slice_select == eSlicePlate)
    {
        if (current_plate->is_slice_result_valid())
        {
            enable = false;
        }
        else if (!current_plate->can_slice())
        {
            enable = false;
        }
        else if (m_plater->has_incompatible_mixed_filament_in_use())
        {
            enable = false;
        }
        else if (m_plater->is_plate_blocked_by_filament_temp_mixing(part_plate_list.get_curr_plate_index()))
        {
            enable = false;
        }
        else if (m_plater->is_plate_blocked_by_cold_plate(part_plate_list.get_curr_plate_index()))
        {
            enable = false;
        }
        else if (m_plater->is_plate_blocked_by_flow_ratio_zero(part_plate_list.get_curr_plate_index()))
        {
            enable = false;
        }
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": m_slice_select %1%, enable= %2% ")%m_slice_select %enable;
    return enable;
}

bool MainFrame::get_enable_print_status()
{
    bool enable = true;

    PartPlateList &part_plate_list = m_plater->get_partplate_list();
    PartPlate *current_plate = part_plate_list.get_curr_plate();
    bool is_all_plates = wxGetApp().plater()->get_preview_canvas3D()->is_all_plates_selected();
    if (m_print_select == ePrintAll)
    {
        if (!part_plate_list.is_all_slice_results_ready_for_print())
        {
            enable = false;
        }
    }
    else if (m_print_select == ePrintPlate)
    {
        if (!current_plate->is_slice_result_ready_for_print())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eExportGcode)
    {
        if (!current_plate->is_slice_result_valid())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eSendGcode)
    {
        if (!current_plate->is_slice_result_valid())
            enable = false;
        if (!can_send_gcode())
            enable = false;
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eUploadGcode)
    {
        if (!current_plate->is_slice_result_valid())
            enable = false;
        if (!can_send_gcode())
            enable = false;
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eExportSlicedFile)
    {
        if (!current_plate->is_slice_result_ready_for_export())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
	}
	else if (m_print_select == eSendToPrinter)
	{
		if (!current_plate->is_slice_result_ready_for_print())
		{
			enable = false;
		}
        enable = enable && !is_all_plates;
	}
    else if (m_print_select == eSendToPrinterAll)
    {
        if (!part_plate_list.is_all_slice_results_ready_for_print())
        {
            enable = false;
        }
    }
    else if (m_print_select == eExportAllSlicedFile)
    {
        if (!part_plate_list.is_all_slice_result_ready_for_export())
        {
            enable = false;
        }
    }
    else if (m_print_select == ePrintMultiMachine)
    {
        if (!current_plate->is_slice_result_ready_for_print())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": m_print_select %1%, enable= %2% ")%m_print_select %enable;

    return enable;
}

void MainFrame::update_side_button_style()
{
    // BBS
    int em = em_unit();

    /*m_slice_btn->SetLayoutStyle(1);
    m_slice_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Center, FromDIP(15));
    m_slice_btn->SetMinSize(wxSize(-1, FromDIP(24)));
    m_slice_btn->SetCornerRadius(FromDIP(12));
    m_slice_btn->SetExtraSize(wxSize(FromDIP(38), FromDIP(10)));
    m_slice_btn->SetBottomColour(wxColour(0x3B4446));*/
    StateColor m_btn_bg_enable = StateColor(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(48, 221, 112), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)
    );

    // m_publish_btn->SetMinSize(wxSize(FromDIP(125), FromDIP(24)));
    // m_publish_btn->SetCornerRadius(FromDIP(12));
    // m_publish_btn->SetBackgroundColor(m_btn_bg_enable);
    // m_publish_btn->SetBorderColor(m_btn_bg_enable);
    // m_publish_btn->SetBackgroundColour(wxColour(59,68,70));
    // m_publish_btn->SetTextColor(StateColor::darkModeColorFor("#FFFFFE"));

    m_slice_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Left, FromDIP(15));
    m_slice_btn->SetCornerRadius(FromDIP(12));
    m_slice_btn->SetExtraSize(wxSize(FromDIP(38), FromDIP(10)));
    m_slice_btn->SetMinSize(wxSize(-1, FromDIP(24)));

    m_slice_option_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Center);
    m_slice_option_btn->SetCornerRadius(FromDIP(12));
    m_slice_option_btn->SetExtraSize(wxSize(FromDIP(10), FromDIP(10)));
    m_slice_option_btn->SetIconOffset(FromDIP(2));
    m_slice_option_btn->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));

    m_print_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Left, FromDIP(15));
    m_print_btn->SetCornerRadius(FromDIP(12));
    m_print_btn->SetExtraSize(wxSize(FromDIP(38), FromDIP(10)));
    m_print_btn->SetMinSize(wxSize(-1, FromDIP(24)));

    m_print_option_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Center);
    m_print_option_btn->SetCornerRadius(FromDIP(12));
    m_print_option_btn->SetExtraSize(wxSize(FromDIP(10), FromDIP(10)));
    m_print_option_btn->SetIconOffset(FromDIP(2));
    m_print_option_btn->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
}

void MainFrame::update_slice_print_status(SlicePrintEventType event, bool can_slice, bool can_print)
{
    bool enable_print = true, enable_slice = true;

    if (event == eEventPlateUpdate)
        enable_slice = get_enable_slice_status();
    else if (!can_slice)
        // Don't hard-disable the slice button; let get_enable_slice_status()
        // decide. In eSliceAll mode other plates may still be sliceable even
        // if the caller thinks the current plate isn't.
        enable_slice = get_enable_slice_status();

    if (!can_print)
        enable_print = false;


    //process print logic
    if (enable_print)
    {
        enable_print = get_enable_print_status();
    }

    //process slice logic
    if (event != eEventPlateUpdate && enable_slice)
    {
        enable_slice = get_enable_slice_status();
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" m_slice_select %1%: can_slice= %2%, can_print %3%, enable_slice %4%, enable_print %5% ")%m_slice_select % can_slice %can_print %enable_slice %enable_print;
    m_print_btn->Enable(enable_print);
    m_slice_btn->Enable(enable_slice);
    m_slice_enable = enable_slice;
    m_print_enable = enable_print;

    if (wxGetApp().mainframe)
        wxGetApp().plater()->update_title_dirty_status();
}


void MainFrame::on_dpi_changed(const wxRect& suggested_rect)
{
    wxGetApp().update_fonts(this);
    this->SetFont(this->normal_font());

#ifdef _MSW_DARK_MODE
    // update common mode sizer
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(m_tabpanel)->Rescale();
#endif

#ifndef __APPLE__
    // BBS
    m_topbar->Rescale();
#endif

    m_tabpanel->Rescale();

    update_side_button_style();

    m_slice_btn->Rescale();
    m_print_btn->Rescale();
    m_slice_option_btn->Rescale();
    m_print_option_btn->Rescale();

    // update Plater
    wxGetApp().plater()->msw_rescale();

    // update Tabs
    //BBS GUI refactor: remove unused layout new/dlg
    //if (m_layout != ESettingsLayout::Dlg) // Do not update tabs if the Settings are in the separated dialog
    m_param_panel->msw_rescale();
    if(m_project)
        m_project->msw_rescale();
    if(m_monitor)
        m_monitor->msw_rescale();
    if(m_multi_machine)
        m_multi_machine->msw_rescale();
    if(m_calibration)
        m_calibration->msw_rescale();

    // BBS
#if 0
    for (size_t id = 0; id < m_menubar->GetMenuCount(); id++)
        msw_rescale_menu(m_menubar->GetMenu(id));
#endif

    // Workarounds for correct Window rendering after rescale

    /* Even if Window is maximized during moving,
     * first of all we should imitate Window resizing. So:
     * 1. cancel maximization, if it was set
     * 2. imitate resizing
     * 3. set maximization, if it was set
     */
    const bool is_maximized = this->IsMaximized();
    if (is_maximized)
        this->Maximize(false);

    /* To correct window rendering (especially redraw of a status bar)
     * we should imitate window resizing.
     */
    const wxSize& sz = this->GetSize();
    this->SetSize(sz.x + 1, sz.y + 1);
    this->SetSize(sz);

    this->Maximize(is_maximized);
}

void MainFrame::on_sys_color_changed()
{
    wxBusyCursor wait;

    // update label colors in respect to the system mode
    wxGetApp().init_label_colours();

    // A UI theme colours the bar behind the tab buttons and the title bar. Both were set when they
    // were built, so redo them: this is also how a live theme switch reaches them (Theme.hpp).
    m_tabpanel->GetBtnsListCtrl()->UpdateColours();
    if (m_topbar)
        m_topbar->ThemeChanged();
    if (m_mac_topbar_panel) {
        m_mac_topbar_panel->SetBackgroundColour(Theme::colour("titlebar_bg", wxColour(38, 46, 48)));
        m_mac_topbar_panel->Refresh();
    }

#ifndef __WINDOWS__
    wxGetApp().force_colors_update();
    wxGetApp().update_ui_from_settings();
#endif //__APPLE__

#ifdef __WXMSW__
    wxGetApp().UpdateDarkUI(m_tabpanel);
 //   m_statusbar->update_dark_ui();
#ifdef _MSW_DARK_MODE
    // update common mode sizer
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(m_tabpanel)->Rescale();
#endif
#endif

    // BBS
    m_tabpanel->Rescale();
    m_param_panel->msw_rescale();

    // update Plater
    wxGetApp().plater()->sys_color_changed();
    if(m_monitor)
        m_monitor->on_sys_color_changed();
    if(m_calibration)
        m_calibration->on_sys_color_changed();
    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->sys_color_changed();
    for (auto tab : wxGetApp().model_tabs_list)
        tab->sys_color_changed();
    wxGetApp().plate_tab->sys_color_changed();

    MenuFactory::sys_color_changed(m_menubar);

    // Before RecreateAll: the hub view switches theme in place (it is excluded from the reload).
    if (m_home)
        m_home->sys_color_changed();
    WebView::RecreateAll();

    this->Refresh();
}

// On macOS, we use system menu bar, which handles the key accelerators automatically and breaks key handling in normal typing
// See https://github.com/SoftFever/OrcaSlicer/issues/8152
// So we disable some of the accelerators on macOS, by replacing the accelerator seperator to a hyphen.
#ifdef __APPLE__
static const wxString sep = " - ";
#else
static const wxString sep = "\t";
#endif

static wxMenu* generate_help_menu()
{
    wxMenu* helpMenu = new wxMenu();

    // shortcut key
    append_menu_item(helpMenu, wxID_ANY, _L("Keyboard Shortcuts") + sep + "&?", _L("Show the list of the keyboard shortcuts"),
        [](wxCommandEvent&) { wxGetApp().keyboard_shortcuts(); });
    // Show Beginner's Tutorial
    append_menu_item(helpMenu, wxID_ANY, _L("Setup Wizard"), _L("Setup Wizard"), [](wxCommandEvent &) {wxGetApp().ShowUserGuide();});

    helpMenu->AppendSeparator();
    // Open Config Folder
    append_menu_item(helpMenu, wxID_ANY, _L("Show Configuration Folder"), _L("Show Configuration Folder"),
        [](wxCommandEvent&) { Slic3r::GUI::desktop_open_datadir_folder(); });
    // Export Logs
    append_menu_item(helpMenu, wxID_ANY, _L("Export Logs"), _L("Export application logs as ZIP file"),
        [](wxCommandEvent&) { wxGetApp().mainframe->export_logs(); });

    append_menu_item(helpMenu, wxID_ANY, _L("Show Tip of the Day"), _L("Show Tip of the Day"), [](wxCommandEvent&) {
        wxGetApp().plater()->get_dailytips()->open();
        GLCanvas3D* canvas = wxGetApp().plater()->get_current_canvas3D();
        if (canvas)
            canvas->set_as_dirty();
        });

    // Report a bug
    //append_menu_item(helpMenu, wxID_ANY, _L("Report Bug(TODO)"), _L("Report a bug of EdgeSlicer"),
    //    [](wxCommandEvent&) {
    //        //TODO
    //    });
    // Check New Version
    append_menu_item(
        helpMenu, wxID_ANY, _L("Check for Update"), _L("Check for Update"),
        [](wxCommandEvent&) {
            wxGetApp().check_new_version_sf(true, UPDATE_BUSER);
        }, "", nullptr, []() {
            return true;
        });

    // Profile packages come only from a self-hosted server named in the ini ("profile_upgrade_url");
    // without one there is nothing to check, so the item is not offered.
    if (!wxGetApp().app_config->get_preset_upgrade_url().empty())
        append_menu_item(
            helpMenu, wxID_ANY, _L("Check for Process Preset Updates"), _L("Check for Process Preset Updates"),
            [](wxCommandEvent&) { 
                wxGetApp().check_preset_version();

            },
            "", nullptr, []() { return true; });

    // No "Check for Web Resource Updates" / "Import Web Resource": the web pages are served from the
    // installed resources only (HttpServer::map_url_to_file_path), so a downloaded or imported
    // package would never be shown.

    append_menu_item(helpMenu, wxID_ANY, _L("Import Profile"), _L("Import Profile"), [](wxCommandEvent&) {
        wxGetApp().import_presets();
    });

    append_menu_item(helpMenu, wxID_ANY, _L("Open Network Test"), _L("Open Network Test"), [](wxCommandEvent&) {
        // Plain modal dialog: its test threads never touch the dialog itself (they keep only
        // its NetworkTestRunner alive), so it can be destroyed as soon as it closes.
        NetworkTestDialog dlg(wxGetApp().mainframe);
        dlg.ShowModal();
    });

    // Windows Firewall dropping Bambu LAN discovery (UDP 2021/1990) is the usual reason printers
    // never show up in the Device list; the dialog reads the rules and offers a one-UAC fix.
    // Windows only: there is no Windows Firewall to check elsewhere.
    if (FirewallCheckDialog::supported())
        append_menu_item(helpMenu, wxID_ANY, _L("Check Windows Firewall..."),
                         _L("Check whether Windows Firewall blocks printer discovery, and fix its rules"),
                         [](wxCommandEvent&) { FirewallCheckDialog::show_modal(wxGetApp().mainframe); });

    // The checklist the first-time notice shows (account, firewall, LAN mode, access code, SD card, network).
    append_menu_item(helpMenu, wxID_ANY, _L("Bambu Lab Printer Help..."),
                     _L("What a Bambu Lab printer needs to show up and connect"),
                     [](wxCommandEvent&) { BambuSetupNoticeDialog::show_modal(wxGetApp().mainframe); });

    // About
#ifndef __APPLE__
    wxString about_title = wxString::Format(_L("&About %s"), SLIC3R_APP_FULL_NAME);
    append_menu_item(helpMenu, wxID_ANY, about_title, about_title,
            [](wxCommandEvent&) { Slic3r::GUI::about(); });
#endif

    return helpMenu;
}


static void add_common_publish_menu_items(wxMenu* publish_menu, MainFrame* mainFrame)
{
#ifndef __WINDOWS__
    append_menu_item(publish_menu, wxID_ANY, _L("Upload Models"), _L("Upload Models"),
        [](wxCommandEvent&) {
            if (!wxGetApp().getAgent()) {
                BOOST_LOG_TRIVIAL(info) << "publish: no agent";
                return;
            }

            json j;
            NetworkAgent* agent = GUI::wxGetApp().getAgent();

            //if (GUI::wxGetApp().plater()->model().objects.empty()) return;
            wxGetApp().open_publish_page_dialog();
        });

    append_menu_item(publish_menu, wxID_ANY, _L("Download Models"), _L("Download Models"),
        [](wxCommandEvent&) {
            if (!wxGetApp().getAgent()) {
                BOOST_LOG_TRIVIAL(info) << "publish: no agent";
                return;
}

            //if (GUI::wxGetApp().plater()->model().objects.empty()) return;
            wxGetApp().open_mall_page_dialog();
        });
#endif
}

static void add_common_view_menu_items(wxMenu* view_menu, MainFrame* mainFrame, std::function<bool(void)> can_change_view)
{
    // The camera control accelerators are captured by GLCanvas3D::on_char().
    append_menu_item(view_menu, wxID_ANY, _L("Default View") + "\t" + ctrl + "0", _L("Default View"), [mainFrame](wxCommandEvent&) {
        mainFrame->select_view("plate");
        mainFrame->plater()->get_current_canvas3D()->zoom_to_bed();
        },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    //view_menu->AppendSeparator();
    //TRN To be shown in the main menu View->Top
    append_menu_item(view_menu, wxID_ANY, _L("Top") + "\t" + ctrl + "1", _L("Top View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("top"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    //TRN To be shown in the main menu View->Bottom
    append_menu_item(view_menu, wxID_ANY, _L("Bottom") + "\t" + ctrl + "2", _L("Bottom View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("bottom"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Front") + "\t" + ctrl + "3", _L("Front View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("front"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Rear") + "\t" + ctrl + "4", _L("Rear View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("rear"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Left") + "\t" + ctrl + "5", _L("Left View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("left"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Right") + "\t" + ctrl + "6", _L("Right View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("right"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);

#ifdef __APPLE__
    const wxString fitCameraLabel = _L("Fit in all view");
#else
    const wxString fitCameraLabel = _L("Fit in all view") + "\tZ";
#endif
    append_menu_item(view_menu, wxID_ANY, fitCameraLabel, _L("Fit in all view"),
        [mainFrame](wxCommandEvent&) { mainFrame->ZoomCameraToFit(); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
}

// Ultra: rebuild the contextual Account menu for the currently-selected printer's vendor.
// Snapmaker / Bambu Lab get real login+logout (shows the signed-in account name); Flashforge
// is a placeholder until FF cloud login (Phase B); other vendors fall back to Snapmaker.
void MainFrame::refresh_account_menu(wxMenu* menu)
{
    if (!menu) return;
    while (menu->GetMenuItemCount() > 0)
        menu->Destroy(menu->FindItemByPosition(0));
    wxMenu* m_account_menu = menu; // keep the body below unchanged

    auto& app = wxGetApp();
    int brand = 0; // 0 = Snapmaker/other, 1 = Bambu, 2 = Flashforge
    if (app.preset_bundle) {
        if (app.preset_bundle->is_bbl_vendor()) {
            brand = 1;
        } else {
            const Preset&        pp = app.preset_bundle->printers.get_edited_preset();
            const VendorProfile* v  = pp.vendor;
            if (!v) { const Preset* parent = app.preset_bundle->printers.get_selected_preset_parent(); if (parent) v = parent->vendor; }
            if (v && v->id == "Flashforge") brand = 2;
        }
    }

    if (brand == 1) { // Bambu Lab
        if (app.is_user_login()) {
            std::string nm = app.get_bambu_user_name();
            wxMenuItem* who = m_account_menu->Append(wxID_ANY, _L("Bambu Lab") + ": " + from_u8(nm.empty() ? "signed in" : nm));
            who->Enable(false);
            append_menu_item(m_account_menu, wxID_ANY, _L("Log out of Bambu Account"), _L("Sign out of your Bambu Lab account"),
                [](wxCommandEvent&) { wxGetApp().request_user_logout(); });
        } else {
            wxMenuItem* who = m_account_menu->Append(wxID_ANY, _L("Bambu Lab") + ": " + _L("signed out"));
            who->Enable(false);
            append_menu_item(m_account_menu, wxID_ANY, _L("Log in to Bambu Account..."), _L("Sign in to your Bambu Lab account to see your cloud printers"),
                [](wxCommandEvent&) { wxGetApp().request_login(true); });
        }
    } else if (brand == 2) { // Flashforge
        wxMenuItem* soon = m_account_menu->Append(wxID_ANY, _L("Flashforge account (coming soon)"));
        soon->Enable(false);
    } else { // Snapmaker / default
        auto* ui = app.sm_get_userinfo();
        if (ui && ui->is_user_login()) {
            std::string nm = ui->get_user_name();
            wxMenuItem* who = m_account_menu->Append(wxID_ANY, _L("Snapmaker") + ": " + from_u8(nm.empty() ? "signed in" : nm));
            who->Enable(false);
            append_menu_item(m_account_menu, wxID_ANY, _L("Log out of Snapmaker Account"), _L("Sign out of your Snapmaker account"),
                [](wxCommandEvent&) { wxGetApp().sm_request_user_logout(); });
        } else {
            wxMenuItem* who = m_account_menu->Append(wxID_ANY, _L("Snapmaker") + ": " + _L("signed out"));
            who->Enable(false);
            append_menu_item(m_account_menu, wxID_ANY, _L("Log in to Snapmaker Account..."), _L("Sign in to your Snapmaker account"),
                [](wxCommandEvent&) { wxGetApp().sm_request_login(true); });
        }
    }
}

void MainFrame::init_menubar_as_editor()
{
#ifdef __APPLE__
    m_menubar = new wxMenuBar();
#endif

    // File menu
    wxMenu* fileMenu = new wxMenu;
    {
#ifdef __APPLE__
        // New Window
        append_menu_item(fileMenu, wxID_ANY, _L("New Window"), _L("Start a new window"),
                         [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr,
                         [this] { return m_plater != nullptr && wxGetApp().app_config->get("app", "single_instance") == "false"; }, this);
#endif
        // New Project
        append_menu_item(fileMenu, wxID_ANY, _L("New Project") + "\t" + ctrl + "N", _L("Start a new project"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->new_project(); }, "", nullptr,
            [this](){return can_start_new_project(); }, this);
        // Open Project

#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Open Project") + dots + "\t" + ctrl + "O", _L("Open a project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->load_project(); }, "menu_open", nullptr,
            [this](){return can_open_project(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Open Project") + dots + "\t" + ctrl + "O", _L("Open a project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->load_project(); }, "", nullptr,
            [this](){return can_open_project(); }, this);
#endif

        // Recent Project
        wxMenu* recent_projects_menu = new wxMenu();
        wxMenuItem* recent_projects_submenu = append_submenu(fileMenu, recent_projects_menu, wxID_ANY, _L("Recent projects"), "");
        m_recent_projects.UseMenu(recent_projects_menu);
        Bind(wxEVT_MENU, [this](wxCommandEvent& evt) {
            size_t file_id = evt.GetId() - wxID_FILE1;
            wxString filename = m_recent_projects.GetHistoryFile(file_id);
                open_recent_project(file_id, filename);
            }, wxID_FILE1, wxID_FILE1 + 49); // [5050, 5100)

        std::vector<std::string> recent_projects = wxGetApp().app_config->get_recent_projects();
        std::reverse(recent_projects.begin(), recent_projects.end());
        for (const std::string& project : recent_projects)
        {
            m_recent_projects.AddFileToHistory(from_u8(project));
        }
        m_recent_projects.LoadThumbnails();

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_open_project() && (m_recent_projects.GetCount() > 0)); }, recent_projects_submenu->GetId());

        // The Home tab shows Recent and Print History; the old start page (Snapmaker's model
        // library) stays one click away here.
        append_menu_item(fileMenu, wxID_ANY, _L("Start page"), _L("Show the start page with recent projects on the Home tab"),
            [this](wxCommandEvent&) { show_start_page(); }, "", nullptr,
            [this]() { return m_home != nullptr; }, this);

        // BBS: close save project
#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project") + "\t" + ctrl + "S", _L("Save current project to file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(); }, "menu_save", nullptr,
            [this](){return m_plater != nullptr && can_save(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project") + "\t" + ctrl + "S", _L("Save current project to file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(); }, "", nullptr,
            [this](){return m_plater != nullptr && can_save(); }, this);
#endif

#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project as") + dots + "\t" + ctrl + shift + "S", _L("Save current project as"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(true); }, "menu_save", nullptr,
            [this](){return m_plater != nullptr && can_save_as(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project as") + dots + "\t" + ctrl + shift + "S", _L("Save current project as"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(true); }, "", nullptr,
            [this](){return m_plater != nullptr && can_save_as(); }, this);
#endif


        fileMenu->AppendSeparator();

        // BBS
        wxMenu *import_menu = new wxMenu();
#ifndef __APPLE__
        append_menu_item(import_menu, wxID_ANY, _L("Import 3MF/STL/STEP/SVG/OBJ/AMF/GLB") + dots + "\t" + ctrl + "I", _L("Load a model"),
            [this](wxCommandEvent&) { if (m_plater) {
            m_plater->add_model();
        } }, "menu_import", nullptr,
            [this](){return can_add_models(); }, this);
#else
        append_menu_item(import_menu, wxID_ANY, _L("Import 3MF/STL/STEP/SVG/OBJ/AMF/GLB") + dots + "\t" + ctrl + "I", _L("Load a model"),
            [this](wxCommandEvent&) { if (m_plater) { m_plater->add_model(); } }, "", nullptr,
            [this](){return can_add_models(); }, this);
#endif
        append_menu_item(import_menu, wxID_ANY, _L("Import Zip Archive") + dots, _L("Load models contained within a zip archive"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->import_zip_archive(); }, "menu_import", nullptr,
            [this]() { return can_add_models(); });
        append_menu_item(import_menu, wxID_ANY, _L("Import Configs") + dots /*+ "\t" + ctrl + "I"*/, _L("Load configs"),
            [this](wxCommandEvent&) { load_config_file(); }, "menu_import", nullptr,
            [this](){return true; }, this);
        // Ultra: import the config block embedded in a sliced G-code file.
        append_menu_item(import_menu, wxID_ANY, _L("Import Config from G-code") + dots, _L("Load print/filament/printer settings embedded in a sliced G-code file"),
            [this](wxCommandEvent&) { import_config_from_gcode(); }, "menu_import", nullptr,
            [this](){return true; }, this);

        append_submenu(fileMenu, import_menu, wxID_ANY, _L("Import"), "");


        wxMenu* export_menu = new wxMenu();
        // BBS export as STL
        append_menu_item(export_menu, wxID_ANY, _L("Export all objects as one STL") + dots, _L("Export all objects as one STL"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_stl(); }, "menu_export_stl", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export all objects as STLs") + dots, _L("Export all objects as STLs"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_stl(false, false, true); }, "menu_export_stl", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export as STEP") + dots,
            _L("Export the selected objects, or all objects on the current plate, as a STEP file of solids"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_step(); }, "menu_export_stl", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export Generic 3MF") + dots/* + "\t" + ctrl + "G"*/, _L("Export 3mf file without using some 3mf-extensions"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_core_3mf(); }, "menu_export_sliced_file", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export Bambu 3MF") + dots, _L("Export a project Bambu Studio opens with its settings (settings Bambu Studio does not have are left out)"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_bambu_3mf(); }, "menu_export_sliced_file", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export && Open in Bambu Studio") + dots, _L("Export a project Bambu Studio opens with its settings, then launch Bambu Studio with it"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_and_open_in_bambu_studio(); }, "menu_export_sliced_file", nullptr,
            [this](){return can_export_model(); }, this);
        // BBS export .gcode.3mf
        append_menu_item(export_menu, wxID_ANY, _L("Export plate sliced file") + dots + "\t" + ctrl + "G", _L("Export current sliced file"),
            [this](wxCommandEvent&) { if (m_plater) wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE)); }, "menu_export_sliced_file", nullptr,
            [this](){return can_export_gcode(); }, this);

        append_menu_item(export_menu, wxID_ANY, _L("Export all plate sliced file") + dots/* + "\t" + ctrl + "G"*/, _L("Export all plate sliced file"),
            [this](wxCommandEvent&) { if (m_plater) wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_ALL_SLICED_FILE)); }, "menu_export_sliced_file", nullptr,
            [this]() {return can_export_all_gcode(); }, this);

        append_menu_item(export_menu, wxID_ANY, _L("Export G-code") + dots/* + "\t" + ctrl + "G"*/, _L("Export current plate as G-code"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_gcode(false); }, "menu_export_gcode", nullptr,
            [this]() {return can_export_gcode(); }, this);
        append_menu_item(
            export_menu, wxID_ANY, _L("Export Preset Bundle") + dots /* + "\t" + ctrl + "E"*/, _L("Export current configuration to files"),
            [this](wxCommandEvent &) { export_config(); },
            "menu_export_config", nullptr,
            []() { return true; }, this);

        append_submenu(fileMenu, export_menu, wxID_ANY, _L("Export"), "");

        fileMenu->AppendSeparator();

#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_EXIT, _L("Quit"), wxString::Format(_L("Quit")),
            [this](wxCommandEvent&) { request_quit(false); }, "menu_exit", nullptr);
#else
        append_menu_item(fileMenu, wxID_EXIT, _L("Quit"), wxString::Format(_L("Quit")),
            [this](wxCommandEvent&) { request_quit(false); }, "", nullptr);
#endif
    }

    // Edit menu
    wxMenu* editMenu = nullptr;
    if (m_plater != nullptr)
    {
        editMenu = new wxMenu();

    auto handle_key_event = [](wxKeyEvent& evt) {
        if (wxGetApp().imgui()->update_key_data(evt)) {
            GLCanvas3D* canvas = wxGetApp().plater()->get_current_canvas3D();
            if (canvas) {
                canvas->render();
                return true;
            }
        }
        return false;
    };
#ifndef __APPLE__
        // BBS undo
        append_menu_item(editMenu, wxID_ANY, _L("Undo") + "\t" + ctrl + "Z",
            _L("Undo"), [this](wxCommandEvent&) { m_plater->undo(); },
            "menu_undo", nullptr, [this](){return m_plater->can_undo(); }, this);
        // BBS redo
        append_menu_item(editMenu, wxID_ANY, _L("Redo") + "\t" + ctrl + "Y",
            _L("Redo"), [this](wxCommandEvent&) { m_plater->redo(); },
            "menu_redo", nullptr, [this](){return m_plater->can_redo(); }, this);
        editMenu->AppendSeparator();
        // BBS Cut TODO
        append_menu_item(editMenu, wxID_ANY, _L("Cut") + "\t" + ctrl + "X",
            _L("Cut selection to clipboard"), [this](wxCommandEvent&) {m_plater->cut_selection_to_clipboard(); },
            "menu_cut", nullptr, [this]() {return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Copy
        append_menu_item(editMenu, wxID_ANY, _L("Copy") + "\t" + ctrl + "C",
            _L("Copy selection to clipboard"), [this](wxCommandEvent&) { m_plater->copy_selection_to_clipboard(); },
            "menu_copy", nullptr, [this](){return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Paste
        append_menu_item(editMenu, wxID_ANY, _L("Paste") + "\t" + ctrl + "V",
            _L("Paste clipboard"), [this](wxCommandEvent&) { m_plater->paste_from_clipboard(); },
            "menu_paste", nullptr, [this](){return m_plater->can_paste_from_clipboard(); }, this);
        // BBS Delete selected
        append_menu_item(editMenu, wxID_ANY, _L("Delete selected") + "\t" + _L("Del"),
            _L("Deletes the current selection"),[this](wxCommandEvent&) { m_plater->remove_selected(); },
            "menu_remove", nullptr, [this](){return can_delete(); }, this);
        //BBS: delete all
        append_menu_item(editMenu, wxID_ANY, _L("Delete all") + "\t" + ctrl + "D",
            _L("Deletes all objects"),[this](wxCommandEvent&) { m_plater->delete_all_objects_from_model(); },
            "menu_remove", nullptr, [this](){return can_delete_all(); }, this);
        editMenu->AppendSeparator();
        // BBS Clone Selected
        append_menu_item(editMenu, wxID_ANY, _L("Clone selected") /*+ "\t" + ctrl + "M"*/,
            _L("Clone copies of selections"),[this](wxCommandEvent&) {
                m_plater->clone_selection();
            },
            "menu_remove", nullptr, [this](){return can_clone(); }, this);
        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("Duplicate Current Plate"),
            _L("Duplicate the current plate"),[this](wxCommandEvent&) {
                m_plater->duplicate_plate();
            },
            "menu_remove", nullptr, [this](){return true;}, this);
        editMenu->AppendSeparator();
#else
        // BBS undo
        append_menu_item(editMenu, wxID_ANY, _L("Undo") + sep + ctrl_t + "Z",
            _L("Undo"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'Z';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->undo(); },
            "", nullptr, [this](){return m_plater->can_undo(); }, this);
        // BBS redo
        append_menu_item(editMenu, wxID_ANY, _L("Redo") + sep + ctrl_t + "Y",
            _L("Redo"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'Y';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->redo(); },
            "", nullptr, [this](){return m_plater->can_redo(); }, this);
        editMenu->AppendSeparator();
        // BBS Cut TODO
        append_menu_item(editMenu, wxID_ANY, _L("Cut") + sep + ctrl_t + "X",
            _L("Cut selection to clipboard"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'X';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->cut_selection_to_clipboard(); },
            "", nullptr, [this]() {return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Copy
        append_menu_item(editMenu, wxID_ANY, _L("Copy") + sep + ctrl_t + "C",
            _L("Copy selection to clipboard"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'C';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->copy_selection_to_clipboard(); },
            "", nullptr, [this](){return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Paste
        append_menu_item(editMenu, wxID_ANY, _L("Paste") + sep + ctrl_t + "V",
            _L("Paste clipboard"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'V';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->paste_from_clipboard(); },
            "", nullptr, [this](){return m_plater->can_paste_from_clipboard(); }, this);
#if 0
        // BBS Delete selected
        append_menu_item(editMenu, wxID_ANY, _L("Delete selected") + "\t" + _L("Backspace"),
            _L("Deletes the current selection"),[this](wxCommandEvent&) {
                m_plater->remove_selected();
            },
            "", nullptr, [this](){return can_delete(); }, this);
#endif
        //BBS: delete all
        append_menu_item(editMenu, wxID_ANY, _L("Delete all") + "\t" + ctrl + "D",
            _L("Deletes all objects"),[this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'D';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->delete_all_objects_from_model(); },
            "", nullptr, [this](){return can_delete_all(); }, this);
        editMenu->AppendSeparator();
        // BBS Clone Selected
        append_menu_item(editMenu, wxID_ANY, _L("Clone selected") + "\t" + ctrl + "K",
            _L("Clone copies of selections"),[this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'M';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->clone_selection();
            },
            "", nullptr, [this](){return can_clone(); }, this);
        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("Duplicate Current Plate"),
            _L("Duplicate the current plate"),[this, handle_key_event](wxCommandEvent&) {
                m_plater->duplicate_plate();
            },
            "", nullptr, [this](){return true;}, this);
        editMenu->AppendSeparator();

#endif

        // BBS Select All
        append_menu_item(editMenu, wxID_ANY, _L("Select all") + sep + ctrl_t + "A",
            _L("Selects all objects"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'A';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->select_all(); },
            "", nullptr, [this](){return can_select(); }, this);
        // BBS Deslect All
        append_menu_item(editMenu, wxID_ANY, _L("Deselect all") + sep + _L("Esc"),
            _L("Deselects all objects"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.m_keyCode = WXK_ESCAPE;
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->deselect_all(); },
            "", nullptr, [this](){return can_deselect(); }, this);
        //editMenu->AppendSeparator();
        //append_menu_check_item(editMenu, wxID_ANY, _L("Show Model Mesh(TODO)"),
        //    _L("Display triangles of models."), [this](wxCommandEvent& evt) {
        //        wxGetApp().app_config->set_bool("show_model_mesh", evt.GetInt() == 1);
        //    }, nullptr, [this]() {return can_select(); }, [this]() { return wxGetApp().app_config->get("show_model_mesh").compare("true") == 0; }, this);
        //append_menu_check_item(editMenu, wxID_ANY, _L("Show Model Shadow(TODO)"), _L("Display shadow of objects."),
        //    [this](wxCommandEvent& evt) {
        //        wxGetApp().app_config->set_bool("show_model_shadow", evt.GetInt() == 1);
        //    }, nullptr, [this]() {return can_select(); }, [this]() { return wxGetApp().app_config->get("show_model_shadow").compare("true") == 0; }, this);
        //editMenu->AppendSeparator();
        //append_menu_check_item(editMenu, wxID_ANY, _L("Show Printable Box(TODO)"), _L("Display printable box."),
        //    [this](wxCommandEvent& evt) {
        //        wxGetApp().app_config->set_bool("show_printable_box", evt.GetInt() == 1);
        //    }, nullptr, [this]() {return can_select(); }, [this]() { return wxGetApp().app_config->get("show_printable_box").compare("true") == 0; }, this);
    }

    // BBS

    //publish menu

    /*if (m_plater) {
        publishMenu = new wxMenu();
        add_common_publish_menu_items(publishMenu, this);
        publishMenu->AppendSeparator();
    }*/

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater) {
        viewMenu = new wxMenu();
        add_common_view_menu_items(viewMenu, this, std::bind(&MainFrame::can_change_view, this));
        viewMenu->AppendSeparator();

        //BBS perspective view
        wxWindowID camera_id_base = wxWindow::NewControlId(int(wxID_CAMERA_COUNT));
        auto perspective_item = append_menu_radio_item(viewMenu, wxID_CAMERA_PERSPECTIVE + camera_id_base, _L("Use Perspective View"), _L("Use Perspective View"),
            [this](wxCommandEvent&) {
                wxGetApp().app_config->set_bool("use_perspective_camera", true);
                wxGetApp().update_ui_from_settings();
            }, nullptr);
        //BBS orthogonal view
        auto orthogonal_item = append_menu_radio_item(viewMenu, wxID_CAMERA_ORTHOGONAL + camera_id_base, _L("Use Orthogonal View"), _L("Use Orthogonal View"),
            [this](wxCommandEvent&) {
                wxGetApp().app_config->set_bool("use_perspective_camera", false);
                wxGetApp().update_ui_from_settings();
            }, nullptr);
        this->Bind(wxEVT_UPDATE_UI, [viewMenu, camera_id_base](wxUpdateUIEvent& evt) {
                if (wxGetApp().app_config->get("use_perspective_camera").compare("true") == 0)
                    viewMenu->Check(wxID_CAMERA_PERSPECTIVE + camera_id_base, true);
                else
                    viewMenu->Check(wxID_CAMERA_ORTHOGONAL + camera_id_base, true);
            }, perspective_item->GetId());
        append_menu_check_item(viewMenu, wxID_ANY, _L("Auto Perspective"), _L("Automatically switch between orthographic and perspective when changing from top/bottom/side views."),
            [this](wxCommandEvent&) {
                wxGetApp().app_config->set_bool("auto_perspective", !wxGetApp().app_config->get_bool("auto_perspective"));
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return m_tabpanel->GetSelection() == TabPosition::tp3DEditor || m_tabpanel->GetSelection() == TabPosition::tpPreview; },
            [this]() { return wxGetApp().app_config->get_bool("auto_perspective"); }, this);

        viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &G-code Window") + sep + "C", _L("Show G-code window in Preview scene."),
            [this](wxCommandEvent &) {
                wxGetApp().toggle_show_gcode_window();
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return m_tabpanel->GetSelection() == tpPreview; },
            [this]() { return wxGetApp().show_gcode_window(); }, this);

        append_menu_check_item(
            viewMenu, wxID_ANY, _L("Show 3D Navigator"), _L("Show 3D navigator in Prepare and Preview scene."),
            [this](wxCommandEvent&) {
                wxGetApp().toggle_show_3d_navigator();
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return m_tabpanel->GetSelection() == TabPosition::tp3DEditor || m_tabpanel->GetSelection() == TabPosition::tpPreview; },
            [this]() { return wxGetApp().show_3d_navigator(); }, this);

        append_menu_item(
            viewMenu, wxID_ANY, _L("Reset Window Layout"), _L("Reset to default window layout"),
            [this](wxCommandEvent&) { m_plater->reset_window_layout(); }, "", this,
            [this]() {
                return (m_tabpanel->GetSelection() == TabPosition::tp3DEditor || m_tabpanel->GetSelection() == TabPosition::tpPreview) &&
                       m_plater->is_sidebar_enabled();
            },
            this);

        viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &Labels") + "\t" + ctrl + "E", _L("Show object labels in 3D scene."),
            [this](wxCommandEvent&) { m_plater->show_view3D_labels(!m_plater->are_view3D_labels_shown()); m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT)); }, this,
            [this]() { return m_plater->is_view3D_shown(); }, [this]() { return m_plater->are_view3D_labels_shown(); }, this);

        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &Overhang"), _L("Show object overhang highlight in 3D scene."),
            [this](wxCommandEvent &) {
                m_plater->show_view3D_overhang(!m_plater->is_view3D_overhang_shown());
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return m_plater->is_view3D_shown(); }, [this]() { return m_plater->is_view3D_overhang_shown(); }, this);

        /*viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &Wireframe") + "\t" + ctrl + shift + _L("Enter"), _L("Show wireframes in 3D scene."),
            [this](wxCommandEvent&) { m_plater->toggle_show_wireframe(); m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT)); }, this,
            [this]() { return m_plater->is_wireframe_enabled(); }, [this]() { return m_plater->is_show_wireframe(); }, this);*/

        // Slice Compare: viewMenu is populated here unconditionally (before the
        // __APPLE__ / topbar platform split below), so these items reach both
        // the Windows/Linux topbar dropdown (m_topbar->AddDropDownSubMenu) and
        // the native macOS menu bar (m_menubar->Append).
        viewMenu->AppendSeparator();
        append_menu_item(viewMenu, wxID_ANY, _L("Compare Slices") + dots, _L("Compare two sliced results visually"),
            [this](wxCommandEvent&) { Slic3r::GUI::open_slice_compare_frame(this, false); }, "", nullptr,
            []() { return true; }, this);
        append_menu_item(viewMenu, wxID_ANY, _L("Compare with Previous Slice"), _L("Compare the last two slices of this session"),
            [this](wxCommandEvent&) { Slic3r::GUI::open_slice_compare_frame(this, true); }, "", nullptr,
            []() { return true; }, this);
    }

    wxWindowID config_id_base = wxWindow::NewControlId(int(ConfigMenuCnt));
   #ifdef __APPLE__
    wxWindowID bambu_studio_id_base = wxWindow::NewControlId(int(2));
    wxMenu* parent_menu = m_menubar->OSXGetAppleMenu();
    //auto preference_item = new wxMenuItem(parent_menu, OrcaSlicerMenuPreferences + bambu_studio_id_base, _L("Preferences") + "\t" + ctrl + ",", "");
#else
    wxMenu* parent_menu = m_topbar->GetTopMenu();
    auto preference_item = new wxMenuItem(parent_menu, ConfigMenuPreferences + config_id_base, _L("Preferences") + "\t" + ctrl + "P", "");

#endif
   

#ifdef __APPLE__
    wxString about_title = wxString::Format(_L("&About %s"), SLIC3R_APP_FULL_NAME);

    append_menu_item(
        parent_menu, wxID_ANY, _L(about_title), "",
        [this](wxCommandEvent &) { Slic3r::GUI::about();},
        "", nullptr, []() { return true; }, this, 0);
    append_menu_item(
        parent_menu, wxID_ANY, _L("Preferences") + "\t" + ctrl + ",", "",
        [this](wxCommandEvent &) {
            PreferencesDialog dlg(this);
            dlg.ShowModal();
            plater()->get_current_canvas3D()->force_set_focus();
#if ENABLE_GCODE_LINES_ID_IN_H_SLIDER
            if (dlg.seq_top_layer_only_changed() || dlg.seq_seq_top_gcode_indices_changed())
#else
            if (dlg.seq_top_layer_only_changed())
#endif
                plater()->refresh_print();
        },
        "", nullptr, []() { return true; }, this, 1);
    append_menu_item(
        parent_menu, wxID_ANY, _L("Themes") + dots, _L("Colours, fonts, corners and the title bar"),
        [this](wxCommandEvent &) { wxGetApp().open_themes(); },
        "", nullptr, []() { return true; }, this, 2);
    //parent_menu->Insert(1, preference_item);
#endif
    // Help menu
    auto helpMenu = generate_help_menu();

#ifndef __APPLE__
    m_topbar->SetFileMenu(fileMenu);
    if (editMenu)
        m_topbar->AddDropDownSubMenu(editMenu, _L("Edit"));
    if (viewMenu)
        m_topbar->AddDropDownSubMenu(viewMenu, _L("View"));
    //BBS add Preference

    append_menu_item(
        m_topbar->GetTopMenu(), wxID_ANY, _L("Preferences") + "\t" + ctrl + "P", "",
        [this](wxCommandEvent &) {
            // Orca: Use GUI_App::open_preferences instead of direct call so windows associations are updated on exit
            wxGetApp().open_preferences();
            plater()->get_current_canvas3D()->force_set_focus();
        },
        "", nullptr, []() { return true; }, this);
    // Themes live in a window of their own so Preferences stays quick to open. No shortcut: the
    // obvious letters are taken.
    append_menu_item(
        m_topbar->GetTopMenu(), wxID_ANY, _L("Themes") + dots, _L("Colours, fonts, corners and the title bar"),
        [this](wxCommandEvent &) { wxGetApp().open_themes(); },
        "", nullptr, []() { return true; }, this);

    m_topbar->AddDropDownSubMenu(helpMenu, _L("Help"));

    // SoftFever calibrations

    // Temperature
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Temperature"), _L("Temperature Calibration"),
        [this](wxCommandEvent&) {
            if (!m_temp_calib_dlg)
                m_temp_calib_dlg = new Temp_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_temp_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Flow rate (with submenu)
    auto flowrate_menu = new wxMenu();
    append_menu_item(
        flowrate_menu, wxID_ANY, _L("Pass 1"), _L("Flow rate test - Pass 1"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(false, 1); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(flowrate_menu, wxID_ANY, _L("Pass 2"), _L("Flow rate test - Pass 2"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(false, 2); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    flowrate_menu->AppendSeparator();
    append_menu_item(flowrate_menu, wxID_ANY, _L("YOLO (Recommended)"), _L("Orca YOLO flowrate calibration, 0.01 step"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(true, 1); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(flowrate_menu, wxID_ANY, _L("YOLO (perfectionist version)"), _L("Orca YOLO flowrate calibration, 0.005 step"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(true, 2); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    m_topbar->GetCalibMenu()->AppendSubMenu(flowrate_menu, _L("Flow rate"));

    // Pressure Advance
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Pressure advance"), _L("Pressure advance"),
        [this](wxCommandEvent&) {
            if (!m_pa_calib_dlg)
                m_pa_calib_dlg = new PA_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_pa_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Retraction test
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Retraction test"), _L("Retraction test"),
        [this](wxCommandEvent&) {
            if (!m_retraction_calib_dlg)
                m_retraction_calib_dlg = new Retraction_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_retraction_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Max Volumetric Speed
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Max flowrate"), _L("Max flowrate"),
        [this](wxCommandEvent&) {
            if (!m_vol_test_dlg)
                m_vol_test_dlg = new MaxVolumetricSpeed_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vol_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Cornering (with submenu)
    auto cornering_menu = new wxMenu();
    append_menu_item(
        cornering_menu, wxID_ANY, _L("Junction Deviation"), _L("Junction Deviation calibration"),
        [this](wxCommandEvent&) {
            if (!m_junction_deviation_calib_dlg)
                m_junction_deviation_calib_dlg = new Junction_Deviation_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_junction_deviation_calib_dlg->ShowModal();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    m_topbar->GetCalibMenu()->AppendSubMenu(cornering_menu, _L("Cornering"));

    // Input Shaping (with submenu)
    auto input_shaping_menu = new wxMenu();
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Frequency"), _L("Input Shaping Frequency"),
        [this](wxCommandEvent&) {
            if (!m_IS_freq_calib_dlg)
                m_IS_freq_calib_dlg = new Input_Shaping_Freq_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_IS_freq_calib_dlg->ShowModal();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Damping/zeta factor"), _L("Input Shaping Damping/zeta factor"),
        [this](wxCommandEvent&) {
            if (!m_IS_damp_calib_dlg)
                m_IS_damp_calib_dlg = new Input_Shaping_Damp_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_IS_damp_calib_dlg->ShowModal();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    m_topbar->GetCalibMenu()->AppendSubMenu(input_shaping_menu, _L("Input Shaping"));

    // VFA
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("VFA"), _L("VFA"),
        [this](wxCommandEvent&) {
            if (!m_vfa_test_dlg)
                m_vfa_test_dlg = new VFA_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vfa_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // help
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Tutorial"), _L("Calibration help"),
        [this](wxCommandEvent&) {
            std::string url = "https://github.com/SoftFever/OrcaSlicer/wiki/Calibration";
            if (const std::string country_code = wxGetApp().app_config->get_country_code(); country_code == "CN") {
                // Use gitee mirror for China users
                url = "https://gitee.com/n0isyfox/orca-slicer-docs/wikis/%E6%A0%A1%E5%87%86/%E6%89%93%E5%8D%B0%E5%8F%82%E6%95%B0%E6%A0%A1%E5%87%86";
            }
            wxLaunchDefaultBrowser(url, wxBROWSER_NEW_WINDOW);
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

#else
    m_menubar->Append(fileMenu, wxString::Format("&%s", _L("File")));
    if (editMenu)
        m_menubar->Append(editMenu, wxString::Format("&%s", _L("Edit")));
    if (viewMenu)
        m_menubar->Append(viewMenu, wxString::Format("&%s", _L("View")));
    /*if (publishMenu)
        m_menubar->Append(publishMenu, wxString::Format("&%s", _L("3D Models")));*/

    // SoftFever calibrations
    auto calib_menu = new wxMenu();

    // Temperature
    append_menu_item(calib_menu, wxID_ANY, _L("Temperature"), _L("Temperature"),
        [this](wxCommandEvent&) {
            if (!m_temp_calib_dlg)
                m_temp_calib_dlg = new Temp_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_temp_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Flowrate (with submenu)
    auto flowrate_menu = new wxMenu();
    append_menu_item(flowrate_menu, wxID_ANY, _L("Pass 1"), _L("Flow rate test - Pass 1"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(false, 1); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(flowrate_menu, wxID_ANY, _L("Pass 2"), _L("Flow rate test - Pass 2"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(false, 2); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_submenu(calib_menu,flowrate_menu,wxID_ANY,_L("Flow rate"),_L("Flow rate"),"",
                   [this]() {return m_plater->is_view3D_shown();; });
    flowrate_menu->AppendSeparator();
    append_menu_item(flowrate_menu, wxID_ANY, _L("YOLO (Recommended)"), _L("Orca YOLO flowrate calibration, 0.01 step"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(true, 1); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(flowrate_menu, wxID_ANY, _L("YOLO (perfectionist version)"), _L("Orca YOLO flowrate calibration, 0.005 step"),
        [this](wxCommandEvent&) { if (m_plater) m_plater->calib_flowrate(true, 2); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Pressure Advance
    append_menu_item(calib_menu, wxID_ANY, _L("Pressure advance"), _L("Pressure advance"),
        [this](wxCommandEvent&) {
            if (!m_pa_calib_dlg)
                m_pa_calib_dlg = new PA_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_pa_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Retraction test
    append_menu_item(calib_menu, wxID_ANY, _L("Retraction test"), _L("Retraction test"),
        [this](wxCommandEvent&) {
            if (!m_retraction_calib_dlg)
                m_retraction_calib_dlg = new Retraction_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_retraction_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Max Volumetric Speed
    append_menu_item(calib_menu, wxID_ANY, _L("Max flowrate"), _L("Max flowrate"),
        [this](wxCommandEvent&) {
            if (!m_vol_test_dlg)
                m_vol_test_dlg = new MaxVolumetricSpeed_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vol_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Cornering (with submenu)
    auto cornering_menu = new wxMenu();
    append_menu_item(
        cornering_menu, wxID_ANY, _L("Junction Deviation"), _L("Junction Deviation calibration"),
        [this](wxCommandEvent&) {
            if (!m_junction_deviation_calib_dlg)
                m_junction_deviation_calib_dlg = new Junction_Deviation_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_junction_deviation_calib_dlg->ShowModal();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    calib_menu->AppendSubMenu(cornering_menu, _L("Cornering"));

    // Input Shaping (with submenu)
    auto input_shaping_menu = new wxMenu();
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Frequency"), _L("Input Shaping Frequency"),
        [this](wxCommandEvent&) {
            if (!m_IS_freq_calib_dlg)
                m_IS_freq_calib_dlg = new Input_Shaping_Freq_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_IS_freq_calib_dlg->ShowModal();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Damping/zeta factor"), _L("Input Shaping Damping/zeta factor"),
        [this](wxCommandEvent&) {
            if (!m_IS_damp_calib_dlg)
                m_IS_damp_calib_dlg = new Input_Shaping_Damp_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_IS_damp_calib_dlg->ShowModal();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    calib_menu->AppendSubMenu(input_shaping_menu, _L("Input Shaping"));

    // VFA
    append_menu_item(calib_menu, wxID_ANY, _L("VFA"), _L("VFA"),
        [this](wxCommandEvent&) {
            if (!m_vfa_test_dlg)
                m_vfa_test_dlg = new VFA_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vfa_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    // help
    append_menu_item(calib_menu, wxID_ANY, _L("Tutorial"), _L("Calibration help"),
        [this](wxCommandEvent&) { wxLaunchDefaultBrowser("https://github.com/SoftFever/OrcaSlicer/wiki/Calibration", wxBROWSER_NEW_WINDOW); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    m_menubar->Append(calib_menu,wxString::Format("&%s", _L("Calibration")));
    // Ultra: the Account entry lives in the custom top bar (BBLTopbar), not this native
    // menubar (which the fork hides) - see BBLTopbar's ID_ACCOUNT tool + refresh_account_menu.
    if (helpMenu)
        m_menubar->Append(helpMenu, wxString::Format("&%s", _L("Help")));
    SetMenuBar(m_menubar);

#endif

#ifdef _MSW_DARK_MODE
    if (wxGetApp().tabs_as_menu())
        m_menubar->EnableTop(6, false);
#endif

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent &) {
            Close();
        }, wxID_EXIT);
    }
#endif // __APPLE__
}

void MainFrame::set_max_recent_count(int max)
{
    max = max < 0 ? 0 : max > 10000 ? 10000 : max;
    size_t count = m_recent_projects.GetCount();
    m_recent_projects.SetMaxFiles(max);
    if (count != m_recent_projects.GetCount()) {
        count = m_recent_projects.GetCount();
        std::vector<std::string> recent_projects;
        for (size_t i = 0; i < count; ++i) {
            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
        }
        wxGetApp().app_config->set_recent_projects(recent_projects);
        wxGetApp().app_config->save();
        if (m_webview)
            m_webview->SendRecentList(-1);

        // wcp 订阅
        json data;
        wxGetApp().mainframe->get_recent_projects(data, INT_MAX);
        wxGetApp().recent_file_notify(data);
        
    }
}

void MainFrame::open_menubar_item(const wxString& menu_name,const wxString& item_name)
{
    if (m_menubar == nullptr)
        return;
    // Get menu object from menubar
    int     menu_index = m_menubar->FindMenu(menu_name);
    wxMenu* menu       = m_menubar->GetMenu(menu_index);
    if (menu == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Mainframe open_menubar_item function couldn't find menu: " << menu_name;
        return;
    }
    // Get item id from menu
    int     item_id   = menu->FindItem(item_name);
    if (item_id == wxNOT_FOUND)
    {
        // try adding three dots char
        item_id = menu->FindItem(item_name + dots);
    }
    if (item_id == wxNOT_FOUND)
    {
        BOOST_LOG_TRIVIAL(error) << "Mainframe open_menubar_item function couldn't find item: " << item_name;
        return;
    }
    // wxEVT_MENU will trigger item
    wxPostEvent((wxEvtHandler*)menu, wxCommandEvent(wxEVT_MENU, item_id));
}

void MainFrame::init_menubar_as_gcodeviewer()
{
    //BBS do not show gcode viewer mebu
#if 0
    wxMenu* fileMenu = new wxMenu;
    {
        append_menu_item(fileMenu, wxID_ANY, _L("&Open G-code") + dots + "\t" + ctrl + "O", _L("Open a G-code file"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->load_gcode(); }, "open", nullptr,
            [this]() {return m_plater != nullptr; }, this);
#ifdef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from Disk") + dots + "\t" + ctrl + shift + "R",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from Disk") + sep + "F5",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#endif // __APPLE__
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("Export &Toolpaths as OBJ") + dots, _L("Export toolpaths as OBJ"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->export_toolpaths_to_obj(); }, "export_plater", nullptr,
            [this]() {return can_export_toolpaths(); }, this);
        append_menu_item(fileMenu, wxID_ANY, _L("Open &Slicer") + dots, _L("Open Slicer"),
            [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr,
            []() {return true; }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_EXIT, _L("&Quit"), wxString::Format(_L("Quit %s"), SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { request_quit(false); });
    }

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater != nullptr) {
        viewMenu = new wxMenu();
        add_common_view_menu_items(viewMenu, this, std::bind(&MainFrame::can_change_view, this));
    }

    // helpmenu
    auto helpMenu = generate_help_menu();

    m_menubar = new wxMenuBar();
    m_menubar->Append(fileMenu, _L("&File"));
    if (viewMenu != nullptr) m_menubar->Append(viewMenu, _L("&View"));
    // Add additional menus from C++
    wxGetApp().add_config_menu(m_menubar);
    m_menubar->Append(helpMenu, _L("&Help"));
    SetMenuBar(m_menubar);

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            Close();
            }, wxID_EXIT);
    }
#endif // __APPLE__
#endif
}

void MainFrame::update_menubar()
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const bool is_fff = plater()->printer_technology() == ptFFF;
}

void MainFrame::reslice_now()
{
    if (!m_plater)
        return;
    if (!m_plater->confirm_filament_grouping_before_slice())
        return;
    (void) m_plater->reslice();
}

void MainFrame::start_slice()
{
    if (!m_plater)
        return;
    m_plater->exit_gizmo();
    m_plater->update(true, true);
    if (m_slice_select == eSliceAll)
        wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_ALL));
    else
        wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
}

struct ConfigsOverwriteConfirmDialog : MessageDialog
{
    ConfigsOverwriteConfirmDialog(wxWindow *parent, wxString name, bool exported)
        : MessageDialog(parent,
                        wxString::Format(exported ? _L("A file exists with the same name: %s, do you want to overwrite it?") :
                                                  _L("A config exists with the same name: %s, do you want to overwrite it?"),
                                         name),
                        exported ? _L("Overwrite file") : _L("Overwrite config"),
                        wxYES_NO | wxNO_DEFAULT)
    {
        add_button(wxID_YESTOALL, false, _L("Yes to All"));
        add_button(wxID_NOTOALL, false, _L("No to All"));
    }
};

void MainFrame::export_config()
{
    ExportConfigsDialog export_configs_dlg(nullptr);
    export_configs_dlg.ShowModal();
    return;

    // Generate a cummulative configuration for the selected print, filaments and printer.
    wxDirDialog dlg(this, _L("Choose a directory"),
        from_u8(!m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir()), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    wxString path;
    if (dlg.ShowModal() == wxID_OK)
        path = dlg.GetPath();
    if (!path.IsEmpty()) {
        // Export the config bundle.
        wxGetApp().app_config->update_config_dir(into_u8(path));
        try {
            auto files = wxGetApp().preset_bundle->export_current_configs(into_u8(path), [this](std::string const & name) {
                    ConfigsOverwriteConfirmDialog dlg(this, from_u8(name), true);
                    int res = dlg.ShowModal();
                    int ids[]{wxID_NO, wxID_YES, wxID_NOTOALL, wxID_YESTOALL};
                    return std::find(ids, ids + 4, res) - ids;
            }, false);
            if (!files.empty())
                m_last_config = from_u8(files.back());
            MessageDialog dlg(this, wxString::Format(_L_PLURAL("There is %d config exported. (Only non-system configs)",
                "There are %d configs exported. (Only non-system configs)", files.size()), files.size()),
                              _L("Export result"), wxOK);
            dlg.ShowModal();
        } catch (const std::exception &ex) {
            show_error(this, ex.what());
        }
    }
}

// Load a config file containing a Print, Filament & Printer preset.
void MainFrame::load_config_file()
{
    //BBS do not load config file
 //   if (!wxGetApp().check_and_save_current_preset_changes(_L("Loading profile file"), "", false))
 //       return;
    wxFileDialog dlg(this, _L("Select profile to load:"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        "config.json", "Config files (*.json;*.zip;*.orca_printer;*.orca_filament)|*.json;*.zip;*.orca_printer;*.orca_filament", wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
     wxArrayString files;
    if (dlg.ShowModal() != wxID_OK)
        return;
    dlg.GetPaths(files);
    std::vector<std::string> cfiles;
    for (auto file : files) {
        cfiles.push_back(into_u8(file));
        m_last_config = file;
    }
    bool update = false;
    wxGetApp().preset_bundle->import_presets(cfiles, [this](std::string const & name) {
            ConfigsOverwriteConfirmDialog dlg(this, from_u8(name), false);
            int           res = dlg.ShowModal();
            int           ids[]{wxID_NO, wxID_YES, wxID_NOTOALL, wxID_YESTOALL};
            return std::find(ids, ids + 4, res) - ids;
        },
        ForwardCompatibilitySubstitutionRule::Enable);
    if (!cfiles.empty()) {
        wxGetApp().app_config->update_config_dir(get_dir_name(cfiles.back()));
        wxGetApp().load_current_presets();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " presets has been import,and size is" << cfiles.size();
        NetworkAgent* agent = wxGetApp().getAgent();
        if (agent) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " user is: " << agent->get_user_id();
        }
    }
    wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
    update_side_preset_ui();
    auto msg = wxString::Format(_L_PLURAL("There is %d config imported. (Only non-system and compatible configs)",
        "There are %d configs imported. (Only non-system and compatible configs)", cfiles.size()), cfiles.size());
    if(cfiles.empty())
        msg += _L("\nHint: Make sure you have added the corresponding printer before importing the configs.");
    MessageDialog dlg2(this,msg ,
                        _L("Import result"), wxOK);
    dlg2.ShowModal();
}

// Ultra: import the config block embedded in a sliced G-code file (PrusaSlicer-style).
void MainFrame::import_config_from_gcode()
{
    wxFileDialog dlg(this, _L("Select a sliced G-code file to import settings from:"),
        wxGetApp().app_config->get_last_dir(), "",
        "G-code files (*.gcode;*.g;*.ngc)|*.gcode;*.g;*.ngc", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path = into_u8(dlg.GetPath());
    try {
        ConfigSubstitutions subst = wxGetApp().preset_bundle->load_config_file(path, ForwardCompatibilitySubstitutionRule::Enable);
        if (!subst.empty())
            show_substitutions_info(subst, path);
        wxGetApp().app_config->update_config_dir(get_dir_name(from_u8(path)));
        wxGetApp().load_current_presets();
        wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
        update_side_preset_ui();
        MessageDialog(this, _L("Settings imported from the G-code file as modified presets."), _L("Import result"), wxOK).ShowModal();
    } catch (const std::exception& ex) {
        show_error(this, ex.what());
    }
}

// Load a config file containing a Print, Filament & Printer preset from command line.
bool MainFrame::load_config_file(const std::string &path)
{
    try {
        ConfigSubstitutions config_substitutions = wxGetApp().preset_bundle->load_config_file(path, ForwardCompatibilitySubstitutionRule::Enable);
        if (!config_substitutions.empty())
            show_substitutions_info(config_substitutions, path);
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return false;
    }
    wxGetApp().load_current_presets();
    return true;
}

//BBS: export current config bundle as BBL default reference
//void MainFrame::export_current_configbundle()
//{
    // BBS do not export profile
   // if (!wxGetApp().check_and_save_current_preset_changes(_L("Exporting current profile bundle"),
   //     _L("Some presets are modified and the unsaved changes will not be exported into profile bundle."), false, true))
   //     return;

   // // validate current configuration in case it's dirty
   // auto err = wxGetApp().preset_bundle->full_config().validate();
   // if (! err.empty()) {
   //     show_error(this, err);
   //     return;
   // }
   // // Ask user for a file name.
   // wxFileDialog dlg(this, _L("Save BBL Default bundle as:"),
   //     !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
   //     "BBL_config_bundle.ini",
   //     file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
   // wxString file;
   // if (dlg.ShowModal() == wxID_OK)
   //     file = dlg.GetPath();
   // if (!file.IsEmpty()) {
   //     // Export the config bundle.
   //     wxGetApp().app_config->update_config_dir(get_dir_name(file));
   //     try {
   //         wxGetApp().preset_bundle->export_current_configbundle(file.ToUTF8().data());
   //     } catch (const std::exception &ex) {
			//show_error(this, ex.what());
   //     }
   // }
//}

//BBS: export all the system preset configs to seperate files
/*void MainFrame::export_system_configs()
{
    // Ask user for a file name.
    wxDirDialog dlg(this, _L("choose a directory"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    wxString path;
    if (dlg.ShowModal() == wxID_OK)
        path = dlg.GetPath();
    if (!path.IsEmpty()) {
        // Export the config bundle.
        wxGetApp().app_config->update_config_dir(path.ToStdString());
        try {
            wxGetApp().preset_bundle->export_system_configs(path.ToUTF8().data());
        } catch (const std::exception &ex) {
            show_error(this, ex.what());
        }
    }
}*/

//void MainFrame::export_configbundle(bool export_physical_printers /*= false*/)
//{
////    ; //BBS do not export config bundle
//}

// Loading a config bundle with an external file name used to be used
// to auto - install a config bundle on a fresh user account,
// but that behavior was not documented and likely buggy.
//void MainFrame::load_configbundle(wxString file/* = wxEmptyString, const bool reset_user_profile*/)
//{
//    ; //BBS do not import config bundle
//}

// Load a provied DynamicConfig into the Print / Filament / Printer tabs, thus modifying the active preset.
// Also update the plater with the new presets.
void MainFrame::load_config(const DynamicPrintConfig& config)
{
	PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();
	const auto       *opt_printer_technology = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
	if (opt_printer_technology != nullptr && opt_printer_technology->value != printer_technology) {
		printer_technology = opt_printer_technology->value;
		this->plater()->set_printer_technology(printer_technology);
	}
#if 0
	for (auto tab : wxGetApp().tabs_list)
		if (tab->supports_printer_technology(printer_technology)) {
			if (tab->type() == Slic3r::Preset::TYPE_PRINTER)
				static_cast<TabPrinter*>(tab)->update_pages();
			tab->load_config(config);
		}
    if (m_plater)
        m_plater->on_config_change(config);
#else
	// Load the currently selected preset into the GUI, update the preset selection box.
    //FIXME this is not quite safe for multi-extruder printers,
    // as the number of extruders is not adjusted for the vector values.
    // (see PresetBundle::update_multi_material_filament_presets())
    // Better to call PresetBundle::load_config() instead?
    for (auto tab : wxGetApp().tabs_list)
        if (tab->supports_printer_technology(printer_technology)) {
            // Only apply keys, which are present in the tab's config. Ignore the other keys.
			for (const std::string &opt_key : tab->get_config()->diff(config))
				// Ignore print_settings_id, printer_settings_id, filament_settings_id etc.
				if (! boost::algorithm::ends_with(opt_key, "_settings_id"))
					tab->get_config()->option(opt_key)->set(config.option(opt_key));
        }

    wxGetApp().load_current_presets();
#endif
}

//BBS: GUI refactor
void MainFrame::select_tab(wxPanel* panel)
{
    if (!panel)
        return;
    if (panel == m_param_panel) {
        panel = m_plater;
    } else if (dynamic_cast<ParamsPanel*>(panel)) {
        wxGetApp().params_dialog()->Popup();
        return;
    }
    int page_idx = m_tabpanel->FindPage(panel);
    if (page_idx == tp3DEditor && m_tabpanel->GetSelection() == tpPreview)
        return;
    //BBS GUI refactor: remove unused layout new/dlg
    /*if (page_idx != wxNOT_FOUND && m_layout == ESettingsLayout::Dlg)
        page_idx++;*/
    select_tab(size_t(page_idx));
}

//BBS
void MainFrame::jump_to_monitor(std::string dev_id)
{
    // Selecting the tab shows the holder, which builds the panel; go through the getter
    // so the pointer below is valid even if the notebook refuses the selection.
    m_tabpanel->SetSelection(tpMonitor);
    MonitorPanel* mon = monitor();
    if (!mon)
        return;
    mon->select_machine(dev_id);
}

// Ultra: Flashforge device stack (Orca-Flashforge port)
void MainFrame::jump_to_monitor(int evt_type, int conn_id)
{
    // Routed to the Flashforge device tab once it is mounted (FFDeviceTab);
    // until then fall back to selecting the Device tab.
    wxWindow* target = m_ff_device != nullptr ? (wxWindow*) m_ff_device : (wxWindow*) monitor();
    if (target == nullptr)
        return;
    m_tabpanel->SetSelection(tpMonitor);
    wxCommandEvent event((wxEventType) evt_type, target->GetId());
    event.SetEventObject(target);
    event.SetInt(conn_id);
    wxPostEvent(target, event);
}

void MainFrame::jump_to_monitor_exit(const std::string& /*dev_id*/)
{
    m_tabpanel->SetSelection(tpMonitor);
    MonitorPanel* mon = monitor();
    if (!mon)
        return;
    mon->update_all();
}

ProgressDialog* MainFrame::createLogProgress()
{
    if (m_log_progress_dlg != nullptr) {
        m_log_progress_dlg->Destroy();
        m_log_progress_dlg = nullptr;
    }
    m_log_progress_dlg = new ProgressDialog(_L("Upload log"), "", 100, this, wxPD_AUTO_HIDE);
    return m_log_progress_dlg;
}

void MainFrame::jump_to_multipage()
{
    m_tabpanel->SetSelection(tpMultiDevice);
    MultiMachinePage* mm = multi_machine();
    if (!mm)
        return;
    mm->jump_to_send_page();
}


//BBS GUI refactor: remove unused layout new/dlg
void MainFrame::select_tab(size_t tab/* = size_t(-1)*/)
{
    //bool tabpanel_was_hidden = false;

    // Controls on page are created on active page of active tab now.
    // We should select/activate tab before its showing to avoid an UI-flickering
    auto select = [this, tab](bool was_hidden) {
        // when tab == -1, it means we should show the last selected tab
        //BBS GUI refactor: remove unused layout new/dlg
        //size_t new_selection = tab == (size_t)(-1) ? m_last_selected_tab : (m_layout == ESettingsLayout::Dlg && tab != 0) ? tab - 1 : tab;
        size_t new_selection = tab == (size_t)(-1) ? m_last_selected_tab : tab;

        if (m_tabpanel->GetSelection() != (int)new_selection)
            m_tabpanel->SetSelection(new_selection);
#ifdef _MSW_DARK_MODE
        /*if (wxGetApp().tabs_as_menu()) {
            if (Tab* cur_tab = dynamic_cast<Tab*>(m_tabpanel->GetPage(new_selection)))
                update_marker_for_tabs_menu((m_layout == ESettingsLayout::Old ? m_menubar : m_settings_dialog.menubar()), cur_tab->title(), m_layout == ESettingsLayout::Old);
            else if (tab == 0 && m_layout == ESettingsLayout::Old)
                m_plater->get_current_canvas3D()->render();
        }*/
#endif
        if (tab == MainFrame::tp3DEditor && m_layout == ESettingsLayout::Old)
            m_plater->canvas3D()->render();
        else if (was_hidden) {
            Tab* cur_tab = dynamic_cast<Tab*>(m_tabpanel->GetPage(new_selection));
            if (cur_tab)
                cur_tab->OnActivate();
        }
    };

    select(false);
}

void MainFrame::request_select_tab(TabPosition pos)
{
    wxCommandEvent* evt = new wxCommandEvent(EVT_SELECT_TAB);
    evt->SetInt(pos);
    wxQueueEvent(this, evt);
}

int MainFrame::get_calibration_curr_tab() {
    if (m_calibration)
        return m_calibration->get_tabpanel()->GetSelection();
    return -1;
}

// Set a camera direction, zoom to all objects.
void MainFrame::select_view(const std::string& direction)
{
     if (m_plater)
         m_plater->select_view(direction);
}

void MainFrame::ZoomCameraToFit() const
{
    if (m_plater == nullptr || !can_change_view())
    {
        return;
    }

    GLCanvas3D* const canvas = m_plater->canvas3D();
    if (canvas != nullptr)
    {
        canvas->ZoomToFit();
    }
}

// #ys_FIXME_to_delete
void MainFrame::on_presets_changed(SimpleEvent &event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr) {
        return;
    }

    // Update preset combo boxes(Print settings, Filament, Material, Printer) from their respective tabs.
    auto presets = tab->get_presets();
    if (m_plater != nullptr && presets != nullptr) {

        // FIXME: The preset type really should be a property of Tab instead
        Slic3r::Preset::Type preset_type = tab->type();
        if (preset_type == Slic3r::Preset::TYPE_INVALID) {
            wxASSERT(false);
            return;
        }

        m_plater->on_config_change(*tab->get_config());

        m_plater->sidebar().update_presets(preset_type);
    }
}

// #ys_FIXME_to_delete
void MainFrame::on_value_changed(wxCommandEvent& event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr)
        return;

    auto opt_key = event.GetString();
    if (m_plater) {
        m_plater->on_config_change(*tab->get_config()); // propagate config change events to the plater
        if (opt_key == "extruders_count") {
            auto value = event.GetInt();
            m_plater->on_filaments_change(value);
        }
    }
}

void MainFrame::on_config_changed(DynamicPrintConfig* config) const
{
    if (m_plater)
        m_plater->on_config_change(*config); // propagate config change events to the plater
}

void MainFrame::set_print_button_to_default(PrintSelectType select_type)
{
    // Keep the user's remembered print/export action instead of resetting it to the computed
    // default. get_remembered_print_select() already rejects anything this printer does not offer.
    PrintSelectType remembered;
    if (get_remembered_print_select(remembered))
        select_type = remembered;

    switch (select_type) {
    case ePrintPlate:
    case eSendGcode:
    case eExportGcode:
    case eExportSlicedFile:
    case eExportAllSlicedFile:
    case ePrintMultiMachine:
        apply_print_select_state(select_type);
        // Only the print-host flow (eSendGcode) needs a host. Export actions, eExportGcode
        // included, never depend on one: with "remember last print action" on, a restored
        // "Export G-code file" must stay enabled on a printer without a print host. The dropdown
        // path (select_print_action) does not AND can_send_gcode() at all.
        if (PrintSelectKeys::requires_print_host(static_cast<int>(select_type))) {
            m_print_enable = m_print_enable && can_send_gcode();
            m_print_btn->Enable(m_print_enable);
        }
        break;
    default:
        // unsupported from this entry point (ePrintAll / eSendToPrinter / eSendToPrinterAll /
        // eUploadGcode are never offered on Edge).
        return;
    }
}

void MainFrame::add_to_recent_projects(const wxString& filename)
{
    if (wxFileExists(filename))
    {
        m_recent_projects.AddFileToHistory(filename);
        std::vector<std::string> recent_projects;
        size_t count = m_recent_projects.GetCount();
        for (size_t i = 0; i < count; ++i)
        {
            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
        }
        wxGetApp().app_config->set_recent_projects(recent_projects);
        if (m_webview)
            m_webview->SendRecentList(0);

        // wcp 订阅
        json data;
        wxGetApp().mainframe->get_recent_projects(data, INT_MAX);
        wxGetApp().recent_file_notify(data);
    }
}

std::string MainFrame::FileHistory::GetThumbnailUrl_str(int index) const 
{
    if (m_thumbnails[index].empty())
        return "";
    std::stringstream ss;
    ss << "data:image/png;base64,";
    ss << wxBase64Encode(m_thumbnails[index].data(), m_thumbnails[index].size());
    return ss.str();
}

std::wstring MainFrame::FileHistory::GetThumbnailUrl(int index) const
{
    if (m_thumbnails[index].empty()) return L"";
    std::wstringstream wss;
    wss << L"data:image/png;base64,";
    wss << wxBase64Encode(m_thumbnails[index].data(), m_thumbnails[index].size());
    return wss.str();
}

void MainFrame::FileHistory::AddFileToHistory(const wxString &file)
{
    if (this->m_fileMaxFiles == 0)
        return;
    wxFileHistory::AddFileToHistory(file);
    if (m_load_called)
        m_thumbnails.push_front(bbs_3mf_get_thumbnail(into_u8(file).c_str()));
    else
        m_thumbnails.push_front("");
}

void MainFrame::FileHistory::RemoveFileFromHistory(size_t i)
{
    if (i >= m_thumbnails.size()) // FIX zero max
        return;
    wxFileHistory::RemoveFileFromHistory(i);
    m_thumbnails.erase(m_thumbnails.begin() + i);
}

size_t MainFrame::FileHistory::FindFileInHistory(const wxString & file)
{
    return m_fileHistory.Index(file);
}

void MainFrame::FileHistory::LoadThumbnails()
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, GetCount()), [this](tbb::blocked_range<size_t> range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            auto thumbnail = bbs_3mf_get_thumbnail(into_u8(GetHistoryFile(i)).c_str());
            if (!thumbnail.empty()) {
                m_thumbnails[i] = thumbnail;
            }
        }
    });
    m_load_called = true;
}

inline void MainFrame::FileHistory::SetMaxFiles(int max)
{
    m_fileMaxFiles  = max;
    size_t numFiles = m_fileHistory.size();
    while (numFiles > m_fileMaxFiles)
        RemoveFileFromHistory(--numFiles);
}

void MainFrame::get_recent_projects(nlohmann::json& data, int images) {
    for (size_t i = 0; i < m_recent_projects.GetCount(); ++i) {
        json item;
        std::string proj    = m_recent_projects.GetHistoryFile(i).ToStdString(wxConvUTF8);
        
        item["project_name"] = proj.substr(proj.find_last_of("/\\") + 1);
        item["path"]  = proj;
        boost::system::error_code ec;
        std::time_t               t;
        try {
            t = boost::filesystem::last_write_time(proj, ec);
        }
        catch (std::exception& e) {
            std::string e_msg = e.what();
            BOOST_LOG_TRIVIAL(error) << e.what(); 
        }        
        if (!ec) {
            std::string time = wxDateTime(t).FormatISOCombined(' ').ToStdString();
            item["time"]      = time;
            if (i <= images) {

                auto thumbnail = m_recent_projects.GetThumbnailUrl_str(i);
                if (!thumbnail.empty())
                    item["image"] = thumbnail;
            }
        } else {
            item["time"] = "File is missing";
        }
        data.push_back(item);
    }
}

void MainFrame::get_recent_projects(boost::property_tree::wptree &tree, int images)
{
    for (size_t i = 0; i < m_recent_projects.GetCount(); ++i) {
        boost::property_tree::wptree item;
        std::wstring proj = m_recent_projects.GetHistoryFile(i).ToStdWstring();
        item.put(L"project_name", proj.substr(proj.find_last_of(L"/\\") + 1));
        item.put(L"path", proj);
        boost::system::error_code ec;
        std::time_t t = boost::filesystem::last_write_time(proj, ec);
        if (!ec) {
            std::wstring time = wxDateTime(t).FormatISOCombined(' ').ToStdWstring();
            item.put(L"time", time);
            if (i <= images) {
                auto thumbnail = m_recent_projects.GetThumbnailUrl(i);
                if (!thumbnail.empty()) item.put(L"image", thumbnail);
            }
        } else {
            item.put(L"time", _L("File is missing"));
        }
        tree.push_back({L"", item});
    }
}

void MainFrame::open_recent_project(size_t file_id, wxString const & filename)
{
    if (file_id == size_t(-1)) {
        file_id = m_recent_projects.FindFileInHistory(filename);
    }
    if (wxFileExists(filename)) {
        CallAfter([this, filename] {
            if (wxGetApp().can_load_project())
                m_plater->load_project(filename);
        });
    }
    else
    {
        MessageDialog msg(this, _L("The project is no longer available."), _L("Error"), wxOK | wxYES_DEFAULT);
        if (msg.ShowModal() == wxID_YES)
        {
            m_recent_projects.RemoveFileFromHistory(file_id);
            std::vector<std::string> recent_projects;
            size_t count = m_recent_projects.GetCount();
            for (size_t i = 0; i < count; ++i)
            {
                recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
            }
            wxGetApp().app_config->set_recent_projects(recent_projects);
            if (m_webview)
                m_webview->SendRecentList(-1);

            // wcp 订阅
            json data;
            wxGetApp().mainframe->get_recent_projects(data, INT_MAX);
            wxGetApp().recent_file_notify(data);
        }
    }
}

void MainFrame::sm_remove_recent_project(wxString const& filename) {
    size_t file_id = -1;
    if (filename.IsEmpty())
        while (m_recent_projects.GetCount() > 0)
            m_recent_projects.RemoveFileFromHistory(0);
    else
        file_id = m_recent_projects.FindFileInHistory(filename);

    if (file_id != size_t(-1))
        m_recent_projects.RemoveFileFromHistory(file_id);

    std::vector<std::string> recent_projects;
    size_t                   count = m_recent_projects.GetCount();
    for (size_t i = 0; i < count; ++i) {
        recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
    }
    wxGetApp().app_config->set_recent_projects(recent_projects);

    // wcp 订阅
    json data;
    wxGetApp().mainframe->get_recent_projects(data, INT_MAX);
    wxGetApp().recent_file_notify(data);
    

}

void MainFrame::remove_recent_project(size_t file_id, wxString const &filename)
{
    if (file_id == size_t(-1)) {
        if (filename.IsEmpty())
            while (m_recent_projects.GetCount() > 0)
                m_recent_projects.RemoveFileFromHistory(0);
        else
            file_id = m_recent_projects.FindFileInHistory(filename);
    }
    if (file_id != size_t(-1))
        m_recent_projects.RemoveFileFromHistory(file_id);
    std::vector<std::string> recent_projects;
    size_t count = m_recent_projects.GetCount();
    for (size_t i = 0; i < count; ++i)
    {
        recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
    }
    wxGetApp().app_config->set_recent_projects(recent_projects);
    if (m_webview)
        m_webview->SendRecentList(-1);

    // wcp 订阅
    json data;
    wxGetApp().mainframe->get_recent_projects(data, INT_MAX);
    wxGetApp().recent_file_notify(data);
}

WebViewPanel* MainFrame::start_page()
{
    if (m_webview == nullptr && m_home != nullptr)
        m_webview = m_home->start_page();
    return m_webview;
}

void MainFrame::show_start_page()
{
    if (m_home == nullptr)
        return;
    start_page();
    m_home->show_start_page();
    select_tab(MainFrame::tpHome);
}

void MainFrame::load_url(wxString url)
{
    BOOST_LOG_TRIVIAL(trace) << "load_url:" << url;
    auto evt = new wxCommandEvent(EVT_LOAD_URL, this->GetId());
    evt->SetString(url);
    wxQueueEvent(this, evt);
}

void MainFrame::load_printer_url(wxString url, wxString apikey)
{
    BOOST_LOG_TRIVIAL(trace) << "load_printer_url:" << url;
    auto evt = new LoadPrinterViewEvent(EVT_LOAD_PRINTER_URL, this->GetId());
    evt->SetString(url);
    evt->SetAPIkey(apikey);
    wxQueueEvent(this, evt);
}

void MainFrame::load_printer_url()
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    if (preset_bundle.use_bbl_device_tab())
        return;

    auto     cfg = preset_bundle.printers.get_edited_preset().config;
    wxString url = cfg.opt_string("print_host_webui").empty() ? cfg.opt_string("print_host") : cfg.opt_string("print_host_webui");
    wxString apikey;
    const auto host_type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type")->value;
    if (cfg.has("printhost_apikey") && (host_type == htPrusaLink || host_type == htPrusaConnect))
        apikey = cfg.opt_string("printhost_apikey");
    if (!url.empty()) {
        if (!url.Lower().starts_with("http"))
            url = wxString::Format("http://%s", url);

        load_printer_url(url, apikey);
    }
}

bool MainFrame::is_printer_view() const { return m_tabpanel->GetSelection() == TabPosition::tpMonitor; }


void MainFrame::refresh_plugin_tips()
{
    if (m_webview != nullptr)
        m_webview->ShowNetpluginTip();
}

void MainFrame::RunScript(wxString js)
{
    if (m_webview != nullptr)
        m_webview->RunScript(js);
}

void MainFrame::downloadOpenProject(const std::string& fileUrl, const std::string& fileName, std::string completeFilePath)
{
    // std::string fileUrl = "https://public.resource.snapmaker.com/model/public/3mf/test_for_download.3mf";
    // std::string           filename     = "test_for_download.3mf";
    wxString fileNameEx = wxString::FromUTF8(fileName.c_str()).Lower();
    std::string releaFileName = "";

    bool strRes = fileNameEx.EndsWith(".3mf");

    if (strRes)
        releaFileName = fileName;
    else
        releaFileName = fileName + ".3mf";

    GenericDownloadDialog dlg(_L("downloading the model"), fileUrl, releaFileName, completeFilePath);
    auto res = dlg.ShowModal();

    if (res != wxID_OK)
        return;

    // DownloadManager may save under a uniquified name (e.g. a(1).3mf); use the path returned by the dialog.
    std::string path_to_open = dlg.get_file_path();
    if (path_to_open.empty()) {
        if (completeFilePath.empty()) {
            auto downloadPath = wxGetApp().app_config->get("download_path");
            completeFilePath  = downloadPath + "/" + releaFileName;
        }
        path_to_open = completeFilePath;
    }
    if (!boost::filesystem::exists(path_to_open))
    {
        BOOST_LOG_TRIVIAL(warning) << boost::format("the file '%1%' not exists") % path_to_open;
        return;
    }

    // Auto-open project if it's a .3mf file
    boost::filesystem::path path(path_to_open);
    std::string             extension = boost::algorithm::to_lower_copy(path.extension().string());
    if (extension == ".3mf") {
        BOOST_LOG_TRIVIAL(info) << boost::format("GenericDownloadDialog: Auto-opening project file '%1%'") % path_to_open;
        wxString wx_file_path = wxString::FromUTF8(path_to_open.c_str());
        if (wxGetApp().can_load_project() && wxGetApp().mainframe && wxGetApp().mainframe->plater()) {
            wxGetApp().mainframe->plater()->load_project(wx_file_path);
        }
    }
    else
    {
        // Not a valid 3mf file, show error message
        wxString msg = wxString::Format(_L("The downloaded file '%s' is not a valid 3MF project file."), releaFileName);
        MessageDialog(this, msg, _L("Invalid File"), wxOK | wxICON_WARNING).ShowModal();
    }

    
}

void MainFrame::technology_changed()
{
    // update menu titles
    PrinterTechnology pt = plater()->printer_technology();
    if (int id = m_menubar->FindMenu(pt == ptFFF ? _omitL("Material Settings") : _L("Filament Settings")); id != wxNOT_FOUND)
        m_menubar->SetMenuLabel(id, pt == ptSLA ? _omitL("Material Settings") : _L("Filament Settings"));
}

//
// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void MainFrame::update_ui_from_settings()
{
    if (m_plater)
        m_plater->update_ui_from_settings();
    for (auto tab: wxGetApp().tabs_list)
        tab->update_ui_from_settings();
}


void MainFrame::show_sync_dialog()
{
    SimpleEvent* evt = new SimpleEvent(EVT_SYNC_CLOUD_PRESET);
    wxQueueEvent(this, evt);
}

void MainFrame::export_logs()
{
    // 1. Get log folder path
    auto log_folder = boost::filesystem::path(data_dir()) / "log";

    // 2. Check if log folder exists
    if (!boost::filesystem::exists(log_folder) || boost::filesystem::is_empty(log_folder)) {
        MessageDialog dlg(this, _L("No logs found."), _L("Export Logs"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }

    // 3. Show save file dialog
    std::time_t t = std::time(0);
    std::tm* now_time = std::localtime(&t);
    std::stringstream buf;
    buf << std::put_time(now_time, "logs_%Y%m%d_%H%M%S.zip");
    wxString default_filename = from_u8(buf.str());

    wxFileDialog dlg(
        this,
        _L("Save logs as ZIP file"),
        wxEmptyString,
        default_filename,
        "ZIP files (*.zip)|*.zip",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (dlg.ShowModal() != wxID_OK)
        return;

    wxString zip_path = dlg.GetPath();

    // Write version info and flush to log file before zipping
    GUI_App::log_version_info();

    // 4. Create ZIP file and add all logs
    try {
        Zipper zipper(into_u8(zip_path), Zipper::TIGHT_COMPRESSION);

        // Iterate through log directory
        for (auto& entry : boost::filesystem::directory_iterator(log_folder)) {
            if (boost::filesystem::is_regular_file(entry.path())) {
                std::string filename = entry.path().filename().string();

                // Read file content
                std::ifstream file(entry.path().string(), std::ios::binary);
                if (!file.is_open()) {
                    BOOST_LOG_TRIVIAL(warning) << "Failed to open log file: " << entry.path().string();
                    continue;
                }

                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                file.close();

                // Add to ZIP
                zipper.add_entry(filename, content.c_str(), content.size());
            }
        }

        // Ultra: include the FlashNetwork logs (Flashforge device stack) and the
        // go2rtc relay config so one zip carries everything a report needs.
        auto add_tree = [&zipper](const boost::filesystem::path& root, const std::string& prefix) {
            if (!boost::filesystem::exists(root))
                return;
            for (auto& entry : boost::filesystem::recursive_directory_iterator(root)) {
                if (!boost::filesystem::is_regular_file(entry.path()))
                    continue;
                if (boost::filesystem::file_size(entry.path()) > 64 * 1024 * 1024)
                    continue; // don't ship runaway files
                std::ifstream file(entry.path().string(), std::ios::binary);
                if (!file.is_open())
                    continue;
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                const std::string rel = boost::filesystem::relative(entry.path(), root).generic_string();
                zipper.add_entry(prefix + rel, content.c_str(), content.size());
            }
        };
        add_tree(boost::filesystem::path(data_dir()) / "FlashNetwork", "FlashNetwork/");
        {
            const auto go2rtc_cfg = boost::filesystem::path(data_dir()) / "go2rtc.yaml";
            if (boost::filesystem::exists(go2rtc_cfg)) {
                std::ifstream file(go2rtc_cfg.string(), std::ios::binary);
                if (file.is_open()) {
                    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    zipper.add_entry("go2rtc.yaml", content.c_str(), content.size());
                }
            }
        }

        zipper.finalize();

        // 5. Show success message
        MessageDialog success_dlg(this,
            _L("Logs exported successfully."),
            _L("Export Logs"),
            wxOK | wxICON_INFORMATION);
        success_dlg.ShowModal();

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to export logs: " << e.what();
        MessageDialog error_dlg(this,
            wxString::Format(_L("Failed to export logs: %s"), e.what()),
            _L("Export Logs"),
            wxOK | wxICON_ERROR);
        error_dlg.ShowModal();
    }
}


void MainFrame::update_side_preset_ui()
{
    // select last preset
    for (auto tab : wxGetApp().tabs_list) {
        tab->update_tab_ui();
    }

    //BBS: update the preset
    m_plater->sidebar().update_presets(Preset::TYPE_PRINTER);
    m_plater->sidebar().update_presets(Preset::TYPE_FILAMENT);


    //take off multi machine
    if(m_multi_machine){m_multi_machine->clear_page();}
}

void MainFrame::on_select_default_preset(SimpleEvent& evt)
{
    MessageDialog dialog(this,
                    _L("Do you want to synchronize your personal data from Bambu Cloud?\n"
                        "It contains the following information:\n"
                        "1. The Process presets\n"
                        "2. The Filament presets\n"
                        "3. The Printer presets"),
                    _L("Synchronization"),
                    wxCENTER |
                    wxYES_DEFAULT | wxYES_NO |
                    wxICON_INFORMATION);

    /* get setting list */
    NetworkAgent* agent = wxGetApp().getAgent();
    switch ( dialog.ShowModal() )
    {
        case wxID_YES: {
            wxGetApp().app_config->set_bool("sync_user_preset", true);
            wxGetApp().start_sync_user_preset(true);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: true";
            break;
        }
        case wxID_NO:
            wxGetApp().app_config->set_bool("sync_user_preset", false);
            wxGetApp().stop_sync_user_preset();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: false";
            break;
        default:
            break;
    }

    update_side_preset_ui();
}

std::string MainFrame::get_base_name(const wxString &full_name, const char *extension) const
{
    boost::filesystem::path filename = boost::filesystem::path(full_name.wx_str()).filename();
    if (extension != nullptr)
		filename = filename.replace_extension(extension);
    return filename.string();
}

std::string MainFrame::get_dir_name(const wxString &full_name) const
{
    return boost::filesystem::path(into_u8(full_name)).parent_path().string();
}


// ----------------------------------------------------------------------------
// SettingsDialog
// ----------------------------------------------------------------------------

SettingsDialog::SettingsDialog(MainFrame* mainframe)
:DPIDialog(NULL, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE, "settings_dialog"),
//: DPIDialog(mainframe, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Settings"), wxDefaultPosition, wxDefaultSize,
//        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX, "settings_dialog"),
    m_main_frame(mainframe)
{
    if (wxGetApp().is_gcode_viewer())
        return;

#if defined(__WXMSW__)
    // ys_FIXME! temporary workaround for correct font scaling
    // Because of from wxWidgets 3.1.3 auto rescaling is implemented for the Fonts,
    // From the very beginning set dialog font to the wxSYS_DEFAULT_GUI_FONT
    this->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));
#else
    this->SetFont(wxGetApp().normal_font());
    this->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif // __WXMSW__

    // Load the icon either from the exe, or from the ico file.
#if _WIN32
    {
        TCHAR szExeFileName[MAX_PATH];
        GetModuleFileName(nullptr, szExeFileName, MAX_PATH);
        SetIcon(wxIcon(szExeFileName, wxBITMAP_TYPE_ICO));
    }
#else
    SetIcon(wxIcon(var("Snapmaker_Orca_128px.png"), wxBITMAP_TYPE_PNG));
#endif // _WIN32

    //just hide the Frame on closing
    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) { this->Hide(); });

#ifdef _MSW_DARK_MODE
    if (wxGetApp().tabs_as_menu()) {
        // menubar
        //m_menubar = new wxMenuBar();
        //add_tabs_as_menu(m_menubar, mainframe, this);
        //this->SetMenuBar(m_menubar);
    }
#endif

    // initialize layout
    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(this);
    SetSizer(sizer);
    Fit();

    const wxSize min_size = wxSize(85 * em_unit(), 50 * em_unit());
#ifdef __APPLE__
    // Using SetMinSize() on Mac messes up the window position in some cases
    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
    SetSize(min_size);
#else
    SetMinSize(min_size);
    SetSize(GetMinSize());
#endif
    Layout();
}

void SettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const int& em = em_unit();
    const wxSize& size = wxSize(85 * em, 50 * em);

    // BBS
    m_tabpanel->Rescale();

    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->msw_rescale();

    SetMinSize(size);
    Fit();
    Refresh();
}


} // GUI
} // Slic3r
