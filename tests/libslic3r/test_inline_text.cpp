#include <catch2/catch.hpp>

// Inline shapes and symbol fallback wired into the text of the Emboss tool (part 2): the text layout
// (text2vshapes) with the bundled symbol font and with shapes of a volume's table, curved text and
// letter-by-letter placement with shapes, the 3MF round trip, hostile project data and volume names.

#include <libslic3r/libslic3r.h>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/CutSurface.hpp>
#include <libslic3r/Emboss.hpp>
#include <libslic3r/EmbossBend.hpp>
#include <libslic3r/EmbossBendSurface.hpp>
#include <libslic3r/EmbossShape.hpp>
#include <libslic3r/FontFallback.hpp>
#include <libslic3r/InlineShapes.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/UntrustedInput.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/miniz_extension.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <cereal/archives/binary.hpp>

#include <cmath>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Emboss;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::string resources_path() { return std::string(TEST_DATA_DIR) + "/../../resources"; }
std::string kr_font_path() { return resources_path() + "/fonts/NotoSansKR-Regular.ttf"; }
std::string symbol_font_path() { return resources_path() + "/fonts/NotoSansSymbols2-Subset.ttf"; }

BuiltinShapeLibrary &library()
{
    static BuiltinShapeLibrary lib(resources_path() + "/shapes/inline");
    static bool                loaded = false;
    if (!loaded) {
        std::string error;
        REQUIRE(lib.load(&error));
        loaded = true;
    }
    return lib;
}

std::shared_ptr<const FontFile> symbol_font()
{
    static std::shared_ptr<const FontFile> font = Emboss::create_font_file(symbol_font_path().c_str());
    REQUIRE(font != nullptr);
    return font;
}

FontFileWithCache primary_font()
{
    std::unique_ptr<FontFile> ff = Emboss::create_font_file(kr_font_path().c_str());
    REQUIRE(ff != nullptr);
    return FontFileWithCache(std::move(ff));
}

FontProp left_top_prop(float size_mm = 8.f)
{
    FontProp fp(size_mm);
    fp.align = FontProp::Align(FontProp::HorizontalAlign::left, FontProp::VerticalAlign::top);
    return fp;
}

TextGlyphSources sources_with(const InlineShapeTable *table, bool fallback = true)
{
    TextGlyphSources s;
    s.inline_shapes = table;
    s.library       = &library();
    s.fallback_font = fallback ? symbol_font() : nullptr;
    s.use_fallback  = fallback;
    return s;
}

std::wstring wide(const std::string &utf8) { return boost::nowide::widen(utf8); }

std::string placeholder(uint16_t code) { return codepoints_to_utf8({code}); }

InlineShape builtin(const std::string &id)
{
    std::optional<InlineShape> e = library().make_entry(id);
    REQUIRE(e.has_value());
    return *e;
}

const char *rect_svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\" viewBox=\"0 0 40 20\">"
                       "<rect x=\"0\" y=\"0\" width=\"40\" height=\"20\" fill=\"#c00\"/></svg>";

InlineShape user_svg(const std::string &name, const std::string &svg)
{
    InlineShape s;
    s.source   = InlineShapeSource::Svg;
    s.id       = name;
    s.svg_data = std::make_shared<const std::string>(svg);
    return s;
}

double area_of(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a;
}

std::string temp_3mf(const std::string &name)
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());
    return (dir / (std::to_string(get_current_pid()) + "_" + name)).string();
}

void store_project(const std::string &path, const std::vector<TextConfiguration> &texts)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "plate";
    object->add_volume(make_cube(40., 40., 3.))->name = "base";
    int i = 0;
    for (const TextConfiguration &tc : texts) {
        ModelVolume *text = object->add_volume(make_cube(10., 4., 1.));
        text->name        = "text" + std::to_string(i++);
        EmbossShape es;
        es.scale            = 5e-6;
        es.projection.depth = 1.;
        text->emboss_shape  = es;
        text->text_configuration = tc;
    }
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    PlateData          plate;
    plate.plate_index = 0;
    StoreParams sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(sp));
}

std::vector<TextConfiguration> load_texts(const std::string &path)
{
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    bool loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf, &file_version, nullptr,
                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *preset : project_presets)
        delete preset;
    REQUIRE(loaded);
    std::vector<TextConfiguration> out;
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            if (v->text_configuration.has_value())
                out.push_back(*v->text_configuration);
    return out;
}

struct ZipEntry
{
    std::string name;
    std::string data;
};

std::vector<ZipEntry> read_zip(const std::string &path)
{
    std::vector<ZipEntry> out;
    mz_zip_archive        zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, path));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        size_t size = 0;
        void  *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        REQUIRE(data != nullptr);
        out.push_back({stat.m_filename, std::string(static_cast<const char *>(data), size)});
        mz_free(data);
    }
    close_zip_reader(&zip);
    return out;
}

void write_zip(const std::string &path, const std::vector<ZipEntry> &entries)
{
    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, path));
    for (const ZipEntry &e : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, e.name.c_str(), e.data.data(), e.data.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    close_zip_writer(&zip);
}

std::string xml_attr_escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

