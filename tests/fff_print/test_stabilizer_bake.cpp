// Side stabilizers baked into real geometry (libslic3r/Support/StabilizerBake.hpp), painted
// stabilizer points (paint-on supports, EnforcerBlockerType::STABILIZER), and the Off / Auto /
// Manual modes of the stabilizer_supports setting.
//
// The bake's real gate is the parity oracle: the analytic mesh, sliced at the object's own layer
// heights, must give the same cross-sections the live generator prints (slice_struts). The rest
// checks it is a closed mesh, lands where the live struts stand (rotated and scaled parts too), goes
// into the model as an object or a part and survives a 3MF round trip, and that the source object's
// live stabilizers go off.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/StabilizerBake.hpp"
#include "libslic3r/Support/StabilizerMesh.hpp"
#include "libslic3r/Support/Stabilizers.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

double area_of(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return unscaled(unscaled(a));
}

struct Bench
{
    Print print;
    Model model;
};

// The pin of test_stabilizers.cpp: 6 mm wide, 60 mm tall, supports on and nothing to support.
DynamicPrintConfig pin_config(const char *mode, const char *ring_spacing = "15", const char *tip_gap = "0")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              "1" },
        { "support_type",                "normal(auto)" },
        { "support_on_build_plate_only", "1" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "stabilizer_supports",         mode },
        { "stabilizer_ring_spacing",     ring_spacing },
        { "stabilizer_points_per_ring",  "3" },
        { "stabilizer_tip_gap",          tip_gap },
    });
    return config;
}

ModelObject *add_pin(Model &model, double rotation_z = 0., const Vec3d &scale = Vec3d::Ones())
{
    ModelObject *object = model.add_object();
    object->name = "pin";
    object->add_volume(TriangleMesh(its_make_cylinder(3., 60., M_PI / 90.)));
    ModelInstance *inst = object->add_instance();
    inst->set_rotation(Vec3d(0., 0., rotation_z));
    inst->set_scaling_factor(scale);
    object->ensure_on_bed();
    return object;
}

void slice(Bench &b, const DynamicPrintConfig &config)
{
    for (ModelObject *o : b.model.objects)
        b.print.auto_assign_extruders(o);
    b.print.apply(b.model, config);
    b.print.set_status_silent();
    b.print.process();
}

// Paints a stabilizer point (a disc of `radius` mm of the surface) around `world`, a point on the
// first volume's surface given in world coordinates with the surface's outward normal there.
void paint_spot(ModelObject &object, const Vec3d &world, const Vec3d &world_normal, float radius = 1.5f)
{
    ModelVolume        *mv = object.volumes.front();
    const Transform3d   m  = object.instances.front()->get_matrix() * mv->get_matrix();
    const Vec3d         p  = m.inverse() * world;
    const Vec3d         n  = (m.linear().inverse().transpose() * world_normal).normalized();
    const indexed_triangle_set &its = mv->mesh().its;
    // The facet the point lies on.
    int    facet = -1;
    double best  = std::numeric_limits<double>::max();
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const Vec3d a = its.vertices[its.indices[i](0)].cast<double>(), b = its.vertices[its.indices[i](1)].cast<double>(),
                    c = its.vertices[its.indices[i](2)].cast<double>();
        const Vec3d nrm = (b - a).cross(c - a);
        if (nrm.norm() < 1e-12)
            continue;
        const Vec3d  un = nrm.normalized();
        const double d  = std::abs((p - a).dot(un));
        const Vec3d  q  = p - un * (p - a).dot(un);
        // Barycentric test of the projection.
        const Vec3d  v0 = b - a, v1 = c - a, v2 = q - a;
        const double d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
        const double den = d00 * d11 - d01 * d01;
        const double v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den, u = 1. - v - w;
        if (u < -1e-4 || v < -1e-4 || w < -1e-4 || un.dot(n) < 0.5)
            continue;
        if (d < best) {
            best  = d;
            facet = int(i);
        }
    }
    REQUIRE(facet >= 0);
    TriangleSelector sel(mv->mesh());
    sel.deserialize(mv->supported_facets.get_data(), false);
    Transform3d linear = Transform3d::Identity();
    linear.linear()    = m.linear();
    sel.select_patch(facet,
                     std::make_unique<TriangleSelector::Sphere>(p.cast<float>(), (p + n * 50.).cast<float>(), radius, linear,
                                                                TriangleSelector::ClippingPlane()),
                     EnforcerBlockerType::STABILIZER, linear, true, 0.f);
    mv->supported_facets.set(sel);
}

