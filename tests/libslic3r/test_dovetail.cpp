// Basic dovetail feature coverage: completion, valid topology, output selection
// and retained data. These are smoke tests, not precision acceptance tests.
// Exact volume/world-placement diagnostics are opt-in [DovetailAccuracy] tests.
// GUI clicks, reset widgets and the GUI undo manager require integration tests.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Format/3mf.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>
using namespace Slic3r;
using Catch::Matchers::WithinAbs;
namespace {
TriangleMesh dovetail_box(float x = 40.f, float y = 40.f, float z = 20.f)
{
    indexed_triangle_set m;
    m.vertices = {Vec3f(0, 0, 0), Vec3f(x, 0, 0), Vec3f(x, y, 0), Vec3f(0, y, 0),
                  Vec3f(0, 0, z), Vec3f(x, 0, z), Vec3f(x, y, z), Vec3f(0, y, z)};
    m.indices  = {Vec3i32(0, 2, 1), Vec3i32(0, 3, 2), Vec3i32(4, 5, 6), Vec3i32(4, 6, 7), Vec3i32(0, 5, 4), Vec3i32(0, 1, 5),
                  Vec3i32(3, 6, 2), Vec3i32(3, 7, 6), Vec3i32(0, 7, 3), Vec3i32(0, 4, 7), Vec3i32(1, 6, 5), Vec3i32(1, 2, 6)};
    return TriangleMesh(std::move(m));
}
Cut::Groove standard_groove()
{
    Cut::Groove g;
    g.depth       = 2.f;
    g.width       = 12.f;
    g.flaps_angle = float(PI / 3.);
    g.angle       = 0.f;
    return g;
}
ModelObject* body(Model& model)
{
    auto* object = model.add_object();
    object->add_volume(dovetail_box())->set_offset(Vec3d::Zero());
    object->add_instance();
    return object;
}
auto both_sides() { return ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower; }
void paint_top_bottom(ModelVolume&        volume,
                      EnforcerBlockerType top    = EnforcerBlockerType::Extruder3,
                      EnforcerBlockerType bottom = EnforcerBlockerType::Extruder4)
{
    TriangleSelector selector(volume.mesh());
    selector.set_facet(2, top);
    selector.set_facet(3, top);
    selector.set_facet(0, bottom);
    selector.set_facet(1, bottom);
    volume.mmu_segmentation_facets.set(selector);
}
struct PaintMeasure
{
    double area   = 0.;
    Vec3d  moment = Vec3d::Zero();
};
PaintMeasure measure_paint(const ModelObjectPtrs& objects, EnforcerBlockerType state, size_t instance = 0)
{
    PaintMeasure out;
    for (const auto* object : objects)
        for (const auto* part : object->volumes)
            if (part->is_model_part()) {
                TriangleMesh mesh(part->mmu_segmentation_facets.get_facets(*part, state));
                mesh.transform(object->instances.at(instance)->get_matrix() * part->get_matrix());
                for (const auto& f : mesh.its.indices) {
                    const Vec3d a = mesh.its.vertices[f[0]].cast<double>(), b = mesh.its.vertices[f[1]].cast<double>(),
                                c     = mesh.its.vertices[f[2]].cast<double>();
                    const double area = 0.5 * (b - a).cross(c - a).norm();
                    out.area += area;
                    out.moment += area * (a + b + c) / 3.;
                }
            }
    return out;
}
struct Shape
{
    double                             volume        = 0.;
    double                             cached_volume = 0.;
    size_t                             faces         = 0;
    BoundingBoxf3                      bounds;
    std::set<std::array<long long, 3>> vertices;
};
Shape shape(const ModelObjectPtrs& objects, bool preview = false, size_t instance = 0)
{
    Shape out;
    for (const auto* object : objects) {
        if (preview && object != objects.front())
            continue;
        REQUIRE(instance < object->instances.size());
        for (const auto* part : object->volumes)
            if (part->is_model_part()) {
                TriangleMesh m = part->mesh();
                m.transform(object->instances[instance]->get_matrix() * part->get_matrix());
                out.cached_volume += m.volume();
                // Independent double-precision signed tetrahedra: float cached mesh
                // volume loses accuracy for many thin faces and is not the oracle.
                double signed_volume = 0.;
                for (const auto& face : m.its.indices) {
                    const Vec3d a = m.its.vertices[face[0]].cast<double>();
                    const Vec3d b = m.its.vertices[face[1]].cast<double>();
                    const Vec3d c = m.its.vertices[face[2]].cast<double>();
                    signed_volume += a.dot(b.cross(c)) / 6.;
                }
                out.volume += std::abs(signed_volume);
                out.faces += m.facets_count();
                out.bounds.merge(m.bounding_box());
                REQUIRE(std::all_of(m.its.vertices.begin(), m.its.vertices.end(), [](const Vec3f& v) { return v.allFinite(); }));
                REQUIRE(std::all_of(m.its.indices.begin(), m.its.indices.end(),
                                    [&](const Vec3i32& f) { return f.minCoeff() >= 0 && size_t(f.maxCoeff()) < m.its.vertices.size(); }));
                for (const auto& v : m.its.vertices)
                    out.vertices.insert({std::llround(v.x() * 1000.), std::llround(v.y() * 1000.), std::llround(v.z() * 1000.)});
            }
    }
    return out;
}
Model run(ModelObject*             object,
          Cut::Groove              groove     = standard_groove(),
          int                      count      = 1,
          float                    gap        = 2.f,
          float                    radius     = 30.f,
          bool                     preview    = false,
          ModelObjectCutAttributes attributes = both_sides(),
          Transform3d              rotation   = Transform3d::Identity(),
          int                      instance   = 0)
{
    Cut         cut(object, instance, rotation, attributes);
    const auto& result = cut.perform_with_groove(groove, rotation, count, gap, radius, preview);
    Model       out;
    for (auto* o : result) {
        if (preview && o != result.front())
            continue;
        out.add_object(*o);
    }
    return out;
}
void same_shape(const Shape& a, const Shape& b)
{
    CHECK(a.faces == b.faces);
    CHECK_THAT(a.volume, WithinAbs(b.volume, std::max(1., std::abs(b.volume) * 0.05)));
    CHECK((a.bounds.min - b.bounds.min).norm() < 1.);
    CHECK((a.bounds.max - b.bounds.max).norm() < 1.);
}
void check_standard_body(const Shape& s)
{
    REQUIRE(s.faces > 0);
    CHECK(s.volume > 0.8 * 32000.);
    CHECK(s.volume <= 1.1 * 32000.);
    CHECK((s.bounds.min - Vec3d(-20, -20, -10)).norm() < 1.);
    CHECK((s.bounds.max - Vec3d(20, 20, 10)).norm() < 1.);
}
} // namespace

