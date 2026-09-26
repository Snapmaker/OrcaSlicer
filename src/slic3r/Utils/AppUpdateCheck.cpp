#include "AppUpdateCheck.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <vector>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace AppUpdate {

const char* const GITHUB_LATEST_RELEASE_API = "https://api.github.com/repos/aceRage/EdgeSlicer/releases/latest";
const char* const GITHUB_NIGHTLY_RELEASE_API = "https://api.github.com/repos/aceRage/EdgeSlicer/releases/tags/nightly";
const char* const GITHUB_TRUSTED_URL_PREFIX = "https://github.com/aceRage/EdgeSlicer/";
const char* const GITHUB_RELEASES_PAGE      = "https://github.com/aceRage/EdgeSlicer/releases/latest";
const char* const NOTICE_BEGIN_MARKER       = "<!-- update-notice -->";
const char* const NOTICE_END_MARKER         = "<!-- /update-notice -->";
const char* const NIGHTLY_VERSION_MARKER    = "nightly-version";

namespace {

// UTF-8 bullet (U+2022) and ellipsis (U+2026), spelled as bytes so the source stays ASCII.
const char* const BULLET   = "\xE2\x80\xA2 ";
const char* const ELLIPSIS = "\xE2\x80\xA6";

// The update-notice section is written for the dialog, so it is shown whole - these caps only
// keep a runaway section (a forgotten end marker is handled separately) from swamping it.
constexpr size_t MARKER_MAX_LINES = 40;
constexpr size_t MARKER_MAX_CHARS = 4000;

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool ends_with_icase(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

bool contains_icase(const std::string& s, const std::string& needle)
{
    return to_lower(s).find(to_lower(needle)) != std::string::npos;
}

bool starts_with(const std::string& s, const std::string& prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || (static_cast<unsigned char>(c) & 0x80); }

bool all_digits(const std::string& s)
{
    return !s.empty() && s.size() <= 9 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string replace_all(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Drops every <!-- ... --> (also across lines). An unterminated comment runs to the end.
std::string strip_html_comments(const std::string& s)
{
    std::string out;
    size_t pos = 0;
    for (;;) {
        size_t b = s.find("<!--", pos);
        if (b == std::string::npos) { out.append(s, pos, std::string::npos); break; }
        out.append(s, pos, b - pos);
        size_t e = s.find("-->", b + 4);
        if (e == std::string::npos) break;
        pos = e + 3;
    }
    return out;
}

// [text](url) -> text, ![alt](url) -> nothing, [text][ref] -> text, <https://x> -> https://x.
std::string strip_links(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const bool image = s[i] == '!' && i + 1 < s.size() && s[i + 1] == '[';
        if (s[i] == '[' || image) {
            const size_t open  = image ? i + 1 : i;
            const size_t close = s.find(']', open + 1);
            if (close != std::string::npos && close + 1 < s.size() && (s[close + 1] == '(' || s[close + 1] == '[')) {
                const char   closer = s[close + 1] == '(' ? ')' : ']';
                const size_t end    = s.find(closer, close + 2);
                if (end != std::string::npos) {
                    if (!image) out += s.substr(open + 1, close - open - 1);
                    i = end + 1;
                    continue;
                }
            }
        }
        if (s[i] == '<' && (s.compare(i + 1, 8, "https://") == 0 || s.compare(i + 1, 7, "http://") == 0)) {
            const size_t end = s.find('>', i + 1);
            if (end != std::string::npos) {
                out += s.substr(i + 1, end - i - 1);
                i = end + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// Removes the HTML tags release notes realistically carry; anything else in angle brackets
// (such as "<name>.gcode.3mf") is text and stays.
std::string strip_html_tags(const std::string& s)
{
    static const std::regex br(R"(<br\s*/?>)", std::regex::icase);
    static const std::regex tags(R"(</?(p|div|span|details|summary|sub|sup|b|i|u|em|strong|img|a|kbd|code|pre|ul|ol|li|h[1-6]|center|table|tr|td|th|picture|source|video)(\s[^<>]*)?/?>)",
                                 std::regex::icase);
    std::string out = std::regex_replace(s, br, " ");
    return std::regex_replace(out, tags, "");
}

std::string decode_entities(std::string s)
{
    s = replace_all(s, "&nbsp;", " ");
    s = replace_all(s, "&lt;", "<");
    s = replace_all(s, "&gt;", ">");
    s = replace_all(s, "&quot;", "\"");
    s = replace_all(s, "&#39;", "'");
    s = replace_all(s, "&amp;", "&");
    return s;
}

// Emphasis and code markers out, backslash escapes resolved. A '*' or '_' counts as markup
// only at a word edge, so "snake_case" and "2*3" survive.
std::string strip_inline_markup(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            out += s[++i];
            continue;
        }
        if (c == '`')
            continue;
        if (c == '*' || c == '_') {
            size_t run = 1;
            while (i + run < s.size() && s[i + run] == c) ++run;
            const bool prev_word = i > 0 && is_word_char(s[i - 1]);
            const bool next_word = i + run < s.size() && is_word_char(s[i + run]);
            if (prev_word && next_word) {
                out.append(run, c); // intra-word: literal
            } else if (!prev_word && !next_word && (i + run >= s.size() || s[i + run] == ' ') && (i == 0 || s[i - 1] == ' ')) {
                out.append(run, c); // a lone "*" between spaces is not markup either
            }
            i += run - 1;
            continue;
        }
        out += c;
    }
    return out;
}

std::string collapse_spaces(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (char c : s) {
        const bool sp = c == ' ' || c == '\t';
        if (sp && prev_space) continue;
        out += sp ? ' ' : c;
        prev_space = sp;
    }
    return trim(out);
}

bool is_horizontal_rule(const std::string& t)
{
    if (t.size() < 3) return false;
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    size_t n = 0;
    for (char ch : t) {
        if (ch == c) ++n;
        else if (ch != ' ') return false;
    }
    return n >= 3;
}

bool is_table_separator(const std::string& t)
{
    if (t.find('-') == std::string::npos || t.find('|') == std::string::npos) return false;
    return std::all_of(t.begin(), t.end(), [](char c) { return c == '|' || c == '-' || c == ':' || c == ' '; });
}

// Cuts `s` to at most `budget` bytes at a word boundary (never inside a UTF-8 sequence).
std::string cut_at_word(const std::string& s, size_t budget)
{
    if (s.size() <= budget) return s;
    size_t cut = budget;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut; // UTF-8 continuation
    const size_t space = s.rfind(' ', cut);
    if (space != std::string::npos && space > budget / 2) cut = space;
    return trim(s.substr(0, cut));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Versions

std::string Version::to_string() const
{
    std::string s = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]) + "." +
                    std::to_string(parts[3]);
    if (!prerelease.empty()) s += "-" + prerelease;
    return s;
}

Version parse_version(const std::string& text)
{
    Version     v;
    std::string s = trim(text);
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    if (const size_t plus = s.find('+'); plus != std::string::npos) s.erase(plus); // build metadata

    std::string numbers = s, suffix;
    if (const size_t dash = s.find('-'); dash != std::string::npos) {
        numbers = s.substr(0, dash);
        suffix  = s.substr(dash + 1);
    }

    std::vector<std::string> items;
    size_t start = 0;
    for (;;) {
        const size_t dot = numbers.find('.', start);
        items.push_back(numbers.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (items.size() < 2 || items.size() > 4) return v;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!all_digits(items[i])) return v;
        v.parts[i] = std::atoi(items[i].c_str());
    }

    // "-edge" is the fork's tag suffix, not a prerelease. Anything else after the dash is.
    std::string pre;
    size_t      pos = 0;
    while (pos <= suffix.size() && !suffix.empty()) {
        const size_t dash  = suffix.find('-', pos);
        std::string  token = suffix.substr(pos, dash == std::string::npos ? std::string::npos : dash - pos);
        if (!token.empty() && to_lower(token) != "edge") {
            if (!pre.empty()) pre += "-";
            pre += token;
        }
        if (dash == std::string::npos) break;
        pos = dash + 1;
    }
    v.prerelease = pre;
    v.valid      = true;
    return v;
}

int compare_versions(const Version& a, const Version& b)
{
    if (a.valid != b.valid) return a.valid ? 1 : -1;
    if (!a.valid) return 0;
    for (size_t i = 0; i < a.parts.size(); ++i)
        if (a.parts[i] != b.parts[i]) return a.parts[i] < b.parts[i] ? -1 : 1;
    if (a.prerelease == b.prerelease) return 0;
    if (a.prerelease.empty()) return 1; // release beats prerelease
    if (b.prerelease.empty()) return -1;
    return a.prerelease < b.prerelease ? -1 : 1;
}

// ---------------------------------------------------------------------------------------------
// Nightly versions

std::string NightlyVersion::to_string() const
{
    std::string s = base.to_string() + "-nightly." + date;
    if (!commit.empty()) s += "+" + commit;
    return s;
}

std::string NightlyVersion::date_display() const
{
    return date.size() == 8 ? date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2) : date;
}

NightlyVersion parse_nightly(const std::string& text)
{
    // <number>-nightly.<8 digits>, then optionally [+-]<hex commit>, then nothing.
    static const std::regex re(R"(^[vV]?([0-9]+(?:\.[0-9]+){1,3})-nightly\.([0-9]{8})(?:[+-]([0-9A-Fa-f]{4,40}))?$)");
    NightlyVersion n;
    std::smatch    m;
    const std::string t = trim(text);
    if (!std::regex_match(t, m, re)) return n;
    n.base = parse_version(m[1].str());
    if (!n.base.valid) return n;
    n.date   = m[2].str();
    n.commit = to_lower(m[3].str());
    const int month = std::atoi(n.date.substr(4, 2).c_str());
    const int day   = std::atoi(n.date.substr(6, 2).c_str());
    n.valid = month >= 1 && month <= 12 && day >= 1 && day <= 31;
    return n;
}

bool is_nightly_version(const std::string& text) { return parse_nightly(text).valid; }

int compare_nightly(const NightlyVersion& a, const NightlyVersion& b)
{
    if (a.valid != b.valid) return a.valid ? 1 : -1;
    if (!a.valid) return 0;
    if (const int c = compare_versions(a.base, b.base); c != 0) return c;
    if (a.date != b.date) return a.date < b.date ? -1 : 1; // YYYYMMDD orders as text
    return 0;
}

bool nightly_is_newer(const NightlyVersion& remote, const NightlyVersion& local)
{
    if (!remote.valid) return false;
    if (!local.valid) return true;
    const int c = compare_nightly(remote, local);
    if (c != 0) return c > 0;
    // Same release number and day: commits cannot be ordered by their hashes, but the nightly
    // release only ever holds the latest build, so a different commit is the newer one.
    // Hashes of different lengths name the same commit when one starts with the other.
    if (remote.commit.empty() || local.commit.empty()) return false;
    return !starts_with(remote.commit, local.commit) && !starts_with(local.commit, remote.commit);
}

Channel channel_from_config(const std::string& value) { return to_lower(trim(value)) == "nightly" ? Channel::Nightly : Channel::Stable; }

const char* channel_config_value(Channel channel) { return channel == Channel::Nightly ? "nightly" : "stable"; }

bool is_skipped(const std::string& candidate, const std::string& skip_version)
{
    if (trim(skip_version).empty()) return false;
    const NightlyVersion cn = parse_nightly(candidate);
    const NightlyVersion sn = parse_nightly(skip_version);
    if (cn.valid || sn.valid) {
        // A skipped nightly hides that build and older ones; it says nothing about releases.
        if (cn.valid && sn.valid) return !nightly_is_newer(cn, sn);
        return false;
    }
    const Version c = parse_version(candidate);
    const Version s = parse_version(skip_version);
    if (c.valid && s.valid) return compare_versions(c, s) <= 0;
    return trim(candidate) == trim(skip_version);
}

Platform current_platform()
{
#if defined(_WIN32)
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::MacOS;
#else
    const char* flatpak_id = std::getenv("FLATPAK_ID");
    if ((flatpak_id != nullptr && *flatpak_id != '\0') || boost::filesystem::exists("/.flatpak-info"))
        return Platform::LinuxFlatpak;
    return Platform::Linux;
#endif
}

std::string effective_local_version(const std::string& local_version)
{
    const char* fake = std::getenv("EDGESLICER_FAKE_LOCAL_VERSION");
    if (fake == nullptr || *fake == '\0') return local_version;
    return parse_version(fake).valid ? std::string(fake) : local_version;
}

// ---------------------------------------------------------------------------------------------
// Release notes

std::string markdown_line_to_plain(const std::string& raw)
{
    std::string line = raw;
    // Tabs count as four columns for nesting, like GitHub's renderer.
    line = replace_all(line, "\t", "    ");
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    std::string t = trim(line);

    // Block quotes: "> text", possibly nested.
    while (!t.empty() && t[0] == '>') t = trim(t.substr(1));

    if (t.empty() || is_horizontal_rule(t) || is_table_separator(t)) return {};
    if (starts_with(t, "```") || starts_with(t, "~~~")) return {};

    std::string prefix;
    if (t[0] == '#') {
        size_t level = 0;
        while (level < t.size() && t[level] == '#') ++level;
        if (level <= 6 && (level == t.size() || t[level] == ' ')) {
            t = trim(t.substr(level));
            while (!t.empty() && t.back() == '#') t.pop_back(); // closing hashes
            t = trim(t);
        }
    } else if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
        t = trim(t.substr(2));
        if (starts_with(t, "[ ] ") || starts_with(t, "[x] ") || starts_with(t, "[X] ")) t = trim(t.substr(4));
        prefix = std::string(std::min<size_t>(indent / 2, 4), ' ') + BULLET;
    } else if (t[0] == '|') {
        // A table row: keep the cells, drop the pipes.
        std::string cells = t;
        if (!cells.empty() && cells.front() == '|') cells.erase(0, 1);
        if (!cells.empty() && cells.back() == '|') cells.pop_back();
        t = replace_all(cells, "|", " \xE2\x80\x93 "); // en dash between cells
    } else {
        // "1. item" / "1) item" keep their number; nested ones keep a little indent.
        size_t d = 0;
        while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
        if (d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ')
            prefix = std::string(std::min<size_t>(indent / 2, 4), ' ');
    }

    t = strip_html_comments(t);
    t = strip_links(t);
    t = strip_html_tags(t);
    t = strip_inline_markup(t);
    t = decode_entities(t);
    t = collapse_spaces(t);
    if (t.empty()) return {};
    return prefix + t;
}

CompactNotes compact_release_notes(const std::string& body_in, size_t max_lines, size_t max_chars)
{
    CompactNotes notes;
    std::string  body = replace_all(body_in, "\r\n", "\n");
    body              = replace_all(body, "\r", "\n");

    // The markers are HTML comments; accept any spacing and case inside them.
    static const std::regex begin_re(R"(<!--\s*update-notice\s*-->)", std::regex::icase);
    static const std::regex end_re(R"(<!--\s*/\s*update-notice\s*-->)", std::regex::icase);
    std::smatch             bm;
    std::string             section = body;
    if (std::regex_search(body, bm, begin_re)) {
        const size_t    after = size_t(bm.position(0) + bm.length(0));
        const std::string rest = body.substr(after);
        std::smatch     em;
        if (std::regex_search(rest, em, end_re)) {
            const std::string candidate = rest.substr(0, size_t(em.position(0)));
            if (!trim(strip_html_comments(candidate)).empty()) {
                section            = candidate;
                notes.from_markers = true;
                max_lines          = std::max(max_lines, MARKER_MAX_LINES);
                max_chars          = std::max(max_chars, MARKER_MAX_CHARS);
            }
        }
    }

    section = strip_html_comments(section);

    std::vector<std::string> out;
    size_t                   used_chars = 0, used_lines = 0;
    bool                     in_fence = false;
    size_t                   start    = 0;
    while (start <= section.size()) {
        const size_t nl  = section.find('\n', start);
        std::string  raw = section.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start            = nl == std::string::npos ? section.size() + 1 : nl + 1;

        const std::string t = trim(raw);
        if (starts_with(t, "```") || starts_with(t, "~~~")) { in_fence = !in_fence; continue; }
        if (in_fence) continue; // code blocks do not belong in a notice

        const std::string plain = markdown_line_to_plain(raw);
        if (plain.empty()) {
            if (!out.empty() && !out.back().empty()) out.emplace_back(); // one blank between paragraphs
            continue;
        }
        if (used_lines >= max_lines) { notes.truncated = true; break; }

        const size_t remaining = max_chars > used_chars ? max_chars - used_chars : 0;
        if (plain.size() > remaining) {
            notes.truncated = true;
            if (remaining >= 40) out.push_back(cut_at_word(plain, remaining) + ELLIPSIS);
            break;
        }
        out.push_back(plain);
        used_chars += plain.size();
        ++used_lines;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();

    for (size_t i = 0; i < out.size(); ++i) {
        if (i) notes.text += "\n";
        notes.text += out[i];
    }
    return notes;
}

// ---------------------------------------------------------------------------------------------
// GitHub answer

namespace {

std::string json_string(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool json_bool(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

bool trusted(const std::string& url) { return starts_with(url, GITHUB_TRUSTED_URL_PREFIX); }

std::string pick_asset(const nlohmann::json& release, Platform platform)
{
    auto it = release.find("assets");
    if (it == release.end() || !it->is_array()) return {};

    struct Asset { std::string name, url; };
    std::vector<Asset> assets;
    for (const auto& a : *it) {
        if (!a.is_object()) continue;
        Asset asset { json_string(a, "name"), json_string(a, "browser_download_url") };
        if (!asset.name.empty() && trusted(asset.url)) assets.push_back(asset);
    }

    auto first = [&](auto pred) -> std::string {
        for (const auto& a : assets)
            if (pred(a.name)) return a.url;
        return {};
    };

    std::string url;
    switch (platform) {
    case Platform::Windows:
        url = first([](const std::string& n) { return ends_with_icase(n, ".exe") && contains_icase(n, "installer"); });
        if (url.empty()) url = first([](const std::string& n) { return ends_with_icase(n, ".exe"); });
        break;
    case Platform::MacOS: {
        url = first([](const std::string& n) { return ends_with_icase(n, ".dmg") && contains_icase(n, "universal"); });
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
        const bool arm = true;
#else
        const bool arm = false;
#endif
        if (url.empty())
            url = first([arm](const std::string& n) {
                if (!ends_with_icase(n, ".dmg")) return false;
                return arm ? (contains_icase(n, "arm64") || contains_icase(n, "aarch64") || contains_icase(n, "apple"))
                           : (contains_icase(n, "x86_64") || contains_icase(n, "x64") || contains_icase(n, "intel"));
            });
        if (url.empty()) url = first([](const std::string& n) { return ends_with_icase(n, ".dmg"); });
        break;
    }
    case Platform::Linux:
    case Platform::LinuxFlatpak:
        // AppImage/tarball users pick their build on the release page.
        break;
    }
    return url;
}

// "2026-09-26T13:22:38Z" -> "20260926"; anything else -> "".
std::string date_of(const std::string& iso)
{
    static const std::regex re(R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})T)");
    std::smatch m;
    if (!std::regex_search(iso, m, re)) return {};
    return m[1].str() + m[2].str() + m[3].str();
}

// Parses the JSON object and the tag; false (with info.error set) when it is not a release.
bool parse_release_object(const std::string& json_body, nlohmann::json& j, ReleaseInfo& info)
{
    try {
        j = nlohmann::json::parse(json_body);
    } catch (const std::exception& e) {
        info.error = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        info.error = "unexpected answer (not a JSON object)";
        return false;
    }
    info.tag = json_string(j, "tag_name");
    if (info.tag.empty()) {
        // GitHub reports rate limits and missing releases as {"message": "..."}.
        const std::string message = json_string(j, "message");
        info.error                = message.empty() ? "no tag_name in the answer" : "GitHub: " + message;
        return false;
    }
    return true;
}

// Links, flags, date and notes: everything but the version.
void fill_release_details(const nlohmann::json& j, Platform platform, ReleaseInfo& info)
{
    info.draft          = json_bool(j, "draft");
    info.published_date = date_of(json_string(j, "published_at"));

    info.html_url = json_string(j, "html_url");
    if (!trusted(info.html_url)) info.html_url = GITHUB_RELEASES_PAGE;

    info.download_url = pick_asset(j, platform);
    if (info.download_url.empty()) info.download_url = info.html_url;

    info.notes = compact_release_notes(json_string(j, "body"));
}

// The nightly's version from the body marker, else from the first asset name that carries one.
NightlyVersion nightly_version_of(const nlohmann::json& j)
{
    static const std::regex marker(R"(<!--\s*nightly-version\s*:\s*([^\s>]+)\s*-->)", std::regex::icase);
    static const std::regex in_name(R"([vV]?[0-9]+(?:\.[0-9]+){1,3}-nightly\.[0-9]{8}(?:[+-][0-9A-Fa-f]{4,40})?)");
    const std::string body = json_string(j, "body");
    std::smatch       m;
    if (std::regex_search(body, m, marker)) {
        const NightlyVersion n = parse_nightly(m[1].str());
        if (n.valid) return n;
    }
    if (auto it = j.find("assets"); it != j.end() && it->is_array()) {
        for (const auto& a : *it) {
            if (!a.is_object()) continue;
            const std::string name = json_string(a, "name");
            if (std::regex_search(name, m, in_name)) {
                const NightlyVersion n = parse_nightly(m[0].str());
                if (n.valid) return n;
            }
        }
    }
    return {};
}

} // namespace

ReleaseInfo parse_github_release(const std::string& json_body, Platform platform)
{
    ReleaseInfo    info;
    nlohmann::json j;
    if (!parse_release_object(json_body, j, info)) return info;

    info.version = parse_version(info.tag);
    if (!info.version.valid) {
        info.error = "unrecognised release tag '" + info.tag + "'";
        return info;
    }
    info.version_str = info.version.to_string();
    info.prerelease  = json_bool(j, "prerelease") || !info.version.prerelease.empty();
    fill_release_details(j, platform, info);
    info.ok = true;
    return info;
}

ReleaseInfo parse_github_nightly(const std::string& json_body, Platform platform)
{
    ReleaseInfo    info;
    nlohmann::json j;
    if (!parse_release_object(json_body, j, info)) return info;

    info.nightly = nightly_version_of(j);
    if (!info.nightly.valid) {
        info.error = "the nightly release names no nightly version";
        return info;
    }
    info.version     = info.nightly.base;
    info.version_str = info.nightly.to_string();
    info.prerelease  = true;
    fill_release_details(j, platform, info);
    if (!info.notes.from_markers) {
        info.notes      = CompactNotes();
        info.notes.text = "New nightly build (" + info.nightly.date_display() +
                               (info.nightly.commit.empty() ? std::string() : ", " + info.nightly.commit) + ").";
    }
    info.ok = true;
    return info;
}

Verdict evaluate(const ReleaseInfo& release, const std::string& local_version)
{
    if (!release.ok || !release.version.valid) return Verdict::Invalid;
    if (release.draft || release.prerelease) return Verdict::Ignored;
    if (const NightlyVersion nightly = parse_nightly(local_version); nightly.valid) {
        const int c = compare_versions(release.version, nightly.base);
        if (c != 0) return c > 0 ? Verdict::UpdateAvailable : Verdict::UpToDate;
        // Same release number: the nightly may predate the release (main is bumped to the next
        // version ahead of the release) or follow it (not bumped yet). The dates tell which.
        if (release.published_date.empty()) return Verdict::UpdateAvailable;
        return release.published_date > nightly.date ? Verdict::UpdateAvailable : Verdict::UpToDate;
    }
    const Version local = parse_version(local_version);
    if (!local.valid) return Verdict::Invalid;
    return compare_versions(release.version, local) > 0 ? Verdict::UpdateAvailable : Verdict::UpToDate;
}

Verdict evaluate_nightly(const ReleaseInfo& nightly_release, const std::string& local_version)
{
    if (!nightly_release.ok || !nightly_release.nightly.valid) return Verdict::Invalid;
    if (nightly_release.draft) return Verdict::Ignored;
    if (const NightlyVersion local = parse_nightly(local_version); local.valid)
        return nightly_is_newer(nightly_release.nightly, local) ? Verdict::UpdateAvailable : Verdict::UpToDate;
    const Version local = parse_version(local_version);
    if (!local.valid) return Verdict::Invalid;
    // Compare release numbers only: 2.4.1.0-nightly.X is a build of the 2.4.1.0 line.
    Version local_base    = local;
    local_base.prerelease = std::string();
    return compare_versions(nightly_release.nightly.base, local_base) >= 0 ? Verdict::UpdateAvailable : Verdict::UpToDate;
}

} // namespace AppUpdate
} // namespace Slic3r