// The world-space mesh of an object's instance.
indexed_triangle_set world_mesh(const ModelObject &object, size_t instance = 0)
{
    indexed_triangle_set out;
    for (const ModelVolume *v : object.volumes) {
        indexed_triangle_set its = v->mesh().its;
        its_transform(its, object.instances[instance]->get_matrix() * v->get_matrix(), true);
        its_merge(out, its);
    }
    return out;
}

// How the analytic mesh slices compared to the live struts, layer by layer.
struct Parity
{
    double live      = 0.; // mm^2 summed over layers
    double baked     = 0.;
    double xor_area  = 0.; // symmetric difference
    double in_part   = 0.; // baked area inside the part
    double worst_rel = 0.; // worst symmetric difference / live area, over layers of more than 1 mm^2
    double worst_z   = 0.;
    double worst_iou = 1.;
    size_t layers    = 0;
};

Parity parity(const std::vector<stabilizers::LayerOutline> &outlines, const stabilizers::StabilizerSettings &st,
              const std::vector<stabilizers::Strut> &struts, const indexed_triangle_set &mesh)
{
    Parity out;
    const std::vector<ExPolygons> live = stabilizers::slice_struts(outlines, st, struts, {});
    std::vector<float> zs;
    for (const stabilizers::LayerOutline &l : outlines)
        zs.push_back(l.slice_z);
    const std::vector<ExPolygons> baked = slice_mesh_ex(mesh, zs);
    for (size_t i = 0; i < outlines.size(); ++i) {
        const double a = area_of(live[i]), b = area_of(baked[i]);
        if (a <= 0. && b <= 0.)
            continue;
        const double x = area_of(diff_ex(live[i], baked[i])) + area_of(diff_ex(baked[i], live[i]));
        out.live += a;
        out.baked += b;
        out.xor_area += x;
        out.in_part += area_of(intersection_ex(baked[i], *outlines[i].islands));
        ++out.layers;
        if (a > 1.) {
            const double inter = area_of(intersection_ex(live[i], baked[i]));
            const double uni   = a + b - inter;
            out.worst_iou = std::min(out.worst_iou, uni > 0. ? inter / uni : 1.);
            if (x / a > out.worst_rel) {
                out.worst_rel   = x / a;
                out.worst_z     = zs[i];
            }
        }
    }
    return out;
}

bool load_project(Bench &b, DynamicPrintConfig &config)
{
    const std::string         path = std::string(TEST_DATA_DIR) + "/stabilizers/stabilizer_v1.3mf";
    ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl = false;
    Semver                    file_version;
    const bool ok = load_bbs_3mf(path.c_str(), &config, &ctxt, &b.model, &plate_data, &project_presets, &is_bbl,
                                 &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plate_data);
    return ok && ! b.model.objects.empty();
}

bool slice_project(Bench &b)
{
    DynamicPrintConfig config;
    if (! load_project(b, config))
        return false;
    config.normalize_fdm();
    slice(b, config);
    return ! b.print.objects().empty();
}

// Stores `model` as a project and loads it back.
bool round_trip(Model &model, Model &loaded, const char *name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string  file   = (tmp_root / name).string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    StoreParams store_params;
    store_params.path     = file.c_str();
    store_params.model    = &model;
    store_params.config   = &config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    if (! store_bbs_3mf(store_params))
        return false;

    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded_ok = load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &loaded, &plate_data, &project_presets, &is_bbl_3mf,
                                        &file_version, nullptr,
                                        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                            LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(file);
    return loaded_ok;
}

StabilizerMode mode_of(const ModelObject &object)
{
    const ConfigOption *opt = object.config.option("stabilizer_supports");
    return opt == nullptr ? smOff : StabilizerMode(opt->getInt());
}