// Same as the surface job (cut_curved_glyph_surface): one glyph projected along its own normal
SurfaceCut project_glyph(const ExPolygons &glyph, double shape_scale, const indexed_triangle_set &mesh)
{
    const double  safe    = 1.;
    BoundingBoxf3 mesh_bb = bounding_box(mesh);
    const double  min_z   = mesh_bb.min.z() - safe;
    const double  max_z   = mesh_bb.max.z() + safe;
    Transform3d   tr      = Transform3d::Identity();
    tr.translate(Vec3d(0., 0., min_z));
    tr.scale(shape_scale);
    OrthoProject projection(tr, Vec3d(0., 0., max_z - min_z));
    const float  ratio = static_cast<float>((-mesh_bb.min.z() + safe) / (mesh_bb.max.z() - mesh_bb.min.z() + 2. * safe));
    indexed_triangle_set aoi = its_cut_AoI(mesh, get_extents(glyph), projection);
    if (aoi.indices.empty())
        return {};
    return cut_surface(glyph, {aoi}, projection, ratio);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// phase 0: symbols from the bundled font when the selected font lacks them
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline text: a symbol the font lacks comes from the bundled font in a real layout", "[InlineText][FontFallback]")
{
    const FontProp fp = left_top_prop();
    // U+2714 heavy check mark: not in Noto Sans KR, in the bundled subset; U+263A: in neither
    const std::wstring text = wide("A\xE2\x9C\x94" "B");

    FontFileWithCache with_fb = primary_font();
    GlyphAdvances     adv_fb;
    ExPolygonsWithIds fb = text2vshapes(with_fb, text, fp, []() { return false; }, adv_fb, sources_with(nullptr, true));

    FontFileWithCache without_fb = primary_font();
    GlyphAdvances     adv_none;
    ExPolygonsWithIds none = text2vshapes(without_fb, text, fp, []() { return false; }, adv_none, sources_with(nullptr, false));

    REQUIRE(fb.size() == 3);
    REQUIRE(none.size() == 3);
    // with the fallback the check mark is drawn and takes its place in the line
    CHECK_FALSE(fb[1].expoly.empty());
    CHECK(adv_fb[1].valid);
    CHECK(adv_fb[1].x_min == adv_fb[0].x_max);
    CHECK(adv_fb[2].x_min == adv_fb[1].x_max);
    // without it the character vanishes, as before (no shape, no advance)
    CHECK(none[1].expoly.empty());
    CHECK_FALSE(adv_none[1].valid);
    CHECK(adv_none[2].x_min == adv_none[0].x_max);
    // letters of the selected font are the same either way
    CHECK(fb[0].expoly == none[0].expoly);

    // the fallback glyph is the symbol font's glyph at the same em, on the same baseline
    std::optional<Glyph> direct = letter2glyph(*symbol_font(), 0, 0x2714, 1.f);
    REQUIRE(direct.has_value());
    const BoundingBox in_text = get_extents(fb[1].expoly);
    const BoundingBox alone   = get_extents(direct->shape);
    CHECK_THAT(double(in_text.size().y()), WithinRel(double(alone.size().y()), 0.02));
    const BoundingBox a_box = get_extents(fb[0].expoly); // 'A' sits on the baseline
    CHECK(std::abs((in_text.min.y() - a_box.min.y()) - alone.min.y()) < 0.02 * alone.size().y() + 2000);

    SECTION("a character in no font stays empty")
    {
        FontFileWithCache ff = primary_font();
        GlyphAdvances     adv;
        ExPolygonsWithIds shapes = text2vshapes(ff, wide("A\xE2\x98\xBA" "B"), fp, []() { return false; }, adv, sources_with(nullptr));
        REQUIRE(shapes.size() == 3);
        CHECK(shapes[1].expoly.empty());
        CHECK_FALSE(adv[1].valid);
    }
    SECTION("a symbol the selected font has stays the font's own")
    {
        FontFileWithCache a = primary_font(), b = primary_font();
        GlyphAdvances     adv_a, adv_b;
        const std::wstring star = wide("\xE2\x98\x85"); // U+2605: Noto Sans KR has it
        ExPolygonsWithIds sa = text2vshapes(a, star, fp, []() { return false; }, adv_a, sources_with(nullptr, true));
        ExPolygonsWithIds sb = text2vshapes(b, star, fp, []() { return false; }, adv_b, sources_with(nullptr, false));
        REQUIRE(sa.size() == 1);
        CHECK_FALSE(sa[0].expoly.empty());
        CHECK(sa[0].expoly == sb[0].expoly);
    }
    SECTION("fallback glyphs are scaled to the em of the selected font")
    {
        // the same font data told it has twice the units per em: the fallback glyph doubles
        FontFileWithCache   normal = primary_font();
        std::unique_ptr<FontFile> big = Emboss::create_font_file(kr_font_path().c_str());
        REQUIRE(big != nullptr);
        for (FontFile::Info &info : big->infos)
            info.unit_per_em *= 2;
        FontFileWithCache doubled(std::move(big));
        const std::wstring check = wide("\xE2\x9C\x94");
        GlyphAdvances     adv_n, adv_d;
        ExPolygonsWithIds n = text2vshapes(normal, check, fp, []() { return false; }, adv_n, sources_with(nullptr));
        ExPolygonsWithIds d = text2vshapes(doubled, check, fp, []() { return false; }, adv_d, sources_with(nullptr));
        REQUIRE_FALSE(n[0].expoly.empty());
        REQUIRE_FALSE(d[0].expoly.empty());
        CHECK_THAT(double(get_extents(d[0].expoly).size().y()), WithinRel(2. * get_extents(n[0].expoly).size().y(), 0.02));
        CHECK_THAT(adv_d[0].x_max - adv_d[0].x_min, WithinRel(2. * (adv_n[0].x_max - adv_n[0].x_min), 0.01));
    }
    SECTION("the text box sorts characters by the font that draws them")
    {
        FontFileWithCache ff       = primary_font();
        GlyphCoverage     primary  = make_font_coverage(*ff.font_file);
        GlyphCoverage     fallback = make_font_coverage(*symbol_font());
        InlineShapeTable  table;
        const uint16_t    code = *add_inline_shape(table, builtin("star"));
        const std::string text_utf8 = "AB\xE2\x9C\x94\xE2\x9C\x94\n\t" + placeholder(code) + placeholder(0xF7F0) + "\xE2\x98\xBA";
        TextGlyphSplit split = split_text_by_glyph_source(text_utf8, primary, fallback,
                                                          [&table](uint32_t cp) { return find_inline_shape(table, cp) != nullptr; });
        CHECK(split.primary == "AB");
        CHECK(split.fallback == "\xE2\x9C\x94");
        REQUIRE(split.inline_shapes.size() == 1);
        CHECK(split.inline_shapes.front() == code);
        CHECK(split.exist_unknown); // U+263A and the unknown placeholder
        TextGlyphSplit clean = split_text_by_glyph_source("A \xE2\x9C\x94", primary, fallback, {});
        CHECK_FALSE(clean.exist_unknown);
    }
}

// ---------------------------------------------------------------------------------------------
// phase 1: built-in shapes in a text line
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline text: a shape in a text line has its advance and sits on the baseline", "[InlineText]")
{
    InlineShapeTable table;
    const uint16_t   star = *add_inline_shape(table, builtin("star"));
    const std::string text = "H" + placeholder(star) + "H";

    auto layout = [&](const FontProp &fp, GlyphAdvances &adv) {
        FontFileWithCache ff = primary_font();
        return text2vshapes(ff, wide(text), fp, []() { return false; }, adv, sources_with(&table));
    };
    auto expected_glyph = [&](const FontProp &fp) {
        FontFileWithCache ff = primary_font();
        InlineShapeCache  cache;
        std::optional<Glyph> g = make_inline_glyph(table.front(), library(), cache, inline_font_metrics(*ff.font_file, fp));
        REQUIRE(g.has_value());
        return *g;
    };

    const FontProp    fp = left_top_prop();
    GlyphAdvances     adv;
    ExPolygonsWithIds shapes = layout(fp, adv);
    REQUIRE(shapes.size() == 3); // one entry per character, as the bend and per-letter code require
    CHECK(shapes[1].id == star);
    REQUIRE_FALSE(shapes[1].expoly.empty());
    REQUIRE(adv[1].valid);

    const Glyph g = expected_glyph(fp);
    // advance: the cursor moves by the shape's advance, the next letter follows it
    CHECK(adv[1].x_min == adv[0].x_max);
    CHECK_THAT(adv[1].x_max - adv[1].x_min, WithinAbs(double(g.advance_width), 0.5));
    CHECK(adv[2].x_min == adv[1].x_max);
    // baseline: the shape is the placed glyph moved to the cursor and the line's offset ('H' has its
    // bottom on the baseline)
    const BoundingBox h_box     = get_extents(shapes[0].expoly);
    const BoundingBox star_box  = get_extents(shapes[1].expoly);
    const BoundingBox glyph_box = get_extents(g.shape);
    const coord_t     baseline  = h_box.min.y();
    CHECK(std::abs((star_box.min.y() - baseline) - glyph_box.min.y()) <= 2);
    CHECK(std::abs((star_box.min.x() - coord_t(adv[1].x_min)) - glyph_box.min.x()) <= 2);
    // sized to cap height: the star's design box is the cap height, so it is no taller than 'H'
    CHECK(star_box.size().y() <= h_box.size().y() * 1.02);
    CHECK(star_box.size().y() >= h_box.size().y() * 0.6);
    CHECK(star_box.min.y() >= baseline - h_box.size().y() / 50);

    SECTION("bold and italic text gives a bold, slanted shape")
    {
        FontProp styled = fp;
        styled.boldness = 60.f; // font points, the Bold button sets 20
        styled.skew     = 0.25f;
        GlyphAdvances     adv_s;
        ExPolygonsWithIds s = layout(styled, adv_s);
        REQUIRE(s.size() == 3);
        const Glyph       gs  = expected_glyph(styled);
        const BoundingBox box = get_extents(s[1].expoly);
        CHECK(box.size().x() > star_box.size().x());
        CHECK(area_of(s[1].expoly) > area_of(shapes[1].expoly) * 1.01);
        CHECK(std::abs(box.size().x() - get_extents(gs.shape).size().x()) <= 4);
        // the slant moves the top to the right: the topmost point is right of the plain one
        CHECK(box.max.x() - coord_t(adv_s[1].x_min) > star_box.max.x() - coord_t(adv[1].x_min));
    }
    SECTION("a placeholder the table does not know is empty, like a missing glyph")
    {
        const std::string unknown = "H" + placeholder(star + 1) + "H";
        FontFileWithCache ff = primary_font();
        GlyphAdvances     a;
        ExPolygonsWithIds sh = text2vshapes(ff, wide(unknown), fp, []() { return false; }, a, sources_with(&table));
        REQUIRE(sh.size() == 3);
        CHECK(sh[1].expoly.empty());
        CHECK_FALSE(a[1].valid);
    }
    SECTION("an unusable user SVG is empty too, and nothing throws")
    {
        InlineShapeTable bad;
        const uint16_t   code = *add_inline_shape(bad, user_svg("broken", "<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"M0 0\"/></svg>"));
        FontFileWithCache ff = primary_font();
        GlyphAdvances     a;
        ExPolygonsWithIds sh;
        REQUIRE_NOTHROW(sh = text2vshapes(ff, wide("H" + placeholder(code)), fp, []() { return false; }, a, sources_with(&bad)));
        REQUIRE(sh.size() == 2);
        CHECK(sh[1].expoly.empty());
    }
    SECTION("two volumes sharing a font cache use one code for different shapes")
    {
        InlineShapeTable other;
        const uint16_t   heart = *add_inline_shape(other, builtin("heart"));
        REQUIRE(heart == star); // both tables start at the first free code
        FontFileWithCache ff = primary_font(); // one cache for both calls, as volumes of one style share it
        GlyphAdvances     a1, a2;
        ExPolygonsWithIds s1 = text2vshapes(ff, wide(text), fp, []() { return false; }, a1, sources_with(&table));
        ExPolygonsWithIds s2 = text2vshapes(ff, wide(text), fp, []() { return false; }, a2, sources_with(&other));
        CHECK(s1[1].expoly != s2[1].expoly);
        FontFileWithCache fresh = primary_font();
        GlyphAdvances     a3;
        ExPolygonsWithIds s3 = text2vshapes(fresh, wide(text), fp, []() { return false; }, a3, sources_with(&other));
        CHECK(s2[1].expoly == s3[1].expoly);
    }
    SECTION("a user SVG shape is sized like the built-in ones")
    {
        InlineShapeTable svgs;
        const uint16_t   code = *add_inline_shape(svgs, user_svg("bar", rect_svg));
        FontFileWithCache ff = primary_font();
        GlyphAdvances     a;
        ExPolygonsWithIds sh = text2vshapes(ff, wide("H" + placeholder(code)), fp, []() { return false; }, a, sources_with(&svgs));
        REQUIRE(sh.size() == 2);
        REQUIRE_FALSE(sh[1].expoly.empty());
        const BoundingBox box = get_extents(sh[1].expoly);
        const BoundingBox h   = get_extents(sh[0].expoly);
        // an ink box: the 40 x 20 rectangle is cap height tall, twice as wide, on the baseline
        CHECK_THAT(double(box.size().y()), WithinRel(double(h.size().y()), 0.03));
        CHECK_THAT(double(box.size().x()) / double(box.size().y()), WithinRel(2., 0.02));
        CHECK(std::abs(box.min.y() - h.min.y()) <= h.size().y() / 50);
    }
}

// ---------------------------------------------------------------------------------------------
// volume names
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline text: placeholders never leak into volume names", "[InlineText]")
{
    InlineShapeTable table;
    const uint16_t   star  = *add_inline_shape(table, builtin("star"));
    const uint16_t   heart = *add_inline_shape(table, builtin("heart"));
    InlineShape      logo  = user_svg("my logo", rect_svg);
    const uint16_t   svg   = *add_inline_shape(table, logo);

    const std::string text = "I " + placeholder(heart) + " 3D\nprint " + placeholder(star) + placeholder(svg) + placeholder(0xF8F0);
    const std::string name = text_volume_name(text, table);
    CHECK(name == "I [heart] 3D print [star][my logo]");
    for (uint32_t cp : utf8_to_codepoints(name)) {
        CHECK_FALSE(is_inline_shape_code(cp));
        CHECK(cp != '\n');
    }

    SECTION("names from a hostile project are cleaned")
    {
        InlineShapeTable hostile = table;
        hostile.back().id = "evil" + placeholder(0xF700) + "\n]name[";
        const std::string n = text_volume_name(placeholder(svg), hostile);
        CHECK(n == "[evilname]");
        hostile.back().id.clear();
        CHECK(text_volume_name(placeholder(svg), hostile) == "[shape]");
    }
    SECTION("display names of user SVG files carry no folder")
    {
        CHECK(inline_svg_display_name("C:\\Users\\someone\\Desktop\\Logo [final].svg") == "Logo final");
        CHECK(inline_svg_display_name("/home/someone/x.svg") == "x");
        CHECK(inline_svg_display_name(".svg") == "svg");
        CHECK(inline_svg_display_name(std::string(200, 'a') + ".svg").size() == 40);
    }
}

TEST_CASE("Inline text: inserting at the caret keeps whole characters", "[InlineText]")
{
    const std::string star = placeholder(0xF700);
    std::string       text = "a\xE2\x9C\x94" "b"; // a, check mark (3 bytes), b
    // a caret in the middle of the check mark moves back to its start
    size_t caret = insert_utf8_at(text, 2, 2, star);
    CHECK(text == "a" + star + "\xE2\x9C\x94" "b");
    CHECK(caret == 1 + star.size());
    // a selection is replaced
    std::string sel = "hello";
    caret           = insert_utf8_at(sel, 4, 1, "X");
    CHECK(sel == "hXo");
    CHECK(caret == 2);
    // out of range: appended
    std::string end = "ab";
    caret           = insert_utf8_at(end, 99, 120, star);
    CHECK(end == "ab" + star);
    CHECK(caret == end.size());
}

// ---------------------------------------------------------------------------------------------
// phase 2: user SVGs
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline text: user SVG files are checked before they become shapes", "[InlineText]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(dir);
    const std::string prefix = (dir / (std::to_string(get_current_pid()) + "_inline_")).string();
    auto write = [](const std::string &path, const std::string &data) {
        FILE *f = boost::nowide::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        if (!data.empty())
            fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    };
    std::vector<std::string> files;
    ScopeGuard               cleanup([&files]() {
        boost::system::error_code ec;
        for (const std::string &f : files)
            boost::filesystem::remove(f, ec);
    });

    SECTION("a good file: data and name, never the path")
    {
        const std::string path = prefix + "Badge.svg";
        files.push_back(path);
        write(path, rect_svg);
        std::string                error;
        std::optional<InlineShape> s = load_user_inline_svg(path, &error);
        INFO(error);
        REQUIRE(s.has_value());
        CHECK(s->source == InlineShapeSource::Svg);
        CHECK(s->id.find("Badge") != std::string::npos);
        CHECK(s->id.find('/') == std::string::npos);
        CHECK(s->id.find('\\') == std::string::npos);
        REQUIRE(s->svg_data != nullptr);
        CHECK(*s->svg_data == rect_svg);
        CHECK(is_inline_svg_entry_name(s->path_in_3mf));
        // nothing in the JSON for the 3MF names the local file
        InlineShapeTable t{*s};
        t.front().code = 0xF700;
        CHECK(inline_shapes_to_json(t).find(dir.string()) == std::string::npos);
    }
    SECTION("refused: too large (shared cap), empty, not an SVG, too complex")
    {
        const std::string big = prefix + "big.svg";
        files.push_back(big);
        write(big, std::string(rect_svg) + std::string(size_t(untrusted::SVG_SIZE_LIMIT) + 10, ' '));
        bool        too_large = false;
        std::string error;
        CHECK_FALSE(load_user_inline_svg(big, &error, &too_large).has_value());
        CHECK(too_large);

        const std::string empty = prefix + "empty.svg";
        files.push_back(empty);
        write(empty, "");
        error.clear();
        CHECK_FALSE(load_user_inline_svg(empty, &error, &too_large).has_value());
        CHECK_FALSE(too_large);
        CHECK_FALSE(error.empty());

        const std::string text = prefix + "text.svg";
        files.push_back(text);
        write(text, "this is not an svg");
        CHECK_FALSE(load_user_inline_svg(text, &error).has_value());

        const std::string complex = prefix + "complex.svg";
        files.push_back(complex);
        std::string body;
        for (size_t i = 0; i < untrusted::SVG_MAX_SHAPES + 1; ++i)
            body += "<rect x=\"1\" y=\"1\" width=\"5\" height=\"5\"/>";
        write(complex, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">" + body + "</svg>");
        error.clear();
        CHECK_FALSE(load_user_inline_svg(complex, &error).has_value());
        CHECK(error.find("too complex") != std::string::npos);

        CHECK_FALSE(load_user_inline_svg(prefix + "does_not_exist.svg", &error).has_value());
    }
}

