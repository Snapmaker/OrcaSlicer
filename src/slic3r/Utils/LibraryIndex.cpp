#include "LibraryIndex.hpp"
#include "MeshThumbnail.hpp"

#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <regex>
#include <sstream>

namespace Slic3r {
namespace Library {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

// ------------------------------------------------------------------------------ helpers ----

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static bool ends_with(const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

static std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// A folder path as it is compared and stored: generic separators, no trailing separator (except a
// bare root like "/" or "C:/").
static std::string norm(const std::string& path)
{
    std::string p = fs::path(trim(path)).generic_string();
    while (p.size() > 1 && p.back() == '/' && !(p.size() == 3 && p[1] == ':'))
        p.pop_back();
    return p;
}

// Is `dir` equal to `root` or inside it?
static bool within(const std::string& dir, const std::string& root)
{
    if (dir == root)
        return true;
    if (dir.size() <= root.size() || dir.compare(0, root.size(), root) != 0)
        return false;
    return root.back() == '/' || dir[root.size()] == '/';
}

static std::string xml_unescape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                cp = ent[1] == 'x' || ent[1] == 'X' ? std::stoul(ent.substr(2), nullptr, 16) : std::stoul(ent.substr(1));
            } catch (...) {
                out += s.substr(i, semi - i + 1);
                i = semi;
                continue;
            }
            // UTF-8 encode.
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x110000) { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

// The value of attribute `name` in one tag's text, "" when it is not there.
static std::string attribute(const std::string& tag, const std::string& name)
{
    static const std::regex attr_re(R"re(([A-Za-z_:][-A-Za-z0-9_:.]*)\s*=\s*("([^"]*)"|'([^']*)'))re");
    for (auto it = std::sregex_iterator(tag.begin(), tag.end(), attr_re); it != std::sregex_iterator(); ++it)
        if ((*it)[1].str() == name)
            return xml_unescape((*it)[3].matched ? (*it)[3].str() : (*it)[4].str());
    return std::string();
}

// ------------------------------------------------------------------------------ folders ----

std::vector<Folder> folders_from_json(const std::string& text)
{
    std::vector<Folder> out;
    json j;
    try {
        j = json::parse(text);
    } catch (...) {
        return out;
    }
    if (!j.is_array())
        return out;
    std::set<std::string> seen;
    for (const json& f : j) {
        if (!f.is_object() || !f.contains("path") || !f["path"].is_string())
            continue;
        Folder folder;
        folder.path = norm(f["path"].get<std::string>());
        if (folder.path.empty() || !seen.insert(folder.path).second)
            continue;
        if (f.contains("recursive") && f["recursive"].is_boolean())
            folder.recursive = f["recursive"].get<bool>();
        if (f.contains("category") && f["category"].is_string())
            folder.category = trim(f["category"].get<std::string>());
        if (f.contains("vendor") && f["vendor"].is_string())
            folder.vendor = trim(f["vendor"].get<std::string>());
        out.push_back(std::move(folder));
    }
    return out;
}

std::string folders_to_json(const std::vector<Folder>& folders)
{
    json j = json::array();
    for (const Folder& f : folders)
        j.push_back({{"path", f.path}, {"recursive", f.recursive}, {"category", f.category}, {"vendor", f.vendor}});
    return j.dump();
}

// ------------------------------------------------------------------------------ files ----

std::string file_type(const std::string& file_name)
{
    const std::string n = lower(file_name);
    if (ends_with(n, ".3mf")) return "3mf";
    if (ends_with(n, ".stl")) return "stl";
    if (ends_with(n, ".step") || ends_with(n, ".stp")) return "step";
    if (ends_with(n, ".obj")) return "obj";
    if (ends_with(n, ".amf")) return "amf";
    return std::string();
}

std::string entry_id(const std::string& path)
{
    uint64_t h = 14695981039346656037ull; // FNV-1a 64 offset basis
    for (unsigned char c : path) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) h);
    return buf;
}

std::string rels_thumbnail(const std::string& rels_xml)
{
    static const std::regex rel_re(R"(<\s*Relationship\b[^>]*>)");
    for (auto it = std::sregex_iterator(rels_xml.begin(), rels_xml.end(), rel_re); it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        if (ends_with(attribute(tag, "Type"), "/metadata/thumbnail")) {
            std::string target = attribute(tag, "Target");
            while (!target.empty() && target.front() == '/')
                target.erase(target.begin());
            return target;
        }
    }
    return std::string();
}

std::string model_metadata(const std::string& model_xml_head, const std::string& name)
{
    static const std::regex meta_re(R"(<\s*metadata\b([^>]*)>([^<]*)<\s*/\s*metadata\s*>)");
    for (auto it = std::sregex_iterator(model_xml_head.begin(), model_xml_head.end(), meta_re); it != std::sregex_iterator(); ++it)
        if (attribute((*it)[1].str(), "name") == name)
            return trim(xml_unescape((*it)[2].str()));
    return std::string();
}

std::vector<std::string> plate_names(const std::string& xml)
{
    std::vector<std::string> out;
    static const std::regex plate_re(R"(<\s*plate\s*>([\s\S]*?)<\s*/\s*plate\s*>)");
    static const std::regex meta_re(R"(<\s*metadata\b[^>]*>)");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), plate_re); it != std::sregex_iterator(); ++it) {
        const std::string body = (*it)[1].str();
        int               id   = 0;
        std::string       name;
        for (auto m = std::sregex_iterator(body.begin(), body.end(), meta_re); m != std::sregex_iterator(); ++m) {
            const std::string tag = m->str();
            const std::string key = attribute(tag, "key");
            if (key == "plater_id") {
                try { id = std::stoi(attribute(tag, "value")); } catch (...) { id = 0; }
            } else if (key == "plater_name") {
                name = trim(attribute(tag, "value"));
            }
        }
        if (id < 1 || id > 1000)
            continue;
        if (out.size() < size_t(id))
            out.resize(id);
        out[id - 1] = name;
    }
    return out;
}

