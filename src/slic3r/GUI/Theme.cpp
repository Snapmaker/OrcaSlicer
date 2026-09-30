#include "Theme.hpp"

#include <algorithm>
#include <set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/font.h>
#include <wx/image.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "GUI.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {
namespace Theme {

static constexpr uintmax_t MAX_THEME_JSON = 256 * 1024;
static constexpr uintmax_t MAX_FONT_FILE  = 16 * 1024 * 1024;
static constexpr uintmax_t MAX_IMAGE_FILE = 8 * 1024 * 1024;
static constexpr uintmax_t MAX_PACK_BYTES = 64 * 1024 * 1024;
static constexpr size_t    MAX_PACK_FILES = 500;

static std::string     g_id;
static fs::path        g_dir;
static ThemePack::Spec g_spec;
static bool            g_active = false;
static wxBitmap        g_banner;
static bool            g_banner_tried = false;

static fs::path builtin_dir() { return fs::path(resources_dir()) / "themes"; }

fs::path user_dir() { return fs::path(data_dir()) / "themes"; }

static bool read_text(const fs::path& path, uintmax_t max_bytes, std::string& out)
{
    boost::system::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > max_bytes)
        return false;
    boost::nowide::ifstream f(path.string().c_str(), std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool read_spec(const fs::path& dir, ThemePack::Spec& spec, std::string& error)
{
    std::string text;
    if (!read_text(dir / "theme.json", MAX_THEME_JSON, text)) {
        error = "no readable theme.json (at most 256 KB)";
        return false;
    }
    return ThemePack::parse(text, spec, error);
}

// A file the pack names, when it is there and not too big.
static bool pack_file(const std::string& relative, uintmax_t max_bytes, fs::path& out)
{
    if (!ThemePack::safe_relative_path(relative))
        return false;
    const fs::path p = g_dir / fs::path(relative).make_preferred();
    boost::system::error_code ec;
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) > max_bytes || ec)
        return false;
    out = p;
    return true;
}

// The folder a theme id lives in: an installed one first, then a shipped one.
static bool find_dir(const std::string& id, fs::path& dir, bool& builtin)
{
    if (!ThemePack::valid_id(id))
        return false;
    boost::system::error_code ec;
    for (bool b : {false, true}) {
        const fs::path d = (b ? builtin_dir() : user_dir()) / id;
        if (fs::is_regular_file(d / "theme.json", ec)) {
            dir     = d;
            builtin = b;
            return true;
        }
    }
    return false;
}

std::vector<Entry> available()
{
    std::vector<Entry> list;
    std::set<std::string> seen;
    boost::system::error_code ec;
    for (bool b : {false, true}) {
        const fs::path root = b ? builtin_dir() : user_dir();
        if (!fs::is_directory(root, ec))
            continue;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string id = it->path().filename().string();
            if (!ThemePack::valid_id(id) || seen.count(id) || !fs::is_directory(it->path(), ec))
                continue;
            ThemePack::Spec spec;
            std::string     error;
            if (!read_spec(it->path(), spec, error)) {
                BOOST_LOG_TRIVIAL(warning) << "Theme \"" << id << "\" skipped: " << error;
                continue;
            }
            seen.insert(id);
            list.push_back({id, spec.name, spec.author, spec.description, b});
        }
    }
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
        return boost::algorithm::to_lower_copy(a.name) < boost::algorithm::to_lower_copy(b.name);
    });
    return list;
}

static wxString register_font(const ThemePack::Font& font, const char* which)
{
    if (font.empty())
        return {};
    for (const auto& file : font.files) {
        fs::path p;
        const std::string ext = boost::algorithm::to_lower_copy(fs::path(file).extension().string());
        if ((ext != ".ttf" && ext != ".otf") || !pack_file(file, MAX_FONT_FILE, p)) {
            BOOST_LOG_TRIVIAL(warning) << "Theme: " << which << " font file \"" << file << "\" is missing, too big or not .ttf/.otf";
            continue;
        }
        if (!wxFont::AddPrivateFont(from_path(p)))
            BOOST_LOG_TRIVIAL(warning) << "Theme: could not load the " << which << " font file \"" << file << "\"";
    }
    return from_u8(font.face);
}