// ---------------------------------------------------------------------------------------------
// 3MF
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline text: shapes round-trip through 3MF", "[InlineText][3mf]")
{
    const std::string path = temp_3mf("inline_shapes_roundtrip.3mf");
    ScopeGuard        cleanup([&path]() {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    });

    TextConfiguration a;
    a.style.name            = "test";
    a.style.path            = "test.ttf";
    a.style.type            = EmbossStyle::Type::file_path;
    a.style.prop.size_in_mm = 5.f;
    InlineShape star = builtin("star");
    star.scale       = 1.25f;
    star.dy          = -0.1f;
    const uint16_t c_star = *add_inline_shape(a.inline_shapes, star);
    const uint16_t c_svg  = *add_inline_shape(a.inline_shapes, user_svg("bar", rect_svg));
    a.text = "I" + placeholder(c_star) + "U " + placeholder(c_svg);
    // a second text with the same user SVG: one zip entry for both
    TextConfiguration b = a;
    b.inline_shapes.clear();
    const uint16_t b_svg = *add_inline_shape(b.inline_shapes, user_svg("bar again", rect_svg));
    b.text = placeholder(b_svg) + "!";
    // plain text: an old-style file, no attribute at all
    TextConfiguration plain = a;
    plain.inline_shapes.clear();
    plain.text = "plain";

    store_project(path, {a, b, plain});

    const std::vector<ZipEntry> entries = read_zip(path);
    std::vector<std::string>    svg_entries;
    std::string                 config;
    for (const ZipEntry &e : entries) {
        if (e.name.rfind("3D/inline_", 0) == 0)
            svg_entries.push_back(e.name);
        if (e.name == "Metadata/model_settings.config")
            config = e.data;
    }
    REQUIRE(svg_entries.size() == 1);
    CHECK(svg_entries.front() == inline_svg_entry_name(rect_svg));
    CHECK(is_inline_svg_entry_name(svg_entries.front()));
    // the attribute only where there are shapes; never a local path
    size_t count = 0;
    for (size_t pos = config.find("inline_shapes=\""); pos != std::string::npos; pos = config.find("inline_shapes=\"", pos + 1))
        ++count;
    CHECK(count == 2);

    const std::vector<TextConfiguration> loaded = load_texts(path);
    REQUIRE(loaded.size() == 3);
    const TextConfiguration &la = loaded[0];
    CHECK(la.text == a.text);
    REQUIRE(la.inline_shapes.size() == 2);
    const InlineShape *ls = find_inline_shape(la.inline_shapes, c_star);
    REQUIRE(ls != nullptr);
    CHECK(ls->source == InlineShapeSource::Builtin);
    CHECK(ls->id == "star");
    CHECK_THAT(ls->scale, WithinAbs(1.25, 1e-4));
    CHECK_THAT(ls->dy, WithinAbs(-0.1, 1e-4));
    const InlineShape *lv = find_inline_shape(la.inline_shapes, c_svg);
    REQUIRE(lv != nullptr);
    CHECK(lv->source == InlineShapeSource::Svg);
    CHECK(lv->id == "bar");
    REQUIRE(lv->svg_data != nullptr);
    CHECK(*lv->svg_data == rect_svg);

    const TextConfiguration &lb = loaded[1];
    CHECK(lb.text == b.text);
    REQUIRE(lb.inline_shapes.size() == 1);
    REQUIRE(lb.inline_shapes.front().svg_data != nullptr);
    CHECK(*lb.inline_shapes.front().svg_data == rect_svg);
    CHECK(lb.inline_shapes.front().id == "bar again");

    // an old file (no attribute) opens unchanged
    CHECK(loaded[2].text == "plain");
    CHECK(loaded[2].inline_shapes.empty());

    // and the loaded table lays out like the original
    FontFileWithCache f1 = primary_font(), f2 = primary_font();
    GlyphAdvances     adv1, adv2;
    const FontProp    fp = left_top_prop();
    ExPolygonsWithIds s1 = text2vshapes(f1, wide(a.text), fp, []() { return false; }, adv1, sources_with(&a.inline_shapes));
    ExPolygonsWithIds s2 = text2vshapes(f2, wide(la.text), fp, []() { return false; }, adv2, sources_with(&la.inline_shapes));
    REQUIRE(s1.size() == s2.size());
    for (size_t i = 0; i < s1.size(); ++i)
        CHECK(s1[i].expoly == s2[i].expoly);
    CHECK_FALSE(s2[1].expoly.empty());
    CHECK_FALSE(s2[4].expoly.empty());

    SECTION("undo/redo (cereal) carries the table and the SVG data")
    {
        std::stringstream ss;
        {
            cereal::BinaryOutputArchive out(ss);
            out(a);
        }
        TextConfiguration back;
        {
            cereal::BinaryInputArchive in(ss);
            in(back);
        }
        CHECK(back.text == a.text);
        REQUIRE(back.inline_shapes.size() == 2);
        REQUIRE(back.inline_shapes[1].svg_data != nullptr);
        CHECK(*back.inline_shapes[1].svg_data == rect_svg);
    }
}

