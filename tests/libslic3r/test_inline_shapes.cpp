#include <catch2/catch.hpp>

#include <chrono>
#include <cmath>
#include <set>
#include <sstream>

#include <boost/filesystem.hpp>
#include <cereal/archives/binary.hpp>

#include <cstdio>

#include <libslic3r/libslic3r.h>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/Emboss.hpp>
#include <libslic3r/FontFallback.hpp>
#include <libslic3r/InlineShapes.hpp>
#include <libslic3r/UntrustedInput.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {
std::string resources_path() { return std::string(TEST_DATA_DIR) + "/../../resources"; }
std::string shapes_dir() { return resources_path() + "/shapes/inline"; }
std::string kr_font_path() { return resources_path() + "/fonts/NotoSansKR-Regular.ttf"; }

BuiltinShapeLibrary &library()
{
    static BuiltinShapeLibrary lib(shapes_dir());
    static bool                loaded = false;
    if (!loaded) {
        std::string error;
        REQUIRE(lib.load(&error));
        loaded = true;
    }
    return lib;
}

// font facts as the text layout would pass them: 1 em = 1,000,000 units
InlineFontMetrics test_metrics()
{
    InlineFontMetrics m;
    m.em         = 1000000.;
    m.cap_height = 700000.;
    m.x_height   = 500000.;
    return m;
}

InlineUnitShape load_unit(const std::string &svg, InlineBoxMode mode = InlineBoxMode::InkBox)
{
    std::string                   error;
    std::optional<InlineUnitShape> u = load_inline_svg(svg, mode, &error);
    INFO(error);
    REQUIRE(u.has_value());
    return *u;
}

Point mm_point(double x, double y) { return Point(static_cast<coord_t>(scale_(x)), static_cast<coord_t>(scale_(y))); }

double area_of(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a;
}

size_t point_count(const ExPolygons &shape)
{
    size_t n = 0;
    for (const ExPolygon &e : shape) {
        n += e.contour.size();
        for (const Polygon &h : e.holes)
            n += h.size();
    }
    return n;
}

std::string svg_wrap(const std::string &body, const char *size = "100")
{
    return std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"") + size + "\" height=\"" + size + "\" viewBox=\"0 0 " + size +
           " " + size + "\">" + body + "</svg>";
}

