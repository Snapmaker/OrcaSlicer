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

#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/HomeTabLogic.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ptree.hpp> // MainFrame::get_recent_projects() has a wptree overload

#include <climits>
#include <sstream>
#include <thread>

#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/stattext.h>
#include <wx/webview.h>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

static constexpr const char* SECTION_KEY = "home_tab_section";
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
    } else if (command == "home_section") {
        const std::string section = msg.value("section", std::string());
        if (HomeTab::valid_section(section))
            wxGetApp().app_config->set(SECTION_KEY, section);
    } else if (command == "home_refresh") {
        send_recent();
        send_history();
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

} // namespace GUI
} // namespace Slic3r