TEST_CASE("Inline text: hostile inline_shapes JSON and SVG entries in a 3MF", "[InlineText][3mf]")
{
    const std::string path = temp_3mf("inline_shapes_hostile.3mf");
    ScopeGuard        cleanup([&path]() {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    });

    TextConfiguration tc;
    tc.style.name            = "test";
    tc.style.path            = "test.ttf";
    tc.style.type            = EmbossStyle::Type::file_path;
    tc.style.prop.size_in_mm = 5.f;
    const uint16_t code      = *add_inline_shape(tc.inline_shapes, builtin("star"));
    tc.text                  = "A" + placeholder(code);
    store_project(path, {tc});

    std::vector<ZipEntry> entries = read_zip(path);
    const std::string     good_name = "3D/inline_0badf00d.svg";
    const std::string     bomb_name = "3D/inline_b0b0b0b0.svg";
    const std::string     junk_name = "3D/inline_deadbeef.svg";
    const std::string hostile_json =
        std::string("[") +
        "{\"c\":63232,\"k\":\"b\",\"id\":\"star\"}," +                              // good
        "{\"c\":63233,\"k\":\"f\",\"id\":\"ok\",\"f\":\"" + good_name + "\"}," +    // good user SVG
        "{\"c\":63234,\"k\":\"f\",\"id\":\"escape\",\"f\":\"3D/../../evil.svg\"}," + // path escape: skipped
        "{\"c\":63235,\"k\":\"f\",\"id\":\"abs\",\"f\":\"C:/Windows/win.ini\"}," +   // not an entry name: skipped
        "{\"c\":63236,\"k\":\"f\",\"id\":\"bomb\",\"f\":\"" + bomb_name + "\"}," +  // entry refused by the size cap
        "{\"c\":63237,\"k\":\"f\",\"id\":\"junk\",\"f\":\"" + junk_name + "\"}," +  // entry is not an SVG
        "{\"c\":63238,\"k\":\"f\",\"id\":\"missing\",\"f\":\"3D/inline_00000000.svg\"}," + // no such entry
        "{\"c\":65,\"k\":\"b\",\"id\":\"star\"}," +                                  // code outside the range
        "{\"c\":63232,\"k\":\"b\",\"id\":\"heart\"}," +                              // duplicate code
        "{\"c\":63239,\"k\":\"b\",\"id\":\"star\",\"s\":1e300,\"dy\":-1e300,\"gl\":\"x\",\"a\":99}," + // clamped
        "\"garbage\",42,null,[1,2,3]," +
        "{\"c\":63240,\"k\":\"z\",\"id\":\"what\"}" +
        "]";
    bool replaced = false;
    for (ZipEntry &e : entries)
        if (e.name == "Metadata/model_settings.config") {
            const std::regex attr("inline_shapes=\"[^\"]*\"");
            REQUIRE(std::regex_search(e.data, attr));
            e.data   = std::regex_replace(e.data, attr, "inline_shapes=\"" + xml_attr_escape(hostile_json) + "\"",
                                          std::regex_constants::format_first_only);
            replaced = true;
        }
    REQUIRE(replaced);
    entries.push_back({good_name, rect_svg});
    entries.push_back({bomb_name, std::string(rect_svg) + std::string(size_t(untrusted::SVG_SIZE_LIMIT) + 1024, ' ')});
    entries.push_back({junk_name, std::string(4096, '\x01')});
    entries.push_back({"3D/inline_bad name.svg", rect_svg});  // a name the writer never makes
    entries.push_back({"3D/inline_..svg", rect_svg});
    write_zip(path, entries);

    std::vector<TextConfiguration> loaded;
    REQUIRE_NOTHROW(loaded = load_texts(path));
    REQUIRE(loaded.size() == 1);
    const InlineShapeTable &t = loaded.front().inline_shapes;
    // kept: star, ok, bomb, junk, missing, clamped; skipped: escape, abs, outside, duplicate, garbage, unknown kind
    CHECK(t.size() == 6);
    CHECK(find_inline_shape(t, 63234) == nullptr);
    CHECK(find_inline_shape(t, 63235) == nullptr);
    CHECK(find_inline_shape(t, 65) == nullptr);
    CHECK(find_inline_shape(t, 63240) == nullptr);
    const InlineShape *dup = find_inline_shape(t, 63232);
    REQUIRE(dup != nullptr);
    CHECK(dup->id == "star"); // the first one wins
    const InlineShape *ok = find_inline_shape(t, 63233);
    REQUIRE(ok != nullptr);
    REQUIRE(ok->svg_data != nullptr);
    CHECK(*ok->svg_data == rect_svg);
    const InlineShape *bomb = find_inline_shape(t, 63236);
    REQUIRE(bomb != nullptr);
    CHECK(bomb->svg_data == nullptr); // refused by the shared size cap before it was read
    const InlineShape *clamped = find_inline_shape(t, 63239);
    REQUIRE(clamped != nullptr);
    CHECK(clamped->scale <= 20.f);
    CHECK(clamped->dy >= -5.f);
    CHECK(clamped->anchor == InlineShapeAnchor::Baseline);

    // the layout survives every entry: bad ones are empty, good ones drawn
    std::string text;
    for (uint32_t c : {63232u, 63233u, 63236u, 63237u, 63238u, 63239u})
        text += placeholder(uint16_t(c));
    FontFileWithCache ff = primary_font();
    GlyphAdvances     adv;
    ExPolygonsWithIds shapes;
    REQUIRE_NOTHROW(shapes = text2vshapes(ff, wide(text), left_top_prop(), []() { return false; }, adv, sources_with(&t)));
    REQUIRE(shapes.size() == 6);
    CHECK_FALSE(shapes[0].expoly.empty()); // star
    CHECK_FALSE(shapes[1].expoly.empty()); // good user SVG
    CHECK(shapes[2].expoly.empty());       // bomb: no data
    CHECK(shapes[3].expoly.empty());       // junk: not an SVG
    CHECK(shapes[4].expoly.empty());       // missing entry
    CHECK_FALSE(shapes[5].expoly.empty()); // clamped star
}

