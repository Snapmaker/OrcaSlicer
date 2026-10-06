#include "SnapmakerLan.hpp"
#include "FilamentCommands.hpp"

#include "GUI_App.hpp"
#include "RemoteHub.hpp" // hub_dir
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <map>
#include <set>
#include <mutex>
#include <thread>

namespace Slic3r {
namespace GUI {
namespace SnapmakerLan {

using nlohmann::json;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------- helpers ----

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string base_url(const Device& d)
{
    return "http://" + d.ip + (d.port > 0 && d.port != 80 ? ":" + std::to_string(d.port) : "");
}

// One short GET. Moonraker answers {"result": ...} or {"error": ...}; the parsed body comes back.
static bool get_json(const std::string& url, json& out, std::string& error, int timeout_s = 4, int connect_s = 0)
{
    bool ok = false;
    Http::get(url).tls_policy(Http::TlsPolicy::PrintHost) // printer: keep accepting self-signed certificates
        .timeout_connect(connect_s > 0 ? connect_s : timeout_s)
        .timeout_max(timeout_s)
        .on_complete([&](std::string body, unsigned) {
            try {
                out = json::parse(body);
                ok  = true;
            } catch (const std::exception& e) {
                error = std::string("unreadable answer: ") + e.what();
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try { // Moonraker puts its own message in the body of a 4xx
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error += ": " + j["error"]["message"].get<std::string>();
            } catch (...) {}
        })
        .perform_sync();
    return ok;
}

static std::string str_of(const json& j, const char* key, const std::string& def = "")
{
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : def;
}

static double num_of(const json& j, const char* key, double def = 0)
{
    return j.contains(key) && j[key].is_number() ? j[key].get<double>() : def;
}

// ------------------------------------------------------------- the store ----

static std::mutex s_store_mutex;

static std::string store_path() { return (fs::path(RemoteHub::hub_dir()) / "snapmaker_lan.json").string(); }

static json load_store()
{
    try {
        boost::nowide::ifstream in(store_path());
        if (!in.good())
            return json::object();
        json j;
        in >> j;
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

// Written through a temporary file: another instance reading the list never sees half of it.
static void save_store(const json& j)
{
    try {
        boost::system::error_code ec;
        fs::create_directories(RemoteHub::hub_dir(), ec);
        const fs::path final(store_path());
        const fs::path temp = final.parent_path() / (final.filename().string() + ".tmp");
        {
            boost::nowide::ofstream out(temp.string(), std::ios::binary | std::ios::trunc);
            out << j.dump(1);
        }
        fs::remove(final, ec);
        fs::rename(temp, final, ec);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[SnapmakerLan] could not write the device list: " << e.what();
    }
}

static Device device_of(const json& j)
{
    Device d;
    d.id       = str_of(j, "id");
    d.name     = str_of(j, "name");
    d.model    = str_of(j, "model");
    d.ip       = str_of(j, "ip");
    d.port     = j.contains("port") && j["port"].is_number_integer() ? j["port"].get<int>() : 80;
    d.added_by = str_of(j, "added_by", "manual");
    return d;
}

static json json_of(const Device& d)
{
    json j;
    j["id"]       = d.id;
    j["name"]     = d.name;
    j["model"]    = d.model;
    j["ip"]       = d.ip;
    j["port"]     = d.port;
    j["added_by"] = d.added_by;
    return j;
}

// ------------------------------------------------------------- addresses ----

std::string host_of(const std::string& address)
{
    std::string h = address;
    boost::trim(h);
    const size_t scheme = h.find("://");
    if (scheme != std::string::npos) h = h.substr(scheme + 3);
    const size_t slash = h.find('/');
    if (slash != std::string::npos) h = h.substr(0, slash);
    const size_t at = h.rfind('@'); // user:password@host
    if (at != std::string::npos) h = h.substr(at + 1);
    if (!h.empty() && h[0] == '[') { // [fe80::1]:7125
        const size_t close = h.find(']');
        return close == std::string::npos ? h.substr(1) : h.substr(1, close - 1);
    }
    const size_t colon = h.find(':');
    if (colon != std::string::npos && h.find(':', colon + 1) == std::string::npos) h = h.substr(0, colon); // host:port
    while (!h.empty() && h.back() == '.') h.pop_back(); // a fully qualified "name."
    return h;
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

static bool ends_with_label(const std::string& host, const char* suffix)
{
    const std::string s(suffix);
    return host == s || (host.size() > s.size() && boost::ends_with(host, "." + s));
}

bool is_cloud_host(const std::string& host_in)
{
    const std::string host = lower(host_of(host_in));
    if (host.empty()) return false;
    for (const char* cloud : { "amazonaws.com", "amazonaws.com.cn", "aliyuncs.com", "snapmaker.com", "snapmaker.cn",
                               "snapmaker.net" })
        if (ends_with_label(host, cloud)) return true;
    // An IoT broker by another name (AWS IoT data endpoints all carry ".iot.<region>.").
    return host.find(".iot.") != std::string::npos;
}

static bool is_ipv4_literal(const std::string& h)
{
    int parts = 0, digits = 0, value = 0;
    for (size_t i = 0; i <= h.size(); ++i) {
        if (i == h.size() || h[i] == '.') {
            if (digits == 0 || value > 255) return false;
            ++parts;
            digits = value = 0;
        } else if (std::isdigit((unsigned char) h[i]) && digits < 3) {
            value = value * 10 + (h[i] - '0');
            ++digits;
        } else {
            return false;
        }
    }
    return parts == 4;
}

bool is_lan_host(const std::string& host_in)
{
    const std::string host = host_of(host_in);
    if (host.empty() || host.size() > 253 || is_cloud_host(host)) return false;
    for (char c : host)
        if (!(std::isalnum((unsigned char) c) || c == '.' || c == '-' || c == '_' || c == ':' || c == '%'))
            return false;
    return true;
}

bool is_local_address(const std::string& host_in)
{
    const std::string host = lower(host_of(host_in));
    if (!is_lan_host(host)) return false;
    if (is_ipv4_literal(host)) return true;
    if (host.find(':') != std::string::npos) return true; // an IPv6 literal
    if (host.find('.') == std::string::npos) return true; // a single-label name ("u1")
    return ends_with_label(host, "local") || ends_with_label(host, "lan") || ends_with_label(host, "home.arpa");
}

// ------------------------------------------------------------ list rules ----

bool merge_device(std::vector<Device>& list, const Device& d)
{
    if (!is_lan_host(d.ip)) return false; // the cloud broker (or nothing) is not a LAN address
    auto same = [&d](const Device& e) { return !d.id.empty() && e.id == d.id; };
    auto it   = std::find_if(list.begin(), list.end(), same);
    if (it == list.end())
        it = std::find_if(list.begin(), list.end(), [&d](const Device& e) { return e.ip == d.ip; });
    if (it == list.end()) {
        list.push_back(d);
        return true;
    }
    Device& merged = *it;
    merged.ip      = d.ip;
    if (d.port > 0) merged.port = d.port;
    if (!d.id.empty()) merged.id = d.id;
    if (!d.name.empty()) merged.name = d.name;
    if (!d.model.empty()) merged.model = d.model;
    // How it first arrived is how it stays: a printer someone typed in is theirs to remove.
    const std::string id  = merged.id;
    const size_t      pos = (size_t) (it - list.begin());
    const std::string ip  = merged.ip;
    // A printer that was known under its address and is now known by its serial number (or an old
    // list that already holds two entries for one printer) keeps exactly one entry: every other
    // entry with this id goes, and so does a placeholder (id = its own address) on this address.
    for (size_t i = list.size(); i-- > 0;)
        if (i != pos && ((!id.empty() && list[i].id == id) || (list[i].ip == ip && list[i].id == list[i].ip && id != ip)))
            list.erase(list.begin() + i);
    return true;
}

bool match_host(const std::vector<Device>& list, const std::string& address, Device& out)
{
    std::string host = host_of(address);
    if (host.empty() || is_cloud_host(host)) return false;
    for (char& c : host) c = (char) std::tolower((unsigned char) c);
    for (const Device& d : list) {
        std::string ip = host_of(d.ip);
        for (char& c : ip) c = (char) std::tolower((unsigned char) c);
        if (!ip.empty() && ip == host) { out = d; return true; }
    }
    return false;
}

bool device_for_host(const std::string& address, Device& out)
{
    try {
        return match_host(devices(), address, out);
    } catch (...) {
        return false;
    }
}

std::vector<Device> sanitize(const std::vector<Device>& raw)
{
    std::vector<Device> out;
    for (const Device& d : raw) {
        if (!is_lan_host(d.ip)) continue;
        if (!d.id.empty() && std::any_of(out.begin(), out.end(), [&d](const Device& e) { return e.id == d.id; }))
            continue;
        out.push_back(d);
    }
    // A placeholder (id = its own address) for an address another entry holds under a real id is
    // the same printer seen before its serial number was known.
    std::set<std::string> named;
    for (const Device& e : out)
        if (e.id != e.ip) named.insert(e.ip);
    out.erase(std::remove_if(out.begin(), out.end(), [&named](const Device& d) { return d.id == d.ip && named.count(d.ip); }),
              out.end());
    return out;
}

static std::vector<Device> stored_devices(const json& store)
{
    std::vector<Device> out;
    if (store.contains("devices") && store["devices"].is_array())
        for (const json& d : store["devices"])
            if (d.is_object() && !str_of(d, "ip").empty())
                out.push_back(device_of(d));
    return out;
}

std::vector<Device> devices()
{
    std::lock_guard<std::mutex> lock(s_store_mutex);
    return sanitize(stored_devices(load_store()));
}

bool find(const std::string& id, Device& out)
{
    for (const Device& d : devices())
        if (d.id == id || d.ip == id) {
            out = d;
            return true;
        }
    return false;
}

// Add or update one entry (merge_device's rules), and write back a list that sanitize() has
// cleaned: an old list that picked up the cloud broker as an address, or two entries for one
// printer, heals on the next write.
static void store_device(const Device& d)
{
    if (d.ip.empty())
        return;
    std::lock_guard<std::mutex> lock(s_store_mutex);
    json                        store = load_store();
    const std::vector<Device>   raw   = stored_devices(store);
    std::vector<Device>         list  = sanitize(raw);
    const bool                  took  = merge_device(list, d);
    if (!took)
        BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] not using " << d.ip << " as the address of " << (d.name.empty() ? d.id : d.name)
                                << " (" << d.added_by << "): it is not a LAN address";
    json next = json::array();
    for (const Device& e : list) next.push_back(json_of(e));
    // Every poll re-merges the same printers; only a real change is written, which also keeps the
    // windows that share this file from overwriting each other's writes for nothing.
    json prev = json::array();
    for (const Device& e : raw) prev.push_back(json_of(e));
    if (next == prev)
        return;
    store["devices"] = next;
    save_store(store);
}

bool remove(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_store_mutex);
    json                        store = load_store();
    if (!store.contains("devices") || !store["devices"].is_array())
        return false;
    json kept  = json::array();
    bool found = false;
    for (const json& e : store["devices"]) {
        if (str_of(e, "id") == id || str_of(e, "ip") == id) {
            found = true;
            continue;
        }
        kept.push_back(e);
    }
    if (!found)
        return false;
    store["devices"] = kept;
    save_store(store);
    return true;
}

// ----------------------------------------------------------- identifying ----

// Ask a printer who it is. /machine/system_info carries the model, the serial number and the name
// the person gave it; /printer/info is the fallback for a Moonraker that has neither.
static bool identify(const std::string& ip, int port, Device& out, std::string& error)
{
    Device d;
    d.ip   = ip;
    d.port = port > 0 ? port : 80;
    json j;
    if (!get_json(base_url(d) + "/machine/system_info", j, error, 5) || !j.contains("result")) {
        json        info;
        std::string ignore;
        if (!get_json(base_url(d) + "/printer/info", info, ignore, 5) || !info.contains("result"))
            return false; // `error` says why
        d.name  = str_of(info["result"], "hostname", ip);
        d.model = "Snapmaker";
        d.id    = ip;
    } else {
        const json& sys = j["result"].contains("system_info") ? j["result"]["system_info"] : j["result"];
        const json& p   = sys.contains("product_info") ? sys["product_info"] : sys;
        d.model         = str_of(p, "machine_type", "Snapmaker");
        d.id            = str_of(p, "serial_number");
        d.name          = str_of(p, "device_name");
        if (d.name.empty()) d.name = str_of(p, "hostname", ip);
        if (d.id.empty()) d.id = ip;
    }
    if (d.name.empty()) d.name = ip;
    out = d;
    return true;
}

bool identify_at(const std::string& ip, int port, Device& out, std::string& error) { return identify(ip, port, out, error); }

bool add(const std::string& ip_in, int port, Device& out, std::string& error)
{
    std::string ip = ip_in;
    boost::trim(ip);
    if (ip.empty()) {
        error = "an address is required";
        return false;
    }
    // "10.0.0.5:7125" is accepted as well as a plain address.
    const size_t colon = ip.rfind(':');
    if (colon != std::string::npos && ip.find(':') == colon) {
        try {
            port = std::stoi(ip.substr(colon + 1));
            ip   = ip.substr(0, colon);
        } catch (...) {}
    }
    if (ip.find('/') != std::string::npos || ip.find(' ') != std::string::npos) {
        error = "that does not look like an address: " + ip_in;
        return false;
    }
    if (is_cloud_host(ip)) {
        error = ip + " is the Snapmaker cloud, not the printer: type the printer's own LAN address (the printer's screen shows it)";
        return false;
    }
    if (!identify(ip, port, out, error)) {
        const std::string why = error;
        error = "no Snapmaker answered at " + ip + (why.empty() ? "" : " (" + why + ")");
        return false;
    }
    out.added_by = "manual";
    store_device(out);
    return true;
}

void merge_app_devices()
{
    if (!wxGetApp().app_config)
        return;
    for (const DeviceInfo& d : wxGetApp().app_config->get_devices()) {
        // A printer the Device tab reaches through the Snapmaker cloud carries the cloud's MQTT
        // broker in `ip` (a1pr8yczi3n0se.iot.us-west-1.amazonaws.com). That is not where the printer
        // is, and writing it over the LAN address mDNS / the cameras found turned the card into
        // "offline · <broker>" (2026-09-25). Only a local address from this source is used.
        if (d.ip.empty() || !is_local_address(d.ip))
            continue;
        Device dev;
        dev.ip       = d.ip;
        dev.port     = 80; // the Device tab's port is the MQTT one; Moonraker's HTTP is on 80
        dev.id       = d.sn.empty() ? (d.dev_id.empty() ? d.ip : d.dev_id) : d.sn;
        dev.name     = d.dev_name.empty() ? d.ip : d.dev_name;
        dev.model    = d.model_name;
        dev.added_by = "device_tab";
        store_device(dev);
    }
}

// ------------------------------------------------------------- discovery ----

static std::mutex   s_discovery_mutex;
static long long    s_last_discovery = 0;
static Bonjour::Ptr s_lookup; // kept alive while it runs

void start_discovery()
{
    std::lock_guard<std::mutex> lock(s_discovery_mutex);
    if (s_last_discovery != 0 && now_ms() - s_last_discovery < 60000)
        return;
    s_last_discovery = now_ms();
    // The service and the keys the Device page's own search uses (SSWCP.cpp:1790).
    Bonjour::TxtKeys keys = { "sn", "version", "machine_type", "link_mode", "device_name", "ip" };
    s_lookup              = Bonjour("snapmaker")
                   .set_txt_keys(std::move(keys))
                   .set_retries(2)
                   .set_timeout(6)
                   .on_reply([](BonjourReply&& reply) {
                       if (!reply.ip.is_v4())
                           return;
                       Device d;
                       d.ip     = reply.ip.to_string();
                       d.port   = 80; // the SRV port is the printer's own app port, not Moonraker's
                       auto txt = [&reply](const char* k) {
                           auto it = reply.txt_data.find(k);
                           return it == reply.txt_data.end() ? std::string() : it->second;
                       };
                       d.id    = txt("sn");
                       d.model = txt("machine_type");
                       d.name  = txt("device_name");
                       if (d.name.empty()) {
                           d.name         = reply.hostname;
                           const size_t p = d.name.find(".local");
                           if (p != std::string::npos)
                               d.name = d.name.substr(0, p);
                       }
                       if (d.name.empty()) d.name = reply.service_name;
                       if (d.id.empty()) d.id = d.ip;
                       d.added_by = "discovery";
                       BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] discovered " << d.name << " at " << d.ip;
                       store_device(d);
                   })
                   .on_complete([]() { BOOST_LOG_TRIVIAL(debug) << "[SnapmakerLan] discovery pass done"; })
                   .lookup();
}

// The Stream tab's camera wall is the fourth source: the hub keeps it in
// <datadir>/hub/streams.json and a U1's camera URL points at the printer itself, so a host that
// answers as a Snapmaker is one. Bambu cameras (an access code, an rtsps source) are left alone -
// those printers arrive through the device manager.
static std::mutex s_streams_mutex;
static long long  s_streams_seen = 0; // last write time of streams.json we looked at

void merge_stream_devices()
{
    std::string path = (fs::path(RemoteHub::hub_dir()) / "streams.json").string();
    boost::system::error_code ec;
    const std::time_t         when = fs::last_write_time(path, ec);
    if (ec)
        return;
    {
        std::lock_guard<std::mutex> lock(s_streams_mutex);
        if ((long long) when == s_streams_seen)
            return; // nothing new since the last look
        s_streams_seen = (long long) when;
    }
    json state;
    try {
        boost::nowide::ifstream in(path);
        if (!in.good())
            return;
        in >> state;
    } catch (...) {
        return;
    }
    if (!state.contains("hosts") || !state["hosts"].is_array())
        return;
    const std::vector<Device> known = devices();
    int probes = 0;
    for (const json& h : state["hosts"]) {
        if (!h.is_object() || ++probes > 12) // a camera wall is short; do not walk a long one twice
            continue;
        const std::string ip    = str_of(h, "ip");
        const std::string kind  = str_of(h, "kind");
        const std::string alias = str_of(h, "alias");
        if (ip.empty() || !h.value("code", std::string()).empty() || kind == "rtsps")
            continue; // a Bambu camera: that printer is the device manager's
        if (std::any_of(known.begin(), known.end(), [&ip](const Device& d) { return d.ip == ip; }))
            continue;
        Device      d;
        std::string error;
        if (!identify(ip, 80, d, error) || !boost::icontains(d.model, "Snapmaker"))
            continue;
        if (!alias.empty() && (d.name.empty() || d.name == ip))
            d.name = alias;
        d.added_by = "streams";
        BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] the camera at " << ip << " is a " << d.model;
        store_device(d);
    }
}

// ------------------------------------------------------------ live state ----

bool Presence::observe(bool answered, long long now)
{
    const bool was = online;
    if (answered) {
        fails      = 0;
        last_ok_ms = now;
        online     = true; // online on the first answer
    } else {
        ++fails;
        // Offline after FAILS_TO_OFFLINE misses in a row, or a minute of silence, or at once for a
        // printer that has never answered (there is no last reading to keep showing).
        if (last_ok_ms < 0 || fails >= FAILS_TO_OFFLINE || now - last_ok_ms >= SILENCE_MS)
            online = false;
    }
    return online != was;
}

// One slot per printer AND address: a printer that moved gets a fresh slot rather than the old
// address's history, and two list entries can never read each other's answer. (They could: the
// cloud broker and the LAN address of one U1 were stored under the same serial number, the two
// cards shared one slot, and the broker's failed probe was served as the LAN card's state - U2
// "offline · 10.0.0.106" while it answered on that address, 2026-09-25.)
struct Cached
{
    Status                st;          // what the cards show: the last good reading while online
    std::vector<Toolhead> heads;
    long long             when { 0 };  // when the printer was last asked
    Presence              presence;
    long long             access_checked { 0 }; // /access/info is asked once every few minutes
    std::string           last_error;
};
static std::mutex                    s_status_mutex;
static std::map<std::string, Cached> s_status;
static const long long               ACCESS_RECHECK_MS = 300000;

static std::string slot_of(const Device& d) { return d.id + "|" + base_url(d); }

// "FEE5A5FF" (RRGGBBAA) or an ARGB integer -> "#RRGGBB".
static std::string color_of(const json& cfg, size_t i)
{
    if (cfg.contains("filament_color_rgba") && cfg["filament_color_rgba"].is_array() && i < cfg["filament_color_rgba"].size() &&
        cfg["filament_color_rgba"][i].is_string()) {
        const std::string rgba = cfg["filament_color_rgba"][i].get<std::string>();
        if (rgba.size() >= 6)
            return "#" + rgba.substr(0, 6);
    }
    if (cfg.contains("filament_color") && cfg["filament_color"].is_array() && i < cfg["filament_color"].size() &&
        cfg["filament_color"][i].is_number()) {
        const unsigned argb = (unsigned) cfg["filament_color"][i].get<long long>();
        char            buf[8];
        std::snprintf(buf, sizeof buf, "#%06X", argb & 0xFFFFFFu);
        return buf;
    }
    return "";
}

static std::string arr_str(const json& cfg, const char* key, size_t i)
{
    return cfg.contains(key) && cfg[key].is_array() && i < cfg[key].size() && cfg[key][i].is_string()
               ? cfg[key][i].get<std::string>()
               : std::string();
}

static bool arr_bool(const json& cfg, const char* key, size_t i)
{
    return cfg.contains(key) && cfg[key].is_array() && i < cfg[key].size() && cfg[key][i].is_boolean() &&
           cfg[key][i].get<bool>();
}

// print_task_config's per-toolhead arrays are what the desktop's own update_filament_info reads
// (SSWCP.cpp:1485); the nozzle comes from extruder / extruder1 / ... next to them.
static std::vector<Toolhead> toolheads_of(const json& status_obj)
{
    std::vector<Toolhead> out;
    if (!status_obj.contains("print_task_config") || !status_obj["print_task_config"].is_object())
        return out;
    const json& cfg   = status_obj["print_task_config"];
    size_t      count = 0;
    for (const char* key : { "filament_type", "filament_color_rgba", "filament_exist" })
        if (cfg.contains(key) && cfg[key].is_array())
            count = std::max(count, cfg[key].size());
    for (size_t i = 0; i < count; ++i) {
        Toolhead t;
        t.index    = (int) i;
        t.type     = arr_str(cfg, "filament_type", i);
        t.sub_type = arr_str(cfg, "filament_sub_type", i);
        t.vendor   = arr_str(cfg, "filament_vendor", i);
        t.color    = color_of(cfg, i);
        t.loaded   = arr_bool(cfg, "filament_exist", i);
        t.official = arr_bool(cfg, "filament_official", i);
        const std::string ex = i == 0 ? "extruder" : ("extruder" + std::to_string(i));
        if (status_obj.contains(ex) && status_obj[ex].is_object())
            t.nozzle = num_of(status_obj[ex], "nozzle_diameter");
        out.push_back(t);
    }
    return out;
}

std::vector<std::pair<double, double>> nozzle_temps_of(const json& status_obj)
{
    std::vector<std::pair<double, double>> out;
    for (int i = 0; i < TOOLHEAD_COUNT; ++i) {
        const std::string ex = i == 0 ? "extruder" : ("extruder" + std::to_string(i));
        if (!status_obj.contains(ex) || !status_obj[ex].is_object())
            break;
        out.emplace_back(num_of(status_obj[ex], "temperature"), num_of(status_obj[ex], "target"));
    }
    return out;
}

// What one round of requests to a printer found. `queried` says whether the status query answered
// too (a printer can answer /server/info and then time out on the bigger query while it is busy).
struct Probe
{
    Status                st;
    std::vector<Toolhead> heads;
    bool                  queried { false };
    bool                  access_asked { false };
    std::string           error;
};

// Moonraker's two lightweight reads, each with a short timeout of its own (2 s to connect, 3 s in
// all): /server/info says the printer is there, /printer/objects/query what it is doing.
// /access/info only changes when somebody turns on the printer's login, so it is asked on the first
// probe and then every few minutes rather than on every one.
static Probe probe(const Device& d, bool ask_access)
{
    Probe       r;
    Status&     s = r.st;
    json        info;
    if (!get_json(base_url(d) + "/server/info", info, r.error, 3, 2) || !info.contains("result")) {
        if (r.error.empty()) r.error = "not a Moonraker answer";
        return r; // no answer
    }
    s.online = true;
    s.age_ms = 0;
    s.klippy = str_of(info["result"], "klippy_state");
    if (ask_access) {
        json        access;
        std::string ignore;
        r.access_asked = true;
        if (get_json(base_url(d) + "/access/info", access, ignore, 3, 2) && access.contains("result"))
            s.login_required = access["result"].contains("login_required") && access["result"]["login_required"].is_boolean() &&
                               access["result"]["login_required"].get<bool>();
        // Whether this printer has the commands the phone's load / unload would send. Asked here,
        // with the login check (the first probe, then every few minutes), because the list only
        // changes with a firmware update.
        json help;
        if (get_json(base_url(d) + "/printer/gcode/help", help, ignore, 3, 2))
            s.filament_macros = FilamentCommands::u1_filament_macros_available(help);
    }
    json        q;
    std::string qerror;
    if (get_json(base_url(d) +
                     "/printer/objects/query?print_stats&display_status&heater_bed&extruder&extruder1&extruder2&extruder3&"
                     "print_task_config&" + DeviceControls::moonraker_controls_query(),
                 q, qerror, 3, 2) &&
        q.contains("result") && q["result"].contains("status")) {
        r.queried      = true;
        const json& st = q["result"]["status"];
        r.heads        = toolheads_of(st);
        if (st.contains("print_stats")) {
            const json& ps   = st["print_stats"];
            s.state          = str_of(ps, "state");
            s.filename       = str_of(ps, "filename");
            s.message        = str_of(ps, "message");
            s.print_duration = num_of(ps, "print_duration");
            s.total_duration = num_of(ps, "total_duration");
            if (ps.contains("info") && ps["info"].is_object()) {
                s.layer        = (int) num_of(ps["info"], "current_layer");
                s.total_layers = (int) num_of(ps["info"], "total_layer");
            }
        }
        if (st.contains("display_status"))
            s.progress = num_of(st["display_status"], "progress");
        if (st.contains("heater_bed")) {
            s.bed_temp   = num_of(st["heater_bed"], "temperature");
            s.bed_target = num_of(st["heater_bed"], "target");
        }
        if (st.contains("extruder")) {
            s.nozzle_temp   = num_of(st["extruder"], "temperature");
            s.nozzle_target = num_of(st["extruder"], "target");
        }
        s.nozzles = nozzle_temps_of(st);
        // The U1's heaters, speed factor, cavity light and fans, for the phone's printer screen.
        // Limits: the U1's own (bed 100 C, toolheads 300 C); Klipper refuses anything past its
        // configured max_temp in any case, and says so in the command's answer.
        s.caps = DeviceControls::moonraker_caps(st, 100, 300, TOOLHEAD_COUNT);
    }
    return r;
}

// Slots whose probe is in flight right now: a second caller with a stale answer in hand takes that
// answer instead of queueing another probe behind the first (the phone's Devices page, the hub's
// poll and the event watcher all ask from their own threads).
static std::set<std::string> s_probing;

// The slot's reading as a caller sees it: its age filled in.
static Cached served(const Cached& c, long long now)
{
    Cached out = c;
    if (out.st.online && out.presence.last_ok_ms >= 0) out.st.age_ms = now - out.presence.last_ok_ms;
    return out;
}

static std::string who(const Device& d) { return (d.name.empty() ? std::string("printer") : d.name) + " at " + base_url(d); }

// Fold one probe into the slot: Presence decides online/offline, and while a printer is still
// counted online after a miss its last good reading is what the cards keep showing.
static void apply(Cached& c, const Device& d, Probe&& p, long long now)
{
    c.when = now;
    if (p.access_asked) c.access_checked = now;
    const bool login_required  = p.access_asked ? p.st.login_required : c.st.login_required;
    const bool filament_macros = p.access_asked ? p.st.filament_macros : c.st.filament_macros;
    const bool changed        = c.presence.observe(p.st.online, now);
    if (p.st.online) {
        if (!p.queried && c.st.online) {
            // Answered, but the status query timed out: keep the last state and temperatures rather
            // than blanking a printing card for one slow reply.
            Status keep  = c.st;
            keep.klippy  = p.st.klippy;
            keep.age_ms  = 0;
            p.st         = keep;
            p.heads      = c.heads;
        }
        p.st.login_required  = login_required;
        p.st.filament_macros = filament_macros;
        p.st.caps.filament_actions = filament_macros;
        p.st.caps.filament_assumed = filament_macros;
        c.st                = std::move(p.st);
        c.heads             = std::move(p.heads);
        c.last_error.clear();
    } else {
        c.last_error = p.error;
        if (!c.presence.online) {
            c.st    = Status(); // offline: nothing of the last reading is current any more
            c.heads.clear();
        } // else: still online on its last reading, which served() ages
    }
    if (changed) {
        if (c.presence.online)
            BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] " << who(d) << " is online";
        else
            BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] " << who(d) << " is offline after " << c.presence.fails
                                    << " probe(s) without an answer"
                                    << (c.presence.last_ok_ms >= 0 ? " (last answer " + std::to_string((now - c.presence.last_ok_ms) / 1000) + " s ago)" : std::string())
                                    << ": " << c.last_error;
    } else if (!p.st.online && c.presence.online) {
        BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] " << who(d) << " did not answer (" << c.presence.fails << " in a row, still shown online): "
                                << c.last_error;
    }
}

