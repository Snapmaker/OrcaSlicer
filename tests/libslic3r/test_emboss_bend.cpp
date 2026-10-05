#include <catch2/catch.hpp>

// Curved (arc) text for the Emboss/Text tool: the pure 2D bend, its projection onto curved
// surfaces ("Use surface") and its 3MF round trip.

#include <libslic3r/EmbossBend.hpp>
#include <libslic3r/EmbossBendSurface.hpp>
#include <libslic3r/Emboss.hpp>
#include <libslic3r/EmbossShape.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/IntersectionPoints.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/AABBMesh.hpp>
#include <libslic3r/CutSurface.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/miniz_extension.hpp>

#include <boost/filesystem.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Emboss;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// 1 shape unit = 1 nm, so 1 mm = 1e6 units (real text uses about 5e-6 mm per unit)
constexpr double SCALE = 1e-6;
constexpr double MM    = 1. / SCALE;

ExPolygon rectangle(double x0, double y0, double x1, double y1)
{
    Polygon p;
    p.points = {Point(coord_t(x0), coord_t(y0)), Point(coord_t(x1), coord_t(y0)), Point(coord_t(x1), coord_t(y1)),
                Point(coord_t(x0), coord_t(y1))};
    return ExPolygon(std::move(p)); // counter-clockwise
}

double shape_area(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a;
}

// a row of letter-like boxes, `count` glyphs of 6 x 8 mm with 1 mm gap, centred on x = 0
ExPolygonsWithIds box_text(int count, GlyphAdvances *advances = nullptr)
{
    ExPolygonsWithIds result;
    const double advance = 7. * MM;
    const double start   = -count * advance / 2.;
    for (int i = 0; i < count; ++i) {
        double x0 = start + i * advance + 0.5 * MM;
        result.push_back({unsigned('A' + i), {rectangle(x0, -2. * MM, x0 + 6. * MM, 6. * MM)}});
        if (advances != nullptr)
            advances->push_back({start + i * advance, start + (i + 1) * advance, true});
    }
    return result;
}

EmbossBend angle_bend(float angle, bool inside = false, bool rigid = false)
{
    EmbossBend b;
    b.mode   = EmbossBend::Mode::angle;
    b.angle  = angle;
    b.inside = inside;
    b.rigid  = rigid;
    return b;
}

EmbossBend radius_bend(float radius_mm, bool inside = false, bool rigid = false)
{
    EmbossBend b;
    b.mode   = EmbossBend::Mode::radius;
    b.radius = radius_mm;
    b.inside = inside;
    b.rigid  = rigid;
    return b;
}

BendSpec spec(double radius_mm, bool inside)
{
    BendSpec s;
    s.radius    = radius_mm * MM;
    s.inside    = inside;
    s.tolerance = BEND_TOLERANCE_MM * MM;
    return s;
}

bool same_points(const ExPolygonsWithIds &a, const ExPolygonsWithIds &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].expoly.size() != b[i].expoly.size())
            return false;
        for (size_t j = 0; j < a[i].expoly.size(); ++j)
            if (a[i].expoly[j] != b[i].expoly[j])
                return false;
    }
    return true;
}

std::string font_path() { return std::string(TEST_DATA_DIR) + "/../../resources/fonts/NotoSansKR-Regular.ttf"; }

} // namespace

TEST_CASE("Curved text off or 0 degrees is identical to straight text", "[EmbossBend]")
{
    GlyphAdvances           advances;
    const ExPolygonsWithIds straight = box_text(5, &advances);

    SECTION("mode off") {
        ExPolygonsWithIds shapes = straight;
        BendResult        r      = apply_bend(shapes, EmbossBend{}, SCALE, &advances);
        CHECK_FALSE(r.is_active());
        CHECK(same_points(shapes, straight));
    }
    SECTION("angle mode at 0 degrees") {
        ExPolygonsWithIds shapes = straight;
        BendResult        r      = apply_bend(shapes, angle_bend(0.f), SCALE, &advances);
        CHECK_FALSE(r.is_active());
        CHECK(same_points(shapes, straight));
    }
    SECTION("radius mode at 0 mm") {
        ExPolygonsWithIds shapes = straight;
        apply_bend(shapes, radius_bend(0.f), SCALE, &advances);
        CHECK(same_points(shapes, straight));
    }
    SECTION("inactive spec returns the input") {
        ExPolygons in{rectangle(0, 0, 5 * MM, 3 * MM)};
        CHECK(bend_expolygons(in, BendSpec{}) == in);
    }
}

TEST_CASE("Points of the reference line land on the circle with arc-length spacing", "[EmbossBend]")
{
    for (bool inside : {false, true}) {
        DYNAMIC_SECTION((inside ? "smile" : "arch")) {
            const BendSpec s = spec(25., inside);
            const Vec2d    c = s.center();
            CHECK_THAT(c.y(), WithinAbs(inside ? 25. * MM : -25. * MM, 1e-6));
            CHECK(bend_point(Vec2d::Zero(), s).norm() < 1e-9); // origin stays

            std::vector<double> xs = {-60. * MM, -20. * MM, -3. * MM, 0., 7. * MM, 33. * MM, 70. * MM};
            for (double x : xs) {
                Vec2d p = bend_point(Vec2d(x, 0.), s);
                CHECK_THAT((p - c).norm(), WithinAbs(s.radius, 1.));
            }
            // arc length between consecutive points equals their straight distance
            for (size_t i = 1; i < xs.size(); ++i) {
                Vec2d  a     = bend_point(Vec2d(xs[i - 1], 0.), s) - c;
                Vec2d  b     = bend_point(Vec2d(xs[i], 0.), s) - c;
                double angle = std::atan2(a.x() * b.y() - a.y() * b.x(), a.dot(b));
                CHECK_THAT(std::abs(angle) * s.radius, WithinAbs(xs[i] - xs[i - 1], 1.));
            }
            // a point above the reference line lands at distance R + s*y (arch outside, smile inside)
            Vec2d p = bend_point(Vec2d(12. * MM, 4. * MM), s);
            CHECK_THAT((p - c).norm(), WithinAbs(s.radius + s.side() * 4. * MM, 1.));
        }
    }
}

TEST_CASE("Smile mirrors arch and both keep the winding", "[EmbossBend]")
{
    const BendSpec arch  = spec(30., false);
    const BendSpec smile = spec(30., true);
    for (Vec2d p : {Vec2d(5. * MM, 3. * MM), Vec2d(-17. * MM, -2. * MM), Vec2d(40. * MM, 8. * MM)}) {
        // smile(x, y) = mirror_y(arch(x, -y))
        Vec2d a = bend_point(Vec2d(p.x(), -p.y()), arch);
        Vec2d s = bend_point(p, smile);
        CHECK_THAT(s.x(), WithinAbs(a.x(), 1e-3));
        CHECK_THAT(s.y(), WithinAbs(-a.y(), 1e-3));
    }

    ExPolygons in{rectangle(-10. * MM, 0., 10. * MM, 5. * MM)};
    REQUIRE(in.front().contour.is_counter_clockwise());
    ExPolygons a = bend_expolygons(in, arch);
    ExPolygons s = bend_expolygons(in, smile);
    REQUIRE(a.size() == 1);
    REQUIRE(s.size() == 1);
    CHECK(a.front().contour.is_counter_clockwise());
    CHECK(s.front().contour.is_counter_clockwise());

    // arch: the text centre sits above the arc centre, smile: below it
    BoundingBox ba = get_extents(a), bs = get_extents(s);
    CHECK(ba.center().y() > arch.center().y());
    CHECK(bs.center().y() < smile.center().y());
}

