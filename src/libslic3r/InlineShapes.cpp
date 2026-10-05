#include "InlineShapes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "nlohmann/json.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "NSVGUtils.hpp"
#include "Utils.hpp" // resources_dir()

namespace Slic3r {

// ---------------------------------------------------------------------------------------------
// anchors
// ---------------------------------------------------------------------------------------------
const char *to_string(InlineShapeAnchor anchor)
{
    switch (anchor) {
    case InlineShapeAnchor::XHeightCenter: return "x_height_center";
    case InlineShapeAnchor::CapCenter: return "cap_center";
    case InlineShapeAnchor::Baseline:
    default: return "baseline";
    }
}

std::optional<InlineShapeAnchor> inline_shape_anchor_from_string(const std::string &name)
{
    if (name == "baseline")
        return InlineShapeAnchor::Baseline;
    if (name == "x_height_center")
        return InlineShapeAnchor::XHeightCenter;
    if (name == "cap_center")
        return InlineShapeAnchor::CapCenter;
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// table
// ---------------------------------------------------------------------------------------------
bool InlineShape::same_content(const InlineShape &o) const
{
    if (source != o.source || id != o.id || scale != o.scale || dy != o.dy || gap_l != o.gap_l || gap_r != o.gap_r ||
        flip_x != o.flip_x || anchor != o.anchor)
        return false;
    if (source == InlineShapeSource::Svg) {
        // the data decides, not the name
        const bool a = svg_data != nullptr, b = o.svg_data != nullptr;
        if (a != b)
            return false;
        if (a && svg_data != o.svg_data && *svg_data != *o.svg_data)
            return false;
    }
    return true;
}

const InlineShape *find_inline_shape(const InlineShapeTable &table, uint32_t code)
{
    for (const InlineShape &s : table)
        if (s.code == code)
            return &s;
    return nullptr;
}

std::optional<uint16_t> allocate_inline_shape_code(const InlineShapeTable &table, const CodePointPredicate &font_maps)
{
    for (uint32_t cp = INLINE_SHAPE_CODE_FIRST; cp <= INLINE_SHAPE_CODE_LAST; ++cp) {
        if (find_inline_shape(table, cp) != nullptr)
            continue;
        if (font_maps && font_maps(cp))
            continue;
        return static_cast<uint16_t>(cp);
    }
    return std::nullopt;
}

std::optional<uint16_t> add_inline_shape(InlineShapeTable &table, InlineShape entry, const CodePointPredicate &font_maps)
{
    for (const InlineShape &s : table)
        if (s.same_content(entry))
            return s.code;
    if (table.size() >= InlineShapeLimits::max_shapes_per_text)
        return std::nullopt;
    std::optional<uint16_t> code = allocate_inline_shape_code(table, font_maps);
    if (!code.has_value())
        return std::nullopt;
    entry.code = *code;
    table.push_back(std::move(entry));
    return code;
}

// ---------------------------------------------------------------------------------------------
// text helpers
// ---------------------------------------------------------------------------------------------
std::vector<uint32_t> utf8_to_codepoints(const std::string &s)
{
    std::vector<uint32_t> out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t       i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t            cp = 0xFFFD;
        size_t              len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp  = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp  = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp  = c & 0x07;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (len > 1) {
            bool ok = i + len <= n;
            for (size_t k = 1; ok && k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80)
                    ok = false;
                else
                    cp = (cp << 6) | (cc & 0x3F);
            }
            if (!ok || cp > 0x10FFFF) {
                out.push_back(0xFFFD);
                ++i; // resynchronise on the next byte
                continue;
            }
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

static void append_utf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string codepoints_to_utf8(const std::vector<uint32_t> &cps)
{
    std::string out;
    out.reserve(cps.size());
    for (uint32_t cp : cps)
        append_utf8(out, cp);
    return out;
}

std::string inline_shape_placeholder(const InlineShape &shape)
{
    std::string out;
    append_utf8(out, shape.code);
    return out;
}

size_t count_inline_placeholders(const std::string &utf8)
{
    size_t n = 0;
    for (uint32_t cp : utf8_to_codepoints(utf8))
        if (is_inline_shape_code(cp))
            ++n;
    return n;
}

std::map<uint16_t, size_t> count_inline_shape_uses(const std::string &utf8, const InlineShapeTable &table)
{
    std::map<uint16_t, size_t> uses;
    for (uint32_t cp : utf8_to_codepoints(utf8))
        if (is_inline_shape_code(cp) && find_inline_shape(table, cp) != nullptr)
            ++uses[static_cast<uint16_t>(cp)];
    return uses;
}

size_t purge_unused_inline_shapes(InlineShapeTable &table, const std::string &utf8)
{
    const std::map<uint16_t, size_t> uses = count_inline_shape_uses(utf8, table);
    const size_t                     before = table.size();
    table.erase(std::remove_if(table.begin(), table.end(), [&uses](const InlineShape &s) { return uses.find(s.code) == uses.end(); }),
                table.end());
    return before - table.size();
}

std::string inline_text_display_name(const std::string &utf8, const InlineShapeTable &table)
{
    std::string out;
    out.reserve(utf8.size());
    for (uint32_t cp : utf8_to_codepoints(utf8)) {
        if (!is_inline_shape_code(cp)) {
            append_utf8(out, cp);
            continue;
        }
        const InlineShape *shape = find_inline_shape(table, cp);
        if (shape == nullptr)
            continue; // unknown placeholder
        out += '[';
        out += shape->id.empty() ? std::string("shape") : shape->id;
        out += ']';
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------
namespace {
// FNV-1a
uint64_t fnv1a64(const std::string &s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// 4 decimals: the JSON stays short and a value typed in the UI round-trips exactly
double round4(float v) { return std::round(static_cast<double>(v) * 10000.) / 10000.; }

float clamp_param(const nlohmann::json &j, const char *key, float def, float lo, float hi)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return def;
    const double v = it->get<double>();
    if (!std::isfinite(v))
        return def;
    return static_cast<float>(std::min<double>(hi, std::max<double>(lo, v)));
}

// "3D/inline_<letters, digits, _ or ->.svg", nothing that could leave the archive folder
bool is_valid_inline_svg_entry_name(const std::string &name)
{
    static const char  prefix[] = "3D/inline_";
    static const char  suffix[] = ".svg";
    const size_t       pl = sizeof(prefix) - 1, sl = sizeof(suffix) - 1;
    if (name.size() <= pl + sl || name.size() > pl + sl + 64)
        return false;
    if (name.compare(0, pl, prefix) != 0 || name.compare(name.size() - sl, sl, suffix) != 0)
        return false;
    for (size_t i = pl; i + sl < name.size(); ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}
} // namespace

std::string inline_svg_entry_name(const std::string &svg_data)
{
    const uint32_t h = static_cast<uint32_t>(fnv1a64(svg_data) ^ (fnv1a64(svg_data) >> 32));
    char           buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(h));
    return std::string("3D/inline_") + buf + ".svg";
}

void assign_inline_svg_entry_names(InlineShapeTable &table)
{
    for (InlineShape &s : table)
        if (s.source == InlineShapeSource::Svg && s.path_in_3mf.empty() && s.svg_data)
            s.path_in_3mf = inline_svg_entry_name(*s.svg_data);
}

size_t attach_inline_svg(InlineShapeTable &table, const std::string &entry_name, std::shared_ptr<const std::string> data)
{
    size_t n = 0;
    if (data == nullptr)
        return n;
    for (InlineShape &s : table)
        if (s.source == InlineShapeSource::Svg && s.path_in_3mf == entry_name) {
            s.svg_data = data;
            ++n;
        }
    return n;
}

std::string inline_shapes_to_json(const InlineShapeTable &table)
{
    if (table.empty())
        return {};
    const InlineShape def;
    nlohmann::json    arr = nlohmann::json::array();
    for (const InlineShape &s : table) {
        nlohmann::json j;
        j["c"]  = s.code;
        j["k"]  = s.source == InlineShapeSource::Svg ? "f" : "b";
        j["id"] = s.id;
        if (s.source == InlineShapeSource::Svg) {
            const std::string path = !s.path_in_3mf.empty() ? s.path_in_3mf : (s.svg_data ? inline_svg_entry_name(*s.svg_data) : std::string());
            if (!path.empty())
                j["f"] = path;
        }
        if (s.scale != def.scale)
            j["s"] = round4(s.scale);
        if (s.dy != def.dy)
            j["dy"] = round4(s.dy);
        if (s.gap_l != def.gap_l)
            j["gl"] = round4(s.gap_l);
        if (s.gap_r != def.gap_r)
            j["gr"] = round4(s.gap_r);
        if (s.flip_x)
            j["fx"] = true;
        if (s.anchor != def.anchor)
            j["a"] = static_cast<int>(s.anchor);
        arr.push_back(std::move(j));
    }
    return arr.dump();
}

std::optional<InlineShapeTable> inline_shapes_from_json(const std::string &json, size_t *skipped)
{
    if (skipped)
        *skipped = 0;
    if (json.empty() || json.size() > 256 * 1024)
        return std::nullopt;
    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(json, nullptr, false);
    } catch (...) {
        return std::nullopt;
    }
    if (arr.is_discarded() || !arr.is_array())
        return std::nullopt;

    InlineShapeTable out;
    std::set<uint32_t> seen;
    auto skip = [&skipped]() {
        if (skipped)
            ++*skipped;
    };
    for (const nlohmann::json &j : arr) {
        if (out.size() >= InlineShapeLimits::max_shapes_per_text) {
            skip();
            continue;
        }
        if (!j.is_object()) {
            skip();
            continue;
        }
        auto c = j.find("c");
        auto k = j.find("k");
        if (c == j.end() || !c->is_number_integer() || k == j.end() || !k->is_string()) {
            skip();
            continue;
        }
        const long long code = c->get<long long>();
        if (code < static_cast<long long>(INLINE_SHAPE_CODE_FIRST) || code > static_cast<long long>(INLINE_SHAPE_CODE_LAST) ||
            !seen.insert(static_cast<uint32_t>(code)).second) {
            skip();
            continue;
        }
        InlineShape s;
        s.code = static_cast<uint16_t>(code);
        const std::string kind = k->get<std::string>();
        if (kind == "b")
            s.source = InlineShapeSource::Builtin;
        else if (kind == "f")
            s.source = InlineShapeSource::Svg;
        else {
            skip();
            continue;
        }
        auto id = j.find("id");
        if (id != j.end() && id->is_string())
            s.id = id->get<std::string>().substr(0, 128);
        if (s.source == InlineShapeSource::Builtin && s.id.empty()) {
            skip();
            continue;
        }
        if (s.source == InlineShapeSource::Svg) {
            auto f = j.find("f");
            if (f == j.end() || !f->is_string() || !is_valid_inline_svg_entry_name(f->get<std::string>())) {
                skip();
                continue;
            }
            s.path_in_3mf = f->get<std::string>();
        }
        s.scale = clamp_param(j, "s", s.scale, 0.05f, 20.f);
        s.dy    = clamp_param(j, "dy", s.dy, -5.f, 5.f);
        s.gap_l = clamp_param(j, "gl", s.gap_l, -0.5f, 2.f);
        s.gap_r = clamp_param(j, "gr", s.gap_r, -0.5f, 2.f);
        if (auto fx = j.find("fx"); fx != j.end() && fx->is_boolean())
            s.flip_x = fx->get<bool>();
        if (auto a = j.find("a"); a != j.end() && a->is_number_integer()) {
            const long long av = a->get<long long>();
            if (av >= 0 && av <= static_cast<long long>(InlineShapeAnchor::CapCenter))
                s.anchor = static_cast<InlineShapeAnchor>(av);
        }
        out.push_back(std::move(s));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// loading SVG
// ---------------------------------------------------------------------------------------------
ExPolygons silhouette_of(const ExPolygonsWithIds &shapes)
{
    ExPolygons all;
    for (const ExPolygonsWithId &s : shapes)
        expolygons_append(all, s.expoly);
    return union_ex(all);
}

namespace {
size_t inline_shape_point_count(const ExPolygons &shape)
{
    size_t n = 0;
    for (const ExPolygon &e : shape) {
        n += e.contour.points.size();
        for (const Polygon &h : e.holes)
            n += h.points.size();
    }
    return n;
}

void translate_shape(ExPolygons &shape, coord_t dx, coord_t dy)
{
    for (ExPolygon &e : shape)
        e.translate(Point(dx, dy));
}

bool fail(std::string *error, const char *msg)
{
    if (error)
        *error = msg;
    return false;
}
} // namespace

std::optional<InlineUnitShape> load_inline_svg(const std::string &svg, InlineBoxMode mode, std::string *error)
{
    try {
        if (svg.empty()) {
            fail(error, "empty SVG");
            return std::nullopt;
        }
        if (svg.size() > InlineShapeLimits::max_svg_bytes) {
            fail(error, "SVG is too large");
            return std::nullopt;
        }
        NSVGimage_ptr image = nsvgParse(svg, "px"); // "px": image size and path coordinates stay in the same units (the default "mm" rescales only the paths)
        if (image == nullptr) {
            fail(error, "SVG cannot be parsed");
            return std::nullopt;
        }

        // caps before any geometry is built
        size_t shapes = 0, points = 0;
        for (NSVGshape *s = image->shapes; s != nullptr; s = s->next) {
            if (++shapes > InlineShapeLimits::max_svg_shapes) {
                fail(error, "SVG has too many shapes");
                return std::nullopt;
            }
            for (const NSVGpath *p = s->paths; p != nullptr; p = p->next)
                points += static_cast<size_t>(std::max(p->npts, 0));
            if (points > InlineShapeLimits::max_svg_control_points) {
                fail(error, "SVG has too many points");
                return std::nullopt;
            }
            // dashes would only multiply the work (a silhouette is a solid outline anyway)
            s->strokeDashCount = 0;
        }
        if (shapes == 0) {
            fail(error, "SVG has no shapes");
            return std::nullopt;
        }

        // Size of the box (design box, or control point bounds for a user SVG) that sets the coordinate scale.
        // A design box becomes INLINE_SHAPE_UNIT high; a user SVG is scaled so its longer side is
        // INLINE_SHAPE_UNIT and measured again from the geometry (control points ignore stroke width).
        double box_w_img = 0., box_h_img = 0.;
        if (mode == InlineBoxMode::DesignBox) {
            box_w_img = image->width;
            box_h_img = image->height;
        } else {
            Vec2f mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
            Vec2f mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
            bounds(*image, mn, mx);
            box_w_img = static_cast<double>(mx.x()) - mn.x();
            box_h_img = static_cast<double>(mx.y()) - mn.y();
        }
        if (!std::isfinite(box_w_img) || !std::isfinite(box_h_img) || box_w_img < 0. || box_h_img < 0.) {
            fail(error, "SVG has no usable size");
            return std::nullopt;
        }
        const double scale_ref = mode == InlineBoxMode::DesignBox ? box_h_img : std::max(box_w_img, box_h_img);
        if (!(scale_ref > 1e-6) || (mode == InlineBoxMode::DesignBox && box_w_img <= 0.)) {
            fail(error, "SVG has no usable size");
            return std::nullopt;
        }
        if (mode == InlineBoxMode::DesignBox && box_w_img / box_h_img > InlineShapeLimits::max_aspect) {
            fail(error, "SVG is too wide for its height");
            return std::nullopt;
        }

        NSVGLineParams params(1e6); // flatten to about 1/1000 of the shape height (squared distance in scaled units)
        params.scale     = static_cast<double>(INLINE_SHAPE_UNIT) / scale_ref;
        params.max_level = 7;
        ExPolygonsWithIds shapes_with_ids = create_shape_with_ids(*image, params, /*center_result*/ false);
        ExPolygons        silhouette      = silhouette_of(shapes_with_ids);
        if (silhouette.empty()) {
            fail(error, "SVG has no visible area");
            return std::nullopt;
        }

        if (inline_shape_point_count(silhouette) > InlineShapeLimits::reject_above_points) {
            fail(error, "SVG outline is too detailed");
            return std::nullopt;
        }
        double tolerance = 1500.; // 0.15 % of the shape height
        for (int i = 0; inline_shape_point_count(silhouette) > InlineShapeLimits::simplify_above_points; ++i, tolerance *= 2.) {
            if (i >= 8) {
                fail(error, "SVG outline is too detailed");
                return std::nullopt;
            }
            silhouette = union_ex(expolygons_simplify(silhouette, tolerance));
            if (silhouette.empty()) {
                fail(error, "SVG has no visible area");
                return std::nullopt;
            }
        }

        InlineUnitShape out;
        out.ink_box = mode == InlineBoxMode::InkBox;
        if (mode == InlineBoxMode::DesignBox) {
            // y was negated (SVG y is down): the box is [0,w] x [-h,0]; move it up
            out.box_w = static_cast<coord_t>(std::llround(box_w_img * params.scale));
            out.box_h = static_cast<coord_t>(std::llround(box_h_img * params.scale));
            translate_shape(silhouette, 0, out.box_h);
        } else {
            const BoundingBox bb = get_extents(silhouette);
            translate_shape(silhouette, -bb.min.x(), -bb.min.y());
            out.box_w = bb.size().x();
            out.box_h = bb.size().y();
            if (out.box_w <= 0 || out.box_h <= 0) {
                fail(error, "SVG has no usable size");
                return std::nullopt;
            }
            if (static_cast<double>(out.box_w) / out.box_h > InlineShapeLimits::max_aspect ||
                static_cast<double>(out.box_h) / out.box_w > InlineShapeLimits::max_aspect) {
                fail(error, "SVG is too thin");
                return std::nullopt;
            }
        }
        out.advance_w = out.box_w;
        out.shape     = std::move(silhouette);
        for (const ExPolygon &e : out.shape)
            if (!e.is_valid()) {
                fail(error, "SVG outline is not valid");
                return std::nullopt;
            }
        return out;
    } catch (const std::exception &e) {
        if (error)
            *error = std::string("SVG failed: ") + e.what();
    } catch (...) {
        if (error)
            *error = "SVG failed";
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// built-in library
// ---------------------------------------------------------------------------------------------
namespace {
bool is_valid_shape_id(const std::string &id)
{
    if (id.empty() || id.size() > 48)
        return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

// plain file name, no folders
bool is_plain_file_name(const std::string &f)
{
    if (f.empty() || f.size() > 96 || f == "." || f == "..")
        return false;
    return f.find_first_of("/\\:") == std::string::npos && f.find("..") == std::string::npos;
}

float json_float(const nlohmann::json &j, const char *key, float def, float lo, float hi)
{
    return clamp_param(j, key, def, lo, hi);
}
} // namespace

bool BuiltinShapeLibrary::load(std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loaded = false;
    m_shapes.clear();
    m_cache.clear();
    std::unique_ptr<std::string> data = read_from_disk(m_dir + "/manifest.json");
    if (data == nullptr) {
        if (error)
            *error = "manifest.json not found in " + m_dir;
        return false;
    }
    nlohmann::json j = nlohmann::json::parse(*data, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("shapes") || !j["shapes"].is_array()) {
        if (error)
            *error = "manifest.json is not a valid shape manifest";
        return false;
    }
    std::set<std::string> ids;
    for (const nlohmann::json &e : j["shapes"]) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_string())
            continue;
        BuiltinShapeInfo info;
        info.id = e["id"].get<std::string>();
        if (!is_valid_shape_id(info.id) || !ids.insert(info.id).second)
            continue;
        info.name     = e.contains("name") && e["name"].is_string() ? e["name"].get<std::string>() : info.id;
        info.category = e.contains("category") && e["category"].is_string() ? e["category"].get<std::string>() : std::string();
        info.file     = e.contains("file") && e["file"].is_string() ? e["file"].get<std::string>() : info.id + ".svg";
        if (!is_plain_file_name(info.file))
            continue;
        if (e.contains("keywords") && e["keywords"].is_array())
            for (const nlohmann::json &kw : e["keywords"])
                if (kw.is_string())
                    info.keywords.push_back(kw.get<std::string>());
        if (e.contains("anchor") && e["anchor"].is_string())
            if (auto a = inline_shape_anchor_from_string(e["anchor"].get<std::string>()))
                info.anchor = *a;
        info.scale    = json_float(e, "scale", 1.f, 0.05f, 20.f);
        info.advance  = json_float(e, "advance", 1.f, 0.05f, 8.f);
        info.gap_l    = json_float(e, "gap_l", 0.06f, -0.5f, 2.f);
        info.gap_r    = json_float(e, "gap_r", 0.06f, -0.5f, 2.f);
        info.baseline = json_float(e, "baseline", 0.f, -1.f, 1.f);
        info.flip_x   = e.contains("flip_x") && e["flip_x"].is_boolean() && e["flip_x"].get<bool>();
        m_shapes.push_back(std::move(info));
    }
    if (m_shapes.empty()) {
        if (error)
            *error = "manifest.json lists no usable shape";
        return false;
    }
    m_loaded = true;
    return true;
}

const BuiltinShapeInfo *BuiltinShapeLibrary::find(const std::string &id) const
{
    for (const BuiltinShapeInfo &s : m_shapes)
        if (s.id == id)
            return &s;
    return nullptr;
}

std::optional<InlineShape> BuiltinShapeLibrary::make_entry(const std::string &id) const
{
    const BuiltinShapeInfo *info = find(id);
    if (info == nullptr)
        return std::nullopt;
    InlineShape s;
    s.source = InlineShapeSource::Builtin;
    s.id     = info->id;
    s.scale  = info->scale;
    s.gap_l  = info->gap_l;
    s.gap_r  = info->gap_r;
    s.flip_x = info->flip_x;
    s.anchor = info->anchor;
    return s;
}

std::shared_ptr<const InlineUnitShape> BuiltinShapeLibrary::unit_shape(const std::string &id) const
{
    const BuiltinShapeInfo *info = find(id);
    if (info == nullptr)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(id);
    if (it != m_cache.end())
        return it->second;
    std::shared_ptr<const InlineUnitShape> result;
    if (std::unique_ptr<std::string> svg = read_from_disk(m_dir + "/" + info->file)) {
        if (std::optional<InlineUnitShape> u = load_inline_svg(*svg, InlineBoxMode::DesignBox)) {
            u->advance_w = static_cast<coord_t>(std::llround(static_cast<double>(u->box_w) * info->advance));
            u->baseline  = info->baseline;
            result       = std::make_shared<const InlineUnitShape>(std::move(*u));
        }
    }
    m_cache.emplace(id, result); // a failure is cached too
    return result;
}

BuiltinShapeLibrary &BuiltinShapeLibrary::instance()
{
    static BuiltinShapeLibrary lib(resources_dir() + "/shapes/inline");
    static std::once_flag      once;
    std::call_once(once, [] { lib.load(); });
    return lib;
}

std::shared_ptr<const InlineUnitShape> InlineShapeCache::get(const InlineShape &entry, const BuiltinShapeLibrary &library)
{
    if (entry.source == InlineShapeSource::Builtin)
        return library.unit_shape(entry.id);
    if (entry.svg_data == nullptr || entry.svg_data->empty())
        return nullptr;
    const std::pair<uint64_t, size_t> key{fnv1a64(*entry.svg_data), entry.svg_data->size()};
    auto it = m_svg.find(key);
    if (it != m_svg.end())
        return it->second;
    std::shared_ptr<const InlineUnitShape> result;
    if (std::optional<InlineUnitShape> u = load_inline_svg(*entry.svg_data, InlineBoxMode::InkBox))
        result = std::make_shared<const InlineUnitShape>(std::move(*u));
    m_svg.emplace(key, result);
    return result;
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
std::optional<Emboss::Glyph> place_inline_shape(const InlineShape &e, const InlineUnitShape &u, const InlineFontMetrics &m)
{
    if (u.shape.empty() || u.box_w <= 0 || u.box_h <= 0 || !(m.cap_height > 0.) || !(m.em > 0.))
        return std::nullopt;

    const double scale = std::min(20., std::max(0.05, static_cast<double>(e.scale)));
    const double f     = m.cap_height * scale / static_cast<double>(u.box_h); // unit-box -> glyph units
    const double adv_box = static_cast<double>(u.advance_w > 0 ? u.advance_w : u.box_w) * f;
    const double box_w   = static_cast<double>(u.box_w) * f;
    const double x0      = (adv_box - box_w) / 2. + static_cast<double>(e.gap_l) * m.em;
    const double dy      = static_cast<double>(e.dy) * m.em;
    const double box_h   = static_cast<double>(u.box_h) * f;
    double       y0      = 0.;
    switch (e.anchor) {
    case InlineShapeAnchor::XHeightCenter: y0 = m.x_height / 2. - box_h / 2.; break;
    case InlineShapeAnchor::CapCenter: y0 = m.cap_height / 2. - box_h / 2.; break;
    case InlineShapeAnchor::Baseline:
    default: y0 = -u.baseline * box_h; break;
    }
    y0 += dy;

    Emboss::Glyph glyph;
    auto          map_polygon = [&](const Polygon &src) {
        Polygon dst;
        dst.points.reserve(src.points.size());
        for (const Point &p : src.points) {
            const double x = e.flip_x ? static_cast<double>(u.box_w) - static_cast<double>(p.x()) : static_cast<double>(p.x());
            dst.points.emplace_back(static_cast<coord_t>(std::llround(x * f + x0)),
                                    static_cast<coord_t>(std::llround(static_cast<double>(p.y()) * f + y0)));
        }
        if (e.flip_x)
            std::reverse(dst.points.begin(), dst.points.end()); // a mirror flips the winding
        return dst;
    };
    glyph.shape.reserve(u.shape.size());
    for (const ExPolygon &src : u.shape) {
        ExPolygon dst;
        dst.contour = map_polygon(src.contour);
        for (const Polygon &h : src.holes)
            dst.holes.push_back(map_polygon(h));
        glyph.shape.push_back(std::move(dst));
    }

    // the same two steps, in the same order, as the letters in Emboss get_glyph()
    if (m.boldness_delta != 0.)
        glyph.shape = union_ex(offset_ex(glyph.shape, static_cast<float>(m.boldness_delta)));
    if (m.skew.has_value()) {
        const double ratio = *m.skew;
        auto         skew  = [ratio](Polygon &polygon) {
            for (Point &p : polygon.points)
                p.x() += static_cast<coord_t>(std::llround(static_cast<double>(p.y()) * ratio));
        };
        for (ExPolygon &expolygon : glyph.shape) {
            skew(expolygon.contour);
            for (Polygon &hole : expolygon.holes)
                skew(hole);
        }
    }
    if (glyph.shape.empty())
        return std::nullopt;

    glyph.advance_width = static_cast<int>(std::llround(adv_box + (static_cast<double>(e.gap_l) + e.gap_r) * m.em)) + m.char_gap;
    if (glyph.advance_width < 0)
        glyph.advance_width = 0;
    glyph.left_side_bearing = static_cast<int>(get_extents(glyph.shape).min.x());
    return glyph;
}

std::optional<Emboss::Glyph> make_inline_glyph(const InlineShape &entry, const BuiltinShapeLibrary &library, InlineShapeCache &cache,
                                               const InlineFontMetrics &metrics)
{
    std::shared_ptr<const InlineUnitShape> unit = cache.get(entry, library);
    if (unit == nullptr)
        return std::nullopt;
    return place_inline_shape(entry, *unit, metrics);
}

} // namespace Slic3r