bool has_warning(const PrintObject &po, PrintStateBase::SlicingNotificationType id, std::string *message = nullptr)
{
    for (const PrintStateBase::Warning &w : po.step_state_with_warnings(posSupportMaterial).warnings)
        if (w.message_id == id) {
            if (message != nullptr)
                *message = w.message;
            return true;
        }
    return false;
}

double stabilizer_support_area(const PrintObject &po)
{
    double a = 0.;
    for (const SupportLayer *sl : po.support_layers())
        a += area_of(sl->support_islands);
    return a;
}

} // namespace

// --- the setting -------------------------------------------------------------------------------

TEST_CASE("Side stabilizers saved as a checkbox load as Auto or Off", "[Stabilizers][Config]")
{
    for (const auto &[value, mode] : std::vector<std::pair<std::string, StabilizerMode>>{
             { "1", smAuto }, { "0", smOff }, { "true", smAuto }, { "false", smOff },
             { "auto", smAuto }, { "manual", smManual }, { "off", smOff } }) {
        DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
        cfg.set_deserialize_strict("stabilizer_supports", value);
        INFO("stored as " << value);
        CHECK(cfg.opt_enum<StabilizerMode>("stabilizer_supports") == mode);
    }

    // The owner's project was saved with the checkbox: on for the object, off in the project settings.
    Bench              b;
    DynamicPrintConfig config;
    REQUIRE(load_project(b, config));
    CHECK(mode_of(*b.model.objects.front()) == smAuto);
    CHECK(config.opt_enum<StabilizerMode>("stabilizer_supports") == smOff);
}

// --- painted points and modes ----------------------------------------------------------------

TEST_CASE("Painted stabilizer points add struts by the modes", "[Stabilizers][StabilizerPaint]")
{
    // A spot on the +Y side at 22 mm: between the rings (15, 30) and between their directions (0, 120,
    // 240 degrees), so no ring strut serves it.
    const Vec3d spot_normal(0., 1., 0.);
    auto spot_at = [](const ModelObject &o, double z) {
        const Vec3d off = o.instances.front()->get_offset();
        return Vec3d(off.x(), off.y() + 3., z);
    };

    size_t ring_struts = 0;
    {
        Bench b;
        add_pin(b.model);
        slice(b, pin_config("auto"));
        ring_struts = stabilizers::plan_struts(*b.print.objects().front()).size();
        REQUIRE(ring_struts == 9);
    }

    SECTION("Auto: the rings plus a strut for the painted point")
    {
        Bench        b;
        ModelObject *o = add_pin(b.model);
        paint_spot(*o, spot_at(*o, 22.), spot_normal);
        slice(b, pin_config("auto"));
        const PrintObject &po = *b.print.objects().front();
        REQUIRE(stabilizers::painted_spots(po).size() == 1);
        stabilizers::PlanReport rep;
        const std::vector<stabilizers::Strut> struts = stabilizers::plan_struts(po, &rep);
        CHECK(struts.size() == ring_struts + 1);
        CHECK(rep.painted_placed == 1);
        CHECK(rep.unreachable.empty());
        const auto painted = std::count_if(struts.begin(), struts.end(), [](const stabilizers::Strut &s) { return s.painted; });
        CHECK(painted == 1);
    }

    SECTION("Auto with rings too far apart to place any: the painted point still gets its strut")
    {
        Bench        b;
        ModelObject *o = add_pin(b.model);
        paint_spot(*o, spot_at(*o, 30.), spot_normal);
        slice(b, pin_config("auto", "100"));
        const PrintObject &po = *b.print.objects().front();
        const std::vector<stabilizers::Strut> struts = stabilizers::plan_struts(po);
        REQUIRE(struts.size() == 1);
        const stabilizers::Strut &s = struts.front();
        CHECK(s.painted);
        // On the painted spot: the pin's +Y side at 30 mm, in the print frame (centred on the pin).
        CHECK_THAT(s.tip.x(), WithinAbs(0., 0.1));
        CHECK_THAT(s.tip.y(), WithinAbs(3., 0.1));
        CHECK_THAT(s.tip_z, WithinAbs(30., 0.2));
        CHECK(s.dir.y() > 0.9);
        // ...and it prints: stabilizer support around that height, nothing on the bed but its pillar.
        CHECK(stabilizer_support_area(po) > 1.);
    }

    SECTION("Manual: struts at the painted points only")
    {
        Bench        b;
        ModelObject *o = add_pin(b.model);
        paint_spot(*o, spot_at(*o, 22.), spot_normal);
        paint_spot(*o, spot_at(*o, 40.), spot_normal);
        slice(b, pin_config("manual"));
        const PrintObject &po = *b.print.objects().front();
        const std::vector<stabilizers::Strut> struts = stabilizers::plan_struts(po);
        REQUIRE(struts.size() == 2);
        for (const stabilizers::Strut &s : struts) {
            CHECK(s.painted);
            CHECK(s.dir.y() > 0.9);
        }
        CHECK(stabilizer_support_area(po) > 1.);
        CHECK_FALSE(has_warning(po, PrintStateBase::SlicingStabilizerManualUnpainted));
    }

    SECTION("Manual without paint: nothing, and a warning naming the object")
    {
        Bench b;
        add_pin(b.model);
        slice(b, pin_config("manual"));
        const PrintObject &po = *b.print.objects().front();
        CHECK(stabilizers::plan_struts(po).empty());
        CHECK(stabilizer_support_area(po) < 0.01);
        std::string message;
        CHECK(has_warning(po, PrintStateBase::SlicingStabilizerManualUnpainted, &message));
        CHECK(message.find("pin") != std::string::npos);
    }

    SECTION("Off: nothing, painted points or not")
    {
        Bench        b;
        ModelObject *o = add_pin(b.model);
        paint_spot(*o, spot_at(*o, 22.), spot_normal);
        slice(b, pin_config("off"));
        const PrintObject &po = *b.print.objects().front();
        CHECK(stabilizers::plan_struts(po).empty());
        CHECK(stabilizer_support_area(po) < 0.01);
    }

    SECTION("A painted point no strut can reach: a warning naming the object")
    {
        // 1 mm above the bed: no 45 degree strut has room under it to reach the bed beside the pin.
        Bench        b;
        ModelObject *o = add_pin(b.model);
        paint_spot(*o, spot_at(*o, 1.), spot_normal, 0.5f);
        slice(b, pin_config("auto"));
        const PrintObject &po = *b.print.objects().front();
        stabilizers::PlanReport rep;
        CHECK(stabilizers::plan_struts(po, &rep).size() == ring_struts);
        CHECK(rep.unreachable.size() == 1);
        std::string message;
        CHECK(has_warning(po, PrintStateBase::SlicingStabilizerPaintUnreachable, &message));
        CHECK(message.find("pin") != std::string::npos);
    }
}

