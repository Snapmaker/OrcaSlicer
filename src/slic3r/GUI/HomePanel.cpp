#include "HomePanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GcodeArchive.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "WebViewDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/HomeTabLogic.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ptree.hpp> // MainFrame::get_recent_projects() has a wptree overload

#include <algorithm>
#include <climits>
#include <ctime>
#include <sstream>
#include <thread>

#include <wx/dirdlg.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/stattext.h>
#include <wx/webview.h>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

static constexpr const char* SECTION_KEY = "home_tab_section";
static constexpr const char* LIBRARY_FOLDERS_KEY = "home_library_folders";
static constexpr const char* LIBRARY_SEEDED_KEY  = "home_library_seeded"; // Downloads was added once
static constexpr const char* LIBRARY_HIDDEN_KEY  = "home_library_hidden";
// Coming back to Home rescans the Library when the last scan is older than this; Refresh always does.
static constexpr int64_t LIBRARY_STALE_S = 120;
// A plate thumbnail the archive wrote is a few kB; anything this big is not one.
static constexpr uintmax_t MAX_THUMBNAIL_BYTES = 4 * 1024 * 1024;

static std::string read_small_file(const std::string& path)
{
    boost::system::error_code ec;
    const uintmax_t           size = fs::file_size(path, ec);
    if (ec || size == 0 || size > MAX_THUMBNAIL_BYTES)
        return std::string();
    boost::nowide::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
        return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

HomePanel::HomePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    m_sizer = new wxBoxSizer(wxVERTICAL);

    // The strip above the start page, the only way it is shown: it says what this is and leads back.
    m_strip          = new wxPanel(this, wxID_ANY);
    m_strip_label    = new wxStaticText(m_strip, wxID_ANY, _L("Start page"));
    m_strip_label->SetFont(Label::Head_13);
    m_btn_back = new Button(m_strip, _L("Back to Home"));
    m_btn_back->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto* strip_sizer = new wxBoxSizer(wxHORIZONTAL);
    strip_sizer->Add(m_strip_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    strip_sizer->AddStretchSpacer(1);
    strip_sizer->Add(m_btn_back, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(6));
    m_strip->SetSizer(strip_sizer);
    m_strip->Hide();
    m_btn_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_home(); });

    m_sizer->Add(m_strip, 0, wxEXPAND);
    SetSizer(m_sizer);
    apply_colours();

    // The page is built the first time Home is on screen, not at startup. The tab switch says so
    // (on_tab_changed); a Home tab that is the first page shown hears it from its first real size.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        evt.Skip();
        if (m_browser == nullptr && !m_start_shown && evt.GetSize().x > 0 && IsShownOnScreen())
            CallAfter([this]() { ensure_browser(); });
    });
}

HomePanel::~HomePanel()
{
    *m_alive = false;
    *m_library_cancel = true;
}

void HomePanel::ensure_browser()
{
    if (m_browser != nullptr)
        return;
    wxString url = wxString::Format("file://%s/web/home/index.html", from_u8(resources_dir()));
    BOOST_LOG_TRIVIAL(info) << "HomePanel: building the Home page";
    m_browser = WebView::CreateWebView(this, url);
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "HomePanel: the Home page's web view could not be created";
        return;
    }
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &HomePanel::on_script_message, this);
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, &HomePanel::on_navigating, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &HomePanel::on_new_window, this);
    m_sizer->Add(m_browser, 1, wxEXPAND);
    if (m_start_shown)
        m_browser->Hide();
    Layout();
}

WebViewPanel* HomePanel::start_page()
{
    if (m_start == nullptr) {
        BOOST_LOG_TRIVIAL(info) << "HomePanel: building the start page";
        m_start = new WebViewPanel(this);
        m_start->Hide();
        m_sizer->Add(m_start, 1, wxEXPAND);
        // Keep MainFrame::m_webview in step whoever asked first.
        if (MainFrame* mf = wxGetApp().mainframe)
            if (mf->m_webview == nullptr)
                mf->m_webview = m_start;
    }
    return m_start;
}

