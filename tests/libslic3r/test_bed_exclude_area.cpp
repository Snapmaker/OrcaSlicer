#include <catch2/catch.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;

// bed_exclude_area used to be read two ways: print validation (get_bed_excluded_area) took the
// whole point list as ONE polygon, while PartPlate's exclusion boxes (the "object fully inside"
// check, arrange's fixed items) and Model.cpp took consecutive groups of 4 points as rectangles.
// Upstream Orca's Kobra 3 ring (outline + reversed inner outline, 10 points) excluded the whole
// bed under the box reading (CLI -50); Anycubic's 16-point edge strips are a rectangle list.
// Every reader now goes through bed_exclude_area_polygons() / bed_exclude_area_boxes().

namespace {

Pointfs parse_points(const std::string &s)
{
    ConfigOptionPoints opt;
    opt.deserialize(s);
    return opt.values;
}

// ---- The two readers as they were on main before this change (reference implementations). ----

// PartPlate::calc_bounding_boxes(): one box per complete group of 4 points.
std::vector<BoundingBoxf> old_boxes(const Pointfs &pts)
{
    std::vector<BoundingBoxf> out;
    BoundingBoxf              bb;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i % 4 == 0)
            bb = BoundingBoxf();
        bb.merge(pts[i]);
        if (i % 4 == 3)
            out.emplace_back(bb);
    }
    return out;
}

// get_bed_excluded_area(): the whole list as one counter-clockwise polygon.
Polygons old_polygon(const Pointfs &pts)
{
    Polygon poly;
    for (const Vec2d &p : pts)
        poly.points.emplace_back(scale_(p.x()), scale_(p.y()));
    poly.make_counter_clockwise();
    return { poly };
}

Polygons box_polygons(const std::vector<BoundingBoxf> &boxes)
{
    Polygons out;
    for (const BoundingBoxf &b : boxes) {
        if (b.max.x() - b.min.x() < EPSILON || b.max.y() - b.min.y() < EPSILON)
            continue;
        const Point lo(scale_(b.min.x()), scale_(b.min.y())), hi(scale_(b.max.x()), scale_(b.max.y()));
        out.emplace_back(Points{ lo, Point(hi.x(), lo.y()), hi, Point(lo.x(), hi.y()) });
    }
    return out;
}

double region_area_mm2(const Polygons &polys)
{
    double a = 0.;
    for (const ExPolygon &ex : union_ex(polys, ClipperLib::pftNonZero))
        a += ex.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

// Area of the symmetric difference, in mm^2.
double xor_area_mm2(const Polygons &a, const Polygons &b)
{
    const Polygons ua = to_polygons(union_ex(a, ClipperLib::pftNonZero));
    const Polygons ub = to_polygons(union_ex(b, ClipperLib::pftNonZero));
    return region_area_mm2(diff(ua, ub)) + region_area_mm2(diff(ub, ua));
}

bool same_boxes(const std::vector<BoundingBoxf> &a, const std::vector<BoundingBoxf> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if ((a[i].min - b[i].min).norm() > 1e-9 || (a[i].max - b[i].max).norm() > 1e-9)
            return false;
    return true;
}

// Every instantiable machine preset under resources/profiles, its bed_exclude_area resolved
// through "inherits" within the vendor folder. Returns value -> preset names.
std::map<std::string, std::vector<std::string>> shipped_exclude_areas()
{
    namespace fs = boost::filesystem;
    std::map<std::string, std::vector<std::string>> out;
    const fs::path profiles = fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles";
    REQUIRE(fs::is_directory(profiles));
    for (fs::directory_iterator vit(profiles); vit != fs::directory_iterator(); ++vit) {
        const fs::path machine_dir = vit->path() / "machine";
        if (!fs::is_directory(machine_dir))
            continue;
        std::map<std::string, nlohmann::json> by_name;
        for (fs::recursive_directory_iterator it(machine_dir); it != fs::recursive_directory_iterator(); ++it) {
            if (!fs::is_regular_file(it->path()) || it->path().extension() != ".json")
                continue;
            boost::nowide::ifstream ifs(it->path().string());
            nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
            if (j.is_object() && j.contains("name") && j["name"].is_string()) {
                const std::string preset_name = j["name"].get<std::string>();
                by_name[preset_name]          = std::move(j);
            }
        }
        for (const auto &[name, j] : by_name) {
            if (!j.contains("instantiation") || j["instantiation"] != "true")
                continue;
            const nlohmann::json *cur = &j;
            for (int depth = 0; cur != nullptr && !cur->contains("bed_exclude_area") && depth < 32; ++depth) {
                auto parent = cur->contains("inherits") && (*cur)["inherits"].is_string() ?
                                  by_name.find((*cur)["inherits"].get<std::string>()) : by_name.end();
                cur = parent == by_name.end() ? nullptr : &parent->second;
            }
            if (cur == nullptr || !cur->contains("bed_exclude_area"))
                continue;
            const nlohmann::json &v = (*cur)["bed_exclude_area"];
            std::string joined;
            if (v.is_array()) {
                for (const auto &e : v)
                    if (e.is_string())
                        joined += (joined.empty() ? "" : ",") + e.get<std::string>();
            } else if (v.is_string())
                joined = v.get<std::string>();
            if (parse_points(joined).size() < 3)
                continue; // the "0x0" default: no exclusion
            out[joined].emplace_back(vit->path().filename().string() + "/" + name);
        }
    }
    return out;
}

const char *KOBRA3_RING  = "0x0,255x0,255x255,0x255,0x0,2x2,2x253,253x253,253x2,2x2";
const char *KOBRA3_STRIP = "0x0,3x0,3x420,0x420,0x0,423x0,423x0,423x0,423x0,426x0,426x420,423x420,423x0,0x0,0x0,0x0";

} // namespace

