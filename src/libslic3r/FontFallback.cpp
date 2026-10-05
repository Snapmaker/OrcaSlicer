#include "FontFallback.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>

#include "imgui/imstb_truetype.h" // stbtt_fontinfo (the implementation is compiled in Emboss.cpp)
#include "Utils.hpp"              // resources_dir()
#include "InlineShapes.hpp"       // utf8 helpers, placeholder range

namespace Slic3r {

bool is_font_fallback_candidate(uint32_t cp)
{
    if (cp <= 0x20 || (cp >= 0x7F && cp <= 0xA0)) // controls, space, no-break space
        return false;
    if (cp == 0x00AD || cp == 0x061C || cp == 0x180E)
        return false;
    if (cp >= 0x2000 && cp <= 0x200F) // spaces and zero width marks
        return false;
    if (cp >= 0x2028 && cp <= 0x202F) // separators, bidi controls, narrow no-break space
        return false;
    if ((cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF)
        return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) // surrogate halves
        return false;
    if (cp >= 0xE000 && cp <= 0xF8FF) // private use (inline shape placeholders, icon fonts)
        return false;
    if (cp >= 0xFDD0 && cp <= 0xFDEF) // non-characters
        return false;
    if ((cp & 0xFFFE) == 0xFFFE) // U+xFFFE and U+xFFFF
        return false;
    if (cp > 0x10FFFF)
        return false;
    return true;
}

GlyphSource choose_glyph_source(uint32_t cp, const GlyphCoverage &primary, const GlyphCoverage &fallback)
{
    if (primary && primary(cp))
        return GlyphSource::Primary;
    if (is_font_fallback_candidate(cp) && fallback && fallback(cp))
        return GlyphSource::Fallback;
    return GlyphSource::None;
}

namespace {
bool init_font_info(const Emboss::FontFile &font, unsigned font_index, stbtt_fontinfo &info)
{
    if (font.data == nullptr || font.data->empty() || font_index >= font.infos.size())
        return false;
    const int offset = stbtt_GetFontOffsetForIndex(font.data->data(), static_cast<int>(font_index));
    if (offset < 0)
        return false;
    return stbtt_InitFont(&info, font.data->data(), offset) != 0;
}
} // namespace

GlyphCoverage make_font_coverage(const Emboss::FontFile &font, unsigned font_index)
{
    auto info = std::make_shared<stbtt_fontinfo>();
    if (!init_font_info(font, font_index, *info))
        return [](uint32_t) { return false; };
    return [info](uint32_t cp) { return stbtt_FindGlyphIndex(info.get(), static_cast<int>(cp)) != 0; };
}

double fallback_em_scale(int primary_unit_per_em, int fallback_unit_per_em)
{
    if (primary_unit_per_em <= 0 || fallback_unit_per_em <= 0)
        return 1.;
    return static_cast<double>(primary_unit_per_em) / static_cast<double>(fallback_unit_per_em);
}

FontReferenceHeights font_reference_heights(const Emboss::FontFile &font, unsigned font_index)
{
    FontReferenceHeights h;
    if (font_index < font.infos.size())
        h.em = static_cast<double>(font.infos[font_index].unit_per_em);
    if (h.em <= 0.)
        return h;
    h.cap_height = 0.7 * h.em;
    h.x_height   = 0.5 * h.em;
    stbtt_fontinfo info;
    if (!init_font_info(font, font_index, info))
        return h;
    int x0, y0, x1, y1;
    if (stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1) && y1 > 0)
        h.cap_height = static_cast<double>(y1);
    if (stbtt_GetCodepointBox(&info, 'x', &x0, &y0, &x1, &y1) && y1 > 0)
        h.x_height = static_cast<double>(y1);
    return h;
}

std::string bundled_symbol_font_path() { return resources_dir() + "/fonts/NotoSansSymbols2-Subset.ttf"; }