void HomePanel::show_start_page()
{
    start_page();
    if (m_start_shown)
        return;
    m_start_shown = true;
    if (m_browser)
        m_browser->Hide();
    m_strip->Show();
    m_start->Show();
    Layout();
    if (m_selected)
        notify_start_page(true);
}

void HomePanel::show_home()
{
    if (!m_start_shown)
        return;
    m_start_shown = false;
    if (m_selected)
        notify_start_page(false);
    if (m_start)
        m_start->Hide();
    m_strip->Hide();
    ensure_browser();
    if (m_browser)
        m_browser->Show();
    Layout();
    if (m_selected && m_page_ready) {
        send_recent();
        send_history();
        library_refresh(false);
    }
}

void HomePanel::on_tab_changed(bool selected)
{
    m_selected = selected;
    if (start_page_shown()) {
        notify_start_page(selected);
        return;
    }
    if (!selected)
        return;
    if (m_browser == nullptr) {
        // After the page switch has settled, so the panel has its size.
        CallAfter([this]() { ensure_browser(); });
        return;
    }
    // Coming back: a project was opened or saved, or a file was sent, on another tab since.
    if (m_page_ready) {
        send_recent();
        send_history();
        library_refresh(false);
    }
}

void HomePanel::notify_start_page(bool active)
{
    if (m_start == nullptr)
        return;
    if (wxWebView* view = m_start->getWebView())
        wxGetApp().page_state_notify_webview(view, active ? "active" : "inactive");
}

void HomePanel::apply_colours()
{
    const wxColour bg    = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    const wxColour strip = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    SetBackgroundColour(bg);
    m_strip->SetBackgroundColour(strip);
    m_strip_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    Refresh();
}

void HomePanel::sys_color_changed()
{
    apply_colours();
    m_btn_back->Rescale();
    // The page follows the theme itself: WebView::RecreateAll() reloads it with the new
    // User-Agent, and it asks for everything again when it is up.
}

// ------------------------------------------------------------------------------ the page ----

void HomePanel::on_navigating(wxWebViewEvent& evt)
{
    const std::string url = evt.GetURL().ToStdString(wxConvUTF8);
    if (url == "about:blank" || wxGetApp().is_own_page_url(url)) {
        evt.Skip();
        return;
    }
    // Never away from Home: a web link goes to the system browser, anything else nowhere.
    evt.Veto();
    if (boost::istarts_with(url, "https://") || boost::istarts_with(url, "http://"))
        wxLaunchDefaultBrowser(evt.GetURL());
    else
        BOOST_LOG_TRIVIAL(info) << "HomePanel: dropped a navigation away from the Home page";
}

void HomePanel::on_new_window(wxWebViewEvent& evt)
{
    const std::string url = evt.GetURL().ToStdString(wxConvUTF8);
    if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER &&
        (boost::istarts_with(url, "https://") || boost::istarts_with(url, "http://")))
        wxLaunchDefaultBrowser(evt.GetURL());
}

void HomePanel::on_script_message(wxWebViewEvent& evt)
{
    // Only the installed Home page may ask for anything.
    if (m_browser == nullptr || !wxGetApp().is_own_page_url(m_browser->GetCurrentURL().ToStdString(wxConvUTF8))) {
        BOOST_LOG_TRIVIAL(warning) << "HomePanel: ignored a message from a page that is not ours";
        return;
    }
    json msg;
    try {
        msg = json::parse(evt.GetString().ToStdString(wxConvUTF8));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "HomePanel: a message that is not JSON: " << e.what();
        return;
    }
    if (!msg.is_object())
        return;
    try {
        handle(msg);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "HomePanel: handling \"" << msg.value("command", std::string()) << "\" failed: " << e.what();
    }
}