static Cached probe_cached(const Device& d, bool fresh = false)
{
    const std::string slot = slot_of(d);
    bool              ask_access;
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        auto                        it  = s_status.find(slot);
        const long long             now = now_ms();
        if (it != s_status.end()) {
            const long long age = now - it->second.when;
            if (!fresh && age < it->second.presence.refresh_after_ms())
                return served(it->second, now); // fresh enough (or offline and not due for a retry)
            if (!fresh && s_probing.count(slot))
                return served(it->second, now); // somebody is already asking; never queue behind them
        }
        ask_access = it == s_status.end() || now - it->second.access_checked >= ACCESS_RECHECK_MS;
        s_probing.insert(slot);
    }
    Probe           p;
    const long long started = now_ms();
    try {
        p = probe(d, ask_access);
    } catch (...) {
        p = Probe();
        p.error = "the probe failed";
    }
    const long long now = now_ms();
    if (now - started > 1000)
        BOOST_LOG_TRIVIAL(debug) << "[SnapmakerLan] probing " << who(d) << " took " << (now - started) << " ms"
                                 << (p.st.online ? "" : " (no answer)");
    std::lock_guard<std::mutex> lock(s_status_mutex);
    Cached& c = s_status[slot];
    apply(c, d, std::move(p), now);
    s_probing.erase(slot);
    return served(c, now);
}