TEST_CASE("Dovetail spacing width agrees with analytic dimensions", "[Dovetail][DovetailWidth]")
{
    auto g  = standard_groove();
    g.width = 8;
    CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(8. + 4. / std::sqrt(3.), 0.1));
    g.flaps_angle = float(PI / 2.);
    CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(8., 0.1));
    g.flaps_angle = float(2. * PI / 3.);
    CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(8., 0.1));
    g.flaps_angle = float(PI / 3.);
    g.angle       = float(PI / 12.);
    CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(8. + 4. / std::sqrt(3.) + 67.5 * std::tan(PI / 12.), 0.1));
    CHECK(Cut::calculate_groove_width(g, 40) > Cut::calculate_groove_width(g, 20));
}
TEST_CASE("Dovetail width helper rejects nonfinite intermediate results", "[Dovetail][DovetailWidthInvalid]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const auto  base = standard_groove();
    for (float bad : {nan, inf, -inf}) {
        auto g  = base;
        g.width = bad;
        CHECK(std::isfinite(Cut::calculate_groove_width(g, 30)));
        g       = base;
        g.depth = bad;
        CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(base.width, 0.1));
        g       = base;
        g.angle = bad;
        CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(base.width, 0.1));
        g             = base;
        g.flaps_angle = bad;
        CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(base.width, 0.1));
        CHECK_THAT(Cut::calculate_groove_width(base, bad), WithinAbs(Cut::calculate_groove_width(base, 1), 0.1));
    }
    auto g  = base;
    g.width = std::numeric_limits<float>::max();
    CHECK(std::isfinite(Cut::calculate_groove_width(g, 30)));
    g             = base;
    g.flaps_angle = 0;
    CHECK_THAT(Cut::calculate_groove_width(g, 30), WithinAbs(g.width + 2. * g.depth, 0.1));
}
TEST_CASE("Dovetail depth width and angle controls change the physical cut", "[Dovetail][DovetailDimensions]")
{
    Model                    model;
    auto*                    object    = body(model);
    const auto               base      = standard_groove();
    const Shape              reference = shape(run(object).objects);
    std::vector<Cut::Groove> variants;
    for (float depth : {0.1f, 0.5f, 6.f, 8.f}) {
        auto g            = base;
        g.depth           = depth;
        g.depth_tolerance = std::min(0.02f, depth / 10.f);
        variants.push_back(g);
    }
    for (float width : {0.5f, 2.f, 20.f}) {
        auto g            = base;
        g.width           = width;
        g.width_tolerance = 0.02f;
        variants.push_back(g);
    }
    for (float flap : {30.f, 90.f, 120.f}) {
        auto g        = base;
        g.flaps_angle = float(flap * PI / 180.);
        variants.push_back(g);
    }
    for (float angle : {5.f, 15.f}) {
        auto g  = base;
        g.angle = float(angle * PI / 180.);
        variants.push_back(g);
    }
    for (size_t i = 0; i < variants.size(); ++i) {
        INFO("parameter variant=" << i);
        const Shape edited = shape(run(object, variants[i]).objects);
        check_standard_body(edited);
        CHECK(edited.vertices != reference.vertices);
    }
    same_shape(shape(run(object).objects), reference); // Rebuilding defaults is deterministic after edits.
}
TEST_CASE("Dovetail clearance and interference tolerances change fit", "[Dovetail][DovetailTolerance]")
{
    Model model;
    auto* object      = body(model);
    auto  g           = standard_groove();
    g.depth_tolerance = g.width_tolerance = 0;
    const Shape  zero                     = shape(run(object, g).objects);
    const double exact                    = zero.volume;
    INFO("zero tolerance: geometric volume=" << exact << " cached=" << zero.cached_volume);
    CHECK_THAT(exact, WithinAbs(32000., 3200.));
    g.depth_tolerance = g.width_tolerance = 0.2f;
    const double clearance                = shape(run(object, g).objects).volume;
    CHECK(clearance < exact);
    g.depth_tolerance = 0.6f; // GUI upper bound for depth=2 is 0.3*depth.
    g.width_tolerance = 1.5f; // GUI caps the width allowance at 1.5 mm.
    CHECK(shape(run(object, g).objects).volume < clearance);
    g.depth_tolerance = g.width_tolerance = -0.1f;
    // Negative fit allowance must not remove more material than positive clearance.
    CHECK(shape(run(object, g).objects).volume >= clearance);
    g.depth_tolerance = 0.2f;
    g.width_tolerance = 0;
    CHECK(shape(run(object, g).objects).volume < exact);
    g.depth_tolerance = 0;
    g.width_tolerance = 0.2f;
    CHECK(shape(run(object, g).objects).volume < exact);
}
TEST_CASE("Dovetail groove gaps move teeth without changing the outer envelope", "[Dovetail][DovetailGap]")
{
    Model       model;
    auto*       object    = body(model);
    const auto  g         = standard_groove();
    const Shape touching  = shape(run(object, g, 2, 0).objects);
    const Shape separated = shape(run(object, g, 2, 8).objects);
    check_standard_body(touching);
    check_standard_body(separated);
    CHECK(touching.vertices != separated.vertices);
    // A single groove has no neighbours: count-one must ignore the gap and radius.
    same_shape(shape(run(object, g, 1, 0, 1).objects), shape(run(object, g, 1, 100, 60).objects));
}
TEST_CASE("Dovetail counts cover UI limits and clamp out of range requests", "[Dovetail][DovetailCount]")
{
    Model model;
    auto* object      = body(model);
    auto  g           = standard_groove();
    g.depth           = 0.05f;
    g.width           = 0.1f;
    g.depth_tolerance = g.width_tolerance = 0;
    const Shape one                       = shape(run(object, g, 1, 0.05f).objects);
    same_shape(shape(run(object, g, 0, 0.05f).objects), one);
    same_shape(shape(run(object, g, -5, 0.05f).objects), one);
    const Shape many = shape(run(object, g, 100, 0.05f).objects);
    INFO("100 grooves: geometric=" << many.volume << " cached=" << many.cached_volume);
    check_standard_body(many);
    CHECK(many.faces > one.faces);
    same_shape(shape(run(object, g, 101, 0.05f).objects), many);
}
TEST_CASE("Dovetail sanitizes invalid spacing and radius before cutting", "[Dovetail][DovetailSanitize]")
{
    Model model;
    auto* object    = body(model);
    auto  g         = standard_groove();
    g.angle         = float(5. * PI / 180.);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const Shape zero_gap = shape(run(object, g, 2, 0, 1).objects);
    for (float bad : {-1.f, nan, inf, -inf})
        same_shape(shape(run(object, g, 2, bad, 1).objects), zero_gap);
    const Shape unit_radius = shape(run(object, g, 2, 1, 1).objects);
    for (float bad : {0.f, -1.f, nan, inf, -inf})
        same_shape(shape(run(object, g, 2, 1, bad).objects), unit_radius);
    same_shape(shape(run(object, g, 2, 1e10f, 1).objects), shape(run(object, g, 2, 100000.f, 1).objects));
}
TEST_CASE("Dovetail legacy overload preserves single groove behavior", "[Dovetail][DovetailLegacy]")
{
    Model model;
    auto* object = body(model);
    auto  g      = standard_groove();
    paint_top_bottom(*object->volumes.front());
    for (bool preview : {false, true}) {
        Cut         cut(object, 0, Transform3d::Identity(), both_sides() | ModelObjectCutAttribute::KeepPaint);
        const auto& objects = cut.perform_with_groove(g, Transform3d::Identity(), preview);
        const Shape old     = shape(objects, preview);
        Model       current = run(object, g, 1, 0, 1, preview, both_sides() | ModelObjectCutAttribute::KeepPaint);
        same_shape(old, shape(current.objects));
    }
}
TEST_CASE("Dovetail keep upper lower both and neither select the correct paint", "[Dovetail][DovetailKeepSides]")
{
    Model model;
    auto* object = body(model);
    paint_top_bottom(*object->volumes.front());
    for (bool upper : {false, true})
        for (bool lower : {false, true}) {
            ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepPaint;
            if (upper)
                attrs = attrs | ModelObjectCutAttribute::KeepUpper;
            if (lower)
                attrs = attrs | ModelObjectCutAttribute::KeepLower;
            Model result = run(object, standard_groove(), 1, 2, 30, false, attrs);
            CHECK(result.objects.size() == size_t(upper) + size_t(lower));
            CHECK_THAT(measure_paint(result.objects, EnforcerBlockerType::Extruder3).area,
                       WithinAbs(upper ? 1600. : 0., upper ? 80. : 0.001));
            CHECK_THAT(measure_paint(result.objects, EnforcerBlockerType::Extruder4).area,
                       WithinAbs(lower ? 1600. : 0., lower ? 80. : 0.001));
        }
}
TEST_CASE("Dovetail flip and place on cut act on the requested side only", "[Dovetail][DovetailOutputActions]")
{
    Model model;
    auto* object = body(model);
    paint_top_bottom(*object->volumes.front());
    const Transform3d                            rotation = Geometry::rotation_transform(Vec3d(0.2, 0., 0.25));
    const std::array<ModelObjectCutAttribute, 4> actions  = {ModelObjectCutAttribute::FlipUpper, ModelObjectCutAttribute::FlipLower,
                                                             ModelObjectCutAttribute::PlaceOnCutUpper,
                                                             ModelObjectCutAttribute::PlaceOnCutLower};
    for (size_t i = 0; i < actions.size(); ++i) {
        INFO("output action=" << i);
        Model result = run(object, standard_groove(), 1, 2, 30, false, both_sides() | ModelObjectCutAttribute::KeepPaint | actions[i],
                           rotation);
        REQUIRE(result.objects.size() == 2);
        for (size_t side = 0; side < 2; ++side) {
            const auto paint = measure_paint({result.objects[side]},
                                             side == 0 ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::Extruder4);
            REQUIRE_THAT(paint.area, WithinAbs(1600., 80.));
            Vec3d expected(0, 0, side == 0 ? 10. : -10.);
            if (side == i % 2) {
                const Transform3d flip = Geometry::rotation_transform(Vec3d(PI, 0, 0));
                // The lower cut face points the other way: placing it on the
                // cut needs the additional half turn as well as plane alignment.
                expected = (i < 2 ? flip : (side == 1 ? flip * rotation.inverse() : rotation.inverse())) * expected;
            }
            CHECK((paint.moment / paint.area - expected).norm() < 1.);
        }
    }
}
TEST_CASE("Dovetail oblique vertical and reversed planes keep bounded geometry", "[Dovetail][DovetailPlane]")
{
    Model model;
    auto* object = body(model);
    for (const Vec3d& angles :
         std::array<Vec3d, 5>{Vec3d::Zero(), Vec3d(PI / 2, 0, 0), Vec3d(0, PI / 2, 0), Vec3d(0, PI, 0), Vec3d(0.3, 0.2, 0.4)}) {
        const auto  rotation = Geometry::rotation_transform(angles);
        const Shape result   = shape(run(object, standard_groove(), 1, 2, 30, false, both_sides(), rotation).objects);
        check_standard_body(result);
    }
}
TEST_CASE("Dovetail multi volume paint stays with its source body", "[Dovetail][DovetailSources]")
{
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    for (int i = 0; i < 2; ++i) {
        auto* v = object->add_volume(dovetail_box(16, 16, 20));
        v->set_offset(Vec3d(i == 0 ? -12. : 12., 0, 0));
        paint_top_bottom(*v, i == 0 ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::Extruder4, EnforcerBlockerType::NONE);
    }
    Model result = run(object, standard_groove(), 2, 2, 30, false, both_sides() | ModelObjectCutAttribute::KeepPaint);
    for (int i = 0; i < 2; ++i) {
        const auto measure = measure_paint(result.objects, i == 0 ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::Extruder4);
        REQUIRE_THAT(measure.area, WithinAbs(256., 13.));
        CHECK((measure.moment / measure.area - Vec3d(i == 0 ? -12. : 12., 0, 10)).norm() < 1.);
    }
}
TEST_CASE("Dovetail preserves negative volumes and each modifier kind", "[Dovetail][DovetailModifiers]")
{
    for (auto type : {ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::PARAMETER_MODIFIER, ModelVolumeType::SUPPORT_BLOCKER,
                      ModelVolumeType::SUPPORT_ENFORCER}) {
        Model model;
        auto* object   = body(model);
        auto* modifier = object->add_volume(dovetail_box(4, 4, 10), type);
        modifier->set_offset(Vec3d::Zero());
        modifier->name = "retained modifier";
        Model result   = run(object);
        REQUIRE(result.objects.size() == 2);
        for (const auto* piece : result.objects) {
            int count = 0;
            for (const auto* v : piece->volumes)
                if (v->type() == type) {
                    ++count;
                    CHECK(v->name == modifier->name);
                    CHECK(v->mesh().facets_count() == 12);
                }
            CHECK(count == 1);
        }
        CHECK(object->volumes.size() == 2);
    }
}
TEST_CASE("Dovetail mirroring retains asymmetric triangle paint positions", "[Dovetail][DovetailMirror]")
{
    for (const Vec3d& mirror :
         std::array<Vec3d, 5>{Vec3d(-1, 1, 1), Vec3d(1, -1, 1), Vec3d(-1, -1, 1), Vec3d(1, 1, -1), Vec3d(-1, -1, -1)}) {
        Model model;
        auto* object = body(model);
        auto* volume = object->volumes.front();
        volume->set_mirror(mirror);
        TriangleSelector selector(volume->mesh());
        selector.set_facet(2, EnforcerBlockerType::Extruder3);
        selector.set_facet(3, EnforcerBlockerType::Extruder4);
        volume->mmu_segmentation_facets.set(selector);
        Model result = run(object, standard_groove(), 1, 2, 30, false, both_sides() | ModelObjectCutAttribute::KeepPaint);
        for (int k = 0; k < 2; ++k) {
            const auto m = measure_paint(result.objects, k == 0 ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::Extruder4);
            REQUIRE_THAT(m.area, WithinAbs(800., 40.));
            const Vec3d p = volume->get_matrix() * Vec3d(k == 0 ? 20. / 3. : -20. / 3., k == 0 ? -20. / 3. : 20. / 3., 10);
            CHECK((m.moment / m.area - p).norm() < 1.);
        }
    }
}
namespace {
void check_instance_outputs(bool check_exact_positions)
{
    for (double angle : {0., 0.35}) {
        INFO("active instance Z rotation=" << angle);
        Model            model;
        auto*            object = body(model);
        auto*            volume = object->volumes.front();
        TriangleSelector selector(volume->mesh());
        selector.set_facet(2, EnforcerBlockerType::Extruder3);
        selector.set_facet(0, EnforcerBlockerType::Extruder4);
        selector.set_facet(1, EnforcerBlockerType::Extruder4);
        volume->mmu_segmentation_facets.set(selector);
        auto* first = object->instances.front();
        first->set_offset(Vec3d(-30, 15, 7));
        first->set_scaling_factor(Vec3d(1.25, 0.8, 1.5));
        first->set_mirror(Vec3d(-1, 1, 1));
        auto* active = object->add_instance();
        active->set_offset(Vec3d(60, -20, 7));
        // Selection synchronizes scale, mirroring and Z height across copies.
        // Only XY placement and Z rotation differ in the supported UI state.
        active->set_scaling_factor(first->get_scaling_factor());
        active->set_mirror(first->get_mirror());
        active->set_rotation(Vec3d(0, 0, angle));
        const std::array<Transform3d, 2> originals = {first->get_matrix(), active->get_matrix()};
        Model result = run(object, standard_groove(), 1, 2, 30, false, both_sides() | ModelObjectCutAttribute::KeepPaint,
                           Transform3d::Identity(), 1);
        REQUIRE(result.objects.size() == 2);
        for (const auto* piece : result.objects)
            CHECK(piece->instances.size() == 2);
        for (size_t instance = 0; instance < 2; ++instance) {
            const auto paint = measure_paint(result.objects, EnforcerBlockerType::Extruder3, instance);
            REQUIRE_THAT(paint.area, WithinAbs(800., 40.));
            CHECK(paint.moment.allFinite());
            if (check_exact_positions) {
                const Vec3d expected = originals[instance] * Vec3d(20. / 3., -20. / 3., 10.);
                CHECK((paint.moment / paint.area - expected).norm() < 0.002);
            }
            CHECK(object->instances[instance]->get_matrix().isApprox(originals[instance]));
        }
    }
}
} // namespace
TEST_CASE("Dovetail active instance returns valid mirrored scaled copies", "[Dovetail][DovetailInstances]")
{
    check_instance_outputs(false);
}
TEST_CASE("Dovetail exact paint positions across instances", "[.DovetailAccuracy][DovetailAccuracy]") { check_instance_outputs(true); }
TEST_CASE("Dovetail empty previews and open input do not invent invalid vertices", "[Dovetail][DovetailMeshEdges]")
{
    Model empty;
    auto* object = empty.add_object();
    object->add_instance();
    Model preview = run(object, standard_groove(), 1, 2, 30, true);
    REQUIRE(preview.objects.size() == 1);
    CHECK(preview.objects.front()->volumes.empty());
    Model open;
    auto* opened = body(open);
    auto  mesh   = opened->volumes.front()->mesh().its;
    mesh.indices.erase(mesh.indices.begin());
    opened->volumes.front()->set_mesh(std::move(mesh));
    const Shape open_result = shape(run(opened).objects);
    CHECK(open_result.faces > 0);
}
TEST_CASE("Dovetail model checkpoints can replay results without changing input", "[Dovetail][DovetailCheckpoints]")
{
    Model input;
    auto* object = body(input);
    paint_top_bottom(*object->volumes.front());
    Model before     = input;
    Model after      = run(object, standard_groove(), 2, 2, 30, false, both_sides() | ModelObjectCutAttribute::KeepPaint);
    Model checkpoint = after;
    Model replay     = before;
    CHECK(replay.objects.front()->volumes.front()->mesh().facets_count() == 12);
    replay = checkpoint;
    same_shape(shape(replay.objects), shape(after.objects));
    CHECK_THAT(measure_paint(replay.objects, EnforcerBlockerType::Extruder3).area, WithinAbs(1600., 80.));
    CHECK(input.objects.front()->volumes.front()->mesh().its.vertices == before.objects.front()->volumes.front()->mesh().its.vertices);
    same_shape(shape(run(object, standard_groove(), 2, 2, 30).objects), shape(run(object, standard_groove(), 2, 2, 30).objects));
}
TEST_CASE("Dovetail 3MF geometry and paint survive a format round trip", "[Dovetail][Dovetail3MF]")
{
    Model input;
    auto* object = body(input);
    paint_top_bottom(*object->volumes.front());
    Model cut = run(object, standard_groove(), 2, 2, 30, false, both_sides() | ModelObjectCutAttribute::KeepPaint);
    struct TempFile
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("dovetail-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".3mf");
        ~TempFile()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } file;
    const auto filename = file.path.u8string();
    REQUIRE(store_3mf(filename.c_str(), &cut, nullptr, false));
    Model                     loaded;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    REQUIRE(load_3mf(filename.c_str(), config, substitutions, &loaded, false));
    REQUIRE(loaded.objects.size() == cut.objects.size());
    same_shape(shape(loaded.objects), shape(cut.objects));
    CHECK_THAT(measure_paint(loaded.objects, EnforcerBlockerType::Extruder3).area, WithinAbs(1600., 80.));
    CHECK_THAT(measure_paint(loaded.objects, EnforcerBlockerType::Extruder4).area, WithinAbs(1600., 80.));
}
TEST_CASE("Dovetail outputs slice into closed positive contours away from the seam", "[Dovetail][DovetailSlices]")
{
    Model input;
    auto* object = body(input);
    Model result = run(object, standard_groove(), 3, 2, 30);
    for (size_t i = 0; i < result.objects.size(); ++i) {
        const auto*  piece = result.objects[i];
        TriangleMesh mesh  = piece->volumes.front()->mesh();
        mesh.transform(piece->instances.front()->get_matrix() * piece->volumes.front()->get_matrix());
        const float z      = i == 0 ? 5.f : -5.f;
        const auto  slices = slice_mesh_ex(mesh.its, std::vector<float>{z});
        REQUIRE(slices.size() == 1);
        REQUIRE(slices.front().size() == 1);
        CHECK_THAT(area(slices.front()) * double(SCALING_FACTOR) * double(SCALING_FACTOR), WithinAbs(1600., 80.));
    }
}