void HomePanel::handle(const json& msg)
{
    const std::string command = msg.value("command", std::string());
    const std::string path    = msg.value("path", std::string());
    const std::string id      = msg.value("id", std::string());

    if (command == "home_ready") {
        m_page_ready = true;
        send_init();
        send_recent();
        send_history();
        if (m_library_loaded)
            send_library(); // the page was reloaded (a theme change): it starts empty
        library_refresh(false);
    } else if (command == "home_section") {
        const std::string section = msg.value("section", std::string());
        if (HomeTab::valid_section(section))
            wxGetApp().app_config->set(SECTION_KEY, section);
    } else if (command == "home_refresh") {
        send_recent();
        send_history();
        library_refresh(true);
    } else if (command == "recent_open") {
        if (HomeTab::is_listed(path, m_recent_paths))
            open_recent(path);
    } else if (command == "recent_reveal") {
        if (HomeTab::is_listed(path, m_recent_paths))
            desktop_open_any_folderEx(fs::path(path).make_preferred().string());
    } else if (command == "recent_forget") {
        if (HomeTab::is_listed(path, m_recent_paths))
            forget_recent(path);
    } else if (command == "project_new") {
        wxGetApp().request_open_project("<new>");
    } else if (command == "project_open") {
        wxGetApp().request_open_project(std::string()); // the Open Project dialog
    } else if (command == "history_thumbs") {
        std::vector<std::string> ids;
        if (msg.contains("ids") && msg["ids"].is_array())
            for (const json& v : msg["ids"])
                if (v.is_string() && m_history_ids.count(v.get<std::string>()) && ids.size() < 200)
                    ids.push_back(v.get<std::string>());
        if (!ids.empty())
            send_history_thumbnails(ids);
    } else if (command == "history_open") {
        if (m_history_ids.count(id))
            open_archived(id);
    } else if (command == "history_project") {
        if (m_history_ids.count(id))
            open_archived_project(id);
    } else if (command == "history_reveal") {
        if (m_history_ids.count(id))
            reveal_archived(id);
    } else if (command == "history_delete") {
        if (m_history_ids.count(id))
            delete_archived(id);
    } else if (command == "history_settings") {
        wxGetApp().open_preferences();
    } else if (command == "library_scan") {
        library_refresh(true);
    } else if (command == "library_thumbs") {
        std::vector<std::string> ids;
        if (msg.contains("ids") && msg["ids"].is_array())
            for (const json& v : msg["ids"])
                if (v.is_string() && m_library_paths.count(v.get<std::string>()) && ids.size() < 200)
                    ids.push_back(v.get<std::string>());
        if (!ids.empty())
            send_library_thumbnails(ids);
    } else if (command == "library_open") {
        if (m_library_paths.count(id))
            library_open(id, false);
    } else if (command == "library_import") {
        if (m_library_paths.count(id))
            library_open(id, true);
    } else if (command == "library_reveal") {
        const std::string p = library_path(id);
        if (!p.empty())
            desktop_open_any_folderEx(fs::path(p).make_preferred().string());
    } else if (command == "library_hide") {
        if (m_library_paths.count(id))
            library_hide(id);
    } else if (command == "library_unhide_all") {
        save_library_hidden({});
        library_refresh(true);
    } else if (command == "library_add_folder") {
        library_add_folder();
    } else if (command == "library_update_folder") {
        library_update_folder(msg);
    } else if (command == "library_remove_folder") {
        library_remove_folder(path);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "HomePanel: unknown command \"" << command << "\"";
    }
}

void HomePanel::send(const json& msg)
{
    if (m_browser == nullptr || !m_page_ready)
        return;
    WebView::RunScript(m_browser, wxString::FromUTF8(HomeTab::receive_script(msg)));
}

