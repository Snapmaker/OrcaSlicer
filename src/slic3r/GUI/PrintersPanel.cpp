#include "PrintersPanel.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "RemoteAccess.hpp"
#include "GcodeArchive.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/HubHomeLogic.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <wx/base64.h>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/webview.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;
namespace PMon = Slic3r::PrintersMonitor;

// Phase 1 storage for the printer groups: one JSON value in AppConfig ("printers_groups"). Until
// something is stored, the old Multi-device tab's pick list (section multi_devices, keys 0..5) is
// offered as the first group. The hub-side store (shared with the app and the phone page) will
// implement the same PrintersMonitor::GroupStore.
class AppConfigGroupStore : public PMon::GroupStore
{
public:
    std::vector<PMon::Group> load() override
    {
        AppConfig* cfg = wxGetApp().app_config;
        if (cfg == nullptr) return {};
        const std::string stored = cfg->get("printers_groups");
        if (!stored.empty()) return PMon::parse_groups(stored);
        std::vector<std::string> picked;
        for (int i = 0; i < 6; ++i) picked.push_back(cfg->get("multi_devices", std::to_string(i)));
        return PMon::seed_from_multi_devices(picked);
    }
    bool save(const std::vector<PMon::Group>& groups) override
    {
        AppConfig* cfg = wxGetApp().app_config;
        if (cfg == nullptr) return false;
        cfg->set("printers_groups", PMon::save_groups(groups));
        return true;
    }
};

static long long printers_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The vendor profiles' *_cover.png files, indexed once (they only change with an install).
static const PMon::CoverIndex& printers_cover_index()
{
    static std::once_flag    once;
    static PMon::CoverIndex  index;
    std::call_once(once, []() {
        namespace fs = boost::filesystem;
        std::vector<std::pair<std::string, std::string>> files;
        boost::system::error_code ec;
        const fs::path root = fs::path(resources_dir()) / "profiles";
        for (fs::directory_iterator v(root, ec), end; !ec && v != end; v.increment(ec)) {
            boost::system::error_code ig;
            if (!fs::is_directory(v->path(), ig)) continue;
            for (fs::directory_iterator f(v->path(), ig), fend; !ig && f != fend; f.increment(ig))
                files.emplace_back(v->path().filename().string(), f->path().filename().string());
        }
        index = PMon::index_covers(files);
    });
    return index;
}

// The archive's sidecars, re-read at most every 30 s (each poll would otherwise parse them all).
static std::vector<PMon::ArchiveEntry> printers_archive_entries()
{
    static std::mutex                      mtx;
    static long long                       read_at = 0;
    static std::vector<PMon::ArchiveEntry> cached;
    std::lock_guard<std::mutex> lock(mtx);
    const long long now = printers_now_ms();
    if (read_at == 0 || now - read_at > 30000) {
        read_at = now;
        cached.clear();
        try {
            for (const GcodeArchive::Record& r : GcodeArchive::list()) {
                if (!r.has_thumbnail || !r.json.is_object() || !r.json.contains("printer") || !r.json["printer"].is_object()) continue;
                PMon::ArchiveEntry e;
                e.printer_id = r.json["printer"].value("id", std::string());
                e.record_id  = r.id;
                e.sent_name  = r.json.value("sent_name", std::string());
                e.file       = r.file;
                e.time       = r.time;
                cached.push_back(std::move(e));
            }
        } catch (...) {}
    }
    return cached;
}

// The rows plus what the card shows besides them: the printer's picture and the job's plate picture.
static void printers_enrich(json& payload)
{
    if (!payload.value("ok", false) || !payload.contains("printers")) return;
    std::map<std::string, std::string> jobs;
    for (const json& p : payload["printers"])
        if (p.is_object() && p.contains("id") && p["id"].is_string()) jobs[p["id"].get<std::string>()] = PMon::row_job(p);
    std::map<std::string, std::string> thumbs;
    try { thumbs = PMon::pick_thumbnails(printers_archive_entries(), jobs); } catch (...) {}
    PMon::enrich_rows(payload["printers"], printers_cover_index(), thumbs);
}