// ---------------------------------------------------------------------------------------------
// phase 3: curves and surfaces
// ---------------------------------------------------------------------------------------------
TEST_CASE("Inline text: shapes bend with the letters", "[InlineText][EmbossBend]")
{
    InlineShapeTable table;
    const uint16_t   star  = *add_inline_shape(table, builtin("star"));
    const uint16_t   heart = *add_inline_shape(table, builtin("heart"));
    const std::string text = "Go" + placeholder(star) + "team" + placeholder(heart);

    FontFileWithCache ff = primary_font();
    FontProp          fp(6.f);
    GlyphAdvances     advances;
    const ExPolygonsWithIds straight = text2vshapes(ff, wide(text), fp, []() { return false; }, advances, sources_with(&table));
    REQUIRE(straight.size() == 8);
    REQUIRE(advances.size() == 8);
    REQUIRE_FALSE(straight[2].expoly.empty());
    REQUIRE_FALSE(straight[7].expoly.empty());
    const double scale = get_text_shape_scale(fp, *ff.font_file);

    for (bool rigid : {false, true})
        for (float angle : {90.f, 300.f}) {
            DYNAMIC_SECTION((rigid ? "rigid " : "bent ") << angle << " degrees")
            {
                ExPolygonsWithIds bent = straight;
                EmbossBend        bend;
                bend.mode  = EmbossBend::Mode::angle;
                bend.angle = angle;
                bend.rigid = rigid;
                GlyphAdvances adv = advances;
                BendResult    r   = apply_bend(bent, bend, scale, &adv, BEND_TOLERANCE_MM);
                REQUIRE(r.is_active());
                REQUIRE(bent.size() == straight.size());
                for (size_t i = 0; i < bent.size(); ++i) {
                    INFO("entry " << i);
                    CHECK(bent[i].expoly.empty() == straight[i].expoly.empty());
                    for (const ExPolygon &e : bent[i].expoly)
                        CHECK(e.is_valid());
                }
                // a rigid shape keeps its area like a rigid letter (moved and turned only)
                if (rigid)
                    for (size_t i : {size_t(2), size_t(7)})
                        CHECK_THAT(area_of(bent[i].expoly), WithinRel(area_of(straight[i].expoly), 0.005));
                // the shapes stay in reading order along the arc with the letters: the pivots of the
                // straight text keep their order (no shape jumps)
                std::vector<double> pivots = glyph_pivots(straight, &advances);
                REQUIRE(pivots.size() == straight.size());
                CHECK(pivots[1] < pivots[2]);
                CHECK(pivots[2] < pivots[3]);
                CHECK(pivots[6] < pivots[7]);
            }
        }
}