TEST_CASE("Every shipped bed_exclude_area keeps its excluded region and its arrange boxes", "[BedExcludeArea]")
{
    const auto shipped = shipped_exclude_areas();
    // Bambu, Elegoo, FlyingBear, Snapmaker A/J, Qidi x5, Anycubic x2 at the time of writing.
    REQUIRE(shipped.size() >= 20);

    // Lists the two old readers disagreed on. Everything else must come out exactly as before.
    const std::set<std::string> broken_before = { KOBRA3_RING };

    std::set<std::string> seen_broken;
    for (const auto &[value, presets] : shipped) {
        const Pointfs pts = parse_points(value);
        INFO("bed_exclude_area = " << value << " (" << presets.size() << " presets, e.g. " << presets.front() << ")");

        const std::vector<BoundingBoxf> boxes_before = old_boxes(pts);
        const Polygons                  poly_before  = old_polygon(pts);
        const double disagreement = xor_area_mm2(box_polygons(boxes_before), poly_before);
        const Polygons now = bed_exclude_area_polygons(pts);

        if (disagreement < 1e-6) {
            // Both old readers agreed: the excluded region and the GUI's boxes are unchanged.
            CHECK(broken_before.count(value) == 0);
            CHECK(xor_area_mm2(now, poly_before) < 1e-6);
            CHECK(same_boxes(bed_exclude_area_boxes(pts), boxes_before));
            CHECK(bed_exclude_area_is_rectangles(pts));
        } else {
            CHECK(broken_before.count(value) == 1);
            seen_broken.insert(value);
        }
        // Whatever the reading, the boxes cover the region and no box covers the whole list's
        // bounding box unless the region does.
        CHECK(xor_area_mm2(box_polygons(bed_exclude_area_boxes(pts)), now) < 1e-6);
    }
    CHECK(seen_broken == broken_before);
}

TEST_CASE("Upstream Orca's Kobra 3 ring reads as a 2 mm frame, not the whole bed", "[BedExcludeArea]")
{
    const Pointfs pts = parse_points(KOBRA3_RING);
    REQUIRE_FALSE(bed_exclude_area_is_rectangles(pts));

    const Polygons region = bed_exclude_area_polygons(pts);
    CHECK(region_area_mm2(region) == Approx(255. * 255. - 251. * 251.).epsilon(1e-9));
    // Hole-free pieces only (arrange's fixed items and the plate boxes cannot carry holes), which
    // together make one frame with one hole.
    CHECK(region.size() == 4);
    const ExPolygons frame = union_ex(region);
    REQUIRE(frame.size() == 1);
    CHECK(frame.front().holes.size() == 1);

    // The old box reader made the whole bed one box; now every box is a 2 mm edge strip.
    const std::vector<BoundingBoxf> boxes = bed_exclude_area_boxes(pts);
    REQUIRE(boxes.size() == 4);
    for (const BoundingBoxf &b : boxes) {
        const Vec2d size = b.size();
        CHECK(std::min(size.x(), size.y()) == Approx(2.));
    }
    // A cube in the middle of the bed is clear of it; one touching the edge is not.
    const Polygon centre_cube(Points{ { scale_(117.), scale_(117.) }, { scale_(137.), scale_(117.) },
                                      { scale_(137.), scale_(137.) }, { scale_(117.), scale_(137.) } });
    CHECK(intersection(region, Polygons{ centre_cube }).empty());
    const Polygon edge_cube(Points{ { scale_(1.), scale_(100.) }, { scale_(21.), scale_(100.) },
                                    { scale_(21.), scale_(120.) }, { scale_(1.), scale_(120.) } });
    CHECK_FALSE(intersection(region, Polygons{ edge_cube }).empty());
}

