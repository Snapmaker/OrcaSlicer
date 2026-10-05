#include "PlatePrintHistory.hpp"

#include "Model.hpp"
#include "TriangleMesh.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <random>

namespace Slic3r {
namespace PlateHistory {

using nlohmann::json;

// ------------------------------------------------------------------ actions ----

const char *action_key(Action a)
{
    switch (a) {
    case Action::Sent:           return "sent";
    case Action::SentAndStarted: return "sent_started";
    case Action::UploadedOnly:   return "uploaded";
    case Action::Exported:       return "exported";
    }
    return "sent";
}

Action action_from_key(const std::string &key)
{
    if (key == "sent_started") return Action::SentAndStarted;
    if (key == "uploaded")     return Action::UploadedOnly;
    if (key == "exported")     return Action::Exported;
    return Action::Sent;
}

// -------------------------------------------------------------------- time ----

static std::tm utc_tm(std::time_t t)
{
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

static std::tm local_tm(std::time_t t)
{
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string now_utc_iso()
{
    const std::time_t t  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::tm     tm = utc_tm(t);
    char              buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec);
    return buf;
}

// Days since 1970-01-01 for a proleptic Gregorian date (the days_from_civil algorithm), so the
// local offset and the formatting below need no timegm / _mkgmtime.
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned  yoe = static_cast<unsigned>(y - era * 400);
    const unsigned  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

static void civil_from_days(long long z, int &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned  doe = static_cast<unsigned>(z - era * 146097);
    const unsigned  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy  = static_cast<long long>(yoe) + era * 400;
    const unsigned  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned  mp  = (5 * doy + 2) / 153;
    d                   = doy - (153 * mp + 2) / 5 + 1;
    m                   = mp < 10 ? mp + 3 : mp - 9;
    y                   = static_cast<int>(yy + (m <= 2));
}

int local_utc_offset_minutes()
{
    const std::time_t t  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::tm     l  = local_tm(t);
    const std::tm     u  = utc_tm(t);
    const long long   ls = days_from_civil(l.tm_year + 1900, static_cast<unsigned>(l.tm_mon + 1), static_cast<unsigned>(l.tm_mday)) * 86400LL +
                         l.tm_hour * 3600LL + l.tm_min * 60LL + l.tm_sec;
    const long long   us = days_from_civil(u.tm_year + 1900, static_cast<unsigned>(u.tm_mon + 1), static_cast<unsigned>(u.tm_mday)) * 86400LL +
                         u.tm_hour * 3600LL + u.tm_min * 60LL + u.tm_sec;
    return static_cast<int>((ls - us) / 60);
}

// "YYYY-MM-DDTHH:MM:SSZ" -> seconds since the epoch, or false.
static bool parse_iso_utc(const std::string &s, long long &out)
{
    int y, mo, d, h, mi, sec;
    if (s.size() != 20 || std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &sec) != 6)
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60)
        return false;
    out = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400LL + h * 3600LL + mi * 60LL + sec;
    return true;
}

std::string format_local(const Entry &e)
{
    long long secs = 0;
    if (!parse_iso_utc(e.time_utc, secs))
        return {};
    secs += static_cast<long long>(e.utc_offset_min) * 60;
    long long days = secs / 86400;
    long long rem  = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    int      y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02d:%02d", y, m, d, static_cast<int>(rem / 3600), static_cast<int>((rem % 3600) / 60));
    return buf;
}

std::string make_uid()
{
    static std::mt19937_64 rng{std::random_device{}() ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    char                   buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(rng()));
    return std::string(buf).substr(0, 12);
}

// ------------------------------------------------------------------ history ----

void History::add(Entry e)
{
    if (e.time_utc.empty()) {
        e.time_utc       = now_utc_iso();
        e.utc_offset_min = local_utc_offset_minutes();
    }
    m_entries.push_back(std::move(e));
    // Oldest first, ties keep the order they were added in.
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) { return a.time_utc < b.time_utc; });
    if (m_entries.size() > MAX_ENTRIES)
        m_entries.erase(m_entries.begin(), m_entries.begin() + static_cast<std::ptrdiff_t>(m_entries.size() - MAX_ENTRIES));
}

std::vector<Entry> History::newest_first() const
{
    return std::vector<Entry>(m_entries.rbegin(), m_entries.rend());
}

bool History::was_sent() const { return last_sent() != nullptr; }

const Entry *History::last_sent() const
{
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
        if (is_printer_action(it->action))
            return &*it;
    return nullptr;
}

bool History::modified_since_last_send(const std::string &current_fingerprint) const
{
    const Entry *last = last_sent();
    if (last == nullptr || last->input_hash.empty() || current_fingerprint.empty())
        return false;
    return last->input_hash != current_fingerprint;
}

bool History::set_action(const std::string &uid, Action action)
{
    if (uid.empty())
        return false;
    for (Entry &e : m_entries)
        if (e.uid == uid) {
            e.action = action;
            return true;
        }
    return false;
}

std::string History::serialize() const
{
    if (m_entries.empty())
        return {};
    json arr = json::array();
    for (const Entry &e : m_entries) {
        json j;
        if (!e.uid.empty())           j["id"]      = e.uid;
        j["t"]      = e.time_utc;
        j["off"]    = e.utc_offset_min;
        j["action"] = action_key(e.action);
        if (!e.printer_name.empty())  j["printer"] = e.printer_name;
        if (!e.printer_model.empty()) j["model"]   = e.printer_model;
        if (!e.connection.empty())    j["conn"]    = e.connection;
        if (!e.file_name.empty())     j["file"]    = e.file_name;
        if (e.time_estimate_s > 0)    j["est_s"]   = e.time_estimate_s;
        if (e.filament_g > 0.0)       j["fil_g"]   = e.filament_g;
        if (!e.input_hash.empty())    j["hash"]    = e.input_hash;
        arr.push_back(std::move(j));
    }
    json root;
    root["v"]       = 1;
    root["entries"] = std::move(arr);
    return root.dump();
}