void HomePanel::send_init()
{
    json s;
    s["recent"]            = _u8L("Recent");
    s["history"]           = _u8L("Print History");
    s["search"]            = _u8L("Search");
    s["refresh"]           = _u8L("Refresh");
    s["new_project"]       = _u8L("New Project");
    s["open_project"]      = _u8L("Open Project");
    s["open"]              = _u8L("Open");
    s["show_in_folder"]    = _u8L("Show in folder");
    s["remove_from_list"]  = _u8L("Remove from list");
    s["missing"]           = _u8L("File is missing");
    s["recent_empty"]      = _u8L("Projects you open or save will show up here.");
    s["no_match"]          = _u8L("Nothing matches your search.");
    s["history_empty"]     = _u8L("Files you send to a printer will show up here.");
    s["history_off"]       = _u8L("The G-code archive is off, so new prints are not recorded. Turn it on in Preferences to keep a copy of every file you send.");
    s["open_preferences"]  = _u8L("Open Preferences");
    s["loading"]           = _u8L("Loading...");
    s["all_printers"]      = _u8L("All printers");
    s["open_preview"]      = _u8L("Open in preview");
    s["open_source"]       = _u8L("Open source project");
    s["delete"]            = _u8L("Delete");
    s["plate"]             = _u8L("Plate");
    s["printed"]           = _u8L("Printed");
    s["uploaded"]          = _u8L("Uploaded");
    s["from_phone"]        = _u8L("from phone");
    s["reprinted"]         = _u8L("Reprinted");
    s["times"]             = _u8L("times");
    s["file_gone"]         = _u8L("The archived file is gone");
    s["sort_newest"]       = _u8L("Newest first");
    s["sort_oldest"]       = _u8L("Oldest first");
    s["sort_name"]         = _u8L("Name");
    s["sort_added"]        = _u8L("Recently added");
    s["sort_size"]         = _u8L("Largest first");
    s["library"]           = _u8L("Library");
    s["folders"]           = _u8L("Folders");
    s["manage_folders"]    = _u8L("Manage folders");
    s["add_folder"]        = _u8L("Add folder");
    s["remove"]            = _u8L("Remove");
    s["done"]              = _u8L("Done");
    s["category"]          = _u8L("Category");
    s["vendor"]            = _u8L("Vendor");
    s["type"]              = _u8L("Type");
    s["none"]              = _u8L("None");
    s["include_subfolders"] = _u8L("Include subfolders");
    s["folders_hint"]      = _u8L("Every model file in these folders shows up in the Library. Give a folder a Category and a Vendor to filter by them.");
    s["files"]             = _u8L("files");
    s["offline"]           = _u8L("Not reachable, showing the files from the last scan");
    s["not_scanned"]       = _u8L("Not scanned yet");
    s["scanning"]          = _u8L("Scanning...");
    s["scanned"]           = _u8L("Scanned");
    s["library_empty"]     = _u8L("No model files in your Library folders yet.");
    s["library_no_folders"] = _u8L("Add the folders where you keep your models to browse them here.");
    s["add_to_plate"]      = _u8L("Add to current project");
    s["hide"]              = _u8L("Hide from Library");
    s["hidden"]            = _u8L("hidden");
    s["show_hidden"]       = _u8L("Show them again");
    s["sliced"]            = _u8L("Sliced");
    s["plates"]            = _u8L("plates");
    s["by"]                = _u8L("by");
    s["clear_filters"]     = _u8L("Clear filters");
    s["all"]               = _u8L("All");

    json init;
    init["type"]    = "init";
    init["strings"] = s;
    init["section"] = HomeTab::section_or_default(wxGetApp().app_config->get(SECTION_KEY));
    send(init);
}

void HomePanel::send_recent()
{
    MainFrame* mf = wxGetApp().mainframe;
    if (mf == nullptr)
        return;
    json list = json::array();
    mf->get_recent_projects(list, INT_MAX);

    m_recent_paths.clear();
    json items = json::array();
    for (const json& r : list) {
        if (!r.is_object())
            continue;
        const std::string path = r.value("path", std::string());
        if (path.empty())
            continue;
        boost::system::error_code ec;
        const bool                exists = fs::is_regular_file(fs::path(path), ec);
        json item;
        item["path"]   = path;
        item["name"]   = r.value("project_name", std::string());
        item["folder"] = fs::path(path).parent_path().string();
        item["exists"] = exists;
        item["time"]   = exists ? (long long) fs::last_write_time(fs::path(path), ec) : 0LL;
        item["image"]  = r.value("image", std::string());
        items.push_back(std::move(item));
        m_recent_paths.push_back(path);
    }
    send({{"type", "recent"}, {"items", items}});
}