Status status(const Device& d) { return probe_cached(d).st; }

Status status_now(const Device& d) { return probe_cached(d, true).st; }

bool cached_status(const Device& d, Status& out)
{
    std::lock_guard<std::mutex> lock(s_status_mutex);
    auto                        it = s_status.find(slot_of(d));
    if (it == s_status.end()) return false;
    out = served(it->second, now_ms()).st;
    return true;
}

// The slot as it stands, without asking the printer (a default, offline Cached when never probed).
static Cached peek(const Device& d)
{
    std::lock_guard<std::mutex> lock(s_status_mutex);
    auto                        it = s_status.find(slot_of(d));
    return it == s_status.end() ? Cached() : served(it->second, now_ms());
}

// Every printer side by side, each on its own detached thread: a printer that outlasts the budget
// must not hold the caller open (a std::async future would, in its destructor); its answer lands in
// the slot, where the next call finds it.
static std::vector<Cached> probe_all(const std::vector<Device>& list, long long budget_ms)
{
    struct Probes
    {
        std::mutex              m;
        std::condition_variable cv;
        std::vector<Cached>     got;
        std::vector<bool>       done;
        size_t                  left { 0 };
    };
    auto probes = std::make_shared<Probes>();
    probes->got.resize(list.size());
    probes->done.assign(list.size(), false);
    probes->left = list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const Device d = list[i];
        std::thread([probes, d, i]() {
            Cached c;
            try {
                c = probe_cached(d);
            } catch (...) {}
            std::lock_guard<std::mutex> lock(probes->m);
            probes->got[i]  = std::move(c);
            probes->done[i] = true;
            if (probes->left > 0) --probes->left;
            probes->cv.notify_all();
        }).detach();
    }
    std::vector<Cached> out(list.size());
    std::vector<bool>   done;
    {
        std::unique_lock<std::mutex> lock(probes->m);
        probes->cv.wait_for(lock, std::chrono::milliseconds(budget_ms), [&probes]() { return probes->left == 0; });
        out  = probes->got;
        done = probes->done;
    }
    for (size_t i = 0; i < list.size(); ++i)
        if (!done[i]) out[i] = peek(list[i]); // still being asked: its last reading, if any
    return out;
}