static std::string str_of(const json &j, const char *key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

History History::deserialize(const std::string &text)
{
    History h;
    if (text.empty())
        return h;
    json root = json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (root.is_discarded() || !root.is_object())
        return h;
    auto entries = root.find("entries");
    if (entries == root.end() || !entries->is_array())
        return h;
    for (const json &j : *entries) {
        if (!j.is_object())
            continue;
        Entry e;
        e.uid            = str_of(j, "id");
        e.time_utc       = str_of(j, "t");
        long long unused = 0;
        if (!parse_iso_utc(e.time_utc, unused))
            continue; // an entry without a usable time cannot be placed in the list
        if (auto it = j.find("off"); it != j.end() && it->is_number_integer())
            e.utc_offset_min = std::max(-14 * 60, std::min(14 * 60, it->get<int>()));
        e.action        = action_from_key(str_of(j, "action"));
        e.printer_name  = str_of(j, "printer");
        e.printer_model = str_of(j, "model");
        e.connection    = str_of(j, "conn");
        e.file_name     = str_of(j, "file");
        e.input_hash    = str_of(j, "hash");
        if (auto it = j.find("est_s"); it != j.end() && it->is_number_integer())
            e.time_estimate_s = std::max(0, it->get<int>());
        if (auto it = j.find("fil_g"); it != j.end() && it->is_number())
            e.filament_g = std::max(0.0, it->get<double>());
        h.add(std::move(e));
    }
    return h;
}

// -------------------------------------------------------------- fingerprint ----

namespace {

struct Hasher
{
    std::uint64_t h { 1469598103934665603ULL }; // FNV-1a
    void bytes(const void *p, size_t n)
    {
        const unsigned char *c = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= c[i];
            h *= 1099511628211ULL;
        }
    }
    void i64(long long v) { bytes(&v, sizeof v); }
    void str(const std::string &s)
    {
        i64(static_cast<long long>(s.size()));
        bytes(s.data(), s.size());
    }
    // Quantised: a value that survives a text round trip through the 3MF keeps its hash.
    void q(double v, double step) { i64(static_cast<long long>(std::llround(v / step))); }
};

void hash_linear(Hasher &H, const Transform3d &m)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            H.q(m(r, c), 1e-4);
}

void hash_config(Hasher &H, const ModelConfigObject &cfg)
{
    t_config_option_keys keys = cfg.keys();
    std::sort(keys.begin(), keys.end());
    for (const std::string &k : keys) {
        const std::string value = cfg.opt_serialize(k);
        // A project reload writes extruder 0 (the default) into every object and part that had no
        // setting at all: the same plate, so it must hash the same.
        if (k == "extruder" && (value == "0" || value == "1"))
            continue;
        H.str(k);
        H.str(value);
    }
}

void hash_paint(Hasher &H, const FacetsAnnotation &f)
{
    const TriangleSelector::TriangleSplittingData &d = f.get_data();
    H.i64(static_cast<long long>(d.triangles_to_split.size()));
    H.i64(static_cast<long long>(d.bitstream.size()));
}

std::uint64_t instance_hash(const ModelObject &obj, const ModelInstance &inst, const Vec3d &plate_origin)
{
    Hasher H;
    hash_config(H, obj.config);
    H.i64(static_cast<long long>(obj.volumes.size()));
    for (const ModelVolume *v : obj.volumes) {
        if (v == nullptr)
            continue;
        H.i64(static_cast<long long>(v->type()));
        hash_config(H, v->config);
        // Where the volume really is: instance and volume transforms composed. A project reload
        // moves a volume's offset into the instance (the mesh is re-centred), so neither alone is
        // stable; the composition, and the mesh's extent after it, are.
        const Transform3d   world = inst.get_matrix() * v->get_matrix();
        const TriangleMesh &mesh  = v->mesh();
        hash_linear(H, world);
        H.i64(static_cast<long long>(mesh.its.vertices.size()));
        H.i64(static_cast<long long>(mesh.its.indices.size()));
        if (!mesh.its.vertices.empty()) {
            const BoundingBoxf3 bb = mesh.transformed_bounding_box(world);
            for (int i = 0; i < 3; ++i) {
                H.q(bb.min(i) - plate_origin(i), 1e-2);
                H.q(bb.max(i) - plate_origin(i), 1e-2);
            }
        }
        hash_paint(H, v->supported_facets);
        hash_paint(H, v->seam_facets);
        hash_paint(H, v->mmu_segmentation_facets);
        hash_paint(H, v->fuzzy_skin_facets);
    }
    return H.h;
}

} // namespace

std::string plate_input_fingerprint(const Model &model, const std::vector<std::pair<int, int>> &objects_and_instances, const Vec3d &plate_origin)
{
    std::vector<std::uint64_t> parts;
    for (const auto &oi : objects_and_instances) {
        if (oi.first < 0 || oi.first >= static_cast<int>(model.objects.size()))
            continue;
        const ModelObject *obj = model.objects[oi.first];
        if (obj == nullptr || oi.second < 0 || oi.second >= static_cast<int>(obj->instances.size()))
            continue;
        parts.push_back(instance_hash(*obj, *obj->instances[oi.second], plate_origin));
    }
    if (parts.empty())
        return {};
    // Order independent: the same objects in a different object-list order are the same plate.
    std::sort(parts.begin(), parts.end());
    Hasher H;
    for (std::uint64_t p : parts)
        H.i64(static_cast<long long>(p));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(H.h));
    return buf;
}

} // namespace PlateHistory
} // namespace Slic3r