static constexpr size_t MAX_THUMBNAIL_BYTES = 8 * 1024 * 1024;
static constexpr size_t MODEL_HEAD_BYTES    = 64 * 1024;
static constexpr size_t MAX_SETTINGS_BYTES  = 4 * 1024 * 1024;

static std::string extract(mz_zip_archive& zip, int index, size_t max_bytes)
{
    mz_zip_archive_file_stat st;
    if (index < 0 || !mz_zip_reader_file_stat(&zip, mz_uint(index), &st) || st.m_uncomp_size > max_bytes)
        return std::string();
    std::string data(size_t(st.m_uncomp_size), '\0');
    if (!data.empty() && !mz_zip_reader_extract_to_mem(&zip, mz_uint(index), data.data(), data.size(), 0))
        return std::string();
    return data;
}

// The first `bytes` of an entry, whatever its size (the model file can be hundreds of MB).
static std::string extract_head(mz_zip_archive& zip, int index, size_t bytes)
{
    if (index < 0)
        return std::string();
    mz_zip_reader_extract_iter_state* it = mz_zip_reader_extract_iter_new(&zip, mz_uint(index), 0);
    if (it == nullptr)
        return std::string();
    std::string data(bytes, '\0');
    const size_t got = mz_zip_reader_extract_iter_read(it, data.data(), data.size());
    mz_zip_reader_extract_iter_free(it);
    data.resize(got);
    return data;
}