void load(const std::string& id)
{
    if (id.empty())
        return;
    fs::path dir;
    bool     builtin = false;
    if (!find_dir(id, dir, builtin)) {
        BOOST_LOG_TRIVIAL(warning) << "Theme \"" << id << "\" is not installed; using the stock look";
        return;
    }
    ThemePack::Spec spec;
    std::string     error;
    if (!read_spec(dir, spec, error)) {
        BOOST_LOG_TRIVIAL(error) << "Theme \"" << id << "\" not loaded: " << error;
        return;
    }
    for (const auto& w : spec.warnings)
        BOOST_LOG_TRIVIAL(warning) << "Theme \"" << id << "\": " << w;

    g_id     = id;
    g_dir    = dir;
    g_spec   = std::move(spec);
    g_active = true;

    // Colours: keep every themed colour clear of the stock table's colours, both columns.
    std::set<std::string> reserved;
    auto hex = [](const wxColour& c) { return into_u8(c.GetAsString(wxC2S_HTML_SYNTAX)).substr(0, 7); };
    for (const auto& [light, dark] : StateColor::GetDarkMap()) {
        reserved.insert(hex(light));
        reserved.insert(hex(dark));
    }
    std::map<wxColour, wxColour> map;
    for (const auto& [key, value] : ThemePack::colour_map(g_spec, reserved))
        map.emplace(wxColour(from_u8(key)), wxColour(from_u8(value)));
    StateColor::SetThemeMap(map);

    Label::SetThemeFaces(register_font(g_spec.body, "body"), register_font(g_spec.heading, "heading"),
                         register_font(g_spec.button, "button"));
    StaticBox::SetThemeRadius(g_spec.button_radius, g_spec.box_radius);

    BOOST_LOG_TRIVIAL(info) << "Theme \"" << g_spec.name << "\" loaded from " << dir.string() << " (" << map.size() << " colours)";
}

bool                   active() { return g_active; }
const std::string&     active_id() { return g_id; }
const ThemePack::Spec& spec() { return g_spec; }

int base_dark()
{
    if (!g_active || g_spec.base.empty())
        return -1;
    return g_spec.base == "dark" ? 1 : 0;
}

bool has_colour(const std::string& role) { return g_active && g_spec.palette.count(role) > 0; }

wxColour colour(const std::string& role, const wxColour& fallback)
{
    if (!g_active)
        return fallback;
    auto it = g_spec.palette.find(role);
    return it == g_spec.palette.end() ? fallback : wxColour(from_u8(it->second));
}

const wxBitmap& banner()
{
    if (g_banner_tried || !g_active || g_spec.banner.empty())
        return g_banner;
    g_banner_tried = true;
    fs::path p;
    if (!pack_file(g_spec.banner, MAX_IMAGE_FILE, p)) {
        BOOST_LOG_TRIVIAL(warning) << "Theme: banner \"" << g_spec.banner << "\" is missing or over 8 MB";
        return g_banner;
    }
    wxImage image;
    if (!image.LoadFile(from_path(p)) || !image.IsOk() || image.GetWidth() > 8192 || image.GetHeight() > 1024) {
        BOOST_LOG_TRIVIAL(warning) << "Theme: banner \"" << g_spec.banner << "\" could not be read or is over 8192x1024";
        return g_banner;
    }
    g_banner = wxBitmap(image);
    return g_banner;
}

// ------------------------------------------------------------------------------ installing ----

static bool allowed_file(const std::string& name)
{
    static const std::set<std::string> exts = {".json", ".png", ".jpg", ".jpeg", ".bmp", ".ttf", ".otf", ".txt", ".md"};
    return exts.count(boost::algorithm::to_lower_copy(fs::path(name).extension().string())) > 0;
}