PrintersPanel::PrintersPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_reading(std::make_shared<std::atomic<bool>>(false))
    , m_alive(std::make_shared<bool>(true))
{
    // ?embed=1: shown inside the slicer. ?theme=: the slicer's own theme from the first paint.
    // ?tasks=1: the old cloud send queue is in use (the "Multi-device Management" preference), so
    // the page offers a way back to it.
    std::string path = std::string("/web/orca/monitor.html?embed=1&theme=") + HubHome::theme_name(wxGetApp().dark_mode());
    if (wxGetApp().is_enable_multi_machine())
        path += "&tasks=1";
    m_browser = WebView::CreateWebView(this, wxString::FromUTF8(wxGetApp().page_url(path)));
    if (m_browser == nullptr)
        return;
    // The page follows the theme live (window.__edgeTheme): a reload would only lose its place.
    WebView::SetReloadOnThemeChange(m_browser, false);
    // The app-wide "wx" channel CreateWebView injects (a second named handler is never injected by
    // the Edge backend, see StreamPanel).
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrintersPanel::OnScriptMessage, this, m_browser->GetId());
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrintersPanel::OnPageLoaded, this, m_browser->GetId());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    if (wxWindow* top = wxGetTopLevelParent(this))
        top->Bind(wxEVT_ICONIZE, &PrintersPanel::OnIconize, this);
}

PrintersPanel::~PrintersPanel()
{
    *m_alive = false;
    if (wxWindow* top = wxGetTopLevelParent(this))
        top->Unbind(wxEVT_ICONIZE, &PrintersPanel::OnIconize, this);
}

bool PrintersPanel::Show(bool show)
{
    const bool ret = wxPanel::Show(show);
    m_shown        = show;
    // On show the page refreshes at once (__printersActive(true)) instead of at its next tick.
    SetPageActive(m_shown && !m_iconized);
    return ret;
}

void PrintersPanel::OnIconize(wxIconizeEvent& evt)
{
    evt.Skip(); // the frame's own handlers still run
    m_iconized = evt.IsIconized();
    SetPageActive(m_shown && !m_iconized);
}

void PrintersPanel::OnPageLoaded(wxWebViewEvent& evt)
{
    evt.Skip();
    // A (re)loaded page knows nothing yet: tell it whether it is on screen.
    SetPageActive(m_shown && !m_iconized, true);
}

void PrintersPanel::SetPageActive(bool active, bool force)
{
    if (m_browser == nullptr || (active == m_active && !force))
        return;
    m_active = active;
    WebView::RunScript(m_browser, wxString::Format("if (window.__printersActive) window.__printersActive(%s);", active ? "true" : "false"));
}

void PrintersPanel::sys_color_changed()
{
    if (m_browser != nullptr)
        WebView::RunScript(m_browser, wxString::FromUTF8(HubHome::theme_script(wxGetApp().dark_mode())));
}

void PrintersPanel::RunPageScript(const std::string& fn, const json& arg)
{
    const std::string js = PMon::script_call(fn, arg);
    if (m_browser != nullptr && !js.empty())
        WebView::RunScript(m_browser, wxString::FromUTF8(js));
}

void PrintersPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    // Only our own page may read the printers or command them.
    if (m_browser == nullptr || !wxGetApp().is_own_page_url(m_browser->GetCurrentURL().ToUTF8().data())) {
        BOOST_LOG_TRIVIAL(warning) << "PrintersPanel: ignored a message from a page that is not ours";
        return;
    }
    const std::string     raw = evt.GetString().ToUTF8().data();
    const PMon::Message     m   = PMon::parse_message(raw);
    switch (m.kind) {
    case PMon::Message::Kind::Get: get_printers(); break;
    case PMon::Message::Kind::Groups: get_groups(); break;
    case PMon::Message::Kind::Control: control(m); break;
    case PMon::Message::Kind::Job: job(m.job); break;
    case PMon::Message::Kind::Open: open_device(m.id); break;
    case PMon::Message::Kind::Thumb: thumbnail(m.id); break;
    case PMon::Message::Kind::Tasks:
        if (wxGetApp().is_enable_multi_machine() && wxGetApp().mainframe)
            wxGetApp().mainframe->jump_to_multipage();
        break;
    case PMon::Message::Kind::Invalid:
        // Other pages' messages (common_openurl and friends) reach every view's handler too.
        if (raw.compare(0, 9, "printers_") == 0)
            BOOST_LOG_TRIVIAL(warning) << "PrintersPanel: refused message \"" << raw.substr(0, 80) << "\"";
        break;
    }
}