TEST_CASE("Warped area follows the polar area law", "[EmbossBend]")
{
    const double w = 10. * MM, h = 3. * MM, R = 20. * MM;
    ExPolygons   in{rectangle(-w / 2., 0., w / 2., h)};
    double       arch  = shape_area(bend_expolygons(in, spec(20., false)));
    double       smile = shape_area(bend_expolygons(in, spec(20., true)));
    CHECK_THAT(arch, WithinRel(w * h * (1. + h / (2. * R)), 5e-3));
    CHECK_THAT(smile, WithinRel(w * h * (1. - h / (2. * R)), 5e-3));
}

TEST_CASE("Densified edges stay within the chord tolerance", "[EmbossBend]")
{
    const BendSpec s = spec(15., false);
    const double   y = 4. * MM;
    // long horizontal top edge maps onto an arc of radius R + y
    Polygon in = rectangle(-30. * MM, 0., 30. * MM, y).contour;
    Polygon out = bend_polygon(in, s);
    REQUIRE(out.size() > 20);
    const Vec2d  c   = s.center();
    const double rho = s.radius + y;
    size_t       on_arc = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        Vec2d a = out[i].cast<double>(), b = out[(i + 1) % out.size()].cast<double>();
        if (std::abs((a - c).norm() - rho) > 2. || std::abs((b - c).norm() - rho) > 2.)
            continue; // not a segment of the top arc
        ++on_arc;
        Vec2d mid = (a + b) / 2.;
        CHECK(rho - (mid - c).norm() <= s.tolerance * 1.01 + 2.);
    }
    CHECK(on_arc > 10);
}

TEST_CASE("At 359 degrees the ends of the text do not meet", "[EmbossBend]")
{
    for (bool rigid : {false, true}) {
        DYNAMIC_SECTION((rigid ? "rigid" : "bent")) {
            GlyphAdvances     advances;
            ExPolygonsWithIds shapes = box_text(12, &advances);
            BendResult        r      = apply_bend(shapes, angle_bend(359.f, false, rigid), SCALE, &advances);
            REQUIRE(r.is_active());
            CHECK_THAT(r.angle_deg, WithinAbs(359., 1e-6));
            CHECK_FALSE(r.limited_by_height);

            // angle around the arc centre: 0 at 12 o'clock, clockwise positive
            const Vec2d c = r.spec.center();
            auto angle_range = [&c](const ExPolygons &glyph) {
                double lo = 10., hi = -10.;
                for (const ExPolygon &e : glyph)
                    for (const Point &p : e.contour.points) {
                        Vec2d  d = p.cast<double>() - c;
                        double a = std::atan2(d.x(), d.y());
                        lo       = std::min(lo, a);
                        hi       = std::max(hi, a);
                    }
                return std::make_pair(lo, hi);
            };
            auto first = angle_range(shapes.front().expoly);
            auto last  = angle_range(shapes.back().expoly);
            // the first glyph starts left of 12 o'clock, the last ends right of it, neither reaches 6 o'clock
            CHECK(first.second < 0.);
            CHECK(last.first > 0.);
            // a gap stays open at 6 o'clock between the end and the start of the text
            CHECK(first.first + 2. * M_PI - last.second > 0.);

            // first and last glyph do not overlap
            CHECK(intersection_ex(shapes.front().expoly, shapes.back().expoly).empty());
        }
    }
}

TEST_CASE("Rigid letters keep their shape and sit on the circle", "[EmbossBend]")
{
    for (bool inside : {false, true}) {
        DYNAMIC_SECTION((inside ? "smile" : "arch")) {
            GlyphAdvances           advances;
            const ExPolygonsWithIds straight = box_text(6, &advances);
            ExPolygonsWithIds       shapes   = straight;
            BendResult              r = apply_bend(shapes, radius_bend(30.f, inside, true), SCALE, &advances);
            REQUIRE(r.is_active());
            CHECK_THAT(r.radius_mm, WithinAbs(30., 1e-9));
            const Vec2d c = r.spec.center();

            for (size_t i = 0; i < shapes.size(); ++i) {
                const Polygon &before = straight[i].expoly.front().contour;
                const Polygon &after  = shapes[i].expoly.front().contour;
                REQUIRE(before.size() == after.size());
                // rigid motion: same area and the same distances between all vertices
                CHECK_THAT(after.area(), WithinRel(before.area(), 1e-6));
                for (size_t a = 0; a < before.size(); ++a)
                    for (size_t b = a + 1; b < before.size(); ++b)
                        CHECK_THAT((after[a] - after[b]).cast<double>().norm(),
                                   WithinAbs((before[a] - before[b]).cast<double>().norm(), 2.));
                // the box centre (pivot x, 2 mm above the reference line) is at distance R + s * 2 mm
                Vec2d centre = Vec2d::Zero();
                for (const Point &p : after.points)
                    centre += p.cast<double>();
                centre /= double(after.size());
                CHECK_THAT((centre - c).norm(), WithinAbs(r.spec.radius + r.spec.side() * 2. * MM, 2.));
            }
        }
    }
}

TEST_CASE("Clamping keeps the text on its side of the centre and inside 359 degrees", "[EmbossBend]")
{
    SECTION("radius too small for the text length") {
        GlyphAdvances     advances;
        ExPolygonsWithIds shapes = box_text(20, &advances); // 140 mm long
        BendResult        r      = apply_bend(shapes, radius_bend(5.f), SCALE, &advances);
        REQUIRE(r.is_active());
        CHECK(r.limited_by_length);
        CHECK_THAT(r.angle_deg, WithinAbs(BEND_MAX_ANGLE_DEG, 1e-6));
        CHECK_THAT(r.radius_mm, WithinRel(140. / (BEND_MAX_ANGLE_DEG * M_PI / 180.), 1e-9));
    }
    SECTION("short tall text at a large angle") {
        GlyphAdvances     advances;
        ExPolygonsWithIds shapes = box_text(1, &advances); // 7 mm wide, 6 mm above / 2 mm below the line
        BendResult        arch   = apply_bend(shapes, angle_bend(300.f), SCALE, &advances);
        REQUIRE(arch.is_active());
        CHECK(arch.limited_by_height);
        CHECK(arch.radius_mm >= BEND_MIN_RADIUS_RATIO * 2. - 1e-9);
        CHECK(arch.angle_deg < 300.);

        GlyphAdvances     smile_advances;
        ExPolygonsWithIds smile_shapes = box_text(1, &smile_advances);
        BendResult        smile        = apply_bend(smile_shapes, angle_bend(300.f, true), SCALE, &smile_advances);
        CHECK(smile.limited_by_height);
        CHECK(smile.radius_mm >= BEND_MIN_RADIUS_RATIO * 6. - 1e-9);
    }
}