// What a list request waits for all the printers together: one probe's worst case is about 6 s
// (two 3 s requests), a printer on the LAN answers in well under one, and the rest comes from the
// slot on the next poll.
static const long long LIST_BUDGET_MS = 3500;

std::vector<Status> status_all(const std::vector<Device>& list, long long budget_ms)
{
    std::vector<Status> out;
    for (Cached& c : probe_all(list, budget_ms)) out.push_back(c.st);
    return out;
}

std::vector<Toolhead> toolheads(const Device& d) { return probe_cached(d).heads; }

static json toolheads_json(const std::vector<Toolhead>& heads)
{
    json out = json::array();
    for (const Toolhead& t : heads) {
        json j;
        j["index"]    = t.index;
        j["type"]     = t.type;
        j["sub_type"] = t.sub_type;
        j["vendor"]   = t.vendor;
        j["color"]    = t.color;
        j["loaded"]   = t.loaded;
        j["official"] = t.official;
        j["nozzle"]   = t.nozzle;
        out.push_back(j);
    }
    return out;
}

// The printer reports elapsed time and progress; the desktop's "time left" is the rest.
static int left_time_s(const Status& s)
{
    return (s.progress > 0.01 && s.print_duration > 0) ? (int) (s.print_duration / s.progress - s.print_duration) : 0;
}

