#ifndef slic3r_FontFallback_hpp_
#define slic3r_FontFallback_hpp_

// Groundwork for font fallback in Emboss text: when the selected font has no glyph for a character, take
// it from a bundled symbol font instead of dropping it. This file holds the pure decisions and helpers; it
// is not wired into text2vshapes yet.
//
// The bundled font is a subset of Noto Sans Symbols 2 (SIL OFL 1.1), resources/fonts/NotoSansSymbols2-Subset.ttf,
// with its licence next to it (OFL-NotoSansSymbols2.txt).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Emboss.hpp"

namespace Slic3r {

// "does this font draw this code point?"
using GlyphCoverage = std::function<bool(uint32_t)>;

enum class GlyphSource {
    Primary,  // the selected font draws it
    Fallback, // the primary font does not, the fallback font does
    None,     // nobody draws it (zero width, like today)
};

// True for code points a fallback font may be asked for: not whitespace, a control, a private-use
// (inline-shape placeholder or icon font) code, a surrogate half or a non-character.
bool is_font_fallback_candidate(uint32_t code_point);

// Primary wins whenever it has the glyph. Otherwise a candidate code point goes to the fallback font when it
// has it. An empty std::function means "no such font" (never covers anything).
GlyphSource choose_glyph_source(uint32_t code_point, const GlyphCoverage &primary, const GlyphCoverage &fallback);

// Coverage of the font at font_index (stb_truetype cmap lookup). The returned function reads the font data
// of `font`: the FontFile must outlive it. An invalid font or index gives a function that covers nothing.
GlyphCoverage make_font_coverage(const Emboss::FontFile &font, unsigned font_index = 0);

// Factor that maps fallback glyph units to primary glyph units so both fonts have the same em size
// (primary_unit_per_em / fallback_unit_per_em). 1 when either is not positive.
double fallback_em_scale(int primary_unit_per_em, int fallback_unit_per_em);

// Cap height, x-height and em of a font in font units, measured from the "H" and "x" glyph boxes (0.7 em and
// 0.5 em when the font lacks them). Multiply by 1 / SHAPE_SCALE for the units text glyphs use.
struct FontReferenceHeights
{
    double cap_height = 0.;
    double x_height   = 0.;
    double em         = 0.;
};
FontReferenceHeights font_reference_heights(const Emboss::FontFile &font, unsigned font_index = 0);

// Location of the bundled symbol font subset: resources_dir() + "/fonts/NotoSansSymbols2-Subset.ttf"
std::string bundled_symbol_font_path();

// Curated symbols of the picker, in groups. Every code point is covered by the bundled font.
struct SymbolGroup
{
    const char           *name;
    std::vector<uint32_t> code_points;
};
const std::vector<SymbolGroup> &symbol_picker_groups();

} // namespace Slic3r

#endif // slic3r_FontFallback_hpp_