TEST_CASE("Angle mode keeps the span, radius mode keeps the radius, for short and long text", "[EmbossBend]")
{
    GlyphAdvances adv_short, adv_long;
    ExPolygonsWithIds short_text = box_text(4, &adv_short);
    ExPolygonsWithIds long_text  = box_text(16, &adv_long);

    BendResult a_short = resolve_bend(angle_bend(90.f), measure_bend_input(short_text, &adv_short), SCALE);
    BendResult a_long  = resolve_bend(angle_bend(90.f), measure_bend_input(long_text, &adv_long), SCALE);
    CHECK_THAT(a_short.angle_deg, WithinAbs(90., 1e-9));
    CHECK_THAT(a_long.angle_deg, WithinAbs(90., 1e-9));
    CHECK_THAT(a_long.radius_mm / a_short.radius_mm, WithinRel(4., 1e-9));

    BendResult r_short = resolve_bend(radius_bend(40.f), measure_bend_input(short_text, &adv_short), SCALE);
    BendResult r_long  = resolve_bend(radius_bend(40.f), measure_bend_input(long_text, &adv_long), SCALE);
    CHECK_THAT(r_short.radius_mm, WithinAbs(40., 1e-9));
    CHECK_THAT(r_long.radius_mm, WithinAbs(40., 1e-9));
    CHECK_THAT(r_long.angle_deg / r_short.angle_deg, WithinRel(4., 1e-9));
}

TEST_CASE("Bent real text stays valid", "[EmbossBend]")
{
    if (!boost::filesystem::exists(font_path())) {
        WARN("Font not found: " << font_path());
        return;
    }
    std::unique_ptr<FontFile> font = create_font_file(font_path().c_str());
    REQUIRE(font != nullptr);
    FontFileWithCache ff(std::move(font));
    FontProp          fp;
    fp.size_in_mm = 10.f;
    const double scale = get_text_shape_scale(fp, *ff.font_file);

    const std::wstring text = L"Hello, Curved World!";
    GlyphAdvances      advances;
    ExPolygonsWithIds  straight = text2vshapes(ff, text, fp, []() { return false; }, advances);
    REQUIRE(straight.size() == text.size());
    REQUIRE(advances.size() == text.size());
    CHECK(advances[5].valid); // ','
    CHECK(advances[6].valid); // ' ' has an advance but no outline
    CHECK(straight[6].expoly.empty());

    for (float angle : {10.f, 90.f, 180.f, 359.f})
        for (bool inside : {false, true})
            for (bool rigid : {false, true}) {
                DYNAMIC_SECTION("angle " << angle << (inside ? " smile" : " arch") << (rigid ? " rigid" : " bent")) {
                    ExPolygonsWithIds shapes = straight;
                    BendResult r = apply_bend(shapes, angle_bend(angle, inside, rigid), scale, &advances);
                    REQUIRE(r.is_active());
                    CHECK_FALSE(r.limited_by_height);
                    CHECK_THAT(r.angle_deg, WithinAbs(angle, 1e-3));
                    for (size_t i = 0; i < shapes.size(); ++i) {
                        if (straight[i].expoly.empty())
                            continue;
                        REQUIRE_FALSE(shapes[i].expoly.empty());
                        // winding kept: positive area per glyph
                        CHECK(shape_area(shapes[i].expoly) > 0.);
                        if (rigid)
                            CHECK_THAT(shape_area(shapes[i].expoly), WithinRel(shape_area(straight[i].expoly), 1e-5));
                        // no self-intersection where the straight glyph had none
                        if (get_intersections(straight[i].expoly).empty())
                            CHECK(get_intersections(shapes[i].expoly).empty());
                    }
                    ExPolygons all;
                    for (const ExPolygonsWithId &s : shapes)
                        append(all, s.expoly);
                    CHECK_FALSE(union_ex(all).empty());
                }
            }

    SECTION("long and short text at the same angle") {
        GlyphAdvances     adv_short;
        ExPolygonsWithIds short_text = text2vshapes(ff, L"Hi", fp, []() { return false; }, adv_short);
        ExPolygonsWithIds long_text  = straight;
        BendResult        r_short    = apply_bend(short_text, angle_bend(180.f), scale, &adv_short);
        BendResult        r_long     = apply_bend(long_text, angle_bend(180.f), scale, &advances);
        CHECK_THAT(r_long.angle_deg, WithinAbs(180., 1e-6));
        // two letters at 180 degrees would need a radius below their height: limited
        CHECK(r_short.limited_by_height);
        CHECK(r_short.angle_deg < 180.);
        CHECK(r_short.radius_mm < r_long.radius_mm);
    }
}

namespace {

std::string bend_temp_3mf(const std::string &name)
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());
    return (dir / name).string();
}

// Base cube plus a text volume with the given bend, stored as a project 3MF
void store_text_project(const std::string &path, const EmbossBend &bend, bool use_surface = false, bool per_glyph = false)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "disc";
    object->add_volume(make_cube(40., 40., 3.))->name = "base";
    ModelVolume *text = object->add_volume(make_cube(10., 4., 1.));
    text->name        = "text";

    EmbossShape es;
    es.scale             = 5e-6;
    es.projection.depth  = 1.;
    es.projection.bend   = bend;
    es.projection.use_surface = use_surface;
    text->emboss_shape   = es;
    TextConfiguration tc;
    tc.text              = "Curved";
    tc.style.name        = "test";
    tc.style.path        = "test.ttf";
    tc.style.type        = EmbossStyle::Type::file_path;
    tc.style.prop.size_in_mm = 5.f;
    tc.style.prop.per_glyph  = per_glyph;
    text->text_configuration = tc;

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

std::optional<EmbossShape> load_text_shape(const std::string &path, bool *per_glyph = nullptr)
{
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    bool loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf, &file_version,
                               nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *preset : project_presets)
        delete preset;
    if (!loaded)
        return {};
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            if (v->emboss_shape.has_value() && v->text_configuration.has_value()) {
                if (per_glyph != nullptr)
                    *per_glyph = v->text_configuration->style.prop.per_glyph;
                return v->emboss_shape;
            }
    return {};
}

bool archive_contains(const std::string &path, const std::string &needle)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path))
        return false;
    bool found = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip) && !found; ++i) {
        size_t size = 0;
        void  *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (data == nullptr)
            continue;
        found = std::string(static_cast<const char *>(data), size).find(needle) != std::string::npos;
        mz_free(data);
    }
    close_zip_reader(&zip);
    return found;
}

} // namespace