TEST_CASE("Painted stabilizer points survive a 3MF round trip", "[StabilizerPaint][3mf]")
{
    Model        model;
    ModelObject *o = add_pin(model);
    const Vec3d  off = o->instances.front()->get_offset();
    paint_spot(*o, Vec3d(off.x(), off.y() + 3., 22.), Vec3d(0., 1., 0.));
    paint_spot(*o, Vec3d(off.x() - 3., off.y(), 35.), Vec3d(-1., 0., 0.));
    const std::vector<stabilizers::PaintedSpot> before =
        stabilizers::painted_spots(*o, o->instances.front()->get_matrix(), 15.);
    REQUIRE(before.size() == 2);

    Model loaded;
    REQUIRE(round_trip(model, loaded, "stabilizer_paint_round_trip.3mf"));
    REQUIRE(loaded.objects.size() == 1);
    const ModelObject &lo = *loaded.objects.front();
    REQUIRE(lo.volumes.front()->supported_facets.has_facets(*lo.volumes.front(), EnforcerBlockerType::STABILIZER));
    const std::vector<stabilizers::PaintedSpot> after =
        stabilizers::painted_spots(lo, lo.instances.front()->get_matrix(), 15.);
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK((after[i].pos - before[i].pos).norm() < 0.01);
        CHECK(after[i].normal.dot(before[i].normal) > 0.999);
    }
}

// --- the mesh ----------------------------------------------------------------------------------