static json status_json(const Device& d, const Status& s)
{
    json j;
    j["id"]             = d.id;
    j["name"]           = d.name;
    j["model"]          = d.model;
    j["ip"]             = d.ip;
    j["port"]           = d.port;
    j["added_by"]       = d.added_by;
    j["online"]         = s.online;
    j["login_required"] = s.login_required;
    j["state"]          = s.state;
    j["printing"]       = s.printing();
    j["task"]           = s.filename;
    j["message"]        = s.message;
    j["percent"]        = (int) (s.progress * 100 + 0.5);
    j["layer"]          = s.layer;
    j["total_layers"]   = s.total_layers;
    j["bed_temp"]       = s.bed_temp;
    j["bed_target"]     = s.bed_target;
    j["nozzle_temp"]    = s.nozzle_temp;
    j["nozzle_target"]  = s.nozzle_target;
    j["left_time_s"]    = left_time_s(s);
    return j;
}

void list_json(json& out)
{
    const std::vector<Device> list = devices();
    // A printer that is off must not hold up the ones that are: probe them side by side.
    const std::vector<Cached> got  = probe_all(list, LIST_BUDGET_MS);
    out["devices"] = json::array();
    for (size_t i = 0; i < list.size(); ++i) {
        json j         = status_json(list[i], got[i].st);
        j["toolheads"] = toolheads_json(got[i].heads);
        out["devices"].push_back(j);
    }
}