void HomePanel::send_history()
{
    if (m_browser == nullptr || !m_page_ready)
        return;
    const unsigned generation = ++m_history_generation;
    const bool     enabled    = GcodeArchive::enabled();
    std::weak_ptr<bool> alive = m_alive;
    // The sidecars are files: read them off the GUI thread (the archive may hold thousands).
    std::thread([this, alive, generation, enabled]() {
        std::vector<json> records;
        std::vector<bool> file_present;
        for (GcodeArchive::Record& r : GcodeArchive::list()) {
            r.json["has_thumbnail"] = r.has_thumbnail;
            records.push_back(std::move(r.json));
            file_present.push_back(r.file_present);
        }
        // Fills a missing printer model from the printer's other records and names the model.
        GcodeArchive::annotate_models(records, GcodeArchive::model_names());
        json items = json::array();
        std::vector<std::string> ids;
        for (size_t i = 0; i < records.size(); ++i) {
            const json& j = records[i];
            const json  printer = j.contains("printer") && j["printer"].is_object() ? j["printer"] : json::object();
            const std::string name = GcodeArchive::display_printer_name(printer.value("name", std::string()),
                                                                        printer.value("model", std::string()),
                                                                        printer.value("kind", std::string()));
            const std::string project = j.value("project_path", std::string());
            boost::system::error_code ec;
            const bool project_present = !project.empty() && fs::is_regular_file(fs::path(project), ec);
            json card = HomeTab::history_card(j, name, file_present[i], project_present);
            ids.push_back(card.value("id", std::string()));
            items.push_back(std::move(card));
        }
        wxGetApp().CallAfter([this, alive, generation, enabled, items = std::move(items), ids = std::move(ids)]() {
            if (alive.expired() || generation != m_history_generation)
                return; // the panel is gone, or a newer listing is on its way
            m_history_ids = std::set<std::string>(ids.begin(), ids.end());
            send({{"type", "history"}, {"enabled", enabled}, {"items", items}});
        });
    }).detach();
}

void HomePanel::send_history_thumbnails(const std::vector<std::string>& ids)
{
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, ids]() {
        json images = json::object();
        for (const std::string& id : ids) {
            // find() only accepts an id of the archive's own shape and reads it from the archive.
            const GcodeArchive::Record r = GcodeArchive::find(id);
            if (r.id.empty() || !r.has_thumbnail)
                continue;
            const std::string uri = HomeTab::png_data_uri(read_small_file(r.thumbnail_path));
            if (!uri.empty())
                images[id] = uri;
        }
        if (images.empty())
            return;
        wxGetApp().CallAfter([this, alive, images = std::move(images)]() {
            if (!alive.expired())
                send({{"type", "thumbs"}, {"images", images}});
        });
    }).detach();
}

// ------------------------------------------------------------------------------ actions ----

void HomePanel::open_recent(const std::string& path)
{
    // Asks about unsaved changes, and offers to drop a project that is gone from the list.
    wxGetApp().request_open_project(path);
}

void HomePanel::forget_recent(const std::string& path)
{
    if (MainFrame* mf = wxGetApp().mainframe)
        mf->remove_recent_project(size_t(-1), from_u8(path));
    send_recent();
}

void HomePanel::open_archived(const std::string& id)
{
    const GcodeArchive::Record r = GcodeArchive::find(id);
    Plater*                    plater = wxGetApp().plater();
    if (r.id.empty() || plater == nullptr)
        return;
    if (!r.file_present) {
        show_error(this, _L("The archived file is gone."));
        send_history();
        return;
    }
    if (plater->is_background_process_slicing()) {
        show_info(this, _L("new or open project file is not allowed during the slicing process!"), _L("Open Project"));
        return;
    }
    switch (HomeTab::archive_open_kind(r.file)) {
    case HomeTab::ArchiveOpen::Gcode:
        // Asks about the current project first, then shows the file in the preview.
        plater->load_gcode(from_u8(r.path));
        break;
    case HomeTab::ArchiveOpen::Project:
        if (wxGetApp().can_load_project())
            plater->load_project(from_u8(r.path));
        break;
    case HomeTab::ArchiveOpen::None: break;
    }
}