TEST_CASE("Inline text: shapes letter by letter on a sphere", "[InlineText][EmbossBend][surface][letters]")
{
    InlineShapeTable table;
    const uint16_t   star  = *add_inline_shape(table, builtin("star"));
    const uint16_t   ring  = *add_inline_shape(table, builtin("ring"));
    const std::string text = "A" + placeholder(star) + "B" + placeholder(ring);

    FontFileWithCache ff = primary_font();
    FontProp          fp(6.f);
    fp.per_glyph = true;
    GlyphAdvances           advances;
    const ExPolygonsWithIds shapes = text2vshapes(ff, wide(text), fp, []() { return false; }, advances, sources_with(&table));
    REQUIRE(shapes.size() == 4);
    const double scale = get_text_shape_scale(fp, *ff.font_file);

    const double         rs     = 25.;
    indexed_triangle_set sphere = its_make_sphere(rs, PI / 90.);
    its_translate(sphere, Vec3f(0.f, 0.f, -static_cast<float>(rs)));
    BendSurface surface(sphere);

    for (bool rigid : {true, false}) {
        DYNAMIC_SECTION((rigid ? "rigid" : "bent"))
        {
            EmbossBend bend;
            bend.mode  = EmbossBend::Mode::angle;
            bend.angle = 120.f;
            bend.rigid = rigid;
            std::optional<SurfaceGlyphLayout> layout = surface_glyph_layout(shapes, &advances, bend, scale);
            REQUIRE(layout.has_value());
            REQUIRE(layout->pivots.size() == shapes.size());
            SurfaceArc arc = place_on_surface_arc(surface, layout->pivots_mm, layout->x_min, layout->x_max, layout->params);
            REQUIRE(arc.preview.valid);
            REQUIRE(arc.frames.size() == shapes.size());
            for (size_t i = 0; i < shapes.size(); ++i) {
                INFO("glyph " << i);
                REQUIRE_FALSE(shapes[i].expoly.empty());
                CHECK_FALSE(std::isnan(layout->pivots[i]));
                REQUIRE(arc.frames[i].has_value());
                const ExPolygons local = surface_glyph_shape(shapes[i].expoly, layout->pivots[i], arc.curvature_radius[i], bend, scale);
                REQUIRE_FALSE(local.empty());
                // each glyph (letters and shapes alike) projects into its own frame onto the sphere
                indexed_triangle_set local_mesh = sphere;
                its_transform(local_mesh, arc.frames[i]->inverse());
                SurfaceCut cut = project_glyph(local, scale, local_mesh);
                CHECK_FALSE(cut.empty());
                // the frame origin is on the sphere
                CHECK_THAT((arc.frames[i]->translation() - Vec3d(0., 0., -rs)).norm(), WithinAbs(rs, 0.05));
            }
            // the ring keeps its hole through the placement
            const ExPolygons ring_local = surface_glyph_shape(shapes[3].expoly, layout->pivots[3], arc.curvature_radius[3], bend, scale);
            size_t holes = 0;
            for (const ExPolygon &e : ring_local)
                holes += e.holes.size();
            CHECK(holes >= 1);
        }
    }
}