void list_printers(json& printers)
{
    // Side by side, like list_json: asked one after the other, every printer that was slow or off
    // added its whole timeout to this request (and the hub's poll gives /api/printers 10 s).
    const std::vector<Device> list = devices();
    const std::vector<Cached> got  = probe_all(list, LIST_BUDGET_MS);
    for (size_t i = 0; i < list.size(); ++i) {
        const Device& d = list[i];
        const Cached& c = got[i];
        const Status& s = c.st;
        json         p;
        p["id"]             = "sm:" + d.id;
        p["kind"]           = "snapmaker";
        p["name"]           = d.name.empty() ? d.ip : d.name;
        p["model"]          = d.model;
        p["url"]            = base_url(d);
        // The address and where the printer came from: the Devices tab shows one card per printer,
        // built from this list, and offers Remove on a printer somebody typed in (added_by manual).
        p["ip"]             = d.ip;
        p["port"]           = d.port;
        p["added_by"]       = d.added_by;
        p["via"]            = "lan";
        p["online"]         = s.online;
        // Seconds since the printer last answered (-1: never). Non-zero on an online card while it
        // rides out a missed probe or two (Presence).
        p["seen_s"]         = s.age_ms < 0 ? -1LL : s.age_ms / 1000;
        p["printing"]       = s.printing();
        p["status"]         = s.state;
        p["percent"]        = (int) (s.progress * 100 + 0.5);
        p["layer"]          = s.layer;
        p["total_layers"]   = s.total_layers;
        p["left_time_s"]    = left_time_s(s);
        p["task"]           = s.filename;
        p["login_required"] = s.login_required;
        p["can_upload"]     = s.online && !s.login_required;
        p["can_print"]      = s.online && !s.login_required;
        p["bed_temp"]       = s.bed_temp;
        p["bed_target"]     = s.bed_target;
        // Every toolhead the probe saw (the U1 has four); the phone's card labels them nozzle 1..n.
        p["nozzles"] = json::array();
        for (const auto& n : s.nozzles)
            p["nozzles"].push_back(json { { "temp", n.first }, { "target", n.second } });
        if (p["nozzles"].empty())
            p["nozzles"].push_back(json { { "temp", s.nozzle_temp }, { "target", s.nozzle_target } });
        p["toolheads"]      = toolheads_json(c.heads);
        // Load / unload per toolhead, only on a U1 whose G-code help lists the commands.
        if (s.online && s.filament_macros)
            for (size_t k = 0; k < c.heads.size() && k < p["toolheads"].size(); ++k) {
                FilamentCommands::PrinterState ps;
                ps.printing = s.printing();
                FilamentCommands::write_availability(
                    FilamentCommands::availability(ps, true, c.heads[k].loaded, FilamentCommands::is_flexible(c.heads[k].type)),
                    p["toolheads"][k]);
            }
        // What the phone's native printer screen may set on it (heaters with limits, speed factor,
        // cavity light, fans), only while it answers: an offline card offers nothing.
        if (s.online && !s.caps.heaters.empty()) p["controls"] = DeviceControls::to_json(s.caps);
        // The predicates RemoteControl derives for any Moonraker printer, so the phone's Pause /
        // Resume / Stop buttons work on a printer found over the LAN too. `task` already names the
        // job, so no `stage`; the printer's message is an error only when its state says so.
        p["print_status"]   = s.state;
        p["can_pause"]      = s.online && s.state == "printing";
        p["can_resume"]     = s.online && s.state == "paused";
        p["can_stop"]       = s.online && (s.state == "printing" || s.state == "paused");
        if (s.online && s.state == "error")
            p["print_error"] = json { { "code", "error" }, { "message", s.message } };
        else
            p["print_error"] = nullptr;
        printers.push_back(p);
    }
}

