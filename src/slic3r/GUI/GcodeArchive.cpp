#include "GcodeArchive.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "SnapmakerLan.hpp"
#include "DeviceModelCode.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>

#include <wx/app.h>

namespace Slic3r {
namespace GUI {
namespace GcodeArchive {

namespace fs = boost::filesystem;
using nlohmann::json;

// One archive per PC, but several slicer instances write into it at once. Names carry the pid and a
// counter so two instances never pick the same one, and this mutex only keeps one thread of *this*
// instance in the retention pass at a time.
static std::mutex g_mutex;

// ---------------------------------------------------------------- settings ----

bool enabled()
{
    AppConfig* cfg = wxGetApp().app_config;
    return cfg && cfg->get_bool("ultra_gcode_archive");
}

std::string dir()
{
    AppConfig*        cfg = wxGetApp().app_config;
    const std::string set = cfg ? cfg->get("ultra_gcode_archive_dir") : std::string();
    if (!set.empty())
        return set;
    return (fs::path(data_dir()) / "gcode_archive").string();
}

int max_records()
{
    AppConfig* cfg = wxGetApp().app_config;
    int        n   = 100;
    if (cfg) {
        try {
            const std::string v = cfg->get("ultra_gcode_archive_max");
            if (!v.empty()) n = std::stoi(v);
        } catch (...) { n = 100; }
    }
    return std::max(1, std::min(10000, n));
}

// ----------------------------------------------------------------- helpers ----

// From a worker thread: run fn on the GUI thread and wait for it (bounded). Already there: just run.
static bool on_main(std::function<void()> fn, int timeout_ms = 15000)
{
    if (wxIsMainThread()) {
        try { fn(); } catch (...) { return false; }
        return true;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    wxGetApp().CallAfter([done, fn]() {
        try { fn(); } catch (...) {}
        done->set_value();
    });
    return fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

// The archive's own names, which are also the ids the phone API addresses. Deliberately narrow:
// letters, digits, dot, dash and underscore only, so a name is safe as a Windows file name, as a
// URL path segment and against the hub's allow-list pattern - whatever the project was called.
// The name the printer was really given is kept in the sidecar (sent_name), not in the file name.
static std::string sanitize(std::string s, size_t max_len = 64)
{
    std::string out;
    for (char c : s) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out += keep ? c : '_';
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    if (out.size() > max_len) out.resize(max_len);
    if (out.empty()) out = "x";
    return out;
}

static std::string sha256_of(const fs::path& path)
{
    boost::nowide::ifstream f(path.string().c_str(), std::ios::binary);
    if (!f) return "";
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    std::string buf(64 * 1024, '\0');
    while (f.read(&buf[0], (std::streamsize) buf.size()) || f.gcount() > 0)
        SHA256_Update(&ctx, buf.data(), (size_t) f.gcount());
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    char two[3];
    for (unsigned char b : digest) {
        std::snprintf(two, sizeof two, "%02x", b);
        hex += two;
    }
    return hex;
}

// Temp file first, then rename: a reader never sees half a sidecar.
static bool write_atomic(const fs::path& path, const std::string& data)
{
    const fs::path tmp = path.string() + ".part";
    {
        boost::nowide::ofstream f(tmp.string().c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize) data.size());
        if (!f) return false;
    }
    boost::system::error_code ec;
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

static long long now_seconds()
{
    return (long long) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// "20260904-131502" in local time - the name sorts the folder for a person browsing it.
static std::string stamp(long long unix_seconds)
{
    const std::time_t t = (std::time_t) unix_seconds;
    std::tm           tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// ------------------------------------------------------------------- meta ----

Meta meta_for_plate(int plate, const std::string& mode)
{
    // Shared, not captured by reference: a call that times out leaves the lambda queued, and it
    // must not write into a Meta this function has already returned.
    auto out    = std::make_shared<Meta>();
    out->mode   = mode.empty() ? "upload" : mode;
    out->plate  = plate;
    on_main([out, plate]() {
        Meta&         m      = *out;
        Plater*       plater = wxGetApp().plater();
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (!plater || !bundle) return;
        m.project_title = plater->get_project_name().ToUTF8().data();
        m.project_path  = plater->get_project_filename().ToUTF8().data();
        if (auto* model = bundle->printers.get_edited_preset().config.option<ConfigOptionString>("printer_model"))
            m.printer_model = model->value;

        PartPlateList& plates = plater->get_partplate_list();
        const int      index  = (plate >= 0 && plate < plates.get_plate_count()) ? plate : plates.get_curr_plate_index();
        m.plate               = index;
        PartPlate* p          = (index >= 0 && index < plates.get_plate_count()) ? plates.get_plate(index) : nullptr;
        if (!p) return;
        m.plate_name = p->get_plate_name();

        // One local copy: full_config() returns by value, and a temporary must not be read through
        // afterwards (RemoteSend's file_filaments_of does the same).
        const DynamicPrintConfig full = bundle->full_config();
        std::vector<double>      density;
        if (auto* d = full.option<ConfigOptionFloats>("filament_density"))
            density = d->values;
        std::vector<std::string> types;
        if (auto* t = full.option<ConfigOptionStrings>("filament_type"))
            types = t->values;
        std::vector<std::string> colours;
        if (auto* c = bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
            colours = c->values;

        if (p->is_slice_result_valid() && p->get_slice_result()) {
            const auto& st = p->get_slice_result()->print_statistics;
            if (!st.modes.empty())
                m.estimated_time_s = (int) st.modes.front().time;
            for (const auto& kv : st.total_volumes_per_extruder) {
                Filament f;
                f.index        = (int) kv.first;
                f.type         = kv.first < types.size() ? types[kv.first] : "";
                f.colour       = kv.first < colours.size() ? colours[kv.first] : "";
                const double d = kv.first < density.size() ? density[kv.first] : 1.24;
                f.grams        = kv.second / 1000.0 * d;
                m.estimated_weight_g += f.grams;
                m.filaments.push_back(f);
            }
        }
        // The small plate thumbnail, when the plate already has one. Never rendered here: a send
        // must not wait on OpenGL, and a plate that was sliced from the UI has one anyway.
        if (p->thumbnail_data.is_valid()) {
            try {
                auto png = GCodeThumbnails::compress_thumbnail(p->thumbnail_data, GCodeThumbnailsFormat::PNG);
                if (png && png->data && png->size > 0)
                    m.thumbnail_png.assign(static_cast<const char*>(png->data), png->size);
            } catch (...) {}
        }
    });
    return *out;
}

std::string bambu_printer_name(const std::string& dev_id)
{
    auto name = std::make_shared<std::string>(dev_id);
    auto id   = std::make_shared<std::string>(dev_id);
    on_main([name, id]() {
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm) return;
        std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
        for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
        for (const auto& kv : all)
            if (kv.second && kv.second->dev_id == *id && !kv.second->dev_name.empty()) { *name = kv.second->dev_name; return; }
    }, 5000);
    return *name;
}

// --------------------------------------------------------------- archiving ----

static json meta_json(const Meta& m)
{
    json j;
    j["printer"] = { { "id", m.printer_id }, { "kind", m.printer_kind }, { "name", m.printer_name }, { "model", m.printer_model } };
    if (!m.printer_serial.empty()) j["printer"]["serial"] = m.printer_serial;
    j["plate"]   = m.plate;
    j["plate_name"]    = m.plate_name;
    j["project_title"] = m.project_title;
    j["project_path"]  = m.project_path;
    j["source"]        = m.source;
    j["mode"]          = m.mode;
    j["sent_name"]     = m.file_name; // the name the printer was given, before the archive tidied it
    j["filaments"]     = json::array();
    for (const Filament& f : m.filaments)
        j["filaments"].push_back({ { "index", f.index }, { "type", f.type }, { "colour", f.colour }, { "grams", f.grams } });
    if (m.estimated_time_s > 0)     j["estimated_time_s"] = m.estimated_time_s;
    if (m.estimated_weight_g > 0.0) j["estimated_weight_g"] = m.estimated_weight_g;
    if (!m.mapping.empty())         j["mapping"] = m.mapping;
    if (m.unload_at_end)            j["unload_at_end"] = true;
    if (!m.remote_path.empty())     j["remote_path"] = m.remote_path;
    j["spoolman_deduct"] = m.spoolman_deduct;
    return j;
}

static Record record_from_sidecar(const fs::path& sidecar)
{
    Record r;
    boost::nowide::ifstream f(sidecar.string().c_str(), std::ios::binary);
    if (!f) return r;
    std::stringstream ss;
    ss << f.rdbuf();
    json j;
    try { j = json::parse(ss.str()); } catch (...) { return r; }
    if (!j.is_object() || !j.contains("id")) return r;
    r.id     = j.value("id", std::string());
    r.time   = j.value("time", (long long) 0);
    r.file   = j.value("file", std::string());
    r.size   = j.value("size", (long long) 0);
    r.sha256 = j.value("sha256", std::string());
    r.json   = j;
    r.path   = (sidecar.parent_path() / r.file).string();
    const fs::path thumb = sidecar.parent_path() / (r.id + ".png");
    boost::system::error_code ec;
    r.file_present = !r.file.empty() && fs::is_regular_file(r.path, ec);
    r.has_thumbnail = fs::is_regular_file(thumb, ec);
    if (r.has_thumbnail) r.thumbnail_path = thumb.string();
    return r;
}

// Drop the oldest records until only max are left. Called with g_mutex held.
static void enforce_retention(const fs::path& root, int max)
{
    std::vector<std::pair<long long, fs::path>> sidecars;
    boost::system::error_code                   ec;
    for (fs::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        const Record r = record_from_sidecar(it->path());
        if (r.id.empty()) continue;
        sidecars.emplace_back(r.time, it->path());
    }
    if ((int) sidecars.size() <= max) return;
    std::sort(sidecars.begin(), sidecars.end(),
              [](const std::pair<long long, fs::path>& a, const std::pair<long long, fs::path>& b) {
                  return a.first != b.first ? a.first < b.first : a.second.string() < b.second.string();
              });
    const size_t drop = sidecars.size() - (size_t) max;
    for (size_t i = 0; i < drop; ++i) {
        const Record r = record_from_sidecar(sidecars[i].second);
        boost::system::error_code e;
        if (!r.file.empty()) fs::remove(root / r.file, e);
        fs::remove(root / (r.id + ".png"), e);
        fs::remove(sidecars[i].second, e);
        BOOST_LOG_TRIVIAL(info) << "GcodeArchive: retention removed " << r.id;
    }
}

Record archive(const std::string& sent_file_path, const Meta& meta)
{
    Record out;
    try {
        if (!enabled()) return out;
        boost::system::error_code ec;
        const fs::path            source(sent_file_path);
        if (sent_file_path.empty() || !fs::is_regular_file(source, ec)) {
            BOOST_LOG_TRIVIAL(warning) << "GcodeArchive: nothing to archive at " << sent_file_path;
            return out;
        }
        const fs::path root(dir());
        fs::create_directories(root, ec);
        if (!fs::is_directory(root, ec)) {
            BOOST_LOG_TRIVIAL(error) << "GcodeArchive: cannot use " << root.string();
            return out;
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        const long long             time = now_seconds();
        // <timestamp>_<printer>_<name>, plus this process's pid and a counter when that is taken:
        // several instances share one folder and must never pick the same id.
        const std::string printer = sanitize(meta.printer_name.empty() ? meta.printer_id : meta.printer_name, 40);
        std::string       name    = meta.file_name.empty() ? source.filename().string() : meta.file_name;
        name                      = sanitize(fs::path(name).filename().string(), 80);
        // The extension stays on the file; the id (and so the sidecar) drops it.
        std::string ext;
        for (const char* e : { ".gcode.3mf", ".gcode.gz", ".gcode", ".3mf", ".bgcode" })
            if (name.size() > std::strlen(e) && boost::algorithm::iends_with(name, e)) { ext = e; break; }
        if (ext.empty()) ext = source.extension().string();
        std::string stem = name.substr(0, name.size() - std::min(name.size(), ext.size()));
        if (stem.empty()) stem = "plate";

        // The name is claimed by the copy itself: fail_if_exists means the file system decides, so
        // two instances writing into one folder in the same second cannot end up on the same id.
        const std::string base = stamp(time) + "_" + printer + "_" + stem;
        std::string       id;
        fs::path          dest;
        for (int n = 0; n < 200; ++n) {
            if (n == 0) {
                id = base;
            } else {
                char suffix[32];
                std::snprintf(suffix, sizeof suffix, "-%d-%d", (int) get_current_pid(), n);
                id = base + suffix;
            }
            if (fs::exists(root / (id + ".json"), ec)) continue;
            dest = root / (id + ext);
            ec.clear();
            fs::copy_file(source, dest, fs::copy_option::fail_if_exists, ec);
            if (!ec) break;
            dest.clear();
        }
        if (dest.empty()) {
            BOOST_LOG_TRIVIAL(error) << "GcodeArchive: copying " << source.string() << " into " << root.string()
                                     << " failed: " << ec.message();
            return out;
        }

        bool has_thumb = false;
        if (!meta.thumbnail_png.empty())
            has_thumb = write_atomic(root / (id + ".png"), meta.thumbnail_png);

        json j    = meta_json(meta);
        j["id"]   = id;
        j["time"] = time;
        j["file"] = dest.filename().string();
        j["size"] = (long long) fs::file_size(dest, ec);
        j["sha256"]        = sha256_of(dest);
        j["has_thumbnail"] = has_thumb;
        if (!write_atomic(root / (id + ".json"), j.dump(2))) {
            // No sidecar means no record: do not leave an orphan file behind.
            fs::remove(dest, ec);
            fs::remove(root / (id + ".png"), ec);
            BOOST_LOG_TRIVIAL(error) << "GcodeArchive: writing the sidecar of " << id << " failed";
            return out;
        }

        out.id            = id;
        out.time          = time;
        out.file          = j["file"].get<std::string>();
        out.path          = dest.string();
        out.size          = j["size"].get<long long>();
        out.sha256        = j["sha256"].get<std::string>();
        out.has_thumbnail = has_thumb;
        if (has_thumb) out.thumbnail_path = (root / (id + ".png")).string();
        out.json = j;
        BOOST_LOG_TRIVIAL(info) << "GcodeArchive: stored " << id << " (" << out.size << " bytes) for " << meta.printer_name;

        enforce_retention(root, max_records());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "GcodeArchive: " << e.what();
        return Record();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "GcodeArchive: archiving failed";
        return Record();
    }
    return out;
}

std::vector<Record> list(const std::string& printer_id_filter)
{
    std::vector<Record> out;
    try {
        const fs::path            root(dir());
        boost::system::error_code ec;
        if (!fs::is_directory(root, ec)) return out;
        // The LAN cards, read once for the whole pass: a record's printer is named and filtered as
        // the phone sees it (normalize_printer), so a "connect" record listed under its LAN card is
        // also found under that card's id.
        const std::vector<SnapmakerLan::Device> lan = SnapmakerLan::devices();
        for (fs::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->path().extension() != ".json") continue;
            Record r = record_from_sidecar(it->path());
            if (r.id.empty()) continue;
            normalize_printer(r.json, lan);
            if (!printer_id_filter.empty()) {
                const std::string id = r.json.contains("printer") && r.json["printer"].is_object()
                                           ? r.json["printer"].value("id", std::string())
                                           : std::string();
                if (id != printer_id_filter) continue;
            }
            out.push_back(std::move(r));
        }
        std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) {
            return a.time != b.time ? a.time > b.time : a.id > b.id;
        });
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "GcodeArchive: listing failed: " << e.what();
    } catch (...) {}
    return out;
}

Record find(const std::string& id)
{
    Record out;
    // The id is a file stem this module made (sanitize()); never let a caller's string leave the
    // folder, so it is checked against that same alphabet rather than only for ".." and slashes.
    if (id.empty() || id.size() > 200 || id != sanitize(id, 200))
        return out;
    try {
        const fs::path            root(dir());
        const fs::path            sidecar = root / (id + ".json");
        boost::system::error_code ec;
        if (fs::is_regular_file(sidecar, ec)) {
            Record r = record_from_sidecar(sidecar);
            // The sidecar's own "id" is what list() hands out, so it - not the file stem - is what
            // a caller comes back with. They are the same for every record this module writes, but
            // a folder that was renamed, restored from a backup or copied from another PC can hold
            // a sidecar whose stem and "id" have drifted apart. Take it when it names itself right.
            // As list() hands it out: a reprint of a record listed under a LAN card goes to that card.
            if (r.id == id) { normalize_printer(r.json, SnapmakerLan::devices()); return r; }
            if (r.id.empty()) return Record();
        }
        // No sidecar at <id>.json, or one that calls itself something else: the id the caller has
        // came from list(), so look for the record that actually claims it rather than answering
        // "no such record" for a row the tab is displaying (that mismatch is what made delete
        // fail on the phone, with the list left untouched).
        for (const Record& r : list())
            if (r.id == id) return r;
    } catch (...) {}
    return out;
}

// The sidecar a record was actually read from - <id>.json for everything this module writes, and
// the file that names itself <id> for a folder whose stems have drifted (see find()).
static fs::path sidecar_path_of(const fs::path& root, const std::string& id)
{
    boost::system::error_code ec;
    const fs::path            direct = root / (id + ".json");
    if (fs::is_regular_file(direct, ec) && record_from_sidecar(direct).id == id) return direct;
    for (fs::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        if (record_from_sidecar(it->path()).id == id) return it->path();
    }
    return direct;
}

bool set_mode_in(const std::string& root_dir, const std::string& id, const std::string& mode, const std::string& remote_path)
{
    if (mode != "upload" && mode != "print") return false;
    if (id.empty() || id.size() > 200 || id != sanitize(id, 200)) return false;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        const fs::path root(root_dir);
        const fs::path sidecar = sidecar_path_of(root, id);
        boost::system::error_code ec;
        if (!fs::is_regular_file(sidecar, ec)) return false;
        boost::nowide::ifstream f(sidecar.string().c_str(), std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        f.close();
        json j = json::parse(ss.str());
        if (!j.is_object() || j.value("id", std::string()) != id) return false;
        j["mode"] = mode;
        if (!remote_path.empty()) j["remote_path"] = remote_path;
        return write_atomic(sidecar, j.dump(2));
    } catch (...) {
        return false;
    }
}

bool set_mode(const std::string& id, const std::string& mode, const std::string& remote_path)
{
    return set_mode_in(dir(), id, mode, remote_path);
}

bool note_reprint_in(const std::string& root_dir, const std::string& id, const nlohmann::json& entry)
{
    if (id.empty() || id.size() > 200 || id != sanitize(id, 200) || !entry.is_object()) return false;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        const fs::path root(root_dir);
        const fs::path sidecar = sidecar_path_of(root, id);
        boost::system::error_code ec;
        if (!fs::is_regular_file(sidecar, ec)) return false;
        boost::nowide::ifstream f(sidecar.string().c_str(), std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        f.close();
        json j = json::parse(ss.str());
        if (!j.is_object() || j.value("id", std::string()) != id) return false;
        json& list = j["reprints"];
        if (!list.is_array()) list = json::array();
        list.push_back(entry);
        // The newest REPRINT_HISTORY_MAX: a job reprinted every day for years must not grow its
        // sidecar without bound, and nobody reads that far back.
        while (list.size() > REPRINT_HISTORY_MAX) list.erase(list.begin());
        return write_atomic(sidecar, j.dump(2));
    } catch (...) {
        return false;
    }
}

bool note_reprint(const std::string& id, const nlohmann::json& entry)
{
    return note_reprint_in(dir(), id, entry);
}

bool remove(const std::string& id)
{
    const Record r = find(id);
    if (r.id.empty()) return false;
    try {
        const fs::path            root(dir());
        boost::system::error_code ec;
        // r.path is where record_from_sidecar() resolved the payload, so it removes the file even
        // when the sidecar sits under a different stem than the record's id.
        if (!r.path.empty()) fs::remove(fs::path(r.path), ec);
        else if (!r.file.empty()) fs::remove(root / r.file, ec);
        if (!r.thumbnail_path.empty()) fs::remove(fs::path(r.thumbnail_path), ec);
        else fs::remove(root / (r.id + ".png"), ec);
        fs::remove(sidecar_path_of(root, r.id), ec);
        BOOST_LOG_TRIVIAL(info) << "GcodeArchive: removed " << id;
        return true;
    } catch (...) {}
    return false;
}

// ---------------------------------------------------------- printer names ----

static std::string lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

static bool is_ipv4(const std::string& h)
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

// One word of a name: is it an address rather than a word?
static bool word_is_address(std::string w)
{
    // Brackets and trailing punctuation around it: "(192.168.1.5)", "host.example.com,".
    while (!w.empty() && std::string("()[]<>,;\"'").find(w.back()) != std::string::npos && w.back() != ']') w.pop_back();
    while (!w.empty() && std::string("(<,;\"'").find(w.front()) != std::string::npos) w.erase(w.begin());
    if (w.empty()) return false;
    if (w.find("://") != std::string::npos) return true;          // a URL
    if (w.front() == '[' && w.find(']') != std::string::npos) return true; // [fe80::1]:7125
    std::string host = lower_ascii(w);
    // host:port, a port being 2 to 5 digits: "a1pr8y....amazonaws.com:8883", "192.168.1.5:7125".
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && colon > 0) {
        const std::string port = host.substr(colon + 1);
        if (port.size() >= 2 && port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos) {
            const std::string h = host.substr(0, colon);
            if (h.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-_") == std::string::npos) return true;
        }
    }
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty()) return false;
    if (is_ipv4(host)) return true;
    if (host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-_") != std::string::npos) return false;
    if (host.find('.') == std::string::npos) return false; // a plain word ("H2D2", "3DPO")
    if (SnapmakerLan::is_cloud_host(host)) return true;
    // A dotted host name: two dots or more and a top-level label of letters only
    // ("reaper.tail2dff02.ts.net"), or one of the names a LAN hands out ("u1.local").
    const size_t last = host.rfind('.');
    const std::string tld = host.substr(last + 1);
    for (const char* lan : { "local", "lan", "home", "internal", "arpa" })
        if (tld == lan) return true;
    const bool letters = tld.size() >= 2 && tld.find_first_not_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos;
    return letters && std::count(host.begin(), host.end(), '.') >= 2;
}

bool name_is_address(const std::string& name)
{
    std::string word;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || std::isspace((unsigned char) name[i])) {
            if (word_is_address(word)) return true;
            word.clear();
        } else {
            word += name[i];
        }
    }
    return false;
}

static std::string trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string display_printer_name(const std::string& name_in, const std::string& model_in, const std::string& kind)
{
    const std::string name = trimmed(name_in), model = trimmed(model_in);
    // "Snapmaker" alone is a brand, not a printer; the old cloud-bound records were "Snapmaker <broker>".
    if (!name.empty() && !name_is_address(name) && lower_ascii(name) != "snapmaker") return name;
    if (!model.empty() && !name_is_address(model)) return model;
    if (kind == "connect" || kind == "snapmaker") return "Snapmaker U1";
    if (kind == "bambu") return "Bambu Lab printer";
    if (kind == "printhost") return "Print host";
    return "Printer";
}

bool normalize_printer(nlohmann::json& record, const std::vector<SnapmakerLan::Device>& lan)
{
    if (!record.is_object() || !record.contains("printer") || !record["printer"].is_object()) return false;
    const json recorded = record["printer"];
    json       p        = recorded;
    const std::string kind = p.value("kind", std::string());
    const std::string id   = p.value("id", std::string());
    if (kind == "connect" || id == "connect") {
        const std::string serial = p.contains("serial") && p["serial"].is_string() ? p["serial"].get<std::string>() : "";
        if (!serial.empty())
            for (const SnapmakerLan::Device& d : lan)
                if (!d.id.empty() && lower_ascii(d.id) == lower_ascii(serial)) {
                    p["id"]   = "sm:" + d.id;
                    p["kind"] = "snapmaker";
                    if (!d.model.empty()) p["model"] = d.model;
                    p["name"] = d.name;
                    break;
                }
    }
    const std::string name = display_printer_name(p.value("name", std::string()), p.value("model", std::string()),
                                                  p.value("kind", std::string()));
    if (p.value("name", std::string()) != name) p["name"] = name;
    if (p == recorded) return false;
    record["printer"] = p;
    if (!record.contains("printer_recorded")) record["printer_recorded"] = recorded;
    return true;
}

// ------------------------------------------------------------------ models ----

std::map<std::string, std::string> load_model_names(const std::string& printers_dir)
{
    // What resources/printers held on 2026-09-26, for a folder that cannot be read.
    std::map<std::string, std::string> names = {
        { "BL-P001", "Bambu Lab X1 Carbon" }, { "BL-P002", "Bambu Lab X1" }, { "C11", "Bambu Lab P1P" },
        { "C12", "Bambu Lab P1S" },           { "C13", "Bambu Lab X1E" },    { "N1", "Bambu Lab A1 mini" },
        { "N2S", "Bambu Lab A1" },            { "O1C2", "Bambu Lab H2C" },   { "O1D", "Bambu Lab H2D" },
        { "O1S", "Bambu Lab H2S" },
    };
    boost::system::error_code ec;
    if (printers_dir.empty() || !fs::is_directory(printers_dir, ec)) return names;
    for (fs::directory_iterator it(printers_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".json") continue;
        try {
            boost::nowide::ifstream f(p.string().c_str());
            if (!f.is_open()) continue;
            json jj;
            f >> jj;
            if (!jj.is_object() || !jj.contains("00.00.00.00") || !jj["00.00.00.00"].is_object()) continue;
            const json& printer = jj["00.00.00.00"];
            const std::string display = printer.value("display_name", std::string());
            if (display.empty()) continue;
            names[p.stem().string()] = display;
            if (printer.contains("model_id") && printer["model_id"].is_string()) names[printer["model_id"].get<std::string>()] = display;
            if (printer.contains("subseries") && printer["subseries"].is_array())
                for (const json& s : printer["subseries"])
                    if (s.is_string()) names[s.get<std::string>()] = display;
        } catch (...) {
            // filaments_blacklist.json and friends share the folder and have another shape.
        }
    }
    return names;
}

const std::map<std::string, std::string>& model_names()
{
    static const std::map<std::string, std::string> names =
        load_model_names((fs::path(Slic3r::resources_dir()) / "printers").string());
    return names;
}

std::string canonical_model(const std::string& model_in, const std::map<std::string, std::string>& names)
{
    const std::string model = trimmed(model_in);
    if (model.empty() || name_is_address(model)) return "";
    auto it = names.find(model);
    if (it == names.end()) it = names.find(strip_model_revision(model));
    return it != names.end() ? it->second : model;
}

std::string model_key(const std::string& canonical)
{
    std::string key;
    for (char c : lower_ascii(trimmed(canonical))) {
        if (std::isspace((unsigned char) c)) {
            if (!key.empty() && key.back() != ' ') key += ' ';
        } else {
            key += c;
        }
    }
    return key.empty() ? "other" : key;
}

std::string model_label(const std::string& canonical_in)
{
    const std::string canonical = trimmed(canonical_in);
    if (canonical.empty()) return "Other";
    const std::string key = model_key(canonical);
    if (key == "bambu lab x1 carbon") return "Bambu X1C";
    if (key == "elegoo centauri carbon") return "Elegoo CC";
    if (key.compare(0, 10, "bambu lab ") == 0) return "Bambu " + canonical.substr(10);
    return canonical;
}

int fill_from_siblings(std::vector<json>& records)
{
    // Per printer id: the newest model and the newest real name any of its records has.
    std::map<std::string, std::string> model_of, name_of;
    for (const json& r : records) {
        if (!r.is_object() || !r.contains("printer") || !r["printer"].is_object()) continue;
        const json&       p    = r["printer"];
        const std::string id   = p.value("id", std::string());
        const std::string name = p.value("name", std::string());
        const std::string model = p.value("model", std::string());
        if (id.empty()) continue;
        if (!model.empty() && !name_is_address(model) && !model_of.count(id)) model_of[id] = model;
        if (!name.empty() && name != id && !name_is_address(name) && !name_of.count(id)) name_of[id] = name;
    }
    int filled = 0;
    for (json& r : records) {
        if (!r.is_object() || !r.contains("printer") || !r["printer"].is_object()) continue;
        json&             p  = r["printer"];
        const std::string id = p.value("id", std::string());
        if (id.empty()) continue;
        bool changed = false;
        if (trimmed(p.value("model", std::string())).empty() && model_of.count(id)) {
            p["model"] = model_of[id];
            changed    = true;
        }
        const std::string name = p.value("name", std::string());
        if ((name.empty() || name == id) && name_of.count(id)) {
            p["name"] = name_of[id];
            changed   = true;
        }
        if (changed) ++filled;
    }
    return filled;
}

std::string reprint_target_refusal(const std::string& recorded_id, const std::string& recorded_kind,
                                   const std::string& record_model, const std::string& target_id,
                                   const std::string& target_kind, const std::string& target_model,
                                   const std::map<std::string, std::string>& names)
{
    if (target_id.empty()) return "no printer was named";
    if (!recorded_id.empty() && target_id == recorded_id) return "";
    // A U1 send filed under the Snapmaker cloud ("connect") and a U1's LAN card take the same file.
    auto family = [](const std::string& k) { return k == "connect" ? std::string("snapmaker") : k; };
    if (!recorded_kind.empty() && family(recorded_kind) != family(target_kind))
        return "this file was sent to a " + recorded_kind + " printer and " + target_id + " is a " + target_kind +
               " one; the file a printer takes differs by kind";
    const std::string rm = canonical_model(record_model, names);
    const std::string tm = canonical_model(target_model, names);
    if (rm.empty())
        return "this record does not say which printer model it was sliced for, so it can only go back to the printer it was sent to";
    if (tm.empty()) return "which model " + target_id + " is is not known, so it cannot take a file sliced for a " + rm;
    if (model_key(rm) != model_key(tm)) return "this file was sliced for a " + rm + " and " + target_id + " is a " + tm;
    return "";
}

bool reprint_in_place_allowed(const std::string& recorded_id, const std::string& target_id)
{
    return !recorded_id.empty() && recorded_id == target_id;
}

void annotate_models(std::vector<json>& records, const std::map<std::string, std::string>& names)
{
    fill_from_siblings(records);
    for (json& j : records) {
        if (!j.is_object()) continue;
        const json        p         = j.contains("printer") && j["printer"].is_object() ? j["printer"] : json::object();
        const std::string canonical = canonical_model(p.value("model", std::string()), names);
        j["model_key"]  = model_key(canonical);
        j["model_name"] = model_label(canonical);
    }
}

json archive_models(const std::vector<json>& records, const json& printer_rows, const std::map<std::string, std::string>& names)
{
    std::vector<std::string>                           order;
    std::map<std::string, std::pair<std::string, int>> seen; // key -> (label, records)
    for (const json& r : records) {
        if (!r.is_object()) continue;
        const std::string key = r.value("model_key", std::string("other"));
        auto it = seen.find(key);
        if (it == seen.end()) {
            seen[key] = { r.value("model_name", std::string("Other")), 1 };
            order.push_back(key);
        } else {
            ++it->second.second;
        }
    }
    std::stable_partition(order.begin(), order.end(), [](const std::string& k) { return k != "other"; });

    std::map<std::string, json> targets; // model key -> printers of that model
    if (printer_rows.is_array())
        for (const json& row : printer_rows) {
            if (!row.is_object()) continue;
            const std::string kind = row.value("kind", std::string());
            const std::string id   = row.value("id", std::string());
            if (id.empty() || kind == "connect") continue;
            const std::string canonical = canonical_model(row.value("model", std::string()), names);
            if (canonical.empty()) continue;
            json& list = targets[model_key(canonical)];
            if (!list.is_array()) list = json::array();
            list.push_back({ { "id", id },
                             { "name", display_printer_name(row.value("name", std::string()), canonical, kind) },
                             { "kind", kind },
                             { "online", row.value("online", false) && !row.value("stale", false) } });
        }

    json models = json::array();
    for (const std::string& key : order) {
        json m = { { "key", key }, { "name", seen[key].first }, { "count", seen[key].second } };
        m["printers"] = key != "other" && targets.count(key) ? targets[key] : json::array();
        models.push_back(m);
    }
    return models;
}

} // namespace GcodeArchive
} // namespace GUI
} // namespace Slic3r