static bool write_file(const fs::path& path, const std::string& data)
{
    boost::system::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    boost::nowide::ofstream f(path.string().c_str(), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(data.data(), std::streamsize(data.size()));
    return bool(f);
}

// Unpacks a zip into `into`: theme.json at the top, or inside the zip's one top folder. Only the
// file types a theme uses, only paths that stay inside, within the size and count limits.
static bool unzip(const fs::path& zip_path, const fs::path& into, std::string& error)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, zip_path.string())) {
        error = "not a readable .zip file";
        return false;
    }
    struct Item { mz_uint index; std::string name; uintmax_t size; };
    std::vector<Item>        items;
    std::vector<std::string> folders; // top folders holding a theme.json
    bool                     at_top = false;
    const mz_uint            count  = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory)
            continue;
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        items.push_back({i, name, uintmax_t(st.m_uncomp_size)});
        const size_t slash = name.find('/');
        if (name == "theme.json")
            at_top = true;
        else if (slash != std::string::npos && name.compare(slash, std::string::npos, "/theme.json") == 0)
            folders.push_back(name.substr(0, slash + 1));
    }
    std::string prefix;
    if (!at_top && folders.size() == 1)
        prefix = folders.front();
    if (!at_top && folders.size() != 1) {
        close_zip_reader(&zip);
        error = "the zip has no theme.json at its top or in its one top folder";
        return false;
    }
    uintmax_t total = 0;
    size_t    files = 0;
    bool      ok    = true;
    for (const auto& item : items) {
        if (item.name.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string rel = item.name.substr(prefix.size());
        if (!ThemePack::safe_relative_path(rel) || !allowed_file(rel)) {
            BOOST_LOG_TRIVIAL(info) << "Theme install: skipped \"" << item.name << "\"";
            continue;
        }
        total += item.size;
        if (++files > MAX_PACK_FILES || total > MAX_PACK_BYTES) {
            error = "the theme is too big (at most 500 files and 64 MB)";
            ok    = false;
            break;
        }
        std::string data(size_t(item.size), '\0');
        if (!data.empty() && !mz_zip_reader_extract_to_mem(&zip, item.index, data.data(), data.size(), 0)) {
            error = "could not unpack \"" + rel + "\"";
            ok    = false;
            break;
        }
        if (!write_file(into / fs::path(rel).make_preferred(), data)) {
            error = "could not write \"" + rel + "\"";
            ok    = false;
            break;
        }
    }
    close_zip_reader(&zip);
    return ok;
}

// Copies a theme folder: regular files only (no links), the same types and limits as a zip.
static bool copy_folder(const fs::path& from, const fs::path& into, std::string& error)
{
    boost::system::error_code ec;
    uintmax_t total = 0;
    size_t    files = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::file_status st = fs::symlink_status(it->path(), ec);
        if (fs::is_symlink(st)) {
            if (fs::is_directory(it->path(), ec))
                it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(st))
            continue;
        const std::string rel = fs::relative(it->path(), from, ec).generic_string();
        if (ec || !ThemePack::safe_relative_path(rel) || !allowed_file(rel))
            continue;
        total += fs::file_size(it->path(), ec);
        if (++files > MAX_PACK_FILES || total > MAX_PACK_BYTES) {
            error = "the theme is too big (at most 500 files and 64 MB)";
            return false;
        }
        fs::create_directories((into / rel).parent_path(), ec);
        fs::copy_file(it->path(), into / fs::path(rel).make_preferred(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "could not copy \"" + rel + "\": " + ec.message();
            return false;
        }
    }
    if (ec) {
        error = "could not read the folder: " + ec.message();
        return false;
    }
    return true;
}

std::string install(const fs::path& source, bool overwrite, bool& exists, std::string& error)
{
    exists = false;
    boost::system::error_code ec;
    const bool is_zip = fs::is_regular_file(source, ec) &&
                        boost::algorithm::to_lower_copy(source.extension().string()) == ".zip";
    if (!is_zip && !fs::is_directory(source, ec)) {
        error = "pick a theme folder or a .zip file";
        return {};
    }

    // Unpack next to the installed themes first, so a bad pack never touches one that works.
    const fs::path staging = user_dir() / (".install-" + fs::unique_path("%%%%%%%%").string());
    fs::create_directories(staging, ec);
    if (ec) {
        error = "could not create " + staging.string() + ": " + ec.message();
        return {};
    }
    auto cleanup = [&staging]() { boost::system::error_code e; fs::remove_all(staging, e); };

    if (!(is_zip ? unzip(source, staging, error) : copy_folder(source, staging, error))) {
        cleanup();
        return {};
    }
    ThemePack::Spec spec;
    if (!read_spec(staging, spec, error)) {
        cleanup();
        return {};
    }
    const std::string id     = ThemePack::id_from_name(spec.name);
    const fs::path    target = user_dir() / id;
    if (fs::exists(target, ec)) {
        if (!overwrite) {
            exists = true;
            cleanup();
            return id;
        }
        fs::remove_all(target, ec);
        if (ec) {
            error = "could not replace the installed copy: " + ec.message();
            cleanup();
            return {};
        }
    }
    fs::rename(staging, target, ec);
    if (ec) {
        error = "could not move the theme into place: " + ec.message();
        cleanup();
        return {};
    }
    BOOST_LOG_TRIVIAL(info) << "Theme \"" << spec.name << "\" installed to " << target.string();
    return id;
}

} // namespace Theme
} // namespace GUI
} // namespace Slic3r
