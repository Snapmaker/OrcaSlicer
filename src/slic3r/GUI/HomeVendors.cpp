#include "HomeVendors.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/HomeTabLogic.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/LibraryIndex.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>
#include <thread>

#include <wx/clipbrd.h>
#include <wx/filedlg.h>
#include <wx/secretstore.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

static constexpr const char* CONNECTORS_KEY = "home_vendor_connectors";
static constexpr size_t      LIST_LIMIT     = 32u * 1024 * 1024;   // one page of a list
static constexpr size_t      THUMB_LIMIT    = 4u * 1024 * 1024;
static constexpr size_t      FILE_LIMIT     = 1024u * 1024 * 1024; // a downloaded model

// ------------------------------------------------------------------------------ helpers ----

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// The HTTP call the engine asks for, with the slicer's own client (TLS checked for internet hosts).
// None of the app's own headers go along: a vendor gets only what its connector says.
static Vendors::Response http_call(const Vendors::Request& req, size_t limit)
{
    Vendors::Response out;
    if (!Vendors::is_allowed_url(req.url)) {
        out.error = "address not allowed";
        return out;
    }
    std::string raw_headers;
    auto        http = Http::get(req.url);
    http.clear_headers().timeout_connect(15).timeout_max(limit > LIST_LIMIT ? 1800 : 120).size_limit(limit);
    for (const auto& [k, v] : req.headers)
        http.header(k, v);
    http.on_header_callback([&raw_headers](std::string h) { raw_headers = std::move(h); })
        .on_complete([&out](std::string body, unsigned status) {
            out.status = int(status);
            out.body   = std::move(body);
        })
        .on_error([&out](std::string body, std::string error, unsigned status) {
            out.status = int(status);
            out.body   = std::move(body);
            if (status == 0)
                out.error = error;
        })
        .perform_sync();
    // The last block of headers (after any redirects).
    const size_t last = raw_headers.rfind("HTTP/");
    std::istringstream lines(last == std::string::npos ? raw_headers : raw_headers.substr(last));
    std::string        line;
    while (std::getline(lines, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        value.erase(0, value.find_first_not_of(' '));
        out.headers[lower(line.substr(0, colon))] = value;
    }
    return out;
}

static bool write_atomic(const fs::path& path, const std::string& data)
{
    boost::system::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        boost::nowide::ofstream f(tmp.string().c_str(), std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(data.data(), std::streamsize(data.size()));
        if (!f)
            return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

static std::string read_file(const fs::path& path, size_t limit)
{
    boost::system::error_code ec;
    const uintmax_t           size = fs::file_size(path, ec);
    if (ec || size == 0 || size > limit)
        return std::string();
    boost::nowide::ifstream f(path.string().c_str(), std::ios::binary);
    std::stringstream       ss;
    ss << f.rdbuf();
    return ss.str();
}

// An image the page may show: PNG, JPEG, GIF or WebP, by its bytes (never SVG).
static std::string image_data_uri(const std::string& bytes)
{
    const char* mime = nullptr;
    if (bytes.size() > 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0)
        mime = "image/png";
    else if (bytes.size() > 3 && bytes.compare(0, 3, "\xFF\xD8\xFF", 3) == 0)
        mime = "image/jpeg";
    else if (bytes.size() > 6 && (bytes.compare(0, 6, "GIF87a") == 0 || bytes.compare(0, 6, "GIF89a") == 0))
        mime = "image/gif";
    else if (bytes.size() > 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0)
        mime = "image/webp";
    return mime == nullptr ? std::string() : std::string("data:") + mime + ";base64," + HomeTab::base64(bytes);
}

static std::string safe_file_name(std::string name)
{
    for (char& c : name)
        if ((unsigned char) c < 0x20 || std::string("<>:\"/\\|?*").find(c) != std::string::npos)
            c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
        name.pop_back();
    if (name.size() > 120)
        name.resize(120);
    return name.empty() ? std::string("model") : name;
}

static std::string key_of(const std::string& connector, const std::string& item)
{
    return Library::entry_id(connector + "\x1f" + item);
}

// ------------------------------------------------------------------------------ setup ----

HomeVendors::HomeVendors(wxWindow* parent, SendFn send, std::function<void()> library_changed)
    : m_parent(parent), m_send(std::move(send)), m_library_changed(std::move(library_changed))
{
    for (Vendors::Spec& s : load_specs()) {
        Connector c;
        c.spec = std::move(s);
        m_connectors.push_back(std::move(c));
    }
}

HomeVendors::~HomeVendors()
{
    *m_alive  = false;
    *m_cancel = true;
}

std::vector<Vendors::Spec> HomeVendors::load_specs() const
{
    std::vector<Vendors::Spec> out;
    json                       list;
    try {
        list = json::parse(wxGetApp().app_config->get(CONNECTORS_KEY));
    } catch (...) {
        return out;
    }
    if (!list.is_array())
        return out;
    std::set<std::string> ids;
    for (const json& j : list) {
        try {
            Vendors::Spec s = Vendors::spec_from_json(j);
            if (!s.id.empty() && ids.insert(s.id).second)
                out.push_back(std::move(s));
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "HomeVendors: a saved connector is not usable: " << e.what();
        }
    }
    return out;
}

void HomeVendors::save_specs()
{
    json list = json::array();
    for (const Connector& c : m_connectors)
        list.push_back(Vendors::spec_to_json(c.spec));
    wxGetApp().app_config->set(CONNECTORS_KEY, list.dump());
}

HomeVendors::Connector* HomeVendors::find(const std::string& id)
{
    for (Connector& c : m_connectors)
        if (c.spec.id == id)
            return &c;
    return nullptr;
}

std::string HomeVendors::cache_dir(const std::string& id) const
{
    return (fs::path(data_dir()) / "vendors" / id).string();
}

void HomeVendors::ensure_cache(Connector& c)
{
    if (c.cache_loaded)
        return;
    c.cache_loaded = true;
    const std::string text = read_file(fs::path(cache_dir(c.spec.id)) / "items.json", 256u * 1024 * 1024);
    if (text.empty())
        return;
    try {
        c.cache = Vendors::cache_from_json(json::parse(text));
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "HomeVendors: the cache of connector " << c.spec.id << " is unreadable, starting over";
    }
}

std::vector<std::string> HomeVendors::vendor_tags() const
{
    std::vector<std::string> out;
    for (const Connector& c : m_connectors)
        if (!c.spec.vendor.empty() && std::find(out.begin(), out.end(), c.spec.vendor) == out.end())
            out.push_back(c.spec.vendor);
    return out;
}

// ------------------------------------------------------------------------------ secrets ----

#if wxUSE_SECRETSTORE
static wxString service_of(const std::string& id, const std::string& key)
{
    return wxString::FromUTF8("EdgeSlicer vendor connector/" + id + "/" + key);
}
#endif

bool HomeVendors::secure_store() const
{
#if wxUSE_SECRETSTORE
    wxString why;
    return wxSecretStore::GetDefault().IsOk(&why);
#else
    return false;
#endif
}

bool HomeVendors::has_secret(const std::string& id, const std::string& key) const
{
    if (m_session_secrets.count(id + "/" + key))
        return true;
#if wxUSE_SECRETSTORE
    if (secure_store()) {
        wxString      user;
        wxSecretValue value;
        return wxSecretStore::GetDefault().Load(service_of(id, key), user, value) && value.GetSize() > 0;
    }
#endif
    return false;
}

Vendors::Secrets HomeVendors::secrets_of(const Vendors::Spec& spec) const
{
    Vendors::Secrets out;
    for (const auto& slot : Vendors::secret_slots(spec)) {
        auto s = m_session_secrets.find(spec.id + "/" + slot.first);
        if (s != m_session_secrets.end()) {
            out[slot.first] = s->second;
            continue;
        }
#if wxUSE_SECRETSTORE
        if (secure_store()) {
            wxString      user;
            wxSecretValue value;
            if (wxSecretStore::GetDefault().Load(service_of(spec.id, slot.first), user, value) && value.GetSize() > 0)
                out[slot.first] = std::string(static_cast<const char*>(value.GetData()), value.GetSize());
        }
#endif
    }
    return out;
}

void HomeVendors::set_secret(const std::string& id, const std::string& key, const std::string& value)
{
#if wxUSE_SECRETSTORE
    if (secure_store()) {
        if (value.empty())
            wxSecretStore::GetDefault().Delete(service_of(id, key));
        else if (wxSecretStore::GetDefault().Save(service_of(id, key), wxString::FromUTF8(key), wxSecretValue(value.size(), value.data()))) {
            m_session_secrets.erase(id + "/" + key);
            return;
        } else
            BOOST_LOG_TRIVIAL(warning) << "HomeVendors: the credential store refused a secret; keeping it for this session only";
    }
#endif
    if (value.empty())
        m_session_secrets.erase(id + "/" + key);
    else
        m_session_secrets[id + "/" + key] = value;
}

void HomeVendors::forget_secrets(const Vendors::Spec& spec)
{
    for (const auto& slot : Vendors::secret_slots(spec))
        set_secret(spec.id, slot.first, std::string());
    // Also any the session holds for slots the spec no longer has.
    for (auto it = m_session_secrets.begin(); it != m_session_secrets.end();)
        it = it->first.rfind(spec.id + "/", 0) == 0 ? m_session_secrets.erase(it) : std::next(it);
}

void HomeVendors::ask_secret(const std::string& id, const std::string& key)
{
    Connector* c = find(id);
    if (c == nullptr)
        return;
    const auto slots = Vendors::secret_slots(c->spec);
    auto       slot  = std::find_if(slots.begin(), slots.end(), [&key](const auto& s) { return s.first == key; });
    if (slot == slots.end())
        return;
    const wxString title   = wxString::FromUTF8(c->spec.name);
    const wxString message = wxString::Format(_L("%s for %s. Leave it empty to forget it."), wxString::FromUTF8(slot->second), title) + "\n" +
                             (secure_store() ? _L("It is kept in your system's credential store, not in EdgeSlicer's settings.")
                                             : _L("This system has no credential store, so it is kept only until EdgeSlicer closes."));
    wxString value;
    if (key == "auth_user") {
        wxTextEntryDialog dlg(m_parent, message, title);
        if (dlg.ShowModal() != wxID_OK)
            return;
        value = dlg.GetValue();
    } else {
        wxPasswordEntryDialog dlg(m_parent, message, title);
        if (dlg.ShowModal() != wxID_OK)
            return;
        value = dlg.GetValue();
    }
    std::string v = into_u8(value);
    v.erase(0, v.find_first_not_of(" \t\r\n"));
    v.erase(v.find_last_not_of(" \t\r\n") + 1);
    set_secret(id, key, v);
    send_state();
}

// ------------------------------------------------------------------------------ the page ----

void HomeVendors::notice(const std::string& text, bool error)
{
    m_send({{"type", "vendor_notice"}, {"text", text}, {"error", error}});
}

void HomeVendors::send_state()
{
    m_keys.clear();
    json connectors = json::array();
    json items      = json::array();
    const bool secure = secure_store();
    for (Connector& c : m_connectors) {
        ensure_cache(c);
        json slots = json::array();
        for (const auto& [key, label] : Vendors::secret_slots(c.spec))
            slots.push_back({{"key", key}, {"label", label}, {"set", has_secret(c.spec.id, key)}});
        connectors.push_back({{"id", c.spec.id},
                              {"name", c.spec.name},
                              {"vendor", c.spec.vendor},
                              {"spec", Vendors::spec_to_json(c.spec)},
                              {"slots", slots},
                              {"secure", secure},
                              {"syncing", c.syncing},
                              {"synced_at", c.cache.synced_at},
                              {"count", c.cache.items.size()},
                              {"last_error", c.cache.last_error},
                              {"quota", {{"used", c.cache.quota.used}, {"limit", c.cache.quota.limit}, {"resets", c.cache.quota.resets}}},
                              {"downloads_limited", c.spec.downloads_limited}});
        const bool endpoint = !c.spec.download_path.empty();
        for (const Vendors::Item& i : c.cache.items) {
            const std::string key = key_of(c.spec.id, i.id);
            m_keys[key]           = {c.spec.id, i.id};
            json subs             = json::array();
            for (const Vendors::SubItem& s : i.subs)
                subs.push_back({{"id", s.id}, {"name", s.name}, {"variant", s.variant}, {"size", s.size}, {"plates", s.plates},
                                {"print_time_s", s.print_time_s}, {"print_time_text", s.print_time_text}, {"colours", s.colours},
                                {"can_download", endpoint || !s.download.empty()}});
            items.push_back({{"key", key},
                             {"connector", c.spec.id},
                             {"name", i.name},
                             {"designer", i.designer},
                             {"license", i.license},
                             {"description", i.description.substr(0, 600)},
                             {"tags", i.tags},
                             {"updated", i.updated},
                             {"has_thumb", !i.thumbnail.empty()},
                             {"has_page", !i.page_url.empty()},
                             {"can_download", i.subs.empty() && (endpoint || !i.download.empty())},
                             {"subs", subs}});
        }
    }
    m_send({{"type", "vendors"}, {"connectors", connectors}, {"items", items}});
}

const Vendors::Item* HomeVendors::item_of(const std::string& key, Connector** connector)
{
    auto k = m_keys.find(key);
    if (k == m_keys.end())
        return nullptr;
    Connector* c = find(k->second.first);
    if (c == nullptr)
        return nullptr;
    for (const Vendors::Item& i : c->cache.items)
        if (i.id == k->second.second) {
            if (connector)
                *connector = c;
            return &i;
        }
    return nullptr;
}

bool HomeVendors::handle(const json& msg)
{
    const std::string command = msg.value("command", std::string());
    if (command.rfind("vendor_", 0) != 0)
        return false;
    const std::string id  = msg.value("id", std::string());
    const std::string key = msg.value("key", std::string());

    if (command == "vendor_state")
        send_state();
    else if (command == "vendor_sync") {
        if (find(id))
            sync(id, msg.value("full", false));
    } else if (command == "vendor_test") {
        if (find(id))
            test(Vendors::spec_to_json(find(id)->spec));
    } else if (command == "vendor_save") {
        if (msg.contains("spec"))
            save_connector(msg["spec"]);
    } else if (command == "vendor_delete") {
        if (find(id))
            delete_connector(id);
    } else if (command == "vendor_secret") {
        ask_secret(id, msg.value("slot", std::string()));
    } else if (command == "vendor_forget") {
        if (Connector* c = find(id)) {
            forget_secrets(c->spec);
            send_state();
        }
    } else if (command == "vendor_import")
        import_spec();
    else if (command == "vendor_export_spec") {
        if (find(id))
            export_spec(id);
    } else if (command == "vendor_csv") {
        export_csv(msg.contains("keys") ? msg["keys"] : json::array());
    } else if (command == "vendor_thumbs") {
        std::vector<std::string> keys;
        if (msg.contains("keys") && msg["keys"].is_array())
            for (const json& v : msg["keys"])
                if (v.is_string() && m_keys.count(v.get<std::string>()) && keys.size() < 200)
                    keys.push_back(v.get<std::string>());
        if (!keys.empty())
            send_thumbnails(keys);
    } else if (command == "vendor_open") {
        if (const Vendors::Item* i = item_of(key))
            if (Vendors::is_allowed_url(i->page_url))
                wxLaunchDefaultBrowser(wxString::FromUTF8(i->page_url));
    } else if (command == "vendor_copy") {
        if (const Vendors::Item* i = item_of(key))
            if (Vendors::is_allowed_url(i->page_url) && wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(i->page_url)));
                wxTheClipboard->Close();
                notice(_u8L("Link copied."));
            }
    } else if (command == "vendor_download") {
        if (item_of(key))
            download(key, msg.value("sub", std::string()));
    } else
        BOOST_LOG_TRIVIAL(warning) << "HomeVendors: unknown command \"" << command << "\"";
    return true;
}

// ------------------------------------------------------------------------------ connectors ----

void HomeVendors::save_connector(const json& spec_json)
{
    Vendors::Spec spec;
    try {
        spec = Vendors::spec_from_json(spec_json);
    } catch (const std::exception& e) {
        m_send({{"type", "vendor_invalid"}, {"error", e.what()}});
        return;
    }
    Connector* existing = spec.id.empty() ? nullptr : find(spec.id);
    if (existing == nullptr) {
        // A new connector (an id the page made up is not taken).
        std::random_device rd;
        do
            spec.id = Vendors::make_id(spec.name, rd());
        while (find(spec.id));
        Connector c;
        c.spec         = spec;
        c.cache_loaded = true;
        m_connectors.push_back(std::move(c));
    } else {
        // Secrets of slots the spec no longer has are dropped.
        const auto now_slots = Vendors::secret_slots(spec);
        for (const auto& old : Vendors::secret_slots(existing->spec))
            if (std::none_of(now_slots.begin(), now_slots.end(), [&old](const auto& s) { return s.first == old.first; }))
                set_secret(spec.id, old.first, std::string());
        // A different address or list invalidates what was fetched.
        const bool reset = existing->spec.base_url != spec.base_url || existing->spec.list_path != spec.list_path ||
                           existing->spec.items_path != spec.items_path;
        existing->spec = spec;
        if (reset) {
            existing->cache = Vendors::Cache();
            write_atomic(fs::path(cache_dir(spec.id)) / "items.json", Vendors::cache_to_json(existing->cache).dump());
        }
    }
    save_specs();
    m_send({{"type", "vendor_saved"}, {"id", spec.id}});
    send_state();
}

void HomeVendors::delete_connector(const std::string& id)
{
    Connector* c = find(id);
    MessageDialog dlg(m_parent,
                      wxString::Format(_L("Remove the connector \"%s\"? Its saved credentials and the list it fetched are removed too."),
                                       wxString::FromUTF8(c->spec.name)),
                      _L("Vendors"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return;
    forget_secrets(c->spec);
    boost::system::error_code ec;
    fs::remove_all(cache_dir(id), ec);
    m_connectors.erase(std::remove_if(m_connectors.begin(), m_connectors.end(), [&id](const Connector& x) { return x.spec.id == id; }),
                       m_connectors.end());
    save_specs();
    send_state();
}

void HomeVendors::import_spec()
{
    wxFileDialog dlg(m_parent, _L("Import a vendor connector"), wxEmptyString, wxEmptyString, "JSON (*.json)|*.json",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string text = read_file(into_path(dlg.GetPath()), 256 * 1024);
    json              j;
    try {
        j = json::parse(text);
        j.erase("id"); // always a new connector
        Vendors::spec_from_json(j);
    } catch (const std::exception& e) {
        notice(_u8L("That file is not a connector this version can use:") + " " + e.what(), true);
        return;
    }
    save_connector(j);
    notice(_u8L("Connector imported. Set its credentials, then Refresh."));
}

void HomeVendors::export_spec(const std::string& id)
{
    Connector* c = find(id);
    wxFileDialog dlg(m_parent, _L("Export the connector (without credentials)"), wxEmptyString,
                     wxString::FromUTF8(safe_file_name(c->spec.name) + ".json"), "JSON (*.json)|*.json", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    json j = Vendors::spec_to_json(c->spec);
    j.erase("id");
    if (!write_atomic(into_path(dlg.GetPath()), j.dump(2)))
        notice(_u8L("The file could not be written."), true);
}

void HomeVendors::export_csv(const json& keys)
{
    // What the page shows, in its order; grouped per connector for the CSV.
    std::vector<std::pair<Connector*, std::vector<Vendors::Item>>> groups;
    if (keys.is_array())
        for (const json& k : keys) {
            Connector*           c = nullptr;
            const Vendors::Item* i = k.is_string() ? item_of(k.get<std::string>(), &c) : nullptr;
            if (i == nullptr)
                continue;
            if (groups.empty() || groups.back().first != c)
                groups.push_back({c, {}});
            groups.back().second.push_back(*i);
        }
    if (groups.empty()) {
        notice(_u8L("There is nothing to export."), true);
        return;
    }
    wxFileDialog dlg(m_parent, _L("Export the list as CSV"), wxEmptyString, "vendor-models.csv", "CSV (*.csv)|*.csv",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    std::string csv;
    for (const auto& [c, items] : groups) {
        std::string part = Vendors::to_csv(c->spec.name, c->spec.vendor, items);
        if (!csv.empty())
            part = part.substr(part.find("\r\n") + 2); // one mark and header for the whole file
        csv += part;
    }
    if (write_atomic(into_path(dlg.GetPath()), csv))
        notice(_u8L("List exported."));
    else
        notice(_u8L("The file could not be written."), true);
}

// ------------------------------------------------------------------------------ network ----

void HomeVendors::sync(const std::string& id, bool full)
{
    Connector* c = find(id);
    if (c->syncing)
        return;
    ensure_cache(*c);
    c->syncing = true;
    send_state();
    const Vendors::Spec    spec    = c->spec;
    const Vendors::Secrets secrets = secrets_of(spec);
    const Vendors::Cache   cache   = c->cache;
    const std::string      dir     = cache_dir(id);
    std::weak_ptr<bool>    alive   = m_alive;
    auto                   cancel  = m_cancel;
    std::thread([this, alive, cancel, spec, secrets, cache, dir, full]() {
        Vendors::SyncOptions options;
        options.full = full;
        Vendors::SyncResult r = Vendors::sync(spec, secrets, cache, [](const Vendors::Request& q) { return http_call(q, LIST_LIMIT); },
                                              *cancel, int64_t(std::time(nullptr)), options);
        if (*cancel)
            return;
        if (r.ok || !r.cache.items.empty() || !r.error.empty())
            write_atomic(fs::path(dir) / "items.json", Vendors::cache_to_json(r.cache).dump(-1, ' ', false, json::error_handler_t::replace));
        BOOST_LOG_TRIVIAL(info) << "HomeVendors: synced " << spec.id << ": " << r.cache.items.size() << " items, " << r.requests
                                << " requests" << (r.error.empty() ? std::string() : ", stopped: " + r.error);
        wxGetApp().CallAfter([this, alive, id = spec.id, r = std::move(r)]() mutable {
            if (alive.expired())
                return;
            Connector* c = find(id);
            if (c == nullptr)
                return; // removed meanwhile
            c->syncing = false;
            c->cache   = std::move(r.cache);
            m_thumbs_asked.clear();
            send_state();
            if (!r.error.empty())
                notice(c->spec.name + ": " + r.error, !r.partial);
        });
    }).detach();
}

void HomeVendors::test(const json& spec_json)
{
    Vendors::Spec spec;
    try {
        spec = Vendors::spec_from_json(spec_json);
    } catch (const std::exception& e) {
        notice(e.what(), true);
        return;
    }
    const Vendors::Secrets secrets = secrets_of(spec);
    std::weak_ptr<bool>    alive   = m_alive;
    notice(_u8L("Testing the connection..."));
    std::thread([this, alive, spec, secrets]() {
        const Vendors::TestResult t = Vendors::test_connection(spec, secrets, [](const Vendors::Request& q) { return http_call(q, LIST_LIMIT); });
        wxGetApp().CallAfter([this, alive, t, name = spec.name]() {
            if (alive.expired())
                return;
            if (!t.ok)
                notice(name + ": " + t.error, true);
            else {
                std::string text = name + ": " + _u8L("connected.") + " " + std::to_string(t.items) + " " + _u8L("models on the first page");
                if (t.total >= 0)
                    text += ", " + std::to_string(t.total) + " " + _u8L("in all");
                if (!t.quota.limit.empty())
                    text += ". " + _u8L("API calls used:") + " " + t.quota.used + " / " + t.quota.limit;
                notice(text + ".");
            }
        });
    }).detach();
}

void HomeVendors::send_thumbnails(const std::vector<std::string>& keys)
{
    struct Job
    {
        std::string      key, url, dir;
        Vendors::Request request;
    };
    std::vector<Job> jobs;
    for (const std::string& key : keys) {
        if (!m_thumbs_asked.insert(key).second)
            continue;
        Connector*           c = nullptr;
        const Vendors::Item* i = item_of(key, &c);
        if (i == nullptr || i->thumbnail.empty())
            continue;
        Job job;
        job.key = key;
        job.url = i->thumbnail;
        job.dir = cache_dir(c->spec.id);
        // Credentials only to the API's own site; a CDN gets a bare request.
        job.request = Vendors::same_origin(i->thumbnail, c->spec.base_url) ? Vendors::authorize(c->spec, secrets_of(c->spec), i->thumbnail)
                                                                           : Vendors::Request { i->thumbnail, {} };
        jobs.push_back(std::move(job));
    }
    if (jobs.empty())
        return;
    std::weak_ptr<bool> alive  = m_alive;
    auto                cancel = m_cancel;
    std::thread([this, alive, cancel, jobs = std::move(jobs)]() {
        json images = json::object();
        auto flush  = [&]() {
            if (images.empty())
                return;
            wxGetApp().CallAfter([this, alive, images]() {
                if (!alive.expired())
                    m_send({{"type", "vendor_thumbs"}, {"images", images}});
            });
            images = json::object();
        };
        for (const Job& job : jobs) {
            if (*cancel)
                return;
            const fs::path file = fs::path(job.dir) / "thumbs" / Library::entry_id(job.url);
            std::string    bytes = read_file(file, THUMB_LIMIT);
            if (bytes.empty()) {
                const Vendors::Response r = http_call(job.request, THUMB_LIMIT);
                if (r.status >= 200 && r.status < 300 && !image_data_uri(r.body).empty()) {
                    bytes = r.body;
                    write_atomic(file, bytes);
                }
            }
            const std::string uri = image_data_uri(bytes);
            if (!uri.empty())
                images[job.key] = uri;
            if (images.size() >= 12)
                flush(); // show them as they come
        }
        flush();
    }).detach();
}

void HomeVendors::download(const std::string& key, const std::string& sub_id)
{
    Connector*           c = nullptr;
    const Vendors::Item* i = item_of(key, &c);
    const Vendors::SubItem* sub = nullptr;
    if (!sub_id.empty()) {
        for (const Vendors::SubItem& s : i->subs)
            if (s.id == sub_id)
                sub = &s;
        if (sub == nullptr)
            return;
    }
    Vendors::Request request;
    bool             direct = false;
    const Vendors::Secrets secrets = secrets_of(c->spec);
    if (!Vendors::build_download_request(c->spec, secrets, *i, sub, request, direct)) {
        notice(_u8L("This connector has no way to download files."), true);
        return;
    }
    const std::string label = i->name + (sub != nullptr && !sub->name.empty() ? " - " + sub->name : std::string());
    if (c->spec.downloads_limited) {
        MessageDialog dlg(m_parent,
                          wxString::Format(_L("%s counts downloads against a limit. Download \"%s\"?"), wxString::FromUTF8(c->spec.name),
                                           wxString::FromUTF8(label)),
                          _L("Vendors"), wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
        if (dlg.ShowModal() != wxID_YES)
            return;
    }
    // Into a Library folder by default, so it shows up there.
    wxString dir;
    for (const Library::Folder& f : Library::folders_from_json(wxGetApp().app_config->get("home_library_folders"))) {
        boost::system::error_code ec;
        if (fs::is_directory(fs::path(f.path), ec)) {
            dir = wxString::FromUTF8(f.path);
            break;
        }
    }
    std::string ext = ".3mf";
    if (direct) {
        const std::string path = lower(request.url.substr(0, request.url.find_first_of("?#")));
        for (const char* e : {".3mf", ".stl", ".step", ".stp", ".obj", ".amf", ".zip"})
            if (path.size() > strlen(e) && path.compare(path.size() - strlen(e), strlen(e), e) == 0)
                ext = e;
    }
    wxFileDialog dlg(m_parent, _L("Save the model"), dir, wxString::FromUTF8(safe_file_name(label) + ext), "*" + ext,
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const fs::path target = into_path(dlg.GetPath());
    notice(_u8L("Downloading") + " " + label + "...");

    std::weak_ptr<bool> alive = m_alive;
    auto                cancel = m_cancel;
    std::thread([this, alive, cancel, spec = c->spec, secrets, request, direct, target, label]() {
        std::string error;
        Vendors::Request file_request = request;
        if (!direct) {
            // The endpoint answers with a short-lived link to the file.
            const std::string url = Vendors::resolve_download(spec, secrets, http_call(request, LIST_LIMIT), error);
            if (!url.empty())
                file_request = Vendors::same_origin(url, spec.base_url) ? Vendors::authorize(spec, secrets, url) : Vendors::Request { url, {} };
        }
        bool saved = false;
        if (error.empty() && !*cancel) {
            const Vendors::Response r = http_call(file_request, FILE_LIMIT);
            if (r.status >= 200 && r.status < 300 && !r.body.empty())
                saved = write_atomic(target, r.body);
            if (!saved)
                error = r.status == 0 ? _u8L("The download failed:") + " " + r.error.substr(0, 200)
                        : r.status >= 300 ? _u8L("The download failed with HTTP") + " " + std::to_string(r.status)
                                          : _u8L("The file could not be written.");
        }
        wxGetApp().CallAfter([this, alive, saved, error, target, label]() {
            if (alive.expired())
                return;
            if (saved) {
                notice(_u8L("Saved") + " " + label + " " + _u8L("to") + " " + target.parent_path().string());
                if (m_library_changed)
                    m_library_changed();
            } else
                notice(label + ": " + error, true);
        });
    }).detach();
}

} // namespace GUI
} // namespace Slic3r