TEST_CASE("Curved text parameters round-trip through 3MF", "[EmbossBend][3mf]")
{
    const std::string path = bend_temp_3mf("curved_text_roundtrip.3mf");
    ScopeGuard        cleanup([&path]() {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    });

    SECTION("radius, smile, rigid") {
        store_text_project(path, radius_bend(23.5f, true, true));
        CHECK(archive_contains(path, "bend_radius=\"23.5\""));
        CHECK(archive_contains(path, "bend_inside=\"1\""));
        CHECK(archive_contains(path, "bend_rigid=\"1\""));
        CHECK_FALSE(archive_contains(path, "bend_angle"));
        std::optional<EmbossShape> es = load_text_shape(path);
        REQUIRE(es.has_value());
        CHECK(es->projection.bend == radius_bend(23.5f, true, true));
    }
    SECTION("angle, arch, bent") {
        store_text_project(path, angle_bend(147.f));
        CHECK(archive_contains(path, "bend_angle=\"147\""));
        CHECK_FALSE(archive_contains(path, "bend_inside"));
        CHECK_FALSE(archive_contains(path, "bend_rigid"));
        std::optional<EmbossShape> es = load_text_shape(path);
        REQUIRE(es.has_value());
        CHECK(es->projection.bend == angle_bend(147.f));
    }
    SECTION("straight text writes nothing and an old file loads straight") {
        // angle mode at 0 degrees is straight too: nothing is written, like a file from an older build
        store_text_project(path, angle_bend(0.f));
        CHECK(archive_contains(path, "slic3rpe:shape"));
        CHECK_FALSE(archive_contains(path, "bend_"));
        std::optional<EmbossShape> es = load_text_shape(path);
        REQUIRE(es.has_value());
        CHECK(es->projection.bend == EmbossBend{});
        CHECK_FALSE(es->projection.bend.is_active());
    }
    SECTION("bend together with use surface") {
        store_text_project(path, angle_bend(210.f, false, true), true);
        CHECK(archive_contains(path, "use_surface=\"1\""));
        CHECK(archive_contains(path, "bend_angle=\"210\""));
        CHECK(archive_contains(path, "bend_rigid=\"1\""));
        std::optional<EmbossShape> es = load_text_shape(path);
        REQUIRE(es.has_value());
        CHECK(es->projection.use_surface);
        CHECK(es->projection.bend == angle_bend(210.f, false, true));
    }
    SECTION("letter by letter: bend, use surface and per glyph") {
        store_text_project(path, radius_bend(18.f), true, true);
        bool per_glyph = false;
        std::optional<EmbossShape> es = load_text_shape(path, &per_glyph);
        REQUIRE(es.has_value());
        CHECK(es->projection.use_surface);
        CHECK(per_glyph);
        CHECK(es->projection.bend == radius_bend(18.f));
    }
}

// ---- Curved text projected onto curved surfaces ("Use surface", phase 2) ----

namespace {

// The text frame: x, y along the text, the text looks along +z and is projected along -z onto
// `mesh` (given in text coordinates [mm]). Same projection as the GUI surface job
// (cut_surface_to_its in EmbossJob.cpp): orthographic over the z range of the mesh plus 1 mm,
// with the projection ratio of the text plane z = 0.
struct SurfaceProjection
{
    SurfaceCut           cut;
    indexed_triangle_set model; // the embossed volume made from the cut
};

SurfaceProjection project_onto(const ExPolygons &shapes, double shape_scale, const indexed_triangle_set &mesh, double depth = 1.)
{
    const double safe = 1.;
    BoundingBoxf3 mesh_bb = bounding_box(mesh);
    const double  min_z   = mesh_bb.min.z() - safe;
    const double  max_z   = mesh_bb.max.z() + safe;
    Transform3d   tr      = Transform3d::Identity();
    tr.translate(Vec3d(0., 0., min_z));
    tr.scale(shape_scale);
    OrthoProject projection(tr, Vec3d(0., 0., max_z - min_z));
    const float  ratio = static_cast<float>((-mesh_bb.min.z() + safe) / (mesh_bb.max.z() - mesh_bb.min.z() + 2. * safe));

    // area of interest first, as the job does
    indexed_triangle_set aoi = its_cut_AoI(mesh, get_extents(shapes), projection);
    SurfaceProjection result;
    if (aoi.indices.empty())
        return result;
    result.cut = cut_surface(shapes, {aoi}, projection, ratio);
    if (result.cut.empty())
        return result;
    // emboss outward like the job: front at +depth, back slightly below the surface
    SurfaceCut moved = result.cut;
    for (stl_vertex &v : moved.vertices)
        v.z() += static_cast<float>(depth);
    OrthoProject3d emboss(Vec3d(0., 0., -depth - 0.015));
    result.model = cut2model(moved, emboss);
    return result;
}

// The 2D outlines the job projects: glyphs united (and healed) like EmbossJob::create_shape
ExPolygons united(const ExPolygonsWithIds &shapes, double shape_scale)
{
    EmbossShape es;
    es.shapes_with_ids = shapes;
    es.scale           = shape_scale;
    return union_with_delta(es, UNION_DELTA, UNION_MAX_ITERATIN);
}

// Move the shapes so the arc centre lands on the origin (what "Centre arc on object" does)
void center_arc(ExPolygonsWithIds &shapes, const BendResult &r)
{
    const Vec2d c = r.spec.center();
    const Point offset(coord_t(std::llround(-c.x())), coord_t(std::llround(-c.y())));
    for (ExPolygonsWithId &s : shapes)
        for (ExPolygon &e : s.expoly)
            e.translate(offset);
}

// Area of the cut seen along the projection direction [mm^2]
double projected_area(const indexed_triangle_set &its)
{
    double area = 0.;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3f &a = its.vertices[t[0]], &b = its.vertices[t[1]], &c = its.vertices[t[2]];
        area += 0.5 * ((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y()));
    }
    return area;
}

double shapes_area_mm2(const ExPolygons &shapes, double shape_scale)
{
    return shape_area(shapes) * shape_scale * shape_scale;
}

// Largest distance of the cut vertices from the target mesh [mm]
double max_distance_to(const SurfaceCut &cut, const indexed_triangle_set &mesh)
{
    AABBMesh tree(mesh);
    double   worst = 0.;
    for (const stl_vertex &v : cut.vertices)
        worst = std::max(worst, std::sqrt(tree.squared_distance(v.cast<double>())));
    return worst;
}

indexed_triangle_set sphere_below(double radius)
{
    // top of the sphere at the text origin
    indexed_triangle_set its = its_make_sphere(radius, PI / 90.);
    its_translate(its, Vec3f(0.f, 0.f, -static_cast<float>(radius)));
    return its;
}

indexed_triangle_set lying_cylinder_below(double radius, double length)
{
    // axis along x, top line at z = 0
    indexed_triangle_set its = its_make_cylinder(radius, length, PI / 90.);
    its_translate(its, Vec3f(0.f, 0.f, -static_cast<float>(length / 2.)));
    its_transform(its, Transform3d(Eigen::AngleAxisd(PI / 2., Vec3d::UnitY())));
    its_translate(its, Vec3f(0.f, 0.f, -static_cast<float>(radius)));
    return its;
}

indexed_triangle_set cylinder_cap_below(double radius, double height)
{
    // standing cylinder, its top cap at z = 0
    indexed_triangle_set its = its_make_cylinder(radius, height, PI / 90.);
    its_translate(its, Vec3f(0.f, 0.f, -static_cast<float>(height)));
    return its;
}