TEST_CASE("frustum_between is a closed frustum along any axis", "[StabilizerBake]")
{
    const indexed_triangle_set f = stabilizers::frustum_between(Vec3d(1., 2., 3.), 1.5, Vec3d(4., -1., 9.), 0.5, 24);
    CHECK(its_num_open_edges(f) == 0);
    const double h  = (Vec3d(4., -1., 9.) - Vec3d(1., 2., 3.)).norm();
    // A 24-gon frustum: (1/3) h (A1 + A2 + sqrt(A1 A2)) with A = (24/2) r^2 sin(2 pi / 24).
    const double k  = 12. * std::sin(2. * M_PI / 24.);
    const double a1 = k * 1.5 * 1.5, a2 = k * 0.5 * 0.5;
    CHECK_THAT(double(its_volume(f)), WithinAbs(h / 3. * (a1 + a2 + std::sqrt(a1 * a2)), 1e-3));
}

TEST_CASE("Baked stabilizers are one closed mesh that slices like the live struts", "[StabilizerBake]")
{
    auto check = [](const PrintObject &po, const char *label) {
        const stabilizers::StabilizerSettings     st     = stabilizers::settings_of(po);
        const std::vector<stabilizers::Strut>     struts = stabilizers::plan_struts(po);
        const std::vector<stabilizers::LayerOutline> outlines = stabilizers::outlines_of(po);
        REQUIRE_FALSE(struts.empty());

        stabilizers::MeshReport rep;
        const indexed_triangle_set mesh = stabilizers::stabilizer_mesh(struts, st, {}, &rep);
        INFO(label << ": " << struts.size() << " struts, " << rep.pillars << " pillars, " << rep.triangles << " triangles, volume "
                   << rep.volume << " mm3");
        CHECK(rep.unioned);
        CHECK(rep.closed);
        CHECK(its_num_open_edges(mesh) == 0);
        CHECK(rep.volume > 0.);
        // Light: a few hundred triangles per strut, not the hundreds of thousands of stacked slices.
        CHECK(rep.triangles < 20000);
        // Each shell closed on its own too: the fallback when a union fails.
        for (const indexed_triangle_set &s : stabilizers::stabilizer_shells(struts, st))
            CHECK(its_num_open_edges(s) == 0);

        const Parity p = parity(outlines, st, struts, mesh);
        WARN(label << ": " << struts.size() << " struts, " << rep.pillars << " pillars, " << rep.triangles << " triangles; parity over "
                   << p.layers << " layers: live " << p.live << " mm2, baked " << p.baked << " mm2, symmetric difference "
                   << p.xor_area << " mm2 (" << 100. * p.xor_area / p.live << " %), worst layer " << 100. * p.worst_rel
                   << " % at z=" << p.worst_z << ", worst IoU " << p.worst_iou << ", baked inside the part " << p.in_part << " mm2");
        // Measured 2026-10-01: 0.02 % (pin) and 0.03 % (project) over all layers; the worst single layer,
        // the one under a tip, about 2.5 % (IoU 0.975).
        CHECK(p.xor_area < 0.005 * p.live);
        CHECK(p.worst_iou > 0.95);
        CHECK(p.in_part < 0.01 * p.live);
    };

    SECTION("the 6 x 60 mm pin")
    {
        Bench b;
        add_pin(b.model);
        slice(b, pin_config("auto"));
        check(*b.print.objects().front(), "pin");
    }
    SECTION("the owner's project")
    {
        Bench b;
        REQUIRE(slice_project(b));
        check(*b.print.objects().front(), "project");
    }
}

// --- into the model --------------------------------------------------------------------------------

