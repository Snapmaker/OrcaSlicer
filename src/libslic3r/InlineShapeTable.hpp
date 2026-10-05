#ifndef slic3r_InlineShapeTable_hpp_
#define slic3r_InlineShapeTable_hpp_

// Data model of inline shapes in Emboss text (the per-volume table stored in TextConfiguration).
// Kept apart from InlineShapes.hpp so TextConfiguration.hpp can hold the table without pulling in
// Emboss.hpp (which includes TextConfiguration.hpp). Everything that works on the table is in
// InlineShapes.hpp.

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

namespace Slic3r {

// ---- limits -----------------------------------------------------------------------------------
// Limits of the inline-shape feature itself. The size and complexity of an SVG as untrusted input
// are bounded by the shared caps in UntrustedInput.hpp (SVG_SIZE_LIMIT, SVG_MAX_SHAPES / PATHS /
// POINTS / FLAT_POINTS), applied by load_inline_svg(); these only bound what one glyph may cost.
struct InlineShapeLimits
{
    // distinct shapes in one text volume
    static constexpr size_t max_shapes_per_text = 32;
    // placeholders in one text (checked when inserting)
    static constexpr size_t max_placeholders_per_text = 200;
    // points of the flattened outline of one shape: above simplify_above_points the outline is
    // simplified, above reject_above_points (before simplification) the SVG is refused. A glyph is
    // repeated in the text and bent / projected with the letters, so it must stay light.
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

// ---- table model ------------------------------------------------------------------------------
enum class InlineShapeSource : uint8_t { Builtin = 0, Svg = 1 };
// Where the shape sits vertically (see place_inline_shape).
enum class InlineShapeAnchor : uint8_t {
    Baseline      = 0, // bottom of the shape's box on the baseline
    XHeightCenter = 1, // centre of the box at half the x-height (bullet-like)
    CapCenter     = 2, // centre of the box at half the cap height (plus, minus)
};

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

} // namespace Slic3r

#endif // slic3r_InlineShapeTable_hpp_