// Every glyph lands on the surface, none gets lost or merged, and the footprint keeps its area
void check_on_surface(const ExPolygonsWithIds &glyphs, const SurfaceProjection &p, const indexed_triangle_set &mesh)
{
    const ExPolygons shapes = united(glyphs, SCALE);
    size_t glyph_count = 0;
    for (const ExPolygonsWithId &g : glyphs)
        glyph_count += g.expoly.empty() ? 0 : 1;
    REQUIRE(shapes.size() == glyph_count); // separate glyphs stay separate in 2D
    REQUIRE_FALSE(p.cut.empty());
    // one patch per glyph
    CHECK(its_number_of_patches(p.cut) == glyph_count);
    // every vertex of the projected text lies on the target
    CHECK(max_distance_to(p.cut, mesh) < 1e-3);
    // nothing cut away: the projected footprint has the area of the bent outlines
    CHECK_THAT(projected_area(p.cut), WithinRel(shapes_area_mm2(shapes, SCALE), 2e-3));
    // the embossed volume is closed
    REQUIRE_FALSE(p.model.indices.empty());
    CHECK(its_num_open_edges(p.model) == 0);
}

} // namespace

TEST_CASE("Curved text projects onto a sphere", "[EmbossBend][surface]")
{
    const indexed_triangle_set sphere = sphere_below(30.);
    GlyphAdvances advances;
    const ExPolygonsWithIds straight = box_text(8, &advances);

    SECTION("bent letters around the top, radius 14 mm") {
        ExPolygonsWithIds shapes = straight;
        BendResult r = apply_bend(shapes, radius_bend(14.f), SCALE, &advances, BEND_SURFACE_TOLERANCE_MM);
        REQUIRE(r.is_active());
        center_arc(shapes, r);
        check_on_surface(shapes, project_onto(united(shapes, SCALE), SCALE, sphere), sphere);
    }
    SECTION("rigid letters around the top, radius 20 mm") {
        ExPolygonsWithIds shapes = straight;
        BendResult r = apply_bend(shapes, radius_bend(20.f, false, true), SCALE, &advances, BEND_SURFACE_TOLERANCE_MM);
        REQUIRE(r.is_active());
        center_arc(shapes, r);
        check_on_surface(shapes, project_onto(united(shapes, SCALE), SCALE, sphere), sphere);
    }
    SECTION("smile across the top, not centred") {
        GlyphAdvances     adv5;
        ExPolygonsWithIds shapes = box_text(5, &adv5);
        BendResult r = apply_bend(shapes, angle_bend(60.f, true), SCALE, &adv5, BEND_SURFACE_TOLERANCE_MM);
        REQUIRE(r.is_active());
        check_on_surface(shapes, project_onto(united(shapes, SCALE), SCALE, sphere), sphere);
    }
    SECTION("nothing is lost at 270 degrees") {
        ExPolygonsWithIds shapes = straight;
        BendResult r = apply_bend(shapes, angle_bend(270.f), SCALE, &advances, BEND_SURFACE_TOLERANCE_MM);
        REQUIRE(r.is_active());
        CHECK_THAT(r.angle_deg, WithinAbs(270., 1e-6));
        center_arc(shapes, r);
        check_on_surface(shapes, project_onto(united(shapes, SCALE), SCALE, sphere), sphere);
    }
}

TEST_CASE("Curved text projects onto cylinders", "[EmbossBend][surface]")
{
    GlyphAdvances advances;
    const ExPolygonsWithIds straight = box_text(8, &advances);

    SECTION("arched across a lying cylinder (curved in one direction)") {
        const indexed_triangle_set cylinder = lying_cylinder_below(25., 120.);
        for (bool rigid : {false, true}) {
            ExPolygonsWithIds shapes = straight;
            BendResult r = apply_bend(shapes, angle_bend(90.f, false, rigid), SCALE, &advances, BEND_SURFACE_TOLERANCE_MM);
            REQUIRE(r.is_active());
            check_on_surface(shapes, project_onto(united(shapes, SCALE), SCALE, cylinder), cylinder);
        }
    }
    SECTION("ring on the flat end cap with use surface") {
        const indexed_triangle_set cap = cylinder_cap_below(30., 10.);
        ExPolygonsWithIds shapes = straight;
        BendResult r = apply_bend(shapes, radius_bend(16.f), SCALE, &advances, BEND_SURFACE_TOLERANCE_MM);
        REQUIRE(r.is_active());
        center_arc(shapes, r);
        SurfaceProjection p = project_onto(united(shapes, SCALE), SCALE, cap);
        check_on_surface(shapes, p, cap);
        // flat: the whole cut lies in the cap plane
        for (const stl_vertex &v : p.cut.vertices)
            CHECK(std::abs(v.z()) < 1e-4);
    }
}

TEST_CASE("Straight text with use surface is unchanged by the bend code", "[EmbossBend][surface]")
{
    const indexed_triangle_set sphere = sphere_below(30.);
    GlyphAdvances advances;
    const ExPolygonsWithIds straight = box_text(5, &advances);
    const SurfaceProjection reference = project_onto(united(straight, SCALE), SCALE, sphere);
    REQUIRE_FALSE(reference.cut.empty());

    for (const EmbossBend &bend : {EmbossBend{}, angle_bend(0.f), radius_bend(0.f)}) {
        ExPolygonsWithIds shapes = straight;
        BendResult r = apply_bend(shapes, bend, SCALE, &advances, bend_tolerance_mm(true));
        CHECK_FALSE(r.is_active());
        const SurfaceProjection p = project_onto(united(shapes, SCALE), SCALE, sphere);
        CHECK(p.cut.vertices == reference.cut.vertices);
        CHECK(p.cut.indices == reference.cut.indices);
        CHECK(p.cut.contours == reference.cut.contours);
        CHECK(p.model.vertices == reference.model.vertices);
    }
}

TEST_CASE("Surface tolerance densifies less but stays within it", "[EmbossBend][surface]")
{
    GlyphAdvances advances;
    const ExPolygonsWithIds straight = box_text(8, &advances);
    auto total_points = [](const ExPolygonsWithIds &shapes) {
        size_t n = 0;
        for (const ExPolygonsWithId &s : shapes)
            n += count_points(s.expoly);
        return n;
    };
    ExPolygonsWithIds flat = straight, surface = straight;
    BendResult rf = apply_bend(flat, radius_bend(20.f), SCALE, &advances, bend_tolerance_mm(false));
    BendResult rs = apply_bend(surface, radius_bend(20.f), SCALE, &advances, bend_tolerance_mm(true));
    CHECK_THAT(rf.spec.tolerance * SCALE, WithinAbs(BEND_TOLERANCE_MM, 1e-12));
    CHECK_THAT(rs.spec.tolerance * SCALE, WithinAbs(BEND_SURFACE_TOLERANCE_MM, 1e-12));
    CHECK(total_points(surface) < total_points(flat));
    // the coarser densify still follows the arc: top edge points at rho = R + 6 mm
    for (const ExPolygonsWithId &s : surface)
        for (const ExPolygon &e : s.expoly)
            for (size_t i = 0; i < e.contour.size(); ++i) {
                const Vec2d a = e.contour[i].cast<double>() - rs.spec.center();
                const Vec2d b = e.contour.points[(i + 1) % e.contour.size()].cast<double>() - rs.spec.center();
                const double rho = (26. * MM);
                if (std::abs(a.norm() - rho) < 1e3 && std::abs(b.norm() - rho) < 1e3) {
                    // sagitta of the chord on the circle
                    const double half = (a - b).norm() / 2.;
                    const double sagitta = rho - std::sqrt(std::max(0., rho * rho - half * half));
                    CHECK(sagitta <= BEND_SURFACE_TOLERANCE_MM * MM * 1.01 + 2.);
                }
            }
}