std::shared_ptr<const Emboss::FontFile> bundled_symbol_font()
{
    static std::shared_ptr<const Emboss::FontFile> font;
    static std::once_flag                          once;
    std::call_once(once, [] {
        if (resources_dir().empty())
            return;
        try {
            std::unique_ptr<Emboss::FontFile> file = Emboss::create_font_file(bundled_symbol_font_path().c_str());
            if (file != nullptr && !file->infos.empty())
                font = std::move(file);
        } catch (...) {
            font.reset();
        }
    });
    return font;
}

TextGlyphSplit split_text_by_glyph_source(const std::string &utf8, const GlyphCoverage &primary, const GlyphCoverage &fallback,
                                          const std::function<bool(uint32_t)> &is_inline_shape)
{
    TextGlyphSplit         out;
    std::set<uint32_t>     seen;
    std::vector<uint32_t>  prim, fall;
    for (uint32_t cp : utf8_to_codepoints(utf8)) {
        if (cp == '\n' || cp == '\r' || cp == '\t' || !seen.insert(cp).second)
            continue;
        if (is_inline_shape_code(cp) && is_inline_shape && is_inline_shape(cp)) {
            out.inline_shapes.push_back(cp);
            continue;
        }
        switch (choose_glyph_source(cp, primary, fallback)) {
        case GlyphSource::Primary: prim.push_back(cp); break;
        case GlyphSource::Fallback: fall.push_back(cp); break;
        case GlyphSource::None:
        default:
            if (cp != ' ')
                out.exist_unknown = true;
            break;
        }
    }
    out.primary  = codepoints_to_utf8(prim);
    out.fallback = codepoints_to_utf8(fall);
    return out;
}

const std::vector<SymbolGroup> &symbol_picker_groups()
{
    // all of these are in the bundled subset (checked by the unit tests)
    static const std::vector<SymbolGroup> groups = {
    {"bullets and marks", {0x2022, 0x2713, 0x2714, 0x2715, 0x2716, 0x2717, 0x2718}},
    {"stars", {0x2605, 0x2606, 0x2726, 0x2727, 0x2729, 0x272A, 0x272B, 0x272C, 0x272D, 0x272E, 0x272F, 0x2730, 0x2736, 0x2737, 0x2738, 0x2739, 0x273A, 0x273B, 0x2742, 0x2B50, 0x2B51, 0x2B52}},
    {"hearts and cards", {0x2660, 0x2661, 0x2662, 0x2663, 0x2664, 0x2665, 0x2666, 0x2667, 0x2764, 0x2763, 0x2765}},
    {"geometric", {0x25A0, 0x25A1, 0x25AA, 0x25AB, 0x25B2, 0x25B3, 0x25B6, 0x25B7, 0x25BC, 0x25BD, 0x25C0, 0x25C1, 0x25C6, 0x25C7, 0x25CB, 0x25CF, 0x25D0, 0x25D1, 0x25D2, 0x25D3, 0x25EF, 0x25A3, 0x25C9, 0x25CE}},
    {"arrows", {0x2B05, 0x2B06, 0x2B07, 0x2B0C, 0x2B0D, 0x27A4, 0x2794, 0x279C, 0x27A1, 0x279E, 0x27B2, 0x2799, 0x27A2, 0x27BE}},
    {"weather and nature", {0x2600, 0x2601, 0x2602, 0x2603, 0x2604, 0x2614, 0x2618, 0x2744, 0x2745, 0x2746, 0x2740, 0x2741, 0x273F, 0x2743, 0x26A1, 0x2B1B, 0x26C4, 0x26C5}},
    {"objects", {0x2702, 0x2706, 0x2708, 0x2709, 0x270C, 0x270D, 0x270E, 0x270F, 0x2615, 0x260E, 0x26BD, 0x26BE, 0x267F, 0x26CF}},
    {"signs", {0x2620, 0x2622, 0x2623, 0x26A0, 0x26D4, 0x2680, 0x2681, 0x2682, 0x2683, 0x2684, 0x2685, 0x2668}},
    };
    return groups;
}

} // namespace Slic3r