ThreeMfInfo read_3mf(const std::string& path)
{
    ThreeMfInfo info;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path))
        return info;
    info.ok = true;
    info.sliced = ends_with(lower(path), ".gcode.3mf");

    std::map<std::string, int> names; // entry name -> index
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    static const std::regex plate_png(R"(Metadata/plate_(\d+)\.png)");
    static const std::regex plate_gcode(R"(Metadata/plate_\d+\.gcode)");
    int max_plate = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        std::string name = st.m_filename;
        names.emplace(name, int(i));
        std::smatch m;
        if (std::regex_match(name, m, plate_png)) {
            try { max_plate = std::max(max_plate, std::stoi(m[1].str())); } catch (...) {}
        } else if (std::regex_match(name, plate_gcode)) {
            info.sliced = true;
        }
    }
    auto find = [&names](const std::string& n) { auto it = names.find(n); return it == names.end() ? -1 : it->second; };

    // The cover: what the package says, then the usual places.
    std::vector<std::string> candidates;
    const std::string rels = extract(zip, find("_rels/.rels"), 1024 * 1024);
    if (!rels.empty()) {
        const std::string t = rels_thumbnail(rels);
        if (!t.empty()) candidates.push_back(t);
    }
    for (const char* c : {"Metadata/plate_1.png", "Metadata/thumbnail.png", "Auxiliaries/.thumbnails/thumbnail_3mf.png",
                          "Auxiliaries/.thumbnails/thumbnail_middle.png"})
        candidates.emplace_back(c);
    for (const std::string& c : candidates) {
        if (lower(c).size() < 4 || !ends_with(lower(c), ".png"))
            continue;
        info.thumbnail_png = extract(zip, find(c), MAX_THUMBNAIL_BYTES);
        if (!info.thumbnail_png.empty())
            break;
    }

    const std::string settings = extract(zip, find("Metadata/model_settings.config"), MAX_SETTINGS_BYTES);
    if (!settings.empty())
        info.plate_names = plate_names(settings);
    info.plates = std::max<int>(max_plate, int(info.plate_names.size()));

    const std::string head = extract_head(zip, find("3D/3dmodel.model"), MODEL_HEAD_BYTES);
    if (!head.empty()) {
        info.title    = model_metadata(head, "Title");
        info.designer = model_metadata(head, "Designer");
    }
    close_zip_reader(&zip);
    return info;
}

// ------------------------------------------------------------------------------ entries ----

json entry_to_json(const Entry& e)
{
    return {{"id", e.id}, {"path", e.path}, {"name", e.name}, {"type", e.type}, {"root", e.root},
            {"rel_dir", e.rel_dir}, {"category", e.category}, {"vendor", e.vendor}, {"size", e.size},
            {"mtime", e.mtime}, {"added", e.added}, {"plates", e.plates}, {"plate_names", e.plate_names},
            {"title", e.title}, {"designer", e.designer}, {"sliced", e.sliced}, {"has_thumbnail", e.has_thumbnail}};
}

json page_item(const Entry& e)
{
    return {{"id", e.id}, {"name", e.name}, {"type", e.type}, {"root", e.root}, {"rel_dir", e.rel_dir},
            {"category", e.category}, {"vendor", e.vendor}, {"size", e.size}, {"mtime", e.mtime},
            {"added", e.added}, {"plates", e.plates}, {"plate_names", e.plate_names}, {"title", e.title},
            {"designer", e.designer}, {"sliced", e.sliced}, {"has_thumbnail", e.has_thumbnail}};
}

template<class T> static T get_or(const json& j, const char* key, T fallback)
{
    if (!j.is_object() || !j.contains(key))
        return fallback;
    try {
        return j.at(key).get<T>();
    } catch (...) {
        return fallback;
    }
}

Entry entry_from_json(const json& j)
{
    Entry e;
    e.id            = get_or<std::string>(j, "id", "");
    e.path          = get_or<std::string>(j, "path", "");
    e.name          = get_or<std::string>(j, "name", "");
    e.type          = get_or<std::string>(j, "type", "");
    e.root          = get_or<std::string>(j, "root", "");
    e.rel_dir       = get_or<std::string>(j, "rel_dir", "");
    e.category      = get_or<std::string>(j, "category", "");
    e.vendor        = get_or<std::string>(j, "vendor", "");
    e.size          = get_or<int64_t>(j, "size", 0);
    e.mtime         = get_or<int64_t>(j, "mtime", 0);
    e.added         = get_or<int64_t>(j, "added", 0);
    e.plates        = get_or<int>(j, "plates", 0);
    e.plate_names   = get_or<std::vector<std::string>>(j, "plate_names", {});
    e.title         = get_or<std::string>(j, "title", "");
    e.designer      = get_or<std::string>(j, "designer", "");
    e.sliced        = get_or<bool>(j, "sliced", false);
    e.has_thumbnail = get_or<bool>(j, "has_thumbnail", false);
    return e;
}