TEST_CASE("Bake stabilizers as a separate object", "[StabilizerBake]")
{
    // A rotated, stretched pin as well: the bake must land where its live struts stand.
    for (const auto &[rot, scale] : std::vector<std::pair<double, Vec3d>>{ { 0., Vec3d::Ones() }, { 0.5, Vec3d(1.5, 1., 1.) } }) {
        DYNAMIC_SECTION("rotation " << rot << ", x scale " << scale.x())
        {
            Bench        b;
            ModelObject *src = add_pin(b.model, rot, scale);
            src->instances.front()->set_offset(Vec3d(40., 30., src->instances.front()->get_offset().z()));
            const DynamicPrintConfig config = pin_config("auto");
            slice(b, config);
            const PrintObject &po = *b.print.objects().front();

            StabilizerBakeOptions opts = stabilizer_bake_defaults(po.config());
            CHECK_THAT(opts.tip_diameter, WithinAbs(1.0, 1e-9));
            const StabilizerBakeResult res = bake_stabilizers(po, opts);
            REQUIRE(res.error.empty());
            CHECK(res.mesh_report.closed);
            CHECK(res.struts.size() == stabilizers::plan_struts(po).size());

            ModelObject *stab = apply_stabilizer_bake(b.model, *src, res, opts);
            REQUIRE(stab != nullptr);
            REQUIRE(b.model.objects.size() == 2);
            CHECK(stab->name == "pin stabilizers");
            CHECK(stab->instances.size() == 1);
            // The source's live stabilizers are off now; the rest of its settings stay.
            CHECK(mode_of(*src) == smOff);
            // The new object prints like support, on default settings or its own.
            CHECK(stab->config.option("enable_support")->getBool() == false);
            CHECK(stab->config.opt_int("wall_loops") == 3);
            CHECK(mode_of(*stab) == smOff);
            CHECK(its_num_open_edges(stab->volumes.front()->mesh().its) == 0);

            // In the world: the tips touch the pin at every ring and never cut into it.
            const indexed_triangle_set part  = world_mesh(*src);
            const indexed_triangle_set baked = world_mesh(*stab);
            for (const stabilizers::Strut &s : res.struts) {
                const float z = float(s.tip_z);
                const ExPolygons pp = slice_mesh_ex(part, { z }).front();
                const ExPolygons bb = slice_mesh_ex(baked, { z }).front();
                INFO("tip at z=" << z);
                CHECK(area_of(intersection_ex(pp, bb)) < 0.01);
                // Touching: the baked tip grown by 0.05 mm overlaps the part.
                CHECK(area_of(intersection_ex(pp, offset_ex(bb, scaled<float>(0.05)))) > 0.);
            }

            // Sliced again: the source prints no stabilizers of its own, the baked object prints its
            // pillars from the first layer.
            Print reprint;
            for (ModelObject *o : b.model.objects)
                reprint.auto_assign_extruders(o);
            reprint.apply(b.model, config);
            reprint.set_status_silent();
            reprint.process();
            REQUIRE(reprint.objects().size() == 2);
            const PrintObject *src_po = nullptr, *stab_po = nullptr;
            for (const PrintObject *p : reprint.objects())
                (p->model_object()->name == "pin" ? src_po : stab_po) = p;
            REQUIRE(src_po != nullptr);
            REQUIRE(stab_po != nullptr);
            CHECK(stabilizer_support_area(*src_po) < 0.01);
            CHECK(stab_po->support_layers().empty());
            REQUIRE_FALSE(stab_po->layers().empty());
            CHECK(area_of(stab_po->layers().front()->lslices) > 1.);
        }
    }
}

TEST_CASE("Bake stabilizers as a part of the object", "[StabilizerBake]")
{
    Bench        b;
    ModelObject *src = add_pin(b.model, 0.3);
    slice(b, pin_config("auto"));
    const PrintObject &po = *b.print.objects().front();

    StabilizerBakeOptions opts = stabilizer_bake_defaults(po.config());
    opts.placement = StabilizerBakePlacement::PartOfObject;
    opts.tip_gap   = 0.;
    const StabilizerBakeResult res = bake_stabilizers(po, opts);
    REQUIRE(res.error.empty());
    // Parts of one object fuse where they touch: the tips stop short.
    CHECK(res.tip_gap >= STABILIZER_BAKE_PART_MIN_GAP - 1e-9);

    // The same stabilizers baked as an object, for comparison in the world.
    StabilizerBakeOptions sep = opts;
    sep.placement = StabilizerBakePlacement::SeparateObject;
    sep.tip_gap   = res.tip_gap;
    const StabilizerBakeResult res_sep = bake_stabilizers(po, sep);
    REQUIRE(res_sep.error.empty());

    REQUIRE(apply_stabilizer_bake(b.model, *src, res, opts) == src);
    REQUIRE(b.model.objects.size() == 1);
    REQUIRE(src->volumes.size() == 2);
    const ModelVolume *part = src->volumes.back();
    CHECK(part->is_model_part());
    CHECK(part->name == "Stabilizers");
    CHECK(part->config.opt_int("wall_loops") == 3);
    CHECK(mode_of(*src) == smOff);

    // World placement: the part sits where the separate object would.
    indexed_triangle_set part_world = part->mesh().its;
    its_transform(part_world, src->instances.front()->get_matrix() * part->get_matrix(), true);
    indexed_triangle_set sep_world = res_sep.mesh;
    for (Vec3f &v : sep_world.vertices)
        v += res_sep.instance_offsets.front().cast<float>();
    const BoundingBoxf3 a = bounding_box(part_world), c = bounding_box(sep_world);
    CHECK((a.min - c.min).norm() < 0.01);
    CHECK((a.max - c.max).norm() < 0.01);
}

