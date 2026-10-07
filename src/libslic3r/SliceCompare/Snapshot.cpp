#include "Snapshot.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <cmath>
#include <cctype>
#include <atomic>
#include <chrono>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace Slic3r { namespace SliceCompare {

double LayerRec::seconds() const {
    double t = 0; for (auto& kv : feature_seconds) t += kv.second; return t;
}

namespace {

std::string to_lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool ends_with_ci(const std::string& s, const std::string& suffix)
{
    const std::string ls = to_lower_copy(s);
    const std::string lsuf = to_lower_copy(suffix);
    return ls.size() >= lsuf.size() &&
           ls.compare(ls.size() - lsuf.size(), lsuf.size(), lsuf) == 0;
}

void trim(std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    const size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

// Parses a "; key = value" comment line. Returns false (and leaves key/value
// untouched) if the line isn't such a comment, the key is empty, or the key
// contains whitespace (used to reject prose comments picked up by the
// first/last-400-lines fallback scan).
bool parse_kv_comment(const std::string& line, std::string& key, std::string& value)
{
    size_t i = 0;
    while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
    if (i >= line.size() || line[i] != ';') return false;
    std::string rest = line.substr(i + 1);
    const size_t eq = rest.find('=');
    if (eq == std::string::npos) return false;
    std::string k = rest.substr(0, eq);
    std::string v = rest.substr(eq + 1);
    trim(k);
    trim(v);
    if (k.empty()) return false;
    if (k.find_first_of(" \t") != std::string::npos) return false; // skip keys with spaces
    key = std::move(k);
    value = std::move(v);
    return true;
}

std::filesystem::path make_temp_gcode_path()
{
    static std::atomic<uint64_t> s_counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "sc_plate_" << now << "_" << s_counter.fetch_add(1) << ".gcode";
    return std::filesystem::temp_directory_path() / name.str();
}

// Best-effort cleanup of a temp file on every exit path (success, early
// return on extraction failure, or exception unwinding) — extraction can
// leave a partial/empty file behind even when it reports failure, since
// mz_zip_reader_extract_to_file opens the destination before it starts
// writing.
struct TempFileGuard {
    std::filesystem::path path;
    ~TempFileGuard() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // anonymous namespace

std::map<std::string, std::string> parse_gcode_config(const std::string& gcode_path)
{
    std::map<std::string, std::string> cfg;
    std::ifstream f(gcode_path);
    if (!f.is_open())
        return cfg;

    constexpr size_t kFallbackWindow = 400;
    std::vector<std::string> first_lines;
    std::deque<std::string> last_lines;
    first_lines.reserve(kFallbackWindow);

    bool in_block = false;
    bool found_block = false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("CONFIG_BLOCK_START") != std::string::npos) {
            in_block = true;
            found_block = true;
            continue;
        }
        if (line.find("CONFIG_BLOCK_END") != std::string::npos) {
            in_block = false;
            continue;
        }
        if (in_block) {
            std::string k, v;
            if (parse_kv_comment(line, k, v))
                cfg[k] = v;
            continue;
        }
        if (!found_block) {
            // Keep a bounded window of the first/last N lines in case no
            // CONFIG_BLOCK is ever found (PrusaSlicer-style header/footer).
            if (first_lines.size() < kFallbackWindow)
                first_lines.push_back(line);
            last_lines.push_back(line);
            if (last_lines.size() > kFallbackWindow)
                last_lines.pop_front();
        }
    }

    if (found_block)
        return cfg;

    for (const auto& l : first_lines) {
        std::string k, v;
        if (parse_kv_comment(l, k, v))
            cfg[k] = v;
    }
    for (const auto& l : last_lines) {
        std::string k, v;
        if (parse_kv_comment(l, k, v))
            cfg[k] = v;
    }
    return cfg;
}

namespace {

FileLoadResult load_snapshot_from_gcode_file(const std::string& path)
{
    FileLoadResult out;
    if (!std::filesystem::exists(path)) {
        out.error = "File not found: " + path;
        return out;
    }

    GCodeProcessor processor;
    processor.process_file(path);
    const GCodeProcessorResult& result = processor.extract_result();

    std::map<std::string, std::string> config = parse_gcode_config(path);
    const std::string label = std::filesystem::path(path).stem().string();
    Snapshot snap = build_snapshot(result, std::move(config), label, path);

    if (snap.layers.empty()) {
        out.error = "No extrusion moves found in: " + path;
        return out;
    }
    out.snapshot = std::move(snap);
    return out;
}

FileLoadResult load_snapshot_from_3mf(const std::string& original_path)
{
    FileLoadResult out;

    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, original_path)) {
        out.error = "Unable to open 3mf archive: " + original_path;
        return out;
    }

    bool found = false;
    mz_uint found_index = 0;
    std::string found_name;
    const mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
    mz_zip_archive_file_stat stat;
    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            continue;
        std::string name(stat.m_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        const std::string lname = to_lower_copy(name);
        if (lname.find("metadata/plate_") != std::string::npos && ends_with_ci(lname, ".gcode")) {
            found = true;
            found_index = i;
            found_name = name;
            break;
        }
    }