TEST_CASE("Round outline of curved targets", "[EmbossBend][surface]")
{
    auto vertices = [](const indexed_triangle_set &its) {
        std::vector<Vec3d> v;
        for (const stl_vertex &p : its.vertices)
            v.push_back(p.cast<double>());
        return v;
    };

    SECTION("sphere: centre on the axis, usable up to 45 degrees") {
        std::vector<Vec3d> pts = vertices(sphere_below(30.));
        for (Vec3d &p : pts)
            p += Vec3d(3., -2., 0.);
        std::optional<SurfaceRound> round = surface_round_area(pts);
        REQUIRE(round.has_value());
        CHECK_THAT(round->center.x(), WithinAbs(3., 0.05));
        CHECK_THAT(round->center.y(), WithinAbs(-2., 0.05));
        CHECK_THAT(round->radius, WithinRel(30., 0.01));
        CHECK_THAT(round->usable_radius, WithinAbs(30. * std::sin(PI / 4.), 1.5));
    }
    SECTION("domed lid: the shallow dome is usable up to the rim") {
        // cylinder r = 30 with a spherical cap of radius 60 on top (30 degrees at the rim)
        const double r = 30., rc = 60.;
        auto dome_z = [&](double d) { return std::sqrt(rc * rc - d * d) - rc; };
        std::vector<Vec3d> pts;
        for (int i = 0; i <= 30; ++i)
            for (int j = 0; j < 72; ++j) {
                const double d = r * i / 30., a = 2. * PI * j / 72.;
                pts.emplace_back(d * std::cos(a), d * std::sin(a), dome_z(d));
            }
        for (int j = 0; j < 72; ++j) {
            const double a = 2. * PI * j / 72.;
            pts.emplace_back(r * std::cos(a), r * std::sin(a), dome_z(r) - 15.);
        }
        std::optional<SurfaceRound> round = surface_round_area(pts);
        REQUIRE(round.has_value());
        CHECK(round->center.norm() < 0.05);
        CHECK_THAT(round->radius, WithinRel(30., 0.01));
        CHECK(round->usable_radius > 29.);

        // a knob in the middle of the lid does not shrink the ring
        for (int k = 0; k <= 10; ++k)
            for (int j = 0; j < 36; ++j) {
                const double a = 2. * PI * j / 36.;
                pts.emplace_back(5. * std::cos(a), 5. * std::sin(a), double(k));
                pts.emplace_back(0.5 * k * std::cos(a), 0.5 * k * std::sin(a), 10.);
            }
        round = surface_round_area(pts);
        REQUIRE(round.has_value());
        CHECK(round->usable_radius > 29.);
    }
    SECTION("cylinder end cap: all of it") {
        std::optional<SurfaceRound> round = surface_round_area(vertices(cylinder_cap_below(25., 8.)));
        REQUIRE(round.has_value());
        CHECK_THAT(round->radius, WithinRel(25., 0.01));
        CHECK_THAT(round->usable_radius, WithinRel(25., 0.01));
    }
    SECTION("a box is not round") {
        CHECK_FALSE(surface_round_area(vertices(its_make_cube(40., 30., 10.))).has_value());
    }
    SECTION("half a circle is not a round outline") {
        std::vector<Vec2d> rim;
        for (int i = 0; i <= 40; ++i)
            rim.emplace_back(10. * std::cos(PI * i / 40.), 10. * std::sin(PI * i / 40.));
        CHECK_FALSE(fit_round_outline(rim).has_value());
        for (int i = 41; i < 80; ++i)
            rim.emplace_back(10. * std::cos(PI * i / 40.), 10. * std::sin(PI * i / 40.));
        std::optional<RoundOutline> full = fit_round_outline(rim);
        REQUIRE(full.has_value());
        CHECK_THAT(full->radius, WithinAbs(10., 1e-9));
    }
}

// Rough timings of the surface job for real text, run on demand:
//   libslic3r_tests "[.perf]" -s
TEST_CASE("Timing of curved text on a dome", "[EmbossBend][surface][.perf]")
{
    if (!boost::filesystem::exists(font_path())) {
        WARN("Font not found: " << font_path());
        return;
    }
    std::unique_ptr<FontFile> font = create_font_file(font_path().c_str());
    REQUIRE(font != nullptr);
    FontFileWithCache ff(std::move(font));
    FontProp          fp;
    fp.size_in_mm = 5.f;
    const double scale = get_text_shape_scale(fp, *ff.font_file);

    const std::wstring text = L"Hello, Curved World!";
    GlyphAdvances      advances;
    ExPolygonsWithIds  straight = text2vshapes(ff, text, fp, []() { return false; }, advances);
    REQUIRE(straight.size() == text.size());

    indexed_triangle_set dome = its_make_sphere(40., PI / 90.);
    its_translate(dome, Vec3f(0.f, 0.f, -40.f));

    using clock = std::chrono::steady_clock;
    auto run = [&](const char *name, const EmbossBend &bend, double tolerance_mm) {
        double best = 1e9;
        size_t points = 0, triangles = 0;
        for (int i = 0; i < 3; ++i) {
            auto start = clock::now();
            ExPolygonsWithIds shapes = straight;
            BendResult r = apply_bend(shapes, bend, scale, &advances, tolerance_mm);
            if (r.is_active())
                center_arc(shapes, r);
            EmbossShape es;
            es.shapes_with_ids = std::move(shapes);
            es.scale           = scale;
            ExPolygons outlines = union_with_delta(es, UNION_DELTA, UNION_MAX_ITERATIN);
            SurfaceProjection p = project_onto(outlines, scale, dome);
            best = std::min(best, std::chrono::duration<double, std::milli>(clock::now() - start).count());
            points    = count_points(outlines);
            triangles = p.model.indices.size();
            CHECK_FALSE(p.cut.empty());
        }
        std::cout << "  " << name << ": " << points << " outline points, " << triangles << " triangles, "
                  << best << " ms" << std::endl;
    };
    std::cout << "Surface projection of \"Hello, Curved World!\" (5 mm) onto a 80 mm sphere, best of 3:" << std::endl;
    run("straight", EmbossBend{}, BEND_TOLERANCE_MM);
    for (float angle : {120.f, 270.f}) {
        const std::string a = std::to_string(int(angle));
        run(("bent " + a + " deg, 0.01 mm").c_str(), angle_bend(angle), BEND_TOLERANCE_MM);
        run(("bent " + a + " deg, 0.02 mm").c_str(), angle_bend(angle), BEND_SURFACE_TOLERANCE_MM);
        run(("rigid " + a + " deg").c_str(), angle_bend(angle, false, true), BEND_SURFACE_TOLERANCE_MM);
    }
}

// ---- Curved text letter by letter on the surface (phase 2b) ----