const InlineShape &entry_of(const std::string &id)
{
    static std::map<std::string, InlineShape> entries;
    auto it = entries.find(id);
    if (it == entries.end()) {
        std::optional<InlineShape> e = library().make_entry(id);
        REQUIRE(e.has_value());
        it = entries.emplace(id, *e).first;
    }
    return it->second;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// built-in library
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline shapes: the manifest lists the built-in library", "[InlineShapes]")
{
    const BuiltinShapeLibrary &lib = library();
    REQUIRE(lib.shapes().size() >= 24);

    std::set<std::string> ids;
    for (const BuiltinShapeInfo &s : lib.shapes()) {
        INFO(s.id);
        CHECK(ids.insert(s.id).second);
        CHECK_FALSE(s.name.empty());
        CHECK_FALSE(s.category.empty());
        CHECK(boost::filesystem::exists(shapes_dir() + "/" + s.file));
        CHECK(s.scale > 0.f);
        CHECK(s.advance > 0.f);
    }
    for (const char *id : {"dot", "circle", "ring", "star", "heart", "check", "cross", "arrow_left", "arrow_right", "arrow_up",
                           "arrow_down", "triangle", "square", "diamond", "hexagon", "plus", "minus", "flower", "paw", "lightning",
                           "music_note", "sun", "moon", "smile", "gear"})
        CHECK(ids.count(id) == 1);
    CHECK(lib.find("star") != nullptr);
    CHECK(lib.find("no_such_shape") == nullptr);
    CHECK_FALSE(lib.make_entry("no_such_shape").has_value());
    CHECK(library().unit_shape("no_such_shape") == nullptr);
}

TEST_CASE("Inline shapes: every built-in shape loads, is valid and fits the em box", "[InlineShapes]")
{
    const BuiltinShapeLibrary &lib = library();
    for (const BuiltinShapeInfo &info : lib.shapes()) {
        DYNAMIC_SECTION("shape " << info.id)
        {
            std::shared_ptr<const InlineUnitShape> unit = lib.unit_shape(info.id);
            REQUIRE(unit != nullptr);
            REQUIRE_FALSE(unit->shape.empty());
            for (const ExPolygon &e : unit->shape) {
                CHECK(e.is_valid());
                CHECK(e.contour.is_counter_clockwise());
                CHECK(e.contour.area() > 0.);
            }
            // the design box is square and INLINE_SHAPE_UNIT high
            CHECK(unit->box_h == INLINE_SHAPE_UNIT);
            CHECK(unit->box_w == INLINE_SHAPE_UNIT);
            CHECK(unit->advance_w > 0);

            const BoundingBox bb  = get_extents(unit->shape);
            const coord_t     tol = INLINE_SHAPE_UNIT / 500; // 0.2 %
            CHECK(bb.min.x() >= -tol);
            CHECK(bb.min.y() >= -tol);
            CHECK(bb.max.x() <= unit->box_w + tol);
            CHECK(bb.max.y() <= unit->box_h + tol);
            CHECK(bb.size().y() >= unit->box_h / 5); // not a speck
            CHECK(bb.size().x() >= unit->box_w / 5);
            if (info.anchor == InlineShapeAnchor::Baseline) // clean baseline: the ink rests on the bottom edge
                CHECK(bb.min.y() <= INLINE_SHAPE_UNIT / 100);
            CHECK(point_count(unit->shape) < InlineShapeLimits::simplify_above_points);

            // placed with the text metrics: sized to cap height * scale, advance covers the ink
            const InlineFontMetrics m = test_metrics();
            std::optional<Emboss::Glyph> g = place_inline_shape(entry_of(info.id), *unit, m);
            REQUIRE(g.has_value());
            const BoundingBox gb = get_extents(g->shape);
            CHECK(gb.size().y() <= m.cap_height * info.scale * 1.003);
            CHECK(gb.size().y() >= m.cap_height * info.scale * 0.2);
            CHECK(g->advance_width > 0);
            CHECK(gb.min.x() >= -2);
            CHECK(gb.max.x() <= g->advance_width + 2);
            if (info.anchor == InlineShapeAnchor::Baseline)
                CHECK(gb.min.y() >= -m.cap_height / 1000.); // nothing sinks below the baseline (curve flattening may overshoot by a hair)
        }
    }
}

TEST_CASE("Inline shapes: holes survive loading", "[InlineShapes]")
{
    auto holes = [](const char *id) {
        std::shared_ptr<const InlineUnitShape> u = library().unit_shape(id);
        REQUIRE(u != nullptr);
        size_t n = 0;
        for (const ExPolygon &e : u->shape)
            n += e.holes.size();
        return n;
    };
    CHECK(holes("ring") == 1);
    CHECK(holes("gear") == 1);
    CHECK(holes("flower") == 1);
    CHECK(holes("smile") == 3); // two eyes and the mouth
    CHECK(holes("circle") == 0);
    CHECK(holes("star") == 0);
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline shapes: sizing, baseline, anchors and advance", "[InlineShapes]")
{
    const InlineFontMetrics m = test_metrics();
    InlineShape             e = entry_of("square");
    std::shared_ptr<const InlineUnitShape> square = library().unit_shape("square");
    REQUIRE(square != nullptr);

    SECTION("cap height sizing on the baseline")
    {
        std::optional<Emboss::Glyph> g = place_inline_shape(e, *square, m);
        REQUIRE(g.has_value());
        const BoundingBox bb = get_extents(g->shape);
        CHECK(bb.min.y() == 0);
        CHECK_THAT(double(bb.max.y()), WithinAbs(m.cap_height, 2.));
        CHECK_THAT(double(bb.size().x()), WithinAbs(m.cap_height, 2.)); // the square box stays square
    }
    SECTION("advance is the box width plus the gaps plus the char gap")
    {
        InlineFontMetrics mm = m;
        mm.char_gap          = 12345;
        std::optional<Emboss::Glyph> g = place_inline_shape(e, *square, mm);
        REQUIRE(g.has_value());
        const double expected = m.cap_height + (e.gap_l + e.gap_r) * m.em + 12345;
        CHECK_THAT(double(g->advance_width), WithinAbs(expected, 2.));
        // the ink starts after the left gap
        CHECK_THAT(double(g->left_side_bearing), WithinAbs(e.gap_l * m.em, 2.));
    }
    SECTION("scale")
    {
        e.scale = 0.5f;
        std::optional<Emboss::Glyph> g = place_inline_shape(e, *square, m);
        REQUIRE(g.has_value());
        CHECK_THAT(double(get_extents(g->shape).size().y()), WithinAbs(m.cap_height * 0.5, 2.));
        CHECK_THAT(double(g->advance_width), WithinAbs(m.cap_height * 0.5 + (e.gap_l + e.gap_r) * m.em, 2.));
    }
    SECTION("raise with dy")
    {
        e.dy = 0.1f;
        std::optional<Emboss::Glyph> g = place_inline_shape(e, *square, m);
        REQUIRE(g.has_value());
        CHECK_THAT(double(get_extents(g->shape).min.y()), WithinAbs(0.1 * m.em, 2.));
    }
    SECTION("x-height and cap-height centring")
    {
        e.scale  = 0.4f;
        e.anchor = InlineShapeAnchor::XHeightCenter;
        std::optional<Emboss::Glyph> g = place_inline_shape(e, *square, m);
        REQUIRE(g.has_value());
        BoundingBox bb = get_extents(g->shape);
        CHECK_THAT(double(bb.center().y()), WithinAbs(m.x_height / 2., 2.));

        e.anchor = InlineShapeAnchor::CapCenter;
        g        = place_inline_shape(e, *square, m);
        REQUIRE(g.has_value());
        bb = get_extents(g->shape);
        CHECK_THAT(double(bb.center().y()), WithinAbs(m.cap_height / 2., 2.));
    }
    SECTION("manifest advance hint centres the box in a narrower advance")
    {
        std::shared_ptr<const InlineUnitShape> bolt = library().unit_shape("lightning");
        REQUIRE(bolt != nullptr);
        CHECK(bolt->advance_w < bolt->box_w);
        std::optional<Emboss::Glyph> g = place_inline_shape(entry_of("lightning"), *bolt, m);
        REQUIRE(g.has_value());
        const BoundingBox bb = get_extents(g->shape);
        CHECK(bb.min.x() >= 0);
        CHECK(bb.max.x() <= g->advance_width);
        // balanced: about the same room left and right of the ink (gaps are symmetric)
        const double left = bb.min.x(), right = g->advance_width - bb.max.x();
        CHECK_THAT(left, WithinAbs(right, 3.));
    }
    SECTION("flip mirrors and keeps the winding")
    {
        std::shared_ptr<const InlineUnitShape> arrow = library().unit_shape("arrow_right");
        REQUIRE(arrow != nullptr);
        InlineShape plain = entry_of("arrow_right"), flipped = plain;
        flipped.flip_x = true;
        auto g1 = place_inline_shape(plain, *arrow, m);
        auto g2 = place_inline_shape(flipped, *arrow, m);
        REQUIRE(g1.has_value());
        REQUIRE(g2.has_value());
        CHECK_THAT(area_of(g1->shape), WithinAbs(area_of(g2->shape), 4.));
        for (const ExPolygon &ex : g2->shape)
            CHECK(ex.contour.is_counter_clockwise());
        // the head is on the right in g1 and on the left in g2: the ink centre of mass swaps sides
        auto cx = [](const ExPolygons &s) { double sum = 0.; double a = 0.; for (const ExPolygon &ex : s) { Point c = ex.contour.centroid(); sum += c.x() * ex.area(); a += ex.area(); } return sum / a; };
        CHECK(cx(g1->shape) > g1->advance_width / 2.);
        CHECK(cx(g2->shape) < g2->advance_width / 2.);
    }
    SECTION("bold widens the outline, the advance stays")
    {
        InlineFontMetrics mm = m;
        mm.boldness_delta    = 10000.;
        auto g0 = place_inline_shape(e, *square, m);
        auto g1 = place_inline_shape(e, *square, mm);
        REQUIRE(g0.has_value());
        REQUIRE(g1.has_value());
        const BoundingBox b0 = get_extents(g0->shape), b1 = get_extents(g1->shape);
        CHECK_THAT(double(b1.size().x() - b0.size().x()), WithinAbs(20000., 4.));
        CHECK_THAT(double(b1.size().y() - b0.size().y()), WithinAbs(20000., 4.));
        CHECK(g1->advance_width == g0->advance_width);
    }
    SECTION("skew slants the shape like a letter")
    {
        InlineFontMetrics mm = m;
        mm.skew              = 0.25;
        auto g0 = place_inline_shape(e, *square, m);
        auto g1 = place_inline_shape(e, *square, mm);
        REQUIRE(g0.has_value());
        REQUIRE(g1.has_value());
        const BoundingBox b0 = get_extents(g0->shape), b1 = get_extents(g1->shape);
        CHECK_THAT(double(b1.size().x() - b0.size().x()), WithinAbs(0.25 * m.cap_height, 3.));
        CHECK(b1.min.y() == b0.min.y()); // the baseline stays put
        CHECK(b1.min.x() == b0.min.x());
        CHECK(g1->advance_width == g0->advance_width);
    }
    SECTION("unusable input gives nothing")
    {
        InlineFontMetrics bad = m;
        bad.cap_height        = 0.;
        CHECK_FALSE(place_inline_shape(e, *square, bad).has_value());
        InlineUnitShape empty;
        CHECK_FALSE(place_inline_shape(e, empty, m).has_value());
    }
}

TEST_CASE("Inline shapes: make_inline_glyph resolves built-in and embedded shapes", "[InlineShapes]")
{
    InlineShapeCache cache;
    const InlineFontMetrics m = test_metrics();

    CHECK(make_inline_glyph(entry_of("star"), library(), cache, m).has_value());

    InlineShape unknown;
    unknown.id = "no_such_shape";
    CHECK_FALSE(make_inline_glyph(unknown, library(), cache, m).has_value());

    InlineShape svg;
    svg.source   = InlineShapeSource::Svg;
    svg.id       = "bar";
    svg.svg_data = std::make_shared<const std::string>(svg_wrap("<rect x=\"10\" y=\"20\" width=\"60\" height=\"20\"/>"));
    std::optional<Emboss::Glyph> g = make_inline_glyph(svg, library(), cache, m);
    REQUIRE(g.has_value());
    // a user SVG is sized by its ink: 3:1 bar, height = cap height
    const BoundingBox bb = get_extents(g->shape);
    CHECK_THAT(double(bb.size().y()), WithinAbs(m.cap_height, 4.));
    CHECK_THAT(double(bb.size().x()), WithinAbs(3. * m.cap_height, 8.));
    CHECK(bb.min.y() == 0);

    // two table entries with the same code but different data never share a result
    InlineShape other = svg;
    other.svg_data    = std::make_shared<const std::string>(svg_wrap("<rect x=\"0\" y=\"0\" width=\"20\" height=\"20\"/>"));
    std::optional<Emboss::Glyph> g2 = make_inline_glyph(other, library(), cache, m);
    REQUIRE(g2.has_value());
    CHECK_THAT(double(get_extents(g2->shape).size().x()), WithinAbs(m.cap_height, 4.));

    InlineShape no_data;
    no_data.source = InlineShapeSource::Svg;
    CHECK_FALSE(make_inline_glyph(no_data, library(), cache, m).has_value());
}

// ---------------------------------------------------------------------------------------------
// placeholders and table
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline shapes: placeholder allocation round-trips", "[InlineShapes]")
{
    InlineShapeTable table;

    SECTION("first code, range and uniqueness")
    {
        std::optional<uint16_t> first = allocate_inline_shape_code(table);
        REQUIRE(first.has_value());
        CHECK(*first == INLINE_SHAPE_CODE_FIRST);
        CHECK(is_inline_shape_code(*first));
        CHECK_FALSE(is_inline_shape_code(INLINE_SHAPE_CODE_FIRST - 1));
        CHECK_FALSE(is_inline_shape_code(INLINE_SHAPE_CODE_LAST + 1));
        CHECK_FALSE(is_inline_shape_code(0xF8FF));

        std::set<uint16_t> codes;
        for (size_t i = 0; i < InlineShapeLimits::max_shapes_per_text; ++i) {
            InlineShape s = entry_of("star");
            s.scale       = 1.f + 0.25f * float(i); // distinct entries
            std::optional<uint16_t> c = add_inline_shape(table, s);
            REQUIRE(c.has_value());
            CHECK(is_inline_shape_code(*c));
            CHECK(codes.insert(*c).second);
            const InlineShape *found = find_inline_shape(table, *c);
            REQUIRE(found != nullptr);
            CHECK(found->scale == s.scale);
        }
        // table full
        InlineShape extra = entry_of("heart");
        CHECK_FALSE(add_inline_shape(table, extra).has_value());
        CHECK(table.size() == InlineShapeLimits::max_shapes_per_text);
    }
    SECTION("an equal entry shares its code")
    {
        std::optional<uint16_t> a = add_inline_shape(table, entry_of("star"));
        std::optional<uint16_t> b = add_inline_shape(table, entry_of("star"));
        std::optional<uint16_t> c = add_inline_shape(table, entry_of("heart"));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(c.has_value());
        CHECK(*a == *b);
        CHECK(*a != *c);
        CHECK(table.size() == 2);
        InlineShape bigger = entry_of("star");
        bigger.scale       = 2.f;
        std::optional<uint16_t> d = add_inline_shape(table, bigger);
        REQUIRE(d.has_value());
        CHECK(*d != *a);
        CHECK(table.size() == 3);
    }
    SECTION("codes the font draws are skipped")
    {
        auto font_maps = [](uint32_t cp) { return cp < INLINE_SHAPE_CODE_FIRST + 5; };
        std::optional<uint16_t> c = allocate_inline_shape_code(table, font_maps);
        REQUIRE(c.has_value());
        CHECK(*c == INLINE_SHAPE_CODE_FIRST + 5);
        auto everything = [](uint32_t) { return true; };
        CHECK_FALSE(allocate_inline_shape_code(table, everything).has_value());
    }
    SECTION("the text carries the placeholder; use counts, purge and names")
    {
        std::optional<uint16_t> star  = add_inline_shape(table, entry_of("star"));
        std::optional<uint16_t> heart = add_inline_shape(table, entry_of("heart"));
        REQUIRE(star.has_value());
        REQUIRE(heart.has_value());
        const std::string text = "A" + inline_shape_placeholder(*find_inline_shape(table, *star)) + "B" +
                                 inline_shape_placeholder(*find_inline_shape(table, *star)) + u8"\u00e9";

        // UTF-8 round trip, one code point per placeholder
        const std::vector<uint32_t> cps = utf8_to_codepoints(text);
        REQUIRE(cps.size() == 5);
        CHECK(cps[1] == *star);
        CHECK(cps[4] == 0xE9);
        CHECK(codepoints_to_utf8(cps) == text);

        CHECK(count_inline_placeholders(text) == 2);
        const std::map<uint16_t, size_t> uses = count_inline_shape_uses(text, table);
        REQUIRE(uses.size() == 1);
        CHECK(uses.at(*star) == 2);

        CHECK(inline_text_display_name(text, table) == u8"A[star]B[star]\u00e9");

        // the unused heart goes, and its code is free again
        CHECK(purge_unused_inline_shapes(table, text) == 1);
        CHECK(table.size() == 1);
        CHECK(find_inline_shape(table, *heart) == nullptr);
        std::optional<uint16_t> again = add_inline_shape(table, entry_of("heart"));
        REQUIRE(again.has_value());
        CHECK(*again == *heart);

        // a placeholder the table does not know is dropped from names and ignored by counts
        const std::string stray = "x" + codepoints_to_utf8({0xF820}) + "y";
        CHECK(count_inline_placeholders(stray) == 1);
        CHECK(count_inline_shape_uses(stray, table).empty());
        CHECK(inline_text_display_name(stray, table) == "xy");
    }
    SECTION("broken UTF-8 does not crash")
    {
        const std::string bad = std::string("a\xC3") + "b\xFF\xE2\x82" + "c\xF0\x9F\x98\x80";
        const std::vector<uint32_t> cps = utf8_to_codepoints(bad);
        CHECK_FALSE(cps.empty());
        CHECK(cps.front() == 'a');
        CHECK(cps.back() == 0x1F600);
        CHECK(count_inline_placeholders(bad) == 0);
    }
}

// ---------------------------------------------------------------------------------------------
// JSON and serialisation
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline shapes: JSON round-trip", "[InlineShapes]")
{
    InlineShapeTable table;
    {
        InlineShape a = entry_of("star");
        a.scale       = 1.25f;
        a.dy          = -0.125f;
        a.gap_l       = 0.0f;
        a.gap_r       = 0.2f;
        a.flip_x      = true;
        a.anchor      = InlineShapeAnchor::CapCenter;
        REQUIRE(add_inline_shape(table, a).has_value());
        REQUIRE(add_inline_shape(table, entry_of("heart")).has_value()); // all defaults
        InlineShape s;
        s.source   = InlineShapeSource::Svg;
        s.id       = "my logo \"v2\" \xC3\xA9";
        s.svg_data = std::make_shared<const std::string>(svg_wrap("<circle cx=\"50\" cy=\"50\" r=\"40\"/>"));
        s.scale    = 0.75f;
        REQUIRE(add_inline_shape(table, s).has_value());
    }

    SECTION("round trip keeps everything except the svg data")
    {
        const std::string json = inline_shapes_to_json(table);
        REQUIRE_FALSE(json.empty());
        CHECK(json.find("<svg") == std::string::npos); // the data is a separate zip entry
        CHECK(json.find("3D/inline_") != std::string::npos);

        size_t skipped = 99;
        std::optional<InlineShapeTable> back = inline_shapes_from_json(json, &skipped);
        REQUIRE(back.has_value());
        CHECK(skipped == 0);
        REQUIRE(back->size() == table.size());
        for (size_t i = 0; i < table.size(); ++i) {
            const InlineShape &a = table[i], &b = (*back)[i];
            INFO("entry " << i);
            CHECK(a.code == b.code);
            CHECK(a.source == b.source);
            CHECK(a.id == b.id);
            CHECK(a.scale == b.scale);
            CHECK(a.dy == b.dy);
            CHECK(a.gap_l == b.gap_l);
            CHECK(a.gap_r == b.gap_r);
            CHECK(a.flip_x == b.flip_x);
            CHECK(a.anchor == b.anchor);
        }
        CHECK_FALSE((*back)[2].path_in_3mf.empty());
        CHECK((*back)[2].svg_data == nullptr);

        // the importer hands the zip entry to the entries that name it
        CHECK(attach_inline_svg(*back, (*back)[2].path_in_3mf, table[2].svg_data) == 1);
        REQUIRE((*back)[2].svg_data != nullptr);
        CHECK(*(*back)[2].svg_data == *table[2].svg_data);
        CHECK(attach_inline_svg(*back, "3D/inline_00000000.svg", table[2].svg_data) == 0);
    }
    SECTION("defaults are not written")
    {
        InlineShapeTable one;
        REQUIRE(add_inline_shape(one, entry_of("heart")).has_value());
        const std::string json = inline_shapes_to_json(one);
        CHECK(json.find("\"dy\"") == std::string::npos);
        CHECK(json.find("\"fx\"") == std::string::npos);
        CHECK(json.find("\"gl\"") == std::string::npos);
        CHECK(json.find("\"c\":") != std::string::npos);
    }
    SECTION("an empty table writes nothing")
    {
        CHECK(inline_shapes_to_json({}).empty());
    }
    SECTION("entry names are derived from the data")
    {
        const std::string n1 = inline_svg_entry_name("abc"), n2 = inline_svg_entry_name("abd");
        CHECK(n1 != n2);
        CHECK(n1 == inline_svg_entry_name("abc"));
        CHECK(n1.rfind("3D/inline_", 0) == 0);
        CHECK(n1.size() == std::string("3D/inline_").size() + 8 + 4);
        InlineShapeTable t = table;
        assign_inline_svg_entry_names(t);
        CHECK(t[2].path_in_3mf == inline_svg_entry_name(*t[2].svg_data));
    }
    SECTION("cereal binary round trip (undo/redo) carries the svg data by value")
    {
        std::stringstream ss;
        {
            cereal::BinaryOutputArchive ar(ss);
            ar(table);
        }
        InlineShapeTable back;
        {
            cereal::BinaryInputArchive ar(ss);
            ar(back);
        }
        REQUIRE(back.size() == table.size());
        for (size_t i = 0; i < table.size(); ++i) {
            CHECK(table[i].same_content(back[i]));
            CHECK(table[i].code == back[i].code);
        }
        REQUIRE(back[2].svg_data != nullptr);
        CHECK(back[2].svg_data != table[2].svg_data);
        CHECK(*back[2].svg_data == *table[2].svg_data);
        CHECK(back[0].svg_data == nullptr);
    }
}

TEST_CASE("Inline shapes: the JSON reader tolerates hostile input", "[InlineShapes]")
{
    CHECK_FALSE(inline_shapes_from_json("").has_value());
    CHECK_FALSE(inline_shapes_from_json("not json").has_value());
    CHECK_FALSE(inline_shapes_from_json("{\"c\":63232}").has_value());
    CHECK_FALSE(inline_shapes_from_json(std::string(300 * 1024, '[')).has_value());

    SECTION("bad entries are skipped, good ones kept")
    {
        const std::string json =
            "[{\"c\":63232,\"k\":\"b\",\"id\":\"star\"},"                                   // good
            "{\"c\":63232,\"k\":\"b\",\"id\":\"dup\"},"                                      // duplicate code
            "{\"c\":65,\"k\":\"b\",\"id\":\"low\"},"                                         // outside the placeholder range
            "{\"c\":63233,\"k\":\"x\",\"id\":\"kind\"},"                                     // unknown kind
            "{\"c\":63234,\"k\":\"b\"},"                                                     // built-in without id
            "{\"c\":63235,\"k\":\"f\",\"id\":\"s\",\"f\":\"../../evil.svg\"},"               // escapes the folder
            "{\"c\":63236,\"k\":\"f\",\"id\":\"s\",\"f\":\"3D/inline_ab/../x.svg\"},"        // path games
            "{\"c\":63237,\"k\":\"f\",\"id\":\"s\"},"                                        // svg without entry name
            "{\"c\":\"63750\",\"k\":\"b\",\"id\":\"str\"},"                                  // code is not a number
            "42,"                                                                            // not an object
            "{\"c\":63239,\"k\":\"f\",\"id\":\"ok\",\"f\":\"3D/inline_ab12cd34.svg\"}]";     // good
        size_t skipped = 0;
        std::optional<InlineShapeTable> t = inline_shapes_from_json(json, &skipped);
        REQUIRE(t.has_value());
        CHECK(skipped == 9);
        REQUIRE(t->size() == 2);
        CHECK((*t)[0].id == "star");
        CHECK((*t)[1].path_in_3mf == "3D/inline_ab12cd34.svg");
    }
    SECTION("values are clamped, not trusted")
    {
        std::optional<InlineShapeTable> t = inline_shapes_from_json(
            "[{\"c\":63232,\"k\":\"b\",\"id\":\"star\",\"s\":1e30,\"dy\":-1e9,\"gl\":\"x\",\"gr\":99,\"a\":7,\"fx\":3}]");
        REQUIRE(t.has_value());
        REQUIRE(t->size() == 1);
        const InlineShape &s = t->front();
        CHECK(s.scale == 20.f);
        CHECK(s.dy == -5.f);
        CHECK(s.gap_l == InlineShape().gap_l); // not a number: default
        CHECK(s.gap_r == 2.f);
        CHECK(s.anchor == InlineShapeAnchor::Baseline); // out of range: default
        CHECK_FALSE(s.flip_x);
    }
    SECTION("the number of entries is capped")
    {
        std::string json = "[";
        for (int i = 0; i < 100; ++i) {
            if (i)
                json += ",";
            json += "{\"c\":" + std::to_string(INLINE_SHAPE_CODE_FIRST + i) + ",\"k\":\"b\",\"id\":\"star\"}";
        }
        json += "]";
        size_t skipped = 0;
        std::optional<InlineShapeTable> t = inline_shapes_from_json(json, &skipped);
        REQUIRE(t.has_value());
        CHECK(t->size() == InlineShapeLimits::max_shapes_per_text);
        CHECK(skipped == 100 - InlineShapeLimits::max_shapes_per_text);
    }
}

// ---------------------------------------------------------------------------------------------
// SVG loading, silhouette and limits
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline shapes: colours become a silhouette", "[InlineShapes]")
{
    SECTION("overlapping shapes of different colours become one outline")
    {
        const std::string svg = svg_wrap("<rect x=\"0\" y=\"0\" width=\"60\" height=\"60\" fill=\"#ff0000\"/>"
                                         "<circle cx=\"60\" cy=\"30\" r=\"30\" fill=\"#0000ff\"/>"
                                         "<rect x=\"20\" y=\"20\" width=\"20\" height=\"20\" fill=\"#ffffff\"/>"); // white on top: still ink
        InlineUnitShape u = load_unit(svg);
        REQUIRE(u.shape.size() == 1);
        CHECK(u.shape.front().holes.empty());
        // 60x60 square + the right half of the circle (the left half is inside the square)
        const double expected = 60. * 60. + 0.5 * PI * 30. * 30.;
        const double scale    = double(u.box_h) / 60.; // ink box: 90 wide x 60 high, height normalised by the longer side
        (void) scale;
        const double unit_per_img = double(u.box_w) / 90.;
        CHECK_THAT(area_of(u.shape) / (unit_per_img * unit_per_img), WithinAbs(expected, expected * 0.01));
        CHECK(u.ink_box);
    }
    SECTION("a stroke-only shape becomes filled area")
    {
        InlineUnitShape u = load_unit(svg_wrap("<path d=\"M10 50 L90 50\" fill=\"none\" stroke=\"#00aa00\" stroke-width=\"10\"/>"));
        REQUIRE(u.shape.size() == 1);
        CHECK(area_of(u.shape) > 0.);
        const double aspect = double(u.box_w) / double(u.box_h);
        CHECK_THAT(aspect, WithinAbs(8., 0.5)); // 80 long, 10 wide (stroke width included)
    }
    SECTION("fill none and no stroke is invisible, so there is nothing")
    {
        CHECK_FALSE(load_inline_svg(svg_wrap("<rect width=\"50\" height=\"50\" fill=\"none\"/>"), InlineBoxMode::InkBox).has_value());
    }
    SECTION("fill rule: even-odd donut keeps its hole")
    {
        InlineUnitShape u = load_unit(svg_wrap("<path fill-rule=\"evenodd\" d=\"M0 0 H100 V100 H0 Z M25 25 H75 V75 H25 Z\"/>"));
        REQUIRE(u.shape.size() == 1);
        CHECK(u.shape.front().holes.size() == 1);
        CHECK_THAT(area_of(u.shape) / (double(u.box_w) * double(u.box_h)), WithinAbs(0.75, 0.005));
    }
    SECTION("silhouette_of unions ids regardless of colour")
    {
        ExPolygonsWithIds ids;
        ExPolygonsWithId  a, b;
        a.id    = 0;
        a.expoly = {ExPolygon(Polygon(Points{mm_point(0, 0), mm_point(10, 0), mm_point(10, 10), mm_point(0, 10)}))};
        b.id    = 2;
        b.expoly = {ExPolygon(Polygon(Points{mm_point(5, 0), mm_point(15, 0), mm_point(15, 10), mm_point(5, 10)}))};
        ids.push_back(a);
        ids.push_back(b);
        ExPolygons u = silhouette_of(ids);
        REQUIRE(u.size() == 1);
        CHECK_THAT(u.front().area() * SCALING_FACTOR * SCALING_FACTOR, WithinAbs(150., 0.01));
    }
}

TEST_CASE("Inline shapes: SVG limits and hostile input", "[InlineShapes]")
{
    std::string error;
    SECTION("garbage in, nothing out, no crash")
    {
        for (const char *bad : {"", "   ", "not an svg", "<svg", "<?xml version=\"1.0\"?><html></html>", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>"})
            CHECK_FALSE(load_inline_svg(bad, InlineBoxMode::InkBox, &error).has_value());
        CHECK_FALSE(load_inline_svg(std::string("\0\0\0\0", 4), InlineBoxMode::DesignBox, &error).has_value());
        // odd sizes: whatever nanosvg makes of them, no crash and no throw
        REQUIRE_NOTHROW(load_inline_svg("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 0 0\"><rect width=\"1\" height=\"1\"/></svg>", InlineBoxMode::InkBox));
        REQUIRE_NOTHROW(load_inline_svg("<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"10\" height=\"10\"/></svg>", InlineBoxMode::DesignBox));
    }
    SECTION("over the shared byte cap (UntrustedInput SVG_SIZE_LIMIT)")
    {
        std::string big = svg_wrap(std::string(size_t(untrusted::SVG_SIZE_LIMIT) + 1, ' '));
        CHECK_FALSE(load_inline_svg(big, InlineBoxMode::InkBox, &error).has_value());
        CHECK_FALSE(error.empty());
    }
    SECTION("too many shapes (shared cap)")
    {
        std::string body;
        body.reserve((untrusted::SVG_MAX_SHAPES + 1) * 48);
        for (size_t i = 0; i < untrusted::SVG_MAX_SHAPES + 1; ++i)
            body += "<rect x=\"" + std::to_string(i % 90) + "\" y=\"0\" width=\"5\" height=\"5\"/>";
        CHECK_FALSE(load_inline_svg(svg_wrap(body), InlineBoxMode::InkBox, &error).has_value());
        INFO(error);
        CHECK(error.find("too complex") != std::string::npos);
    }
    SECTION("too many points (shared cap)")
    {
        std::string d = "M0 0";
        const int   segments = int(untrusted::SVG_MAX_POINTS / 3) + 10; // 3 control points per line segment in nanosvg
        d.reserve(size_t(segments) * 10);
        for (int i = 1; i <= segments; ++i)
            d += " L" + std::to_string(i % 97) + " " + std::to_string((i * 7) % 89);
        CHECK_FALSE(load_inline_svg(svg_wrap("<path d=\"" + d + "\"/>"), InlineBoxMode::InkBox, &error).has_value());
        CHECK(error.find("too many points") != std::string::npos);
    }
    SECTION("a zig-zag under the shared caps is still refused or simplified, never slow")
    {
        std::string d = "M0 0";
        for (int i = 1; i <= 12000; ++i)
            d += " L" + std::to_string(i % 97) + " " + std::to_string((i * 7) % 89);
        const auto t0 = std::chrono::steady_clock::now();
        std::optional<InlineUnitShape> u = load_inline_svg(svg_wrap("<path d=\"" + d + "\"/>"), InlineBoxMode::InkBox, &error);
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20.);
        if (u.has_value())
            CHECK(point_count(u->shape) <= InlineShapeLimits::simplify_above_points);
    }
    SECTION("a detailed outline is simplified under the cap")
    {
        std::string d;
        const int n = 8000; // 24k control points: under the shared caps, over simplify_above_points
        for (int i = 0; i < n; ++i) {
            const double a = 2. * PI * i / n, r = 40. + 0.004 * ((i * 37) % 11);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%c%.4f %.4f", i ? 'L' : 'M', 50. + r * std::cos(a), 50. + r * std::sin(a));
            d += buf;
            d += ' ';
        }
        d += "Z";
        std::optional<InlineUnitShape> u = load_inline_svg(svg_wrap("<path d=\"" + d + "\"/>"), InlineBoxMode::InkBox, &error);
        INFO(error);
        REQUIRE(u.has_value());
        CHECK(point_count(u->shape) <= InlineShapeLimits::simplify_above_points);
        CHECK(point_count(u->shape) > 8);
        // still a disc of radius 40 in a box of 80
        CHECK_THAT(area_of(u->shape) / (double(u->box_w) * double(u->box_h)), WithinAbs(PI / 4., 0.01));
    }
    SECTION("external references are ignored")
    {
        const std::string svg = svg_wrap("<image href=\"file:///C:/Windows/win.ini\" width=\"50\" height=\"50\"/>"
                                         "<image xlink:href=\"http://127.0.0.1:1/x.png\" width=\"50\" height=\"50\"/>"
                                         "<use href=\"file:///etc/passwd#x\"/>"
                                         "<rect x=\"10\" y=\"10\" width=\"30\" height=\"30\"/>");
        InlineUnitShape u = load_unit(svg);
        REQUIRE(u.shape.size() == 1);
        CHECK_THAT(double(u.box_w) / double(u.box_h), WithinAbs(1., 0.01)); // only the rectangle counted
    }
    SECTION("dashes cannot make the work explode")
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::optional<InlineUnitShape> u = load_inline_svg(
            svg_wrap("<path d=\"M0 50 L100 50\" fill=\"none\" stroke=\"#000\" stroke-width=\"10\" stroke-dasharray=\"0.0000001\"/>"),
            InlineBoxMode::InkBox, &error);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(secs < 5.);
        REQUIRE(u.has_value()); // dashes are ignored: a solid line
    }
    SECTION("absurd proportions are refused")
    {
        CHECK_FALSE(load_inline_svg(svg_wrap("<rect x=\"0\" y=\"0\" width=\"100\" height=\"0.5\"/>"), InlineBoxMode::InkBox, &error).has_value());
    }
    SECTION("hostile numbers do not overflow")
    {
        for (const char *body : {"<rect width=\"1e38\" height=\"1e38\"/>", "<rect width=\"-5\" height=\"-5\"/>", "<circle r=\"nan\"/>",
                                 "<path d=\"M1e38 1e38 L-1e38 1e38 L0 -1e38 Z\"/>"})
            REQUIRE_NOTHROW(load_inline_svg(svg_wrap(body), InlineBoxMode::InkBox));
    }
}

// ---------------------------------------------------------------------------------------------
// font fallback
// ---------------------------------------------------------------------------------------------
TEST_CASE("Font fallback: candidate code points", "[FontFallback]")
{
    CHECK(is_font_fallback_candidate(0x2605));
    CHECK(is_font_fallback_candidate('A'));
    CHECK(is_font_fallback_candidate(0x1F600));
    for (uint32_t cp : {0x0u, 0x9u, 0xAu, 0x20u, 0x7Fu, 0xA0u, 0x200Bu, 0x2028u, 0x3000u, 0xFEFFu, 0xD800u, 0xDFFFu, 0xE000u, INLINE_SHAPE_CODE_FIRST, 0xF8FFu,
                        0xFDD0u, 0xFFFEu, 0xFFFFu, 0x1FFFFu, 0x110000u})
        CHECK_FALSE(is_font_fallback_candidate(cp));
}

TEST_CASE("Font fallback: decision with an Arial-like primary font", "[FontFallback]")
{
    // what Arial covers of the interesting characters (measured: heart, arrows and bullet yes; star, check, cross no)
    GlyphCoverage arial = [](uint32_t cp) {
        return (cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF) || cp == 0x2022 || cp == 0x2192 || cp == 0x2665;
    };
    // a stand-in fallback that covers the symbols (the real bundled font is checked below)
    GlyphCoverage symbols = [](uint32_t cp) { return cp == 0x2605 || cp == 0x2713 || cp == 0x2665 || cp == 0x2717 || cp == 0x26A1 || cp == 0x20 || cp == INLINE_SHAPE_CODE_FIRST; };

    CHECK(choose_glyph_source('A', arial, symbols) == GlyphSource::Primary);
    CHECK(choose_glyph_source(0x2665, arial, symbols) == GlyphSource::Primary); // the primary wins when both have it
    CHECK(choose_glyph_source(0x2192, arial, symbols) == GlyphSource::Primary);
    CHECK(choose_glyph_source(0x2605, arial, symbols) == GlyphSource::Fallback);
    CHECK(choose_glyph_source(0x2713, arial, symbols) == GlyphSource::Fallback);
    CHECK(choose_glyph_source(0x26A1, arial, symbols) == GlyphSource::Fallback);
    CHECK(choose_glyph_source(0x263A, arial, symbols) == GlyphSource::None); // nobody has it
    CHECK(choose_glyph_source(0x0009, arial, symbols) == GlyphSource::None); // tab: never asks the fallback
    CHECK(choose_glyph_source(0x2003, arial, symbols) == GlyphSource::None);
    CHECK(choose_glyph_source(INLINE_SHAPE_CODE_FIRST, arial, symbols) == GlyphSource::None); // placeholders are not fallback material
    CHECK(choose_glyph_source(0x2605, arial, {}) == GlyphSource::None);                       // no fallback font
    CHECK(choose_glyph_source('A', {}, symbols) == GlyphSource::None);                        // no primary: only candidates fall back
    CHECK(choose_glyph_source(0x2605, {}, symbols) == GlyphSource::Fallback);
    // no space in the symbol font and none in the primary: stays None instead of borrowing
    CHECK(choose_glyph_source(0x20, [](uint32_t) { return false; }, symbols) == GlyphSource::None);
}

TEST_CASE("Font fallback: real fonts, primary and bundled symbol font", "[FontFallback]")
{
    const std::string symbol_path = resources_path() + "/fonts/NotoSansSymbols2-Subset.ttf";
    if (!boost::filesystem::exists(kr_font_path()) || !boost::filesystem::exists(symbol_path)) {
        WARN("fonts not found, skipping");
        return;
    }
    std::unique_ptr<Emboss::FontFile> primary_file  = Emboss::create_font_file(kr_font_path().c_str());
    std::unique_ptr<Emboss::FontFile> fallback_file = Emboss::create_font_file(symbol_path.c_str());
    REQUIRE(primary_file != nullptr);
    REQUIRE(fallback_file != nullptr);
    GlyphCoverage primary  = make_font_coverage(*primary_file);
    GlyphCoverage fallback = make_font_coverage(*fallback_file);

    SECTION("decisions")
    {
        CHECK(choose_glyph_source('A', primary, fallback) == GlyphSource::Primary);
        CHECK(choose_glyph_source(0xAC00, primary, fallback) == GlyphSource::Primary); // Hangul: only the primary
        CHECK(choose_glyph_source(0x2605, primary, fallback) == GlyphSource::Primary); // star: Noto Sans KR has it
        CHECK(choose_glyph_source(0x2192, primary, fallback) == GlyphSource::Primary);
        CHECK(choose_glyph_source(0x2717, primary, fallback) == GlyphSource::Fallback); // ballot X
        CHECK(choose_glyph_source(0x2714, primary, fallback) == GlyphSource::Fallback); // heavy check
        CHECK(choose_glyph_source(0x26A1, primary, fallback) == GlyphSource::Fallback); // high voltage
        CHECK(choose_glyph_source(0x2744, primary, fallback) == GlyphSource::Fallback); // snowflake
        CHECK(choose_glyph_source(0x263A, primary, fallback) == GlyphSource::None);     // white smiling face: in neither
        CHECK(choose_glyph_source(0x0009, primary, fallback) == GlyphSource::None);
        CHECK(choose_glyph_source(INLINE_SHAPE_CODE_FIRST, primary, fallback) == GlyphSource::None);
    }
    SECTION("an invalid font index covers nothing")
    {
        GlyphCoverage none = make_font_coverage(*primary_file, 7);
        CHECK_FALSE(none('A'));
        CHECK_FALSE(none(0x2605));
    }
    SECTION("reference heights come from the glyph boxes")
    {
        const FontReferenceHeights h = font_reference_heights(*primary_file);
        CHECK_THAT(h.em, WithinAbs(1000., 0.5));
        CHECK(h.cap_height > 0.6 * h.em);
        CHECK(h.cap_height < 0.85 * h.em);
        CHECK(h.x_height > 0.4 * h.em);
        CHECK(h.x_height < h.cap_height);
        const FontReferenceHeights bad = font_reference_heights(*primary_file, 9);
        CHECK(bad.cap_height == 0.);
    }
    SECTION("the bundled symbol font")
    {
        CHECK_THAT(font_reference_heights(*fallback_file).em, WithinAbs(1000., 0.5));
        CHECK_THAT(fallback_em_scale(1000, 1000), WithinAbs(1., 1e-12));
        CHECK_THAT(fallback_em_scale(2048, 1000), WithinAbs(2.048, 1e-12));
        CHECK_THAT(fallback_em_scale(1000, 0), WithinAbs(1., 1e-12));

        // the picker only offers symbols the bundled font draws
        std::set<uint32_t> seen;
        size_t             total = 0;
        REQUIRE_FALSE(symbol_picker_groups().empty());
        for (const SymbolGroup &g : symbol_picker_groups()) {
            REQUIRE_FALSE(g.code_points.empty());
            for (uint32_t cp : g.code_points) {
                INFO(g.name << " U+" << std::hex << cp);
                CHECK(fallback(cp));
                CHECK(is_font_fallback_candidate(cp));
                CHECK(cp <= 0xFFFF); // ImWchar and Windows wchar_t are 16 bit
                CHECK(seen.insert(cp).second);
                ++total;
            }
        }
        CHECK(total >= 100);
    }
}

TEST_CASE("Font fallback: the bundled font ships with its licence", "[FontFallback]")
{
    const std::string dir = resources_path() + "/fonts/";
    CHECK(boost::filesystem::exists(dir + "NotoSansSymbols2-Subset.ttf"));
    CHECK(boost::filesystem::file_size(dir + "NotoSansSymbols2-Subset.ttf") < 150 * 1024);
    CHECK(boost::filesystem::exists(dir + "OFL-NotoSansSymbols2.txt"));
    CHECK(boost::filesystem::exists(dir + "NotoSansSymbols2-Subset.README.txt"));
}