void HomePanel::open_archived_project(const std::string& id)
{
    const GcodeArchive::Record r       = GcodeArchive::find(id);
    const std::string          project = r.json.is_object() ? r.json.value("project_path", std::string()) : std::string();
    if (r.id.empty() || project.empty())
        return;
    boost::system::error_code ec;
    if (!fs::is_regular_file(fs::path(project), ec)) {
        show_error(this, _L("The project is no longer available."));
        send_history();
        return;
    }
    wxGetApp().request_open_project(project);
}

void HomePanel::reveal_archived(const std::string& id)
{
    const GcodeArchive::Record r = GcodeArchive::find(id);
    if (r.id.empty())
        return;
    if (r.file_present)
        desktop_open_any_folderEx(fs::path(r.path).make_preferred().string());
    else
        desktop_open_any_folderEx(fs::path(GcodeArchive::dir()).make_preferred().string());
}

void HomePanel::delete_archived(const std::string& id)
{
    const GcodeArchive::Record r = GcodeArchive::find(id);
    if (r.id.empty())
        return;
    MessageDialog dlg(this,
                      _L("Delete this print from the history? The archived file is deleted too, and the phone can no longer reprint it."),
                      _L("Print History"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return;
    if (!GcodeArchive::remove(id))
        BOOST_LOG_TRIVIAL(warning) << "HomePanel: removing archive record " << id << " found no sidecar";
    send_history();
}

// ------------------------------------------------------------------------------ library ----

std::vector<Library::Folder> HomePanel::library_folders() const
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg->get(LIBRARY_SEEDED_KEY) != "1") {
        // The Library starts with the Downloads folder, once: removing it later sticks.
        std::vector<Library::Folder> folders = Library::folders_from_json(cfg->get(LIBRARY_FOLDERS_KEY));
        const std::string downloads = into_u8(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads));
        boost::system::error_code ec;
        if (!downloads.empty() && fs::is_directory(fs::path(downloads), ec)) {
            Library::Folder f;
            f.path = downloads;
            folders.insert(folders.begin(), f);
        }
        cfg->set(LIBRARY_FOLDERS_KEY, Library::folders_to_json(folders));
        cfg->set(LIBRARY_SEEDED_KEY, "1");
    }
    return Library::folders_from_json(cfg->get(LIBRARY_FOLDERS_KEY));
}

void HomePanel::save_library_folders(const std::vector<Library::Folder>& folders)
{
    wxGetApp().app_config->set(LIBRARY_FOLDERS_KEY, Library::folders_to_json(folders));
}

std::set<std::string> HomePanel::library_hidden() const
{
    std::set<std::string> hidden;
    try {
        const json j = json::parse(wxGetApp().app_config->get(LIBRARY_HIDDEN_KEY));
        if (j.is_array())
            for (const json& v : j)
                if (v.is_string())
                    hidden.insert(v.get<std::string>());
    } catch (...) {}
    return hidden;
}

void HomePanel::save_library_hidden(const std::set<std::string>& hidden)
{
    json j = json::array();
    for (const std::string& p : hidden)
        j.push_back(p);
    wxGetApp().app_config->set(LIBRARY_HIDDEN_KEY, j.dump());
}

static std::string library_cache_dir()
{
    return (fs::path(data_dir()) / "library").string();
}

