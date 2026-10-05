#ifndef slic3r_InlineShapes_hpp_
#define slic3r_InlineShapes_hpp_

// Inline shapes in Emboss text: a vector shape (a built-in symbol or a user SVG) that behaves like one
// letter of the text.
//
// The text string holds a private-use "placeholder" character per shape; a per-volume table maps the
// placeholder to the shape and to its size / offset. This file is the pure part of the feature (no GUI,
// no 3MF, no text pipeline): the placeholder allocation, the table model, its JSON form for the 3MF
// attribute, loading a shape (built-in or embedded SVG) into ExPolygons, and turning that into a Glyph
// that the text layout can use exactly like a letter. Design: tests/research_inline_text_shapes.md.
//
// Units. A loaded shape lives in a "unit box": x to the right, y UP, the box is [0,box_w] x [0,box_h] with
// box_h about INLINE_SHAPE_UNIT. A built-in shape's box is the design box of its SVG (viewBox), so
// padding and proportions are kept (a minus stays a thin bar). A user SVG has no design box, so its box
// is the ink bounding box. place_inline_shape() then scales the box height to cap height * scale and
// puts it on the baseline (or centres it) in the integer units the text glyphs use.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

#include "ExPolygon.hpp"
#include "Emboss.hpp" // Emboss::Glyph

namespace Slic3r {

// ---- limits -----------------------------------------------------------------------------------
// Local caps. Another branch (fix/svg-size-caps) adds general SVG caps; reuse its constants once they
// land and delete the duplicates here.
struct InlineShapeLimits
{
    // SVG data accepted by load_inline_svg() (a 3MF entry, or the data of a user file)
    static constexpr size_t max_svg_bytes = 2u * 1024u * 1024u;
    // size of an SVG file a user may pick (checked by the UI before it reads the file)
    static constexpr size_t max_svg_file_pick_bytes = 512u * 1024u;
    // distinct shapes in one text volume
    static constexpr size_t max_shapes_per_text = 32;
    // placeholders in one text
    static constexpr size_t max_placeholders_per_text = 200;
    // nanosvg shapes / control points accepted before any geometry is built
    static constexpr size_t max_svg_shapes         = 2000;
    static constexpr size_t max_svg_control_points = 30000;
    // points of the flattened outline: above simplify_above_points the outline is simplified,
    // above reject_above_points (before simplification) the SVG is rejected
    static constexpr size_t simplify_above_points = 5000;
    static constexpr size_t reject_above_points   = 50000;
    // widest accepted aspect ratio of a shape (width / height)
    static constexpr double max_aspect = 64.;
};

// ---- placeholders -----------------------------------------------------------------------------
// BMP private-use range reserved for placeholders (ImWchar and Windows wchar_t are 16 bit).
// U+F8FF (the Apple logo in Apple fonts) is left out.
constexpr uint32_t INLINE_SHAPE_CODE_FIRST = 0xF700;
constexpr uint32_t INLINE_SHAPE_CODE_LAST  = 0xF8FE;

inline bool is_inline_shape_code(uint32_t cp) { return cp >= INLINE_SHAPE_CODE_FIRST && cp <= INLINE_SHAPE_CODE_LAST; }

// One coordinate unit of a loaded shape is 1/INLINE_SHAPE_UNIT of the design box height.
constexpr int INLINE_SHAPE_UNIT = 1000000;

// ---- table model ------------------------------------------------------------------------------
enum class InlineShapeSource : uint8_t { Builtin = 0, Svg = 1 };
// Where the shape sits vertically (see place_inline_shape).
enum class InlineShapeAnchor : uint8_t {
    Baseline      = 0, // bottom of the shape's box on the baseline
    XHeightCenter = 1, // centre of the box at half the x-height (bullet-like)
    CapCenter     = 2, // centre of the box at half the cap height (plus, minus)
};

const char *to_string(InlineShapeAnchor anchor);
std::optional<InlineShapeAnchor> inline_shape_anchor_from_string(const std::string &name);

struct InlineShape
{
    uint16_t          code   = 0; // placeholder (private-use) code point used in the text
    InlineShapeSource source = InlineShapeSource::Builtin;
    // Builtin: id in the built-in library ("star"). Svg: display name only.
    std::string id;
    // Svg: the embedded file. The local path is never stored (privacy).
    std::shared_ptr<const std::string> svg_data;
    // Svg: entry name in the 3MF ("3D/inline_<hash8>.svg"); empty until assigned.
    std::string path_in_3mf;

    // Placement, relative to the font so it follows the size slider.
    float             scale  = 1.f;  // 1 = cap height
    float             dy     = 0.f;  // baseline offset in em (+ up)
    float             gap_l  = 0.06f; // em, added before the shape
    float             gap_r  = 0.06f; // em, added after the shape
    bool              flip_x = false;
    InlineShapeAnchor anchor = InlineShapeAnchor::Baseline;

    // equal in everything but the placeholder code (used to share one code between equal entries)
    bool same_content(const InlineShape &other) const;