TEST_CASE("Anycubic's 16-point edge strips read as two rectangles", "[BedExcludeArea]")
{
    const Pointfs pts = parse_points(KOBRA3_STRIP);
    REQUIRE(bed_exclude_area_is_rectangles(pts));
    const Polygons region = bed_exclude_area_polygons(pts);
    CHECK(region.size() == 2); // the zero-area padding groups are dropped
    CHECK(region_area_mm2(region) == Approx(2. * 3. * 420.));
    // The boxes are exactly the old ones, padding included (arrange inflates them).
    CHECK(same_boxes(bed_exclude_area_boxes(pts), old_boxes(pts)));
}

TEST_CASE("bed_exclude_area edge cases", "[BedExcludeArea]")
{
    SECTION("the 0x0 default and short lists exclude nothing")
    {
        CHECK(bed_exclude_area_polygons(parse_points("0x0")).empty());
        CHECK(bed_exclude_area_boxes(parse_points("0x0")).empty());
        CHECK(bed_exclude_area_polygons(parse_points("0x0,10x10")).empty());
    }
    SECTION("a single Bambu cutter rectangle")
    {
        const Pointfs pts = parse_points("0x0,18x0,18x28,0x28");
        CHECK(bed_exclude_area_is_rectangles(pts));
        CHECK(region_area_mm2(bed_exclude_area_polygons(pts)) == Approx(18. * 28.));
        CHECK(same_boxes(bed_exclude_area_boxes(pts), old_boxes(pts)));
    }
    SECTION("a 4-point quad that is not a rectangle is a polygon, its box unchanged")
    {
        const Pointfs pts = parse_points("0x0,30x0,20x20,0x20");
        CHECK_FALSE(bed_exclude_area_is_rectangles(pts));
        CHECK(region_area_mm2(bed_exclude_area_polygons(pts)) == Approx(500.));
        CHECK(same_boxes(bed_exclude_area_boxes(pts), old_boxes(pts)));
    }
    SECTION("a triangle (3 points) is a polygon; the box reader used to ignore it")
    {
        const Pointfs pts = parse_points("0x0,30x0,0x30");
        CHECK(region_area_mm2(bed_exclude_area_polygons(pts)) == Approx(450.));
        CHECK(bed_exclude_area_boxes(pts).size() == 1);
    }
    SECTION("a clockwise outline excludes the same region")
    {
        const Polygons ccw = bed_exclude_area_polygons(parse_points("0x0,30x0,20x20,0x20"));
        const Polygons cw  = bed_exclude_area_polygons(parse_points("0x20,20x20,30x0,0x0"));
        CHECK(xor_area_mm2(ccw, cw) < 1e-6);
    }
}

TEST_CASE("Print validation accepts a centred cube on the Kobra 3 ring and strip beds", "[BedExcludeArea]")
{
    struct Bed
    {
        const char *name, *printable_area, *exclude;
        double cx, cy;
    };
    const Bed beds[] = {
        { "Kobra 3 ring", "0x0,255x0,255x255,0x255", KOBRA3_RING, 127.5, 127.5 },
        { "edge strips", "0x0,426x0,426x420,0x420", KOBRA3_STRIP, 213., 210. },
        { "Bambu cutter", "0x0,256x0,256x256,0x256", "0x0,18x0,18x28,0x28", 128., 128. },
    };
    for (const Bed &bed : beds) {
        DYNAMIC_SECTION(bed.name)
        {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({ { "printable_area", bed.printable_area },
                                            { "bed_exclude_area", bed.exclude },
                                            { "layer_change_gcode", "G92 E0" } });
            Model        model;
            ModelObject *object = model.add_object();
            object->name        = "cube.stl";
            object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
            object->add_instance();
            object->center_around_origin();
            object->instances.front()->set_offset(Vec3d(bed.cx, bed.cy, 0.));
            object->ensure_on_bed();
            Print print;
            print.auto_assign_extruders(object);
            print.apply(model, config);
            const StringObjectException err = print.validate();
            INFO(err.string);
            CHECK(err.string.empty());
        }
    }
}