    if (!found) {
        close_zip_reader(&archive);
        out.error = "No plate gcode found in 3mf: " + original_path;
        return out;
    }

    const std::filesystem::path tmp_path = make_temp_gcode_path();
    TempFileGuard tmp_guard{tmp_path}; // removes tmp_path on every exit below,
                                       // including the !extracted early return
                                       // (extraction opens the file before it
                                       // writes, so a failure can still leave
                                       // a partial/empty orphan on disk)
    const bool extracted = mz_zip_reader_extract_to_file(&archive, found_index, tmp_path.string().c_str(), 0);
    close_zip_reader(&archive);

    if (!extracted) {
        out.error = "Failed to extract " + found_name + " from " + original_path;
        return out;
    }

    FileLoadResult inner = load_snapshot_from_gcode_file(tmp_path.string());

    if (!inner.snapshot.has_value()) {
        out.error = inner.error;
        return out;
    }

    out.snapshot = std::move(inner.snapshot);
    out.snapshot->label = std::filesystem::path(original_path).stem().string();
    out.snapshot->source = original_path;
    return out;
}

} // anonymous namespace

FileLoadResult load_snapshot_from_file(const std::string& path)
{
    FileLoadResult out;
    try {
        if (ends_with_ci(path, ".3mf"))
            return load_snapshot_from_3mf(path);
        return load_snapshot_from_gcode_file(path);
    } catch (const std::exception& e) {
        out.error = std::string("Exception while loading ") + path + ": " + e.what();
    } catch (...) {
        out.error = "Unknown exception while loading " + path;
    }
    return out;
}

Snapshot build_snapshot(const GCodeProcessorResult& result,
                        std::map<std::string, std::string> config,
                        const std::string& label, const std::string& source)
{
    Snapshot s;
    s.label = label; s.source = source; s.config = std::move(config);
    s.est_seconds = result.print_statistics.modes[
        (size_t)PrintEstimatedStatistics::ETimeMode::Normal].time;

    const float diam = result.filament_diameters.empty() ? 1.75f : result.filament_diameters[0];
    const float dens = result.filament_densities.empty() ? 1.24f : result.filament_densities[0];
    // grams per mm of filament = area(cm^2) * 0.1cm * density(g/cm^3)
    const double g_per_mm = M_PI * (diam / 20.0) * (diam / 20.0) * 0.1 * dens;

    const GCodeProcessorResult::MoveVertex* prev = nullptr;
    for (const auto& m : result.moves) {
        // EDGE (libvgcode stage 3): actual-speed profile points split a move for the preview only;
        // their extruder delta is interpolated, not a share of the move's.
        if (m.actual_speed_point)
            continue;
        if (m.feedrate > s.max_speed) s.max_speed = m.feedrate;
        if (m.type == EMoveType::Extrude && m.delta_extruder > 0.f && prev) {
            const int zkey = (int)std::lround(m.position.z() * 100.0);
            LayerRec& l = s.layers[zkey];
            if (l.segs.empty() && l.extrusion_mm == 0.0) l.z = m.position.z();
            const float x0 = prev->position.x(), y0 = prev->position.y();
            const float x1 = m.position.x(),    y1 = m.position.y();
            l.segs.push_back({x0, y0, x1, y1, (uint8_t)m.extrusion_role});
            l.extrusion_mm += m.delta_extruder;
            l.feature_extrusion_mm[(uint8_t)m.extrusion_role] += m.delta_extruder;
            const double len = std::hypot((double)x1 - x0, (double)y1 - y0);
            if (m.feedrate > 0.f)
                l.feature_seconds[(uint8_t)m.extrusion_role] += len / m.feedrate;
            l.cells.insert({(int16_t)std::floor(x1 / 10.f), (int16_t)std::floor(y1 / 10.f)});
            l.cells.insert({(int16_t)std::floor(x0 / 10.f), (int16_t)std::floor(y0 / 10.f)});
            if (!l.has_bbox) { l.bx0 = l.bx1 = x1; l.by0 = l.by1 = y1; l.has_bbox = true; }
            l.bx0 = std::min({l.bx0, x0, x1}); l.by0 = std::min({l.by0, y0, y1});
            l.bx1 = std::max({l.bx1, x0, x1}); l.by1 = std::max({l.by1, y0, y1});
            s.filament_mm += m.delta_extruder;
        }
        prev = &m;
    }
    s.filament_g = s.filament_mm * g_per_mm;
    s.layer_count = (int)s.layers.size();
    return s;
}

SnapshotStore& SnapshotStore::instance()
{
    static SnapshotStore s;
    return s;
}

int SnapshotStore::add(Snapshot snap)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const int id = m_next_id++;
    m_items.push_front({id, std::make_shared<Snapshot>(std::move(snap))});
    while (m_items.size() > 8)
        m_items.pop_back();
    return id;
}

std::shared_ptr<const Snapshot> SnapshotStore::get(int id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_items)
        if (item.first == id)
            return item.second;
    return nullptr;
}

std::vector<std::pair<int, std::string>> SnapshotStore::list() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<int, std::string>> out;
    out.reserve(m_items.size());
    for (const auto& item : m_items)
        out.push_back({item.first, item.second->label});
    return out;
}

void SnapshotStore::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
}

}} // namespaces