namespace {

// Same as the surface job (cut_curved_glyph_surface in EmbossJob.cpp): every glyph is projected in
// its own frame along its own normal. Cuts are returned in text coordinates.
struct LetterProjection
{
    SurfaceArc                          arc;
    std::vector<SurfaceCut>             cuts;   // per glyph (empty when not placed), text coordinates
    std::vector<ExPolygons>             local;  // glyph outline in its own frame
};

LetterProjection project_letters(const ExPolygonsWithIds &shapes, const GlyphAdvances &advances, const EmbossBend &bend,
                                 const indexed_triangle_set &mesh)
{
    LetterProjection result;
    std::optional<SurfaceGlyphLayout> layout = surface_glyph_layout(shapes, &advances, bend, SCALE);
    REQUIRE(layout.has_value());
    BendSurface surface(mesh);
    result.arc = place_on_surface_arc(surface, layout->pivots_mm, layout->x_min, layout->x_max, layout->params);
    result.cuts.resize(shapes.size());
    result.local.resize(shapes.size());
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].expoly.empty() || !result.arc.frames[i].has_value())
            continue;
        const Transform3d &frame = *result.arc.frames[i];
        result.local[i] = surface_glyph_shape(shapes[i].expoly, layout->pivots[i], result.arc.curvature_radius[i], bend, SCALE);
        indexed_triangle_set local_mesh = mesh;
        its_transform(local_mesh, frame.inverse());
        SurfaceCut cut = project_onto(result.local[i], SCALE, local_mesh).cut;
        for (stl_vertex &v : cut.vertices)
            v = (frame * v.cast<double>()).cast<float>();
        result.cuts[i] = std::move(cut);
    }
    return result;
}

// area of a mesh patch in 3D [mm^2]
double surface_area(const indexed_triangle_set &its)
{
    double area = 0.;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3d a = its.vertices[t[0]].cast<double>(), b = its.vertices[t[1]].cast<double>(), c = its.vertices[t[2]].cast<double>();
        area += 0.5 * (b - a).cross(c - a).norm();
    }
    return area;
}

// Extent of the cut along two directions through the frame origin (as walked on the patch: the
// 3D bounding box in the frame)
Vec2d extent_in_frame(const SurfaceCut &cut, const Transform3d &frame)
{
    const Transform3d inv = frame.inverse();
    BoundingBoxf bb;
    for (const stl_vertex &v : cut.vertices) {
        const Vec3d p = inv * v.cast<double>();
        bb.merge(Vec2d(p.x(), p.y()));
    }
    return bb.size();
}

// Each glyph: placed, not clipped, on the mesh, and hardly distorted
void check_letters(const ExPolygonsWithIds &shapes, const LetterProjection &p, const indexed_triangle_set &mesh,
                   double max_area_error, double max_aspect_error)
{
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].expoly.empty())
            continue;
        INFO("glyph " << i);
        REQUIRE(p.arc.frames[i].has_value());
        const SurfaceCut &cut = p.cuts[i];
        REQUIRE_FALSE(cut.empty());
        // every vertex on the surface
        CHECK(max_distance_to(cut, mesh) < 1e-3);
        const double flat_area = shapes_area_mm2(p.local[i], SCALE);
        // not clipped: the footprint along its own normal is the whole glyph
        indexed_triangle_set in_frame = cut;
        its_transform(in_frame, p.arc.frames[i]->inverse());
        CHECK_THAT(projected_area(in_frame), WithinRel(flat_area, 2e-3));
        // hardly distorted: the area on the surface and the proportions stay those of the flat glyph
        CHECK_THAT(surface_area(cut), WithinRel(flat_area, max_area_error));
        const BoundingBox bb   = get_extents(p.local[i]);
        const double      flat_aspect = double(bb.size().x()) / double(bb.size().y());
        const Vec2d       ext  = extent_in_frame(cut, *p.arc.frames[i]);
        CHECK_THAT(ext.x() / ext.y(), WithinRel(flat_aspect, max_aspect_error));
        // one patch per glyph (no hole lost or glyph split)
        CHECK(its_number_of_patches(cut) == 1);
    }
}

// Angle of a point around the axis (sphere centre -> arc centre)
double angle_around(const Vec3d &p, const Vec3d &axis_point, const Vec3d &axis, const Vec3d &ref)
{
    const Vec3d d  = p - axis_point;
    const Vec3d u  = (ref - ref.dot(axis) * axis).normalized();
    const Vec3d v  = axis.cross(u);
    return std::atan2(d.dot(v), d.dot(u));
}

} // namespace

TEST_CASE("Curved text letter by letter on a sphere", "[EmbossBend][surface][letters]")
{
    const double               rs     = 30.;
    const indexed_triangle_set sphere = sphere_below(rs);
    const Vec3d                sphere_center(0., 0., -rs);
    GlyphAdvances              advances;
    const ExPolygonsWithIds    text = box_text(8, &advances); // 8 glyphs of 6 x 8 mm, advance 7 mm
    const double               width = 8 * 7.;

    for (bool rigid : {true, false})
        for (float angle : {120.f, 270.f}) {
            DYNAMIC_SECTION((rigid ? "rigid " : "bent ") << angle << " degrees") {
                const EmbossBend bend = angle_bend(angle, false, rigid);
                LetterProjection p    = project_letters(text, advances, bend, sphere);
                REQUIRE(p.arc.preview.valid);
                CHECK_FALSE(p.arc.preview.limited);
                // letters keep their shape: a 6 x 8 mm letter on a 30 mm sphere, bent ones are wedges
                check_letters(text, p, sphere, 0.03, rigid ? 0.02 : 0.08);

                // the arc centre lies on the sphere and the pivots on one circle around it
                const Vec3d axis = (p.arc.preview.center - sphere_center).normalized();
                std::vector<double> heights, angles;
                for (size_t i = 0; i < text.size(); ++i) {
                    const Vec3d o = p.arc.frames[i]->translation();
                    heights.push_back((o - sphere_center).dot(axis));
                    angles.push_back(angle_around(o, sphere_center, axis, p.arc.frames[0]->translation() - sphere_center));
                }
                for (double h : heights)
                    CHECK_THAT(h, WithinAbs(heights.front(), 0.05));
                // spacing by arc length on the surface: equal steps, and the text spans the angle
                const double r_circle = std::sqrt(rs * rs - heights.front() * heights.front());
                for (size_t i = 1; i < angles.size(); ++i) {
                    double step = std::abs(angles[i] - angles[i - 1]);
                    if (step > PI)
                        step = 2. * PI - step;
                    CHECK_THAT(step * r_circle, WithinRel(7., 0.01));
                }
                CHECK_THAT(width / r_circle * 180. / PI, WithinAbs(angle, 1.));
                CHECK_THAT(p.arc.preview.span_deg, WithinAbs(angle, 1.));
            }
        }
}