TEST_CASE("Baked stabilizers survive a 3MF round trip", "[StabilizerBake][3mf]")
{
    SECTION("as a separate object")
    {
        Bench        b;
        ModelObject *src = add_pin(b.model);
        slice(b, pin_config("auto"));
        const StabilizerBakeOptions opts = stabilizer_bake_defaults(b.print.objects().front()->config());
        const StabilizerBakeResult  res  = bake_stabilizers(*b.print.objects().front(), opts);
        REQUIRE(apply_stabilizer_bake(b.model, *src, res, opts) != nullptr);
        const size_t triangles = b.model.objects.back()->volumes.front()->mesh().its.indices.size();

        Model loaded;
        REQUIRE(round_trip(b.model, loaded, "stabilizer_bake_object.3mf"));
        REQUIRE(loaded.objects.size() == 2);
        const ModelObject &lsrc  = *loaded.objects[0];
        const ModelObject &lstab = *loaded.objects[1];
        CHECK(lsrc.name == "pin");
        CHECK(lstab.name == "pin stabilizers");
        CHECK(mode_of(lsrc) == smOff);
        CHECK(lstab.config.option("enable_support")->getBool() == false);
        CHECK(lstab.config.opt_int("wall_loops") == 3);
        CHECK(lstab.volumes.front()->mesh().its.indices.size() == triangles);
        CHECK(its_num_open_edges(lstab.volumes.front()->mesh().its) == 0);
        // Where it stood: the loader may recentre the mesh, so compare the world boxes.
        const BoundingBoxf3 before = bounding_box(world_mesh(*b.model.objects.back())), after = bounding_box(world_mesh(lstab));
        CHECK((before.min - after.min).norm() < 1e-3);
        CHECK((before.max - after.max).norm() < 1e-3);
    }
    SECTION("as a part")
    {
        Bench        b;
        ModelObject *src = add_pin(b.model);
        slice(b, pin_config("auto"));
        StabilizerBakeOptions opts = stabilizer_bake_defaults(b.print.objects().front()->config());
        opts.placement             = StabilizerBakePlacement::PartOfObject;
        const StabilizerBakeResult res = bake_stabilizers(*b.print.objects().front(), opts);
        REQUIRE(apply_stabilizer_bake(b.model, *src, res, opts) != nullptr);

        Model loaded;
        REQUIRE(round_trip(b.model, loaded, "stabilizer_bake_part.3mf"));
        REQUIRE(loaded.objects.size() == 1);
        const ModelObject &lo = *loaded.objects.front();
        REQUIRE(lo.volumes.size() == 2);
        CHECK(lo.volumes[1]->is_model_part());
        CHECK(lo.volumes[1]->name == "Stabilizers");
        CHECK(mode_of(lo) == smOff);
    }
}

TEST_CASE("Objects whose side stabilizers a Bambu Studio export drops", "[StabilizerBake]")
{
    Model        model;
    ModelObject *a = add_pin(model);
    ModelObject *c = add_pin(model);
    a->name = "on by preset";
    c->name = "off by object";
    c->config.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smOff));
    DynamicPrintConfig print = pin_config("manual");
    CHECK(objects_with_live_stabilizers(model, print) == std::vector<std::string>{ "on by preset" });
    print.set_key_value("enable_support", new ConfigOptionBool(false));
    CHECK(objects_with_live_stabilizers(model, print).empty());
    print = pin_config("off");
    a->config.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smAuto));
    CHECK(objects_with_live_stabilizers(model, print) == std::vector<std::string>{ "on by preset" });
}
