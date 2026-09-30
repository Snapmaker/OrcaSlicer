#include "ThemePack.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace Slic3r {
namespace ThemePack {

using json = nlohmann::json;

const std::vector<std::pair<std::string, std::vector<std::string>>>& roles()
{
    // Grouped from the comments in the light -> dark table (GUI/Widgets/StateColor.cpp). #F0F0F0 is
    // left out on purpose: it is also the system button face that plain child windows report
    // (GUI/DarkModeBackground.hpp), so recolouring it would paint a band behind labels.
    static const std::vector<std::pair<std::string, std::vector<std::string>>> r = {
        {"window_bg",        {"#FFFFFF", "#F8F7F7"}},
        {"panel_bg",         {"#F8F8F8", "#F1F1F1", "#F4F4F4", "#FEFFFF", "#E9E9E9"}},
        {"sidebar_bg",       {"#CECECE", "#E7E7E7"}},
        {"text",             {"#000000", "#262E30", "#242424", "#323A3D", "#323A3C", "#303A3C", "#2C2C2E"}},
        {"text_secondary",   {"#363636", "#6B6B6A", "#4A4A4A", "#8F8F8F"}},
        {"text_disabled",    {"#6B6B6B", "#6B6A6A", "#ACACAC", "#9E9E9E"}},
        {"accent",           {"#009688", "#019687"}},
        {"accent_hover",     {"#26A69A"}},
        {"accent_soft",      {"#BFE1DE", "#E5F0EE"}},
        {"accent_text",      {"#FEFEFE"}},
        {"secondary_accent", {"#FF6F00"}},
        {"button_bg",        {"#DFDFDF"}},
        {"button_hover_bg",  {"#D4D4D4"}},
        {"border",           {"#DBDBDB", "#D1D5DC", "#B4B4B4"}},
        {"separator",        {"#EEEEEE", "#E8E8E8", "#F3F4F6", "#A6A9AA", "#EBEBEB"}},
        {"disabled_bg",      {"#F0F0F1"}},
        {"toggle_track",     {"#D9D9D9"}},
        {"error",            {"#D01B1B", "#D32F2F"}},
        {"tabbar_bg",        {"#3B4446"}},
        {"tabbar_hover",     {"#6B6B6C"}},
        // Drawn directly, no stock colour to recolour.
        {"titlebar_bg",      {}},
        {"titlebar_text",    {}},
        {"canvas_bg",        {}},
        {"canvas_bg_top",    {}},
        {"icon",             {}},
    };
    return r;
}

bool known_role(const std::string& role)
{
    for (const auto& r : roles())
        if (r.first == role)
            return true;
    return false;
}

std::string normalize_colour(const std::string& colour)
{
    if (colour.empty() || colour[0] != '#')
        return {};
    std::string hex = colour.substr(1);
    if (!std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
        return {};
    if (hex.size() == 3)
        hex = std::string{hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
    else if (hex.size() == 8)
        hex.resize(6);
    else if (hex.size() != 6)
        return {};
    std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return "#" + hex;
}

bool safe_relative_path(const std::string& path)
{
    if (path.empty() || path.size() > 255)
        return false;
    if (path[0] == '/' || path[0] == '\\')
        return false;
    if (path.find(':') != std::string::npos) // a drive letter or a stream name
        return false;
    for (unsigned char c : path)
        if (c < 0x20)
            return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string::npos)
            end = path.size();
        const std::string seg = path.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        start = end + 1;
    }
    return true;
}

bool valid_id(const std::string& id)
{
    if (id.empty() || id.size() > 64 || id[0] == '.' || id[0] == ' ' || id.back() == ' ' || id.back() == '.')
        return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-' || c == ' ';
    });
}

std::string id_from_name(const std::string& name)
{
    std::string id;
    for (unsigned char c : name)
        id += (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-' || c == ' ') ? char(c) : '-';
    while (!id.empty() && (id[0] == '.' || id[0] == ' '))
        id.erase(0, 1);
    while (!id.empty() && (id.back() == '.' || id.back() == ' '))
        id.pop_back();
    if (id.size() > 64)
        id.resize(64);
    while (!id.empty() && (id.back() == '.' || id.back() == ' '))
        id.pop_back();
    return id.empty() ? std::string("theme") : id;
}

static std::string text_field(const json& j, const char* key, size_t max_len)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return {};
    std::string s = it->get<std::string>();
    if (s.size() > max_len)
        s.resize(max_len);
    return s;
}

static Font parse_font(const json& j, const std::string& which, std::vector<std::string>& warnings)
{
    Font font;
    if (!j.is_object()) {
        warnings.push_back("fonts." + which + " is not an object");
        return font;
    }
    font.face = text_field(j, "face", 128);
    std::vector<std::string> files;
    if (auto it = j.find("file"); it != j.end() && it->is_string())
        files.push_back(it->get<std::string>());
    if (auto it = j.find("files"); it != j.end() && it->is_array())
        for (const auto& f : *it)
            if (f.is_string())
                files.push_back(f.get<std::string>());
    for (const auto& f : files) {
        if (safe_relative_path(f))
            font.files.push_back(f);
        else
            warnings.push_back("fonts." + which + ": ignored the path \"" + f + "\"");
    }
    if (font.face.empty() && !font.files.empty())
        warnings.push_back("fonts." + which + " has files but no face name");
    return font;
}

static int parse_radius(const json& shapes, const char* key, std::vector<std::string>& warnings)
{
    auto it = shapes.find(key);
    if (it == shapes.end())
        return -1;
    if (!it->is_number()) {
        warnings.push_back(std::string("shapes.") + key + " is not a number");
        return -1;
    }
    const double v = it->get<double>();
    if (v < 0 || v > 24) {
        warnings.push_back(std::string("shapes.") + key + " must be 0 to 24");
        return -1;
    }
    return int(v + 0.5);
}