TEST_CASE("Letters wrap past the edge seen from the text", "[EmbossBend][surface][letters]")
{
    // text placed on the side of a sphere, the ring passes the silhouette of the text plane
    const double rs = 30.;
    indexed_triangle_set sphere = its_make_sphere(rs, PI / 90.);
    // text origin at the equator, text z = sphere normal there (+x of the sphere), text y = up
    Transform3d to_text = Transform3d::Identity();
    to_text.linear() << 0., 1., 0., //
                        0., 0., 1., //
                        1., 0., 0.;
    to_text.translation() = Vec3d(0., 0., -rs);
    its_transform(sphere, to_text);
    GlyphAdvances           advances;
    const ExPolygonsWithIds text = box_text(18, &advances); // 326 degrees around the centre
    LetterProjection p = project_letters(text, advances, radius_bend(25.f, false, true), sphere);
    REQUIRE(p.arc.preview.valid);
    check_letters(text, p, sphere, 0.03, 0.02);
    // some letters face away from the text direction: past the silhouette
    bool past = false;
    for (const std::optional<Transform3d> &f : p.arc.frames)
        past |= f.has_value() && f->linear().col(2).z() < 0.;
    CHECK(past);
}

TEST_CASE("Letters arc around the side wall of a cup", "[EmbossBend][surface][letters]")
{
    // standing cylinder (a cup), the text on its side wall: z of the text = outward normal
    const double rc = 25.;
    indexed_triangle_set cup = its_make_cylinder(rc, 80., PI / 180.);
    Transform3d to_text = Transform3d::Identity();
    to_text.linear() << 0., 1., 0., //
                        0., 0., 1., //
                        1., 0., 0.;
    to_text.translation() = Vec3d(0., -40., -rc);
    its_transform(cup, to_text);
    // in text coordinates the cylinder axis is the y axis through (0, *, -rc)
    GlyphAdvances           advances;
    const ExPolygonsWithIds text = box_text(8, &advances);
    for (bool rigid : {true, false}) {
        INFO((rigid ? "rigid" : "bent"));
        LetterProjection p = project_letters(text, advances, radius_bend(30.f, false, rigid), cup);
        REQUIRE(p.arc.preview.valid);
        check_letters(text, p, cup, 0.02, rigid ? 0.02 : 0.08);
        // unrolled, the letters sit on the flat circle: radius 30 around the unrolled centre
        auto unroll = [rc](const Vec3d &q) { return Vec2d(rc * std::atan2(q.x(), q.z() + rc), q.y()); };
        const Vec2d c = unroll(p.arc.preview.center);
        for (const std::optional<Transform3d> &f : p.arc.frames)
            if (f.has_value())
                CHECK_THAT((unroll(f->translation()) - c).norm(), WithinAbs(30., 0.1));
        // the text wraps round the cup: 56 mm of text over 25 mm radius
        double a_min = 1e9, a_max = -1e9;
        for (const std::optional<Transform3d> &f : p.arc.frames)
            if (f.has_value()) {
                const double a = std::atan2(f->translation().x(), f->translation().z() + rc);
                a_min = std::min(a_min, a);
                a_max = std::max(a_max, a);
            }
        CHECK((a_max - a_min) * 180. / PI > 90.);
    }
}

TEST_CASE("Parallel projection of a wide arc distorts the outer letters", "[EmbossBend][surface][letters]")
{
    // the reason for letter by letter placement: the same arc projected in one direction
    const indexed_triangle_set sphere = sphere_below(30.);
    GlyphAdvances     advances;
    ExPolygonsWithIds shapes = box_text(8, &advances);
    BendResult r = apply_bend(shapes, angle_bend(120.f, false, true), SCALE, &advances, BEND_SURFACE_TOLERANCE_MM);
    REQUIRE(r.is_active());
    double worst = 0.;
    for (const ExPolygonsWithId &g : shapes) {
        SurfaceProjection p = project_onto(g.expoly, SCALE, sphere);
        if (p.cut.empty())
            continue;
        worst = std::max(worst, surface_area(p.cut) / shapes_area_mm2(g.expoly, SCALE));
    }
    CHECK(worst > 1.2); // the outer letters stretch by more than 20 % (letter by letter: under 3 %)
}

TEST_CASE("Straight text whose glyph side runs exactly through mesh vertices", "[EmbossBend][surface]")
{
    // Regression: with 4 or 6 glyphs a glyph side lies on x = 7.5 mm, which passes exactly through
    // vertices of the 2 degree sphere (30 * sin 30 * cos 60 = 7.5). Corefine records no intersection
    // for such an original vertex and the cut used to crash (CutSurface.cpp, set_face_type).
    const indexed_triangle_set sphere = sphere_below(30.);
    for (int count : {2, 4, 6}) {
        INFO(count << " glyphs");
        GlyphAdvances           advances;
        const ExPolygonsWithIds straight = box_text(count, &advances);
        check_on_surface(straight, project_onto(united(straight, SCALE), SCALE, sphere), sphere);
    }
}

TEST_CASE("Timing of letters on a dome", "[EmbossBend][surface][.perf]")
{
    if (!boost::filesystem::exists(font_path())) {
        WARN("Font not found: " << font_path());
        return;
    }
    std::unique_ptr<FontFile> font = create_font_file(font_path().c_str());
    REQUIRE(font != nullptr);
    FontFileWithCache ff(std::move(font));
    FontProp          fp;
    fp.size_in_mm = 5.f;
    const double scale = get_text_shape_scale(fp, *ff.font_file);
    const std::wstring text = L"Hello, Curved World!";
    GlyphAdvances      advances;
    ExPolygonsWithIds  shapes = text2vshapes(ff, text, fp, []() { return false; }, advances);

    indexed_triangle_set dome = its_make_sphere(40., PI / 90.);
    its_translate(dome, Vec3f(0.f, 0.f, -40.f));

    using clock = std::chrono::steady_clock;
    std::cout << "Letter by letter, \"Hello, Curved World!\" (5 mm) on a 80 mm sphere, best of 3:" << std::endl;
    for (float angle : {120.f, 270.f})
        for (bool rigid : {true, false}) {
            double best_place = 1e9, best_all = 1e9;
            for (int run = 0; run < 3; ++run) {
                auto start = clock::now();
                std::optional<SurfaceGlyphLayout> layout = surface_glyph_layout(shapes, &advances, angle_bend(angle, false, rigid), scale);
                REQUIRE(layout.has_value());
                BendSurface surface(dome);
                SurfaceArc arc = place_on_surface_arc(surface, layout->pivots_mm, layout->x_min, layout->x_max, layout->params);
                auto placed = clock::now();
                size_t done = 0;
                for (size_t i = 0; i < shapes.size(); ++i) {
                    if (shapes[i].expoly.empty() || !arc.frames[i].has_value())
                        continue;
                    ExPolygons glyph = surface_glyph_shape(shapes[i].expoly, layout->pivots[i], arc.curvature_radius[i],
                                                           angle_bend(angle, false, rigid), scale);
                    indexed_triangle_set local = dome;
                    its_transform(local, arc.frames[i]->inverse());
                    done += project_onto(glyph, scale, local).cut.empty() ? 0 : 1;
                }
                best_place = std::min(best_place, std::chrono::duration<double, std::milli>(placed - start).count());
                best_all   = std::min(best_all, std::chrono::duration<double, std::milli>(clock::now() - start).count());
                CHECK(done > 0);
            }
            std::cout << "  " << (rigid ? "rigid " : "bent ") << angle << " deg: placement " << best_place << " ms, total "
                      << best_all << " ms" << std::endl;
        }
}
