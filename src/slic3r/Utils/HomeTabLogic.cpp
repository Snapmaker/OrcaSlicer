#include "HomeTabLogic.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace HomeTab {

using json = nlohmann::json;

bool valid_section(const std::string& section)
{
    return section == "recent" || section == "library" || section == "history";
}

std::string section_or_default(const std::string& section)
{
    return valid_section(section) ? section : std::string("recent");
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static bool ends_with(const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

ArchiveOpen archive_open_kind(const std::string& file_name)
{
    const std::string name = lower(file_name);
    if (ends_with(name, ".gcode.3mf"))
        return ArchiveOpen::Project;
    if (ends_with(name, ".gcode") || ends_with(name, ".gco") || ends_with(name, ".g"))
        return ArchiveOpen::Gcode;
    return ArchiveOpen::None;
}

std::string clean_colour(const std::string& colour)
{
    if (colour.size() != 4 && colour.size() != 7 && colour.size() != 9)
        return std::string();
    if (colour[0] != '#')
        return std::string();
    std::string out = "#";
    for (size_t i = 1; i < colour.size(); ++i) {
        const unsigned char c = (unsigned char) colour[i];
        if (!std::isxdigit(c))
            return std::string();
        out += char(std::toupper(c));
    }
    return out;
}

template<class T> static T value_or(const json& j, const char* key, T fallback)
{
    if (!j.is_object() || !j.contains(key))
        return fallback;
    try {
        return j.at(key).get<T>();
    } catch (...) {
        return fallback;
    }
}

static std::string stem_of(const std::string& file)
{
    std::string name = file;
    const std::string low = lower(name);
    for (const char* ext : {".gcode.3mf", ".gcode", ".gco", ".g", ".3mf"})
        if (ends_with(low, ext)) {
            name.resize(name.size() - std::string(ext).size());
            break;
        }
    return name;
}

json history_card(const json& sidecar, const std::string& printer, bool file_present, bool project_present)
{
    json card;
    const std::string file  = value_or<std::string>(sidecar, "file", "");
    std::string       title = value_or<std::string>(sidecar, "project_title", "");
    if (title.empty())
        title = stem_of(value_or<std::string>(sidecar, "sent_name", ""));
    if (title.empty())
        title = stem_of(file);

    card["id"]    = value_or<std::string>(sidecar, "id", "");
    card["title"] = title;
    card["file"]  = file;
    card["time"]  = value_or<long long>(sidecar, "time", 0);
    card["size"]  = value_or<long long>(sidecar, "size", 0);

    const int plate    = value_or<int>(sidecar, "plate", -1);
    card["plate"]      = plate >= 0 ? plate + 1 : 0; // 1-based for people; 0 = not known
    card["plate_name"] = value_or<std::string>(sidecar, "plate_name", "");

    card["printer"] = printer;
    card["model"]   = value_or<std::string>(sidecar, "model_name", "");
    card["mode"]    = value_or<std::string>(sidecar, "mode", "upload") == "print" ? "print" : "upload";
    card["source"]  = value_or<std::string>(sidecar, "source", "desktop") == "phone" ? "phone" : "desktop";

    card["print_time_s"] = std::max(0, value_or<int>(sidecar, "estimated_time_s", 0));
    card["weight_g"]     = std::max(0.0, value_or<double>(sidecar, "estimated_weight_g", 0.0));

    json filaments = json::array();
    if (sidecar.is_object() && sidecar.contains("filaments") && sidecar["filaments"].is_array())
        for (const json& f : sidecar["filaments"]) {
            if (!f.is_object())
                continue;
            filaments.push_back({{"type", value_or<std::string>(f, "type", "")},
                                 {"colour", clean_colour(value_or<std::string>(f, "colour", ""))},
                                 {"grams", std::max(0.0, value_or<double>(f, "grams", 0.0))}});
        }
    card["filaments"] = filaments;

    size_t reprints = 0;
    if (sidecar.is_object() && sidecar.contains("reprints") && sidecar["reprints"].is_array())
        reprints = sidecar["reprints"].size();
    card["reprints"] = reprints;

    card["has_thumbnail"]   = value_or<bool>(sidecar, "has_thumbnail", false);
    card["file_present"]    = file_present;
    card["project_present"] = project_present;
    card["can_open"]        = file_present && archive_open_kind(file) != ArchiveOpen::None;
    return card;
}

bool is_listed(const std::string& path, const std::vector<std::string>& listed)
{
    return !path.empty() && std::find(listed.begin(), listed.end(), path) != listed.end();
}

std::string base64(const std::string& bytes)
{
    static const char* const table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string              out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (unsigned char) bytes[i] << 16 | (unsigned char) bytes[i + 1] << 8 | (unsigned char) bytes[i + 2];
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += table[v >> 6 & 63];
        out += table[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = (unsigned char) bytes[i] << 16;
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (unsigned char) bytes[i] << 16 | (unsigned char) bytes[i + 1] << 8;
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += table[v >> 6 & 63];
        out += '=';
    }
    return out;
}

std::string png_data_uri(const std::string& png_bytes)
{
    return png_bytes.empty() ? std::string() : "data:image/png;base64," + base64(png_bytes);
}

std::string receive_script(const json& message)
{
    // ensure_ascii: U+2028 / U+2029 and anything else outside ASCII are \u-escaped; "replace" keeps a
    // stray invalid UTF-8 byte (a file name) from throwing.
    const std::string arg = message.dump(-1, ' ', true, json::error_handler_t::replace);
    return "window.HomeApp && window.HomeApp.receive(" + arg + ");";
}

} // namespace HomeTab
} // namespace Slic3r