void HomePanel::library_refresh(bool force_scan)
{
    if (m_browser == nullptr || !m_page_ready)
        return;
    if (!m_library_loaded) {
        // First time: what the last run found is shown at once, then the folders are scanned again.
        if (m_library_loading)
            return;
        m_library_loading = true;
        std::weak_ptr<bool> alive = m_alive;
        std::thread([this, alive]() {
            Library::Index index = Library::load_index(library_cache_dir());
            wxGetApp().CallAfter([this, alive, index = std::move(index)]() mutable {
                if (alive.expired())
                    return;
                m_library         = std::move(index);
                m_library_loading = false;
                m_library_loaded  = true;
                send_library();
                library_scan();
            });
        }).detach();
        return;
    }
    if (force_scan || int64_t(std::time(nullptr)) - m_library.scanned_at > LIBRARY_STALE_S)
        library_scan();
}

void HomePanel::library_scan()
{
    if (!m_library_loaded)
        return;
    if (m_library_scanning) {
        m_library_rescan = true;
        return;
    }
    m_library_scanning = true;
    m_library_rescan   = false;
    m_library_seen     = 0;
    send({{"type", "library_progress"}, {"scanning", true}, {"files", 0}});

    const std::vector<Library::Folder> folders  = library_folders();
    const std::set<std::string>        hidden   = library_hidden();
    std::shared_ptr<std::atomic<bool>> cancel   = m_library_cancel;
    std::weak_ptr<bool>                alive    = m_alive;
    Library::Index                     previous = m_library;
    std::thread([this, alive, cancel, folders, hidden, previous = std::move(previous)]() {
        const std::string cache = library_cache_dir();
        auto progress = [this, alive](size_t seen) {
            wxGetApp().CallAfter([this, alive, seen]() {
                if (alive.expired() || !m_library_scanning)
                    return;
                m_library_seen = seen;
                send({{"type", "library_progress"}, {"scanning", true}, {"files", seen}});
            });
        };
        Library::Index index = Library::scan(folders, previous, hidden, cache, int64_t(std::time(nullptr)), *cancel, progress);
        if (*cancel)
            return; // the panel is going away: a partial index is not saved
        if (!Library::save_index(cache, index))
            BOOST_LOG_TRIVIAL(warning) << "HomePanel: could not save the Library index in " << cache;
        wxGetApp().CallAfter([this, alive, index = std::move(index)]() mutable {
            if (alive.expired())
                return;
            m_library          = std::move(index);
            m_library_scanning = false;
            send_library();
            if (m_library_rescan)
                library_scan(); // the folders changed while this one ran
        });
    }).detach();
}

void HomePanel::send_library()
{
    const std::vector<Library::Folder> folders = library_folders();
    const std::set<std::string>        hidden  = library_hidden();

    std::map<std::string, const Library::Folder*> by_path;
    for (const Library::Folder& f : folders)
        by_path[f.path] = &f;
    std::map<std::string, const Library::FolderState*> states;
    for (const Library::FolderState& st : m_library.folders)
        states[st.path] = &st;

    json folder_list = json::array();
    for (const Library::Folder& f : folders) {
        auto st = states.find(f.path);
        folder_list.push_back({{"path", f.path},
                               {"recursive", f.recursive},
                               {"category", f.category},
                               {"vendor", f.vendor},
                               {"scanned", st != states.end()},
                               {"online", st == states.end() || st->second->online},
                               {"files", st == states.end() ? 0 : st->second->files}});
    }

    // Tags come from the folders as they are now, so an edit shows before the next scan; files of a
    // folder that was removed, or that the user hid, are left out.
    m_library_paths.clear();
    json items = json::array();
    for (const Library::Entry& e : m_library.entries) {
        if (hidden.count(e.path))
            continue;
        auto f = by_path.find(e.root);
        if (f == by_path.end())
            continue;
        json item        = Library::page_item(e);
        item["category"] = f->second->category;
        item["vendor"]   = f->second->vendor;
        items.push_back(std::move(item));
        m_library_paths[e.id] = e.path;
    }
    send({{"type", "library"},
          {"folders", folder_list},
          {"items", items},
          {"hidden", hidden.size()},
          {"scanning", m_library_scanning},
          {"files_seen", m_library_seen},
          {"scanned_at", m_library.scanned_at}});
}