TEST_CASE("Dovetail seam layers retain closed contours and bounded material area", "[Dovetail][DovetailSeamSlices]")
{
    Model              model;
    auto*              object = body(model);
    Model              cut    = run(object, standard_groove(), 3, 2, 30);
    std::vector<float> heights;
    for (int i = 0; i < 40; ++i)
        heights.push_back(-9.75f + 0.5f * i);
    std::vector<double> cross_section(heights.size(), 0.);
    for (const auto* piece : cut.objects) {
        for (const auto* part : piece->volumes)
            if (part->is_model_part()) {
                TriangleMesh mesh = part->mesh();
                mesh.transform(piece->instances.front()->get_matrix() * part->get_matrix());
                const auto slices = slice_mesh_ex(mesh.its, heights);
                REQUIRE(slices.size() == heights.size());
                for (size_t layer = 0; layer < slices.size(); ++layer)
                    for (const auto& polygon : slices[layer]) {
                        REQUIRE(polygon.contour.points.size() >= 3);
                        REQUIRE(polygon.area() > 0.);
                        cross_section[layer] += polygon.area() * double(SCALING_FACTOR) * double(SCALING_FACTOR);
                    }
            }
    }
    for (size_t i = 0; i < heights.size(); ++i) {
        INFO("layer z=" << heights[i]);
        CHECK(cross_section[i] > 1440.);
        CHECK(cross_section[i] <= 1680.);
        if (std::abs(heights[i]) > 1.1f)
            CHECK_THAT(cross_section[i], WithinAbs(1600., 80.));
    }
}
TEST_CASE("Dovetail cut height transfers material between the requested halves", "[Dovetail][DovetailHeight]")
{
    Model model;
    auto* object = body(model);
    paint_top_bottom(*object->volumes.front());
    double previous_upper = std::numeric_limits<double>::max(), previous_lower = 0.;
    for (double height : {-7., 0., 7.}) {
        Cut         cut(object, 0, Geometry::translation_transform(Vec3d(0, 0, height)), both_sides() | ModelObjectCutAttribute::KeepPaint);
        const auto& results = cut.perform_with_groove(standard_groove(), Transform3d::Identity(), 1, 2.f, 30.f, false);
        REQUIRE(results.size() == 2);
        const Shape upper = shape({results[0]}), lower = shape({results[1]});
        CHECK(upper.volume < previous_upper);
        CHECK(lower.volume > previous_lower);
        previous_upper = upper.volume;
        previous_lower = lower.volume;
        CHECK_THAT(measure_paint({results[0]}, EnforcerBlockerType::Extruder3).area, WithinAbs(1600., 80.));
        CHECK_THAT(measure_paint({results[1]}, EnforcerBlockerType::Extruder4).area, WithinAbs(1600., 80.));
    }
}
TEST_CASE("Dovetail local brush stays in its original top surface region", "[Dovetail][DovetailLocalBrush]")
{
    Model            model;
    auto*            object = body(model);
    auto*            volume = object->volumes.front();
    TriangleSelector selector(volume->mesh());
    auto             cursor = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(7, -4, 10), Vec3f(7, -4, 40), 3.f,
                                                                                  TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(),
                                                                                  TriangleSelector::ClippingPlane());
    selector.select_patch(2, std::move(cursor), EnforcerBlockerType::Extruder3, Transform3d::Identity(), true);
    volume->mmu_segmentation_facets.set(selector);
    const auto before = measure_paint({object}, EnforcerBlockerType::Extruder3);
    REQUIRE(before.area > 1.);
    for (int count : {1, 3})
        for (bool keep : {false, true}) {
            const auto flags = keep ? both_sides() | ModelObjectCutAttribute::KeepPaint : both_sides();
            Model      cut   = run(object, standard_groove(), count, 2, 30, false, flags);
            const auto after = measure_paint(cut.objects, EnforcerBlockerType::Extruder3);
            if (keep) {
                CHECK_THAT(after.area, WithinAbs(before.area, before.area * 0.1));
                REQUIRE(after.area > 0.);
                CHECK((after.moment / after.area - before.moment / before.area).norm() < 1.);
            } else
                CHECK(after.area == 0.);
        }
}

TEST_CASE("Dovetail exact zero clearance volume conservation", "[.DovetailAccuracy][DovetailAccuracy]")
{
    Model model;
    auto* object           = body(model);
    auto  groove           = standard_groove();
    groove.depth_tolerance = groove.width_tolerance = 0.f;
    const Shape result                              = shape(run(object, groove).objects);
    INFO("geometric volume=" << result.volume << " cached volume=" << result.cached_volume);
    CHECK_THAT(result.volume, WithinAbs(32000., 0.1));
}