std::string thumbnail_path(const std::string& cache_dir, const std::string& id)
{
    return (fs::path(cache_dir) / "thumbs" / (id + ".png")).string();
}

static std::string read_file(const std::string& path)
{
    boost::nowide::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
        return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const fs::path& path, const std::string& data)
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

Index load_index(const std::string& cache_dir)
{
    Index index;
    const std::string text = read_file((fs::path(cache_dir) / "index.json").string());
    if (text.empty())
        return index;
    try {
        const json j     = json::parse(text);
        index.scanned_at = get_or<int64_t>(j, "scanned_at", 0);
        if (j.contains("entries") && j["entries"].is_array())
            for (const json& e : j["entries"]) {
                Entry entry = entry_from_json(e);
                if (!entry.id.empty() && !entry.path.empty())
                    index.entries.push_back(std::move(entry));
            }
        if (j.contains("folders") && j["folders"].is_array())
            for (const json& f : j["folders"])
                index.folders.push_back({get_or<std::string>(f, "path", ""), get_or<bool>(f, "online", true), get_or<int>(f, "files", 0)});
    } catch (...) {
        return Index();
    }
    return index;
}

bool save_index(const std::string& cache_dir, const Index& index)
{
    json j;
    j["version"]    = 1;
    j["scanned_at"] = index.scanned_at;
    j["entries"]    = json::array();
    for (const Entry& e : index.entries)
        j["entries"].push_back(entry_to_json(e));
    j["folders"] = json::array();
    for (const FolderState& f : index.folders)
        j["folders"].push_back({{"path", f.path}, {"online", f.online}, {"files", f.files}});
    return write_file(fs::path(cache_dir) / "index.json", j.dump(-1, ' ', false, json::error_handler_t::replace));
}

// ------------------------------------------------------------------------------ scan ----

static int64_t mtime_of(const fs::path& p, boost::system::error_code& ec)
{
    const std::time_t t = fs::last_write_time(p, ec);
    return ec ? 0 : int64_t(t);
}