void PrintersPanel::get_printers()
{
    bool expected = false;
    if (!m_reading->compare_exchange_strong(expected, true))
        return; // the read in flight answers this tick too
    std::weak_ptr<bool>                alive   = m_alive;
    std::shared_ptr<std::atomic<bool>> reading = m_reading;
    // api_printers waits for the GUI thread for the Bambu part and probes the network hosts on the
    // calling thread, so it runs here, never on the GUI thread (RemoteAccess.cpp).
    std::thread([this, alive, reading]() {
        std::pair<int, std::string> res { 503, "" };
        try { res = RemoteAccess::get().monitor_printers(); } catch (...) {}
        auto payload = std::make_shared<json>(PMon::page_payload(res.first, res.second, printers_now_ms()));
        try { printers_enrich(*payload); } catch (...) {}
        *reading = false;
        RemoteAccess::post_to_app([this, alive, payload]() {
            auto a = alive.lock();
            if (!a || !*a) return;
            if ((*payload)["ok"].get<bool>())
                m_targets = PMon::targets_of((*payload)["printers"]);
            RunPageScript("__printers", *payload);
        });
    }).detach();
}

void PrintersPanel::thumbnail(const std::string& archive_id)
{
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, archive_id]() {
        std::string data;
        try {
            const GcodeArchive::Record r = GcodeArchive::find(archive_id);
            boost::system::error_code ec;
            if (!r.id.empty() && r.has_thumbnail && boost::filesystem::is_regular_file(r.thumbnail_path, ec) &&
                boost::filesystem::file_size(r.thumbnail_path, ec) <= 2u * 1024 * 1024) {
                std::ifstream f(boost::filesystem::path(r.thumbnail_path).string(), std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                const std::string png = ss.str();
                // A PNG and nothing else (the page puts it in an <img> as a data: URL).
                if (png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0)
                    data = "data:image/png;base64," + std::string(wxBase64Encode(png.data(), png.size()).ToUTF8().data());
            }
        } catch (...) {}
        const json reply = { { "id", archive_id }, { "data", data } };
        RemoteAccess::post_to_app([this, alive, reply]() {
            auto a = alive.lock();
            if (a && *a) RunPageScript("__printerThumb", reply);
        });
    }).detach();
}

void PrintersPanel::get_groups()
{
    AppConfigGroupStore store;
    RunPageScript("__printerGroups", PMon::groups_json(store.load()));
}

void PrintersPanel::control(const PMon::Message& m)
{
    if (m.action == "stop" && !m.confirm) {
        RunPageScript("__printerControl", json { { "id", m.id }, { "action", m.action }, { "ok", false }, { "error", "Stopping a print needs a confirmation." } });
        return;
    }
    std::weak_ptr<bool> alive = m_alive;
    const std::string   id = m.id, action = m.action;
    const bool          confirm = m.confirm;
    std::thread([this, alive, id, action, confirm]() {
        std::pair<int, std::string> res { 500, "" };
        try { res = RemoteAccess::get().monitor_control(id, action, confirm); } catch (...) {}
        json reply = { { "id", id }, { "action", action }, { "ok", res.first == 200 } };
        try {
            const json body = json::parse(res.second);
            if (res.first == 200) {
                reply["job"]     = body.value("job", 0);
                reply["dry_run"] = body.value("dry_run", false);
            } else {
                reply["error"] = body.value("error", std::string("the command was refused"));
            }
        } catch (...) {
            if (res.first != 200) reply["error"] = "the command was refused";
        }
        RemoteAccess::post_to_app([this, alive, reply]() {
            auto a = alive.lock();
            if (a && *a) RunPageScript("__printerControl", reply);
        });
    }).detach();
}

void PrintersPanel::job(int id)
{
    // In memory, no GUI-thread work: cheap enough to answer right here.
    const std::pair<int, std::string> res = RemoteAccess::get().monitor_job(id);
    json reply = { { "id", id }, { "state", "error" }, { "error", "no such job" } };
    if (res.first == 200) {
        try {
            const json j = json::parse(res.second);
            reply        = { { "id", id }, { "state", j.value("state", std::string("error")) }, { "text", j.value("text", std::string()) },
                             { "error", j.value("error", std::string()) }, { "printer", j.value("printer", std::string()) } };
        } catch (...) {}
    }
    RunPageScript("__printerJob", reply);
}

void PrintersPanel::open_device(const std::string& id)
{
    auto it = m_targets.find(id);
    if (it == m_targets.end()) {
        RunPageScript("__printerOpen", json { { "id", id }, { "ok", false }, { "error", "This printer is not in the list any more." } });
        return;
    }
    std::string why;
    const bool  ok = wxGetApp().mainframe && wxGetApp().mainframe->open_printer_device_page(id, it->second, why);
    RunPageScript("__printerOpen", json { { "id", id }, { "ok", ok }, { "error", ok ? std::string() : why } });
}

} // namespace GUI
} // namespace Slic3r
