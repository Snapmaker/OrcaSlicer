#include "PrintersMonitor.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace Slic3r {
namespace PrintersMonitor {

using json = nlohmann::json;

static bool starts_with(const std::string& s, const char* prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool valid_printer_id(const std::string& id)
{
    if (id.empty() || id.size() > 200)
        return false;
    for (unsigned char c : id) {
        if (c <= 0x20 || c >= 0x7f) return false; // control, space, non-ASCII
        if (c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' || c == '`') return false;
    }
    return true;
}

bool valid_action(const std::string& action) { return action == "pause" || action == "resume" || action == "stop"; }

Message parse_message(const std::string& msg)
{
    Message m;
    if (msg == "printers_get") { m.kind = Message::Kind::Get; return m; }
    if (msg == "printers_groups_get") { m.kind = Message::Kind::Groups; return m; }
    if (msg == "printers_tasks") { m.kind = Message::Kind::Tasks; return m; }
    if (starts_with(msg, "printers_thumb:")) {
        const std::string id = msg.substr(15);
        if (!valid_archive_id(id)) return m;
        m.id   = id;
        m.kind = Message::Kind::Thumb;
        return m;
    }
    if (starts_with(msg, "printers_job:")) {
        const std::string n = msg.substr(13);
        if (n.empty() || n.size() > 9 || !std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return m;
        m.job  = std::stoi(n);
        m.kind = m.job > 0 ? Message::Kind::Job : Message::Kind::Invalid;
        return m;
    }
    if (starts_with(msg, "printers_open:")) {
        const std::string id = msg.substr(14);
        if (!valid_printer_id(id)) return m;
        m.id   = id;
        m.kind = Message::Kind::Open;
        return m;
    }
    if (starts_with(msg, "printers_control:")) {
        // <action>:<0|1>:<id>
        const std::string rest = msg.substr(17);
        const size_t      a    = rest.find(':');
        if (a == std::string::npos) return m;
        const size_t b = rest.find(':', a + 1);
        if (b == std::string::npos) return m;
        const std::string action  = rest.substr(0, a);
        const std::string confirm = rest.substr(a + 1, b - a - 1);
        const std::string id      = rest.substr(b + 1);
        if (!valid_action(action) || (confirm != "0" && confirm != "1") || !valid_printer_id(id)) return m;
        m.action  = action;
        m.confirm = confirm == "1";
        m.id      = id;
        m.kind    = Message::Kind::Control;
        return m;
    }
    return m;
}

static bool is_identifier(const std::string& s)
{
    if (s.empty() || s.size() > 64) return false;
    if (std::isdigit((unsigned char) s[0])) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '$'; });
}

std::string script_call(const std::string& fn, const json& arg)
{
    if (!is_identifier(fn))
        return std::string();
    // ensure_ascii: U+2028 / U+2029 and anything else outside ASCII become \uXXXX escapes.
    // replace: a name with broken UTF-8 must not throw here.
    const std::string a = arg.dump(-1, ' ', true, json::error_handler_t::replace);
    return "if (window." + fn + ") window." + fn + "(" + a + ");";
}

json page_payload(int status, const std::string& body, long long now_ms)
{
    json out;
    out["at"] = now_ms;
    out["printers"] = json::array();
    if (status != 200) {
        out["ok"] = false;
        std::string why;
        try {
            const json e = json::parse(body);
            if (e.is_object() && e.contains("error") && e["error"].is_string()) why = e["error"].get<std::string>();
        } catch (...) {}
        out["error"] = why.empty() ? std::string("the printer list is not available right now") : why;
        return out;
    }
    try {
        json j = json::parse(body);
        if (j.is_object() && j.contains("printers") && j["printers"].is_array()) {
            for (json& p : j["printers"])
                if (p.is_object() && p.contains("id") && p["id"].is_string() && valid_printer_id(p["id"].get<std::string>()))
                    out["printers"].push_back(std::move(p));
            out["ok"] = true;
            return out;
        }
    } catch (...) {}
    out["ok"]    = false;
    out["error"] = "the printer list could not be read";
    return out;
}

std::map<std::string, Target> targets_of(const json& printers)
{
    std::map<std::string, Target> out;
    if (!printers.is_array()) return out;
    for (const json& p : printers) {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_string()) continue;
        const std::string id = p["id"].get<std::string>();
        if (!valid_printer_id(id) || out.count(id)) continue;
        Target t;
        if (p.contains("kind") && p["kind"].is_string()) t.kind = p["kind"].get<std::string>();
        if (p.contains("url") && p["url"].is_string()) t.url = p["url"].get<std::string>();
        out.emplace(id, t);
    }
    return out;
}

bool is_web_url(const std::string& url)
{
    std::string rest;
    if (starts_with(url, "http://")) rest = url.substr(7);
    else if (starts_with(url, "https://")) rest = url.substr(8);
    else return false;
    if (rest.empty() || rest[0] == '/' || rest[0] == ':' || url.size() > 2048) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' || c == '`') return false;
    return true;
}

OpenWay open_way(const Target& t, bool bambu_monitor_shown, bool printer_view_shown)
{
    if (t.kind == "bambu")
        return bambu_monitor_shown ? OpenWay::BambuMonitor : OpenWay::NotHere;
    if (!is_web_url(t.url))
        return OpenWay::NotHere;
    return printer_view_shown ? OpenWay::PrinterWebView : OpenWay::Browser;
}

std::string not_here_reason(const Target& t)
{
    if (t.kind == "bambu")
        return "The Device tab shows Bambu Lab printers while a Bambu Lab printer preset is selected. Select one, then try again.";
    return "This printer has no web page to open.";
}

bool valid_archive_id(const std::string& id)
{
    if (id.empty() || id.size() > 128 || id[0] == '.') return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}

static std::string lower(std::string s)
{
    for (char& c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string url_encode_segment(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += (char) c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

CoverIndex index_covers(const std::vector<std::pair<std::string, std::string>>& vendor_files)
{
    static const std::string suffix = "_cover.png";
    CoverIndex out;
    for (const auto& vf : vendor_files) {
        const std::string& f = vf.second;
        if (vf.first.empty() || f.size() <= suffix.size() || lower(f.substr(f.size() - suffix.size())) != suffix) continue;
        const std::string model = lower(f.substr(0, f.size() - suffix.size()));
        // The first vendor folder wins (several vendors ship the same generic names).
        out.emplace(model, "/profiles/" + url_encode_segment(vf.first) + "/" + url_encode_segment(f));
    }
    return out;
}

std::string cover_for(const CoverIndex& index, const std::string& model_name)
{
    if (model_name.empty()) return "";
    auto it = index.find(lower(model_name));
    return it == index.end() ? std::string() : it->second;
}

std::string short_model_name(const std::string& kind, const std::string& model_name, const std::string& model_code)
{
    if (model_name.empty()) return model_code;
    static const std::string bambu = "Bambu Lab ";
    if (kind == "bambu" && model_name.size() > bambu.size() && model_name.compare(0, bambu.size(), bambu) == 0)
        return model_name.substr(bambu.size());
    return model_name;
}

std::string row_job(const json& row)
{
    if (!row.is_object()) return "";
    std::string job;
    if (row.contains("task") && row["task"].is_string()) job = row["task"].get<std::string>();
    if (job.empty() && row.contains("subtask_name") && row["subtask_name"].is_string()) job = row["subtask_name"].get<std::string>();
    return job;
}

std::map<std::string, std::string> pick_thumbnails(const std::vector<ArchiveEntry>& entries, const std::map<std::string, std::string>& jobs)
{
    std::map<std::string, std::pair<long long, std::string>> best;
    for (const ArchiveEntry& e : entries) {
        if (e.printer_id.empty() || !valid_archive_id(e.record_id)) continue;
        const auto job = jobs.find(e.printer_id);
        if (job == jobs.end() || job->second.empty()) continue;
        if (e.sent_name != job->second && e.file != job->second) continue;
        auto& b = best[e.printer_id];
        if (b.second.empty() || e.time > b.first) b = { e.time, e.record_id };
    }
    std::map<std::string, std::string> out;
    for (const auto& kv : best) out[kv.first] = kv.second.second;
    return out;
}

void enrich_rows(json& printers, const CoverIndex& covers, const std::map<std::string, std::string>& thumbs)
{
    if (!printers.is_array()) return;
    for (json& p : printers) {
        if (!p.is_object()) continue;
        const std::string kind = p.contains("kind") && p["kind"].is_string() ? p["kind"].get<std::string>() : std::string();
        const std::string code = p.contains("model") && p["model"].is_string() ? p["model"].get<std::string>() : std::string();
        std::string       name = p.contains("model_name") && p["model_name"].is_string() ? p["model_name"].get<std::string>() : std::string();
        if (name.empty() && kind != "bambu") name = code; // a Bambu model code is not a name
        p["model_short"] = short_model_name(kind, name, code);
        const std::string pic = cover_for(covers, name);
        if (!pic.empty()) p["picture"] = pic;
        const std::string id = p.contains("id") && p["id"].is_string() ? p["id"].get<std::string>() : std::string();
        auto t = thumbs.find(id);
        if (t != thumbs.end()) p["thumb_id"] = t->second;
    }
}

// ---- groups ----

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char) s[a])) ++a;
    while (b > a && std::isspace((unsigned char) s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<Group> parse_groups(const std::string& stored)
{
    std::vector<Group> out;
    if (trim(stored).empty()) return out;
    json j;
    try { j = json::parse(stored); } catch (...) { return out; }
    if (j.is_object() && j.contains("groups")) j = j["groups"];
    if (!j.is_array()) return out;
    std::set<std::string> seen;
    for (const json& g : j) {
        if (!g.is_object() || !g.contains("id") || !g["id"].is_string()) continue;
        Group grp;
        grp.id = g["id"].get<std::string>();
        if (!valid_printer_id(grp.id) || grp.id == "all" || seen.count(grp.id)) continue;
        if (g.contains("name") && g["name"].is_string()) grp.name = trim(g["name"].get<std::string>());
        if (grp.name.size() > 64) grp.name = grp.name.substr(0, 64);
        if (grp.name.empty()) grp.name = grp.id;
        std::set<std::string> members;
        if (g.contains("printers") && g["printers"].is_array())
            for (const json& p : g["printers"])
                if (p.is_string() && valid_printer_id(p.get<std::string>()) && members.insert(p.get<std::string>()).second)
                    grp.printers.push_back(p.get<std::string>());
        seen.insert(grp.id);
        out.push_back(std::move(grp));
    }
    return out;
}

std::string save_groups(const std::vector<Group>& groups)
{
    json arr = json::array();
    for (const Group& g : groups)
        arr.push_back({ { "id", g.id }, { "name", g.name }, { "printers", g.printers } });
    return json { { "version", 1 }, { "groups", arr } }.dump();
}

std::vector<Group> seed_from_multi_devices(const std::vector<std::string>& dev_ids)
{
    Group g;
    g.id   = "multi-device";
    g.name = "Multi-device";
    std::set<std::string> seen;
    for (const std::string& raw : dev_ids) {
        const std::string id = trim(raw);
        if (valid_printer_id(id) && seen.insert(id).second) g.printers.push_back(id);
    }
    if (g.printers.empty()) return {};
    return { g };
}

json groups_json(const std::vector<Group>& groups)
{
    json arr = json::array();
    arr.push_back({ { "id", "all" }, { "name", "All printers" }, { "all", true }, { "printers", json::array() } });
    for (const Group& g : groups)
        arr.push_back({ { "id", g.id }, { "name", g.name }, { "all", false }, { "printers", g.printers } });
    return arr;
}

} // namespace PrintersMonitor
} // namespace Slic3r