Index scan(const std::vector<Folder>& folders_in, const Index& previous, const std::set<std::string>& hidden,
           const std::string& cache_dir, int64_t now, const std::atomic<bool>& cancel,
           const std::function<void(size_t)>& progress, const ScanLimits& limits)
{
    Index out;
    out.scanned_at = now;

    std::vector<Folder> folders = folders_in;
    for (Folder& f : folders)
        f.path = norm(f.path);

    std::map<std::string, const Entry*> by_path;
    for (const Entry& e : previous.entries)
        by_path.emplace(e.path, &e);

    // Does another Library folder, deeper than `root`, cover files in `dir`? Then they are its.
    auto owned_elsewhere = [&folders](const std::string& dir, const std::string& root) {
        for (const Folder& o : folders) {
            if (o.path == root || o.path.size() <= root.size() || !within(o.path, root))
                continue;
            if (dir == o.path || (o.recursive && within(dir, o.path)))
                return true;
        }
        return false;
    };

    std::set<std::string> taken; // paths already indexed (a folder listed twice, or links)
    size_t                seen = 0;

    for (const Folder& folder : folders) {
        if (cancel)
            break;
        FolderState state;
        state.path = folder.path;
        boost::system::error_code ec;
        const fs::path root(folder.path);
        if (folder.path.empty() || !fs::is_directory(root, ec)) {
            // Unreachable (unplugged drive, share offline) or gone: keep what it had, marked so.
            state.online = false;
            for (const Entry& e : previous.entries)
                if (e.root == folder.path && !hidden.count(e.path) && taken.insert(e.path).second) {
                    Entry kept    = e;
                    kept.category = folder.category;
                    kept.vendor   = folder.vendor;
                    out.entries.push_back(std::move(kept));
                    ++state.files;
                }
            out.folders.push_back(state);
            continue;
        }

        auto visit = [&](const fs::path& file) {
            const std::string name = file.filename().string();
            const std::string type = file_type(name);
            if (type.empty())
                return;
            const std::string path = file.generic_string();
            const std::string dir  = file.parent_path().generic_string();
            if (hidden.count(path) || owned_elsewhere(dir, folder.path) || !taken.insert(path).second)
                return;
            boost::system::error_code fec;
            if (!fs::is_regular_file(file, fec))
                return;
            const int64_t size  = int64_t(fs::file_size(file, fec));
            if (fec) return;
            const int64_t mtime = mtime_of(file, fec);

            Entry e;
            auto  prev = by_path.find(path);
            const std::string thumb = thumbnail_path(cache_dir, entry_id(path));
            if (prev != by_path.end() && prev->second->size == size && prev->second->mtime == mtime &&
                (!prev->second->has_thumbnail || fs::is_regular_file(thumb, fec))) {
                e = *prev->second;
            } else {
                e.id    = entry_id(path);
                e.path  = path;
                e.size  = size;
                e.mtime = mtime;
                e.added = prev != by_path.end() && prev->second->added > 0 ? prev->second->added : now;
                if (type == "3mf") {
                    ThreeMfInfo info = read_3mf(path);
                    e.plates        = info.plates;
                    e.plate_names   = std::move(info.plate_names);
                    e.title         = std::move(info.title);
                    e.designer      = std::move(info.designer);
                    e.sliced        = info.sliced;
                    e.has_thumbnail = !info.thumbnail_png.empty() && write_file(thumb, info.thumbnail_png);
                } else if (type == "stl" || type == "obj" || type == "amf") {
                    // No picture in the file: draw one (once; an unchanged file keeps it).
                    const std::string png = mesh_thumbnail_png(path, type);
                    e.has_thumbnail       = !png.empty() && write_file(thumb, png);
                }
            }
            e.name     = name;
            e.type     = type;
            e.root     = folder.path;
            e.category = folder.category;
            e.vendor   = folder.vendor;
            fs::path rel = file.parent_path().lexically_relative(root);
            e.rel_dir    = rel.empty() || rel == "." ? std::string() : rel.generic_string();
            out.entries.push_back(std::move(e));
            ++state.files;
            if (++seen % 200 == 0 && progress)
                progress(seen);
        };

        if (!folder.recursive) {
            for (fs::directory_iterator it(root, ec), end; !ec && it != end && !cancel; it.increment(ec)) {
                if (out.entries.size() >= limits.max_files) break;
                visit(it->path());
            }
        } else {
            fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
            for (; !ec && it != end && !cancel; it.increment(ec)) {
                if (out.entries.size() >= limits.max_files) break;
                const fs::path& p     = it->path();
                const std::string nm  = p.filename().string();
                boost::system::error_code sec;
                const fs::file_status lst = fs::symlink_status(p, sec);
                if (fs::is_directory(lst)) {
                    // Hidden folders (".git", ".cache", "@eaDir"...) and anything too deep are skipped.
                    if ((!nm.empty() && (nm[0] == '.' || nm[0] == '@' || nm[0] == '$')) || it.depth() + 1 >= limits.max_depth)
                        it.disable_recursion_pending();
                    continue;
                }
                if (fs::is_symlink(lst)) {
                    // A link to a file is followed; a link to a folder is not (no loops, no surprises).
                    if (fs::is_directory(p, sec))
                        continue;
                }
                visit(p);
            }
        }
        // A folder whose listing failed part way is still online; what was read is kept.
        out.folders.push_back(state);
    }

    if (!cancel) {
        // Thumbnails of files no longer in the Library.
        std::set<std::string> keep;
        for (const Entry& e : out.entries)
            if (e.has_thumbnail) keep.insert(e.id + ".png");
        boost::system::error_code ec;
        const fs::path thumbs = fs::path(cache_dir) / "thumbs";
        static const std::regex thumb_name(R"([0-9a-f]{16}\.png)");
        std::vector<fs::path> stale;
        for (fs::directory_iterator it(thumbs, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (std::regex_match(n, thumb_name) && !keep.count(n))
                stale.push_back(it->path());
        }
        for (const fs::path& p : stale)
            fs::remove(p, ec);
    }
    if (progress)
        progress(seen);
    return out;
}

} // namespace Library
} // namespace Slic3r