    template<class Archive> void save(Archive &ar) const
    {
        ar(code, static_cast<uint8_t>(source), id, path_in_3mf, scale, dy, gap_l, gap_r, flip_x, static_cast<uint8_t>(anchor));
        const bool has_data = svg_data != nullptr;
        ar(has_data);
        if (has_data)
            ar(*svg_data);
    }
    template<class Archive> void load(Archive &ar)
    {
        uint8_t source_u8 = 0, anchor_u8 = 0;
        ar(code, source_u8, id, path_in_3mf, scale, dy, gap_l, gap_r, flip_x, anchor_u8);
        source = source_u8 == static_cast<uint8_t>(InlineShapeSource::Svg) ? InlineShapeSource::Svg : InlineShapeSource::Builtin;
        anchor = anchor_u8 <= static_cast<uint8_t>(InlineShapeAnchor::CapCenter) ? static_cast<InlineShapeAnchor>(anchor_u8) :
                                                                                    InlineShapeAnchor::Baseline;
        bool has_data = false;
        ar(has_data);
        svg_data.reset();
        if (has_data) {
            std::string data;
            ar(data);
            svg_data = std::make_shared<const std::string>(std::move(data));
        }
    }
};
using InlineShapeTable = std::vector<InlineShape>;

// "is this code a code point the current font draws?" (so allocation can avoid icon-font codes)
using CodePointPredicate = std::function<bool(uint32_t)>;

const InlineShape *find_inline_shape(const InlineShapeTable &table, uint32_t code);

// Lowest free placeholder code that is not used by the table and not mapped by the font (font_maps may be
// empty). nullopt when the range is exhausted.
std::optional<uint16_t> allocate_inline_shape_code(const InlineShapeTable &table, const CodePointPredicate &font_maps = {});

// Adds the shape (entry.code is ignored) and returns its placeholder. An entry equal to an existing one
// (same_content) returns the existing code. nullopt when the table is full or no code is free.
std::optional<uint16_t> add_inline_shape(InlineShapeTable &table, InlineShape entry, const CodePointPredicate &font_maps = {});

// ---- text helpers (UTF-8 strings, as in TextConfiguration::text) ------------------------------
std::vector<uint32_t> utf8_to_codepoints(const std::string &utf8);
std::string           codepoints_to_utf8(const std::vector<uint32_t> &cps);
// UTF-8 of the placeholder character, for inserting into the text
std::string inline_shape_placeholder(const InlineShape &shape);
// number of placeholder-range characters in the text (whether or not the table knows them)
size_t count_inline_placeholders(const std::string &utf8);
// code -> number of uses, for codes the table knows
std::map<uint16_t, size_t> count_inline_shape_uses(const std::string &utf8, const InlineShapeTable &table);
// removes table entries the text does not use; returns how many were removed
size_t purge_unused_inline_shapes(InlineShapeTable &table, const std::string &utf8);
// text for names (volume name): placeholders become "[star]", unknown placeholders are dropped
std::string inline_text_display_name(const std::string &utf8, const InlineShapeTable &table);

// ---- JSON (3MF attribute) ---------------------------------------------------------------------
// Compact JSON array, e.g. [{"c":63232,"k":"b","id":"star","s":1.2},{"c":63233,"k":"f","id":"logo","f":"3D/inline_ab12cd34.svg"}]
// Only non-default parameters are written. The SVG data of "f" entries is NOT in the JSON: it travels as the
// zip entry named by "f" (see attach_inline_svg). An empty table gives "" (write no attribute).
std::string inline_shapes_to_json(const InlineShapeTable &table);
// Tolerant reader for untrusted input: malformed entries are skipped (counted in *skipped), values are
// clamped, at most max_shapes_per_text entries are kept. nullopt when the text is not a JSON array.
std::optional<InlineShapeTable> inline_shapes_from_json(const std::string &json, size_t *skipped = nullptr);
// "3D/inline_<hash8>.svg" for the data
std::string inline_svg_entry_name(const std::string &svg_data);
// gives Svg entries without an entry name the name derived from their data
void assign_inline_svg_entry_names(InlineShapeTable &table);
// hands the data of a 3MF entry to the Svg entries that reference it; returns the number of entries filled
size_t attach_inline_svg(InlineShapeTable &table, const std::string &entry_name, std::shared_ptr<const std::string> data);

// ---- loaded shape -----------------------------------------------------------------------------
struct InlineUnitShape
{
    ExPolygons shape;          // union of all visible fills and strokes, colours ignored (silhouette)
    coord_t    box_w = 0;      // design box (built-in, box_h = INLINE_SHAPE_UNIT) or ink box (user SVG, longer side about INLINE_SHAPE_UNIT)
    coord_t    box_h = 0;
    coord_t    advance_w = 0;  // width of the advance box; the design box is centred in it
    double     baseline = 0.;  // baseline as a fraction of box_h above the bottom edge (built-in manifest)
    bool       ink_box = false;
};

enum class InlineBoxMode { DesignBox, InkBox };

// Loads SVG data into a unit shape. All colours are ignored: every visible filled or stroked shape is
// unioned into one silhouette (holes and fill rule come from the SVG, strokes become filled outlines,
// dashes are ignored). Applies the caps in InlineShapeLimits; nullopt (with *error) on a malformed,
// empty or over-limit SVG. Never throws.
std::optional<InlineUnitShape> load_inline_svg(const std::string &svg, InlineBoxMode mode, std::string *error = nullptr);

// Union of all shapes of a parsed SVG regardless of colour (the silhouette step of load_inline_svg)
ExPolygons silhouette_of(const ExPolygonsWithIds &shapes);

// ---- built-in library -------------------------------------------------------------------------
struct BuiltinShapeInfo
{
    std::string              id;
    std::string              name;
    std::string              category;
    std::vector<std::string> keywords;
    std::string              file; // SVG file name inside the library directory
    InlineShapeAnchor        anchor   = InlineShapeAnchor::Baseline;
    float                    scale    = 1.f;
    float                    advance  = 1.f; // advance width as a fraction of the box width
    float                    gap_l    = 0.06f;
    float                    gap_r    = 0.06f;
    float                    baseline = 0.f;
    bool                     flip_x   = false;
};

class BuiltinShapeLibrary
{
public:
    // dir contains manifest.json and the SVG files (resources/shapes/inline)
    explicit BuiltinShapeLibrary(std::string dir) : m_dir(std::move(dir)) {}
    BuiltinShapeLibrary(const BuiltinShapeLibrary &) = delete;
    BuiltinShapeLibrary &operator=(const BuiltinShapeLibrary &) = delete;