void prefer_lan(json& printers)
{
    if (!printers.is_array()) return;
    // Decide first, move after: the twin is looked up in the list as it came in.
    std::vector<bool> drop(printers.size(), false);
    for (size_t i = 0; i < printers.size(); ++i) {
        const json& p = printers[i];
        if (!p.is_object() || p.value("kind", std::string()) != "connect" || !p.contains("lan_id") || !p["lan_id"].is_string())
            continue;
        const std::string lan_id = p["lan_id"].get<std::string>();
        for (json& q : printers)
            if (q.is_object() && q.value("kind", std::string()) == "snapmaker" && q.value("id", std::string()) == lan_id) {
                if (q.value("online", false))
                    drop[i] = true; // the LAN card covers this printer
                else
                    q["cloud_online"] = p.value("online", false);
                break;
            }
    }
    json kept = json::array();
    for (size_t i = 0; i < printers.size(); ++i)
        if (!drop[i]) kept.push_back(std::move(printers[i]));
    printers = std::move(kept);
}

// --------------------------------------------------------------- sending ----

bool upload(const Device& d, const std::string& source_path, const std::string& filename, std::function<void(int)> progress,
            std::string& error)
{
    bool ok = false;
    // print=false always: a print that will not start must still leave a file the person can use
    // from the printer's own screen.
    Http::post(base_url(d) + "/server/files/upload").tls_policy(Http::TlsPolicy::PrintHost) // printer: keep accepting self-signed certificates
        .timeout_connect(10)
        .form_add("print", "false")
        .form_add("root", "gcodes")
        .form_add_file("file", fs::path(source_path), filename)
        .on_progress([&progress](Http::Progress prog, bool& cancel) {
            cancel = false;
            if (progress && prog.ultotal > 0)
                progress((int) std::min<size_t>(100, prog.ulnow * 100 / prog.ultotal));
        })
        .on_complete([&ok](std::string, unsigned) { ok = true; })
        .on_error([&error](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try {
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error += ": " + j["error"]["message"].get<std::string>();
            } catch (...) {
                if (!body.empty() && body.size() < 300)
                    error += ": " + body;
            }
        })
        .perform_sync();
    return ok;
}

bool metadata(const Device& d, const std::string& filename, long long& size, std::string& error)
{
    json j;
    if (!get_json(base_url(d) + "/server/files/metadata?filename=" + Http::url_encode(filename), j, error, 8))
        return false;
    if (!j.contains("result")) {
        error = "the printer does not list that file";
        return false;
    }
    size = (long long) num_of(j["result"], "size");
    return true;
}

// --------------------------------------------- the toolhead mapping ----
//
// How a print with more than one filament is started. The U1 takes the mapping as Klipper macros
// over plain HTTP, exactly as its own touchscreen and u1hub do it:
//
//   SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<the file's filament> MAP_EXTRUDER=<toolhead>
//   SET_PRINT_USED_EXTRUDERS EXTRUDERS=<every toolhead used, once each>
//   SET_PRINT_PREFERENCES BED_LEVEL=0 FLOW_CALIBRATE=0 TIME_LAPSE_CAMERA=0
//
// followed by the ordinary start (printer.print.start, i.e. POST /printer/print/start?filename=).
// That is what the fork's own Device page sends - the shipped Flutter bundle builds exactly these
// macros and then printer.print.start; its server.files.start_local_print path has the routing but
// no request builder - and what u1hub sends (it ends with SDCARD_PRINT_FILE, which is what
// /printer/print/start runs). server.files.start_local_print is in any case registered on the
// printer's websocket and MQTT transports only: over HTTP it answers "Method not found".
bool run_script(const Device& d, const std::string& script, std::string& error)
{
    bool ok = false;
    Http::post(base_url(d) + "/printer/gcode/script?script=" + Http::url_encode(script)).tls_policy(Http::TlsPolicy::PrintHost) // printer: keep accepting self-signed certificates
        .timeout_connect(5)
        .timeout_max(60) // the printer answers a script only once it has run it
        // Everything is in the query string, but the body has to be set: without it the Http
        // wrapper leaves CURLOPT_POSTFIELDS alone and curl waits for a body nobody will write.
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .on_complete([&ok](std::string, unsigned) { ok = true; })
        .on_error([&error](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try {
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error = j["error"]["message"].get<std::string>();
            } catch (...) {
                if (!body.empty() && body.size() < 300)
                    error += ": " + body;
            }
        })
        .perform_sync();
    return ok;
}