bool parse(const std::string& text, Spec& out, std::string& error)
{
    out = Spec{};
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("theme.json is not valid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "theme.json is not a JSON object";
        return false;
    }
    out.name = text_field(j, "name", 64);
    if (out.name.empty()) {
        error = "theme.json has no \"name\"";
        return false;
    }
    out.author      = text_field(j, "author", 128);
    out.version     = text_field(j, "version", 32);
    out.description = text_field(j, "description", 512);

    const std::string base = text_field(j, "base", 16);
    if (base == "light" || base == "dark")
        out.base = base;
    else if (!base.empty())
        out.warnings.push_back("base must be \"light\" or \"dark\"");

    if (auto it = j.find("palette"); it != j.end() && it->is_object()) {
        for (const auto& [role, value] : it->items()) {
            const std::string c = value.is_string() ? normalize_colour(value.get<std::string>()) : std::string();
            if (!known_role(role))
                out.warnings.push_back("palette: unknown role \"" + role + "\"");
            else if (c.empty())
                out.warnings.push_back("palette." + role + " is not a #RRGGBB colour");
            else
                out.palette[role] = c;
        }
    }

    if (auto it = j.find("overrides"); it != j.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            const std::string k = normalize_colour(key);
            const std::string c = value.is_string() ? normalize_colour(value.get<std::string>()) : std::string();
            if (k.empty() || c.empty())
                out.warnings.push_back("overrides: ignored \"" + key + "\"");
            else
                out.overrides[k] = c;
        }
    }

    if (auto it = j.find("fonts"); it != j.end() && it->is_object()) {
        if (auto f = it->find("body"); f != it->end())
            out.body = parse_font(*f, "body", out.warnings);
        if (auto f = it->find("heading"); f != it->end())
            out.heading = parse_font(*f, "heading", out.warnings);
        if (auto f = it->find("button"); f != it->end())
            out.button = parse_font(*f, "button", out.warnings);
    }

    if (auto it = j.find("shapes"); it != j.end() && it->is_object()) {
        out.button_radius = parse_radius(*it, "button_radius", out.warnings);
        out.box_radius    = parse_radius(*it, "box_radius", out.warnings);
    }

    if (auto it = j.find("titlebar"); it != j.end() && it->is_object()) {
        const std::string banner = text_field(*it, "banner", 255);
        if (safe_relative_path(banner))
            out.banner = banner;
        else if (!banner.empty())
            out.warnings.push_back("titlebar: ignored the banner path \"" + banner + "\"");
        const std::string align = text_field(*it, "align", 16);
        if (align == "left" || align == "center" || align == "right" || align == "stretch" || align == "tile")
            out.banner_align = align;
        else if (!align.empty())
            out.warnings.push_back("titlebar.align must be left, center, right, stretch or tile");
    }

    if (auto it = j.find("home"); it != j.end() && it->is_object()) {
        for (const auto& [var, value] : it->items()) {
            const std::string c = value.is_string() ? normalize_colour(value.get<std::string>()) : std::string();
            if (c.empty())
                out.warnings.push_back("home." + var + " is not a #RRGGBB colour");
            else
                out.home[var] = c;
        }
    }
    return true;
}

static std::string nudge(const std::string& colour, const std::set<std::string>& reserved)
{
    if (reserved.count(colour) == 0)
        return colour;
    const int rgb  = std::stoi(colour.substr(1), nullptr, 16);
    const int r    = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    const int step = b >= 0x80 ? -1 : 1;
    for (int i = 1; i < 256; ++i) {
        const int nb = b + step * i;
        if (nb < 0 || nb > 0xFF)
            break;
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, nb);
        if (reserved.count(buf) == 0)
            return buf;
    }
    return colour;
}

std::map<std::string, std::string> colour_map(const Spec& spec, const std::set<std::string>& reserved)
{
    std::map<std::string, std::string> map;
    for (const auto& [role, keys] : roles()) {
        auto it = spec.palette.find(role);
        if (it == spec.palette.end())
            continue;
        for (const auto& key : keys)
            map[key] = it->second;
    }
    for (const auto& [key, value] : spec.overrides)
        map[key] = value;
    for (auto& [key, value] : map)
        value = nudge(value, reserved);
    return map;
}

json home_css(const Spec& spec)
{
    static const std::vector<std::pair<const char*, const char*>> from_palette = {
        {"--bg", "panel_bg"},       {"--rail", "window_bg"},      {"--card", "window_bg"},
        {"--ctl", "window_bg"},     {"--thumb", "panel_bg"},      {"--fg", "text"},
        {"--mute", "text_secondary"}, {"--faint", "text_disabled"}, {"--line", "separator"},
        {"--ctl-line", "border"},   {"--hover", "accent_soft"},   {"--accent", "accent"},
        {"--accent-fg", "accent_text"}, {"--accent-soft", "accent_soft"}, {"--danger", "error"},
    };
    static const std::set<std::string> page_vars = {
        "--bg", "--rail", "--card", "--fg", "--mute", "--faint", "--line", "--ctl", "--ctl-line", "--hover",
        "--thumb", "--accent", "--accent-fg", "--accent-soft", "--warn-bg", "--warn-fg", "--danger",
    };
    json css = json::object();
    for (const auto& [var, role] : from_palette)
        if (auto it = spec.palette.find(role); it != spec.palette.end())
            css[var] = it->second;
    for (const auto& [var, value] : spec.home)
        if (page_vars.count(var))
            css[var] = value;
    return css;
}

} // namespace ThemePack
} // namespace Slic3r