    // reads the manifest; false (with *error) when it is missing or unusable
    bool load(std::string *error = nullptr);
    bool loaded() const { return m_loaded; }

    const std::string                   &dir() const { return m_dir; }
    const std::vector<BuiltinShapeInfo> &shapes() const { return m_shapes; }
    const BuiltinShapeInfo              *find(const std::string &id) const;

    // table entry with the manifest's default scale / anchor / gaps (code still to be allocated)
    std::optional<InlineShape> make_entry(const std::string &id) const;

    // Parsed once per library and cached; thread safe. nullptr when the id is unknown or its SVG is unusable.
    std::shared_ptr<const InlineUnitShape> unit_shape(const std::string &id) const;

    // process-wide library of the installed resources (resources_dir()/shapes/inline), loaded on first use
    static BuiltinShapeLibrary &instance();

private:
    std::string                                                              m_dir;
    bool                                                                     m_loaded = false;
    std::vector<BuiltinShapeInfo>                                            m_shapes;
    mutable std::mutex                                                       m_mutex;
    mutable std::map<std::string, std::shared_ptr<const InlineUnitShape>>    m_cache;
};

// Per layout call cache of embedded SVG entries (not shared between volumes: two volumes may reuse a code
// for different shapes, so the cache is keyed by content, never by code).
class InlineShapeCache
{
public:
    // unit shape of a table entry (built-in through the library, Svg parsed from its data); nullptr on failure
    std::shared_ptr<const InlineUnitShape> get(const InlineShape &entry, const BuiltinShapeLibrary &library);

private:
    std::map<std::pair<uint64_t, size_t>, std::shared_ptr<const InlineUnitShape>> m_svg;
};

// ---- layout -----------------------------------------------------------------------------------
// Font facts in the integer units the glyph shapes use (font units / SHAPE_SCALE in Emboss.cpp).
struct InlineFontMetrics
{
    double cap_height = 0.; // reference height, a shape of scale 1 is this tall
    double x_height   = 0.;
    double em         = 0.; // gaps and dy are fractions of this
    // as get_glyph() does for letters: outline offset (FontProp::boldness / SHAPE_SCALE / size_in_mm),
    // skew ratio x:y (FontProp::skew), extra advance (FontProp::char_gap / SHAPE_SCALE)
    double               boldness_delta = 0.;
    std::optional<double> skew;
    int                  char_gap = 0;
};

// Glyph of the shape for the text layout: same struct and conventions as Emboss get_glyph (shape in glyph
// space with the pen at x = 0, y up, baseline y = 0; advance_width and left_side_bearing in the same units;
// boldness and skew applied like letters). nullopt for an empty shape or unusable metrics.
std::optional<Emboss::Glyph> place_inline_shape(const InlineShape &entry, const InlineUnitShape &unit, const InlineFontMetrics &metrics);

// Resolves the entry (built-in library or embedded SVG through the per-call cache) and places it.
// nullopt means "unknown shape": the caller treats it like a missing glyph (zero width).
std::optional<Emboss::Glyph> make_inline_glyph(const InlineShape &entry, const BuiltinShapeLibrary &library, InlineShapeCache &cache,
                                               const InlineFontMetrics &metrics);

} // namespace Slic3r

#endif // slic3r_InlineShapes_hpp_