// "#RRGGBB" -> 0..255 triple. False when it is not a colour.
static bool rgb_of(const std::string& hex, int rgb[3])
{
    std::string h = hex;
    if (!h.empty() && h[0] == '#')
        h = h.substr(1);
    if (h.size() < 6)
        return false;
    for (int i = 0; i < 3; ++i) {
        try {
            rgb[i] = std::stoi(h.substr(i * 2, 2), nullptr, 16);
        } catch (...) {
            return false;
        }
    }
    return true;
}

// "Redmean" colour distance - the cheap approximation of how different two colours look, and the
// one u1hub matches with, so the phone's suggestion agrees with the tools people already use.
static double color_distance(const std::string& a, const std::string& b)
{
    int ca[3], cb[3];
    if (!rgb_of(a, ca) || !rgb_of(b, cb))
        return 1e9;
    const double rm = (ca[0] + cb[0]) / 2.0;
    const double dr = ca[0] - cb[0], dg = ca[1] - cb[1], db = ca[2] - cb[2];
    return std::sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db);
}

static std::string upper_hex(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    return out;
}

std::vector<int> auto_match(const std::vector<FileFilament>& filaments, const std::vector<Toolhead>& heads)
{
    std::vector<int>  out(filaments.size(), -1);
    std::vector<bool> taken(heads.size(), false);
    // 1. every used filament takes the nearest free loaded toolhead.
    for (size_t i = 0; i < filaments.size(); ++i) {
        if (!filaments[i].used)
            continue;
        double best = 1e9;
        int    pick = -1;
        for (size_t h = 0; h < heads.size(); ++h) {
            if (!heads[h].loaded || taken[h])
                continue;
            const double d = color_distance(filaments[i].color, heads[h].color);
            if (d < best) {
                best = d;
                pick = (int) h;
            }
        }
        if (pick >= 0) {
            out[i]      = pick;
            taken[pick] = true;
        }
    }
    // 2. a filament of exactly the same colour shares the toolhead its twin got.
    for (size_t i = 0; i < filaments.size(); ++i) {
        if (out[i] >= 0 || !filaments[i].used || filaments[i].color.empty())
            continue;
        for (size_t j = 0; j < filaments.size(); ++j)
            if (j != i && out[j] >= 0 && upper_hex(filaments[j].color) == upper_hex(filaments[i].color)) {
                out[i] = out[j];
                break;
            }
    }
    // 3. anything still unplaced goes on the first loaded toolhead, as a suggestion the person can
    //    change - never a refusal.
    int first_loaded = -1;
    for (size_t h = 0; h < heads.size(); ++h)
        if (heads[h].loaded) {
            first_loaded = (int) h;
            break;
        }
    for (size_t i = 0; i < filaments.size(); ++i)
        if (out[i] < 0 && filaments[i].used)
            out[i] = first_loaded >= 0 ? first_loaded : 0;
    return out;
}

// The macros for one mapping, without the start: mapping[i] is the toolhead that prints the file's
// filament i, -1 for a filament the file does not use.
std::string mapping_script(const std::vector<int>& mapping, bool unload_at_end)
{
    std::string      script;
    std::vector<int> used; // every toolhead once, in the order the file first uses it
    for (size_t i = 0; i < mapping.size(); ++i) {
        if (mapping[i] < 0)
            continue;
        script += "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=" + std::to_string(i) +
                  " MAP_EXTRUDER=" + std::to_string(mapping[i]) + "\n";
        if (std::find(used.begin(), used.end(), mapping[i]) == used.end())
            used.push_back(mapping[i]);
    }
    if (script.empty())
        return script;
    script += "SET_PRINT_USED_EXTRUDERS EXTRUDERS=";
    for (size_t i = 0; i < used.size(); ++i)
        script += (i ? "," : "") + std::to_string(used[i]);
    script += "\nSET_PRINT_PREFERENCES BED_LEVEL=0 FLOW_CALIBRATE=0 TIME_LAPSE_CAMERA=0";
    if (unload_at_end)
        script += " " + end_unload_parameter(used);
    return script;
}

bool start_print_mapped(const Device& d, const std::string& filename, const std::vector<int>& mapping,
                        bool unload_at_end, json& sent, std::string& error)
{
    const std::string script = mapping_script(mapping, unload_at_end);
    sent["mapping_script"]   = script;
    if (!script.empty() && !run_script(d, script, error)) {
        error = "the printer refused the toolhead mapping: " + error;
        return false;
    }
    // The start itself is the page's own printer.print.start, which is this over HTTP.
    sent["start"] = "/printer/print/start?filename=" + filename;
    if (!start_print(d, filename, error)) {
        error = "the printer refused the print start: " + error;
        return false;
    }
    return true;
}

bool file_on_printer(const Device& d, const std::string& filename, long long expected_size)
{
    if (filename.empty()) return false;
    long long   size = 0;
    std::string error;
    if (!metadata(d, filename, size, error)) return false;
    return expected_size <= 0 || size == expected_size;
}

bool start_print(const Device& d, const std::string& filename, std::string& error)
{
    bool ok = false;
    Http::post(base_url(d) + "/printer/print/start?filename=" + Http::url_encode(filename)).tls_policy(Http::TlsPolicy::PrintHost) // printer: keep accepting self-signed certificates
        .timeout_connect(5)
        .timeout_max(30)
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}")) // as above: a POST needs a body for curl to send it
        .on_complete([&ok](std::string, unsigned) { ok = true; })
        .on_error([&error](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try {
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error = j["error"]["message"].get<std::string>();
            } catch (...) {
                if (!body.empty() && body.size() < 300)
                    error += ": " + body;
            }
        })
        .perform_sync();
    return ok;
}

} // namespace SnapmakerLan
} // namespace GUI
} // namespace Slic3r