void HomePanel::send_library_thumbnails(const std::vector<std::string>& ids)
{
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, ids]() {
        const std::string cache  = library_cache_dir();
        json              images = json::object();
        for (const std::string& id : ids) {
            // Only ids this panel listed reach here; the cover is read from the cache by id.
            const std::string uri = HomeTab::png_data_uri(read_small_file(Library::thumbnail_path(cache, id)));
            if (!uri.empty())
                images[id] = uri;
        }
        if (images.empty())
            return;
        wxGetApp().CallAfter([this, alive, images = std::move(images)]() {
            if (!alive.expired())
                send({{"type", "library_thumbs"}, {"images", images}});
        });
    }).detach();
}

std::string HomePanel::library_path(const std::string& id) const
{
    auto it = m_library_paths.find(id);
    return it == m_library_paths.end() ? std::string() : it->second;
}

void HomePanel::library_open(const std::string& id, bool import)
{
    const std::string path   = library_path(id);
    Plater*           plater = wxGetApp().plater();
    if (path.empty() || plater == nullptr)
        return;
    boost::system::error_code ec;
    if (!fs::is_regular_file(fs::path(path), ec)) {
        show_error(this, _L("The file is no longer there."));
        library_refresh(true);
        return;
    }
    if (plater->is_background_process_slicing()) {
        show_info(this, _L("new or open project file is not allowed during the slicing process!"), _L("Open Project"));
        return;
    }
    if (!import && Library::file_type(fs::path(path).filename().string()) == "3mf") {
        // Asks about unsaved changes first, like Open Project.
        wxGetApp().request_open_project(path);
        return;
    }
    // A mesh, or "Add to current project": the same as dropping the file on the plater (a 3MF asks
    // whether to open it as a project or take its geometry only).
    wxArrayString files;
    files.Add(from_u8(path));
    if (plater->load_files(files))
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->select_tab(size_t(MainFrame::tp3DEditor));
}

void HomePanel::library_hide(const std::string& id)
{
    const std::string path = library_path(id);
    if (path.empty())
        return;
    std::set<std::string> hidden = library_hidden();
    hidden.insert(path);
    save_library_hidden(hidden);
    send_library();
}

void HomePanel::library_add_folder()
{
    wxDirDialog dlg(this, _L("Add a folder to the Library"), wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    std::vector<Library::Folder> folders = library_folders();
    Library::Folder              f;
    f.path = into_u8(dlg.GetPath());
    folders.push_back(f);
    // Normalized and de-duplicated the way it is stored.
    folders = Library::folders_from_json(Library::folders_to_json(folders));
    save_library_folders(folders);
    send_library();
    library_scan();
}

void HomePanel::library_update_folder(const json& msg)
{
    const std::string            path    = msg.value("path", std::string());
    std::vector<Library::Folder> folders = library_folders();
    auto it = std::find_if(folders.begin(), folders.end(), [&path](const Library::Folder& f) { return f.path == path; });
    if (path.empty() || it == folders.end())
        return;
    const bool was_recursive = it->recursive;
    auto text = [&msg](const char* key, const std::string& fallback) {
        return msg.contains(key) && msg[key].is_string() ? msg[key].get<std::string>().substr(0, 80) : fallback;
    };
    it->category  = text("category", it->category);
    it->vendor    = text("vendor", it->vendor);
    it->recursive = msg.contains("recursive") && msg["recursive"].is_boolean() ? msg["recursive"].get<bool>() : it->recursive;
    save_library_folders(Library::folders_from_json(Library::folders_to_json(folders)));
    send_library();
    if (it->recursive != was_recursive)
        library_scan();
}

void HomePanel::library_remove_folder(const std::string& path)
{
    std::vector<Library::Folder> folders = library_folders();
    const size_t                 before  = folders.size();
    folders.erase(std::remove_if(folders.begin(), folders.end(), [&path](const Library::Folder& f) { return f.path == path; }),
                  folders.end());
    if (path.empty() || folders.size() == before)
        return;
    save_library_folders(folders);
    send_library();
    library_scan(); // drops the folder's entries and covers from the cache
}

} // namespace GUI
} // namespace Slic3r
