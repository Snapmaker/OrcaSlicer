#include <catch2/catch.hpp>

// Curved (arc) text for the Emboss/Text tool: the pure 2D bend and its 3MF round trip.

#include <libslic3r/EmbossBend.hpp>
#include <libslic3r/Emboss.hpp>
#include <libslic3r/EmbossShape.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/IntersectionPoints.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/miniz_extension.hpp>

#include <boost/filesystem.hpp>

#include <cmath>
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
void store_text_project(const std::string &path, const EmbossBend &bend)
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
    text->emboss_shape   = es;
    TextConfiguration tc;
    tc.text              = "Curved";
    tc.style.name        = "test";
    tc.style.path        = "test.ttf";
    tc.style.type        = EmbossStyle::Type::file_path;
    tc.style.prop.size_in_mm = 5.f;
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

std::optional<EmbossShape> load_text_shape(const std::string &path)
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
            if (v->emboss_shape.has_value() && v->text_configuration.has_value())
                return v->emboss_shape;
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
}
