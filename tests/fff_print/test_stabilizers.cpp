// Side stabilizers (libslic3r/Support/Stabilizers.hpp): rings of pinpoint struts on the sides of a
// tall, thin part, on pillars that stand on the bed, printed as FDM support.
//
// The ring placer on its own (where the touch points land), then whole slices of a thin pin and of
// the owner's repro project, which are the real gate: the struts must reach the part's wall at the
// ring heights and touch it without overlapping it, everything else must be thin pillars standing
// clear of the part (not a block around it), and every layer must rest on the one below.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/Stabilizers.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Distance from p to the nearest edge of the polygon, mm.
double distance_to_contour(const Polygon &poly, const Vec2d &p)
{
    double best = std::numeric_limits<double>::max();
    for (const Line &l : poly.lines())
        best = std::min(best, unscaled(l.distance_to(Point(scaled(p.x()), scaled(p.y())))));
    return best;
}

ExPolygons square(double size)
{
    const coord_t h = scaled(0.5 * size);
    return { ExPolygon(Polygon({ { -h, -h }, { h, -h }, { h, h }, { -h, h } })) };
}

double total_area(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return unscaled(unscaled(a));
}

struct PinPrint
{
    Print print;
    Model model;
};

// A 6 mm wide, 60 mm tall pin standing on the bed, with supports on and nothing to support - the
// stabilizers are the only thing that can put support next to it.
void slice_pin(PinPrint &p, bool stabilizers, const char *tip_gap = "0")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              "1" },
        { "support_type",                "normal(auto)" },
        { "support_on_build_plate_only", "1" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "stabilizer_supports",         stabilizers ? "1" : "0" },
        { "stabilizer_ring_spacing",     "15" },
        { "stabilizer_points_per_ring",  "3" },
        { "stabilizer_tip_gap",          tip_gap },
    });
    ModelObject *object = p.model.add_object();
    object->name = "pin";
    object->add_volume(TriangleMesh(its_make_cylinder(3., 60., M_PI / 90.)));
    object->add_instance();
    object->ensure_on_bed();
    p.print.auto_assign_extruders(object);
    p.print.apply(p.model, config);
    p.print.set_status_silent();
    p.print.process();
}

// Area of the stabilizer islands on the support layer at `print_z`, and how close they come to
// the part's outline on the object layer at the same height.
struct LayerCheck
{
    double area      = 0.;  // mm^2
    double overlap   = 0.;  // mm^2 of support inside the part
    double min_gap   = std::numeric_limits<double>::max(); // mm, island edge to part outline
    size_t paths     = 0;   // extrusion entities on that support layer
};

LayerCheck check_layer(const PrintObject &po, double print_z)
{
    LayerCheck out;
    const Layer *layer = nullptr;
    for (const Layer *l : po.layers())
        if (std::abs(l->print_z - print_z) < 0.11) { layer = l; break; }
    REQUIRE(layer != nullptr);
    for (const SupportLayer *sl : po.support_layers()) {
        if (std::abs(sl->print_z - layer->print_z) > EPSILON)
            continue;
        for (const ExPolygon &island : sl->support_islands) {
            out.area += unscaled(unscaled(island.area()));
            for (const Point &pt : island.contour.points)
                for (const ExPolygon &part : layer->lslices)
                    out.min_gap = std::min(out.min_gap, distance_to_contour(part.contour, unscaled(pt)));
        }
        out.paths += sl->support_fills.entities.size();
        out.overlap += total_area(intersection_ex(sl->support_islands, layer->lslices));
    }
    return out;
}

// The owner's repro project (tests/data/stabilizers/stabilizer_v1.3mf): a 6 x 60 mm cylinder on a
// Snapmaker U1 with 0.12 mm layers, tree(auto) supports, and the stabilizers switched on for the
// object with a 3 mm pillar and a 0.2 mm tip gap.
struct ProjectPrint
{
    Print print;
    Model model;
};

bool slice_project(ProjectPrint &p)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/stabilizers/stabilizer_v1.3mf";
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl = false;
    Semver                    file_version;
    const bool ok = load_bbs_3mf(path.c_str(), &config, &ctxt, &p.model, &plate_data, &project_presets, &is_bbl,
                                 &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plate_data);
    if (!ok || p.model.objects.empty())
        return false;
    config.normalize_fdm();
    p.print.auto_assign_extruders(p.model.objects.front());
    p.print.apply(p.model, config);
    p.print.set_status_silent();
    p.print.process();
    return !p.print.objects().empty();
}

indexed_triangle_set part_mesh(const PrintObject &po)
{
    indexed_triangle_set its;
    for (const ModelVolume *v : po.model_object()->volumes)
        if (v->is_model_part()) {
            indexed_triangle_set vits = v->mesh().its;
            its_transform(vits, po.trafo_centered() * v->get_matrix());
            its_merge(its, vits);
        }
    return its;
}

// The stabilizers as printed: each layer's area times its height, mm^3.
double printed_volume(const PrintObject &po, const std::vector<ExPolygons> &slices)
{
    double v = 0.;
    for (size_t i = 0; i < slices.size() && i < po.layers().size(); ++i)
        v += total_area(slices[i]) * po.layers()[i]->height;
    return v;
}

// The stabilizer islands of every object layer, as they went into the support layers.
std::vector<ExPolygons> printed_stabilizers(const PrintObject &po)
{
    std::vector<ExPolygons> out(po.layers().size());
    for (size_t i = 0; i < po.layers().size(); ++i)
        for (const SupportLayer *sl : po.support_layers())
            if (std::abs(sl->print_z - po.layers()[i]->print_z) < EPSILON)
                expolygons_append(out[i], sl->support_islands);
    return out;
}

struct Printability
{
    size_t islands           = 0;
    size_t floating          = 0;  // islands that do not touch the layer below at all
    double worst_unsupported = 0.; // mm^2: the largest piece of an island beyond half a line of the layer below
    double worst_z           = 0.;
};

// FDM can only print a layer on top of the one below it. Every island of layer i (i > 0) must
// overlap layer i-1, and no part of it may reach further out than half a support line from it -
// that is what a 45 degree climb at 0.2 mm layers already stays well inside of.
Printability printability(const PrintObject &po, const std::vector<ExPolygons> &slices)
{
    Printability out;
    const float  half_w   = 0.5f * float(support_material_flow(&po).scaled_width());
    const double min_area = 0.04; // the generator drops specks below this, mm^2
    for (size_t i = 1; i < slices.size(); ++i) {
        const Polygons below = offset(to_polygons(slices[i - 1]), half_w);
        for (const ExPolygon &island : slices[i]) {
            if (unscaled(unscaled(island.area())) < min_area)
                continue;
            ++out.islands;
            if (intersection_ex(ExPolygons{ island }, slices[i - 1]).empty())
                ++out.floating;
            const double un = total_area(diff_ex(ExPolygons{ island }, below));
            if (un > out.worst_unsupported) {
                out.worst_unsupported = un;
                out.worst_z           = po.layers()[i]->print_z;
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("Stabilizer rings land on the outline, ring by ring", "[Stabilizers]")
{
    // 0.2 mm layers up to 40 mm, the same 6 mm square on every one.
    const ExPolygons pin = square(6.);
    std::vector<stabilizers::LayerOutline> layers;
    for (int i = 1; i <= 200; ++i)
        layers.push_back({ float(0.2 * i - 0.1), &pin });

    stabilizers::RingParams rp;
    rp.ring_spacing    = 10.;
    rp.points_per_ring = 4;
    const sla::SupportPoints pts = stabilizers::ring_points(layers, rp);

    // Rings at 10, 20 and 30 mm; 40 is inside the top margin.
    REQUIRE(pts.size() == 12);
    for (size_t i = 0; i < pts.size(); ++i) {
        const sla::SupportPoint &sp = pts[i];
        CHECK_THAT(double(sp.pos.z()), WithinAbs(10. * double(i / 4 + 1), 0.21));
        CHECK(distance_to_contour(pin.front().contour, sp.pos.head<2>().cast<double>()) < 0.01);
        CHECK_THAT(double(sp.head_front_radius), WithinAbs(0.5 * rp.tip_diameter, 1e-6));
    }
    // Every ring uses the same angles, so the rings' struts share pillars: the first point of each
    // ring is on the same face centre.
    for (size_t ring = 0; ring < 3; ++ring) {
        CHECK_THAT(double(pts[4 * ring].pos.x()), WithinAbs(3., 1e-3));
        CHECK_THAT(double(pts[4 * ring].pos.y()), WithinAbs(0., 1e-3));
    }
    // ...and each contact knows which way is out.
    for (const stabilizers::Contact &c : stabilizers::ring_contacts(layers, rp)) {
        CHECK_THAT(c.dir.norm(), WithinAbs(1., 1e-9));
        CHECK(c.dir.dot(c.pos) > 0.);
    }
}

TEST_CASE("Stabilizers skip islands wider than the limit", "[Stabilizers]")
{
    const ExPolygons wide = square(30.);
    std::vector<stabilizers::LayerOutline> layers;
    for (int i = 1; i <= 200; ++i)
        layers.push_back({ float(0.2 * i - 0.1), &wide });

    stabilizers::RingParams rp;
    rp.ring_spacing     = 10.;
    rp.max_island_width = 20.;
    CHECK(stabilizers::ring_points(layers, rp).empty());
    rp.max_island_width = 0.;
    CHECK_FALSE(stabilizers::ring_points(layers, rp).empty());
}

TEST_CASE("Stabilizers touch a thin pin at the ring heights and stand on the bed", "[Stabilizers]")
{
    SECTION("off: a pin with nothing to support gets no support at all")
    {
        PinPrint p;
        slice_pin(p, false);
        const PrintObject &po = *p.print.objects().front();
        double total = 0.;
        for (const SupportLayer *sl : po.support_layers())
            total += total_area(sl->support_islands);
        CHECK(total < 0.01);
    }

    SECTION("on: struts reach the wall at each ring and do not cut into it")
    {
        PinPrint p;
        slice_pin(p, true);
        const PrintObject &po = *p.print.objects().front();
        REQUIRE_FALSE(po.support_layers().empty());

        // Support layers stay sorted, and every one of them has extrusions or is a pre-existing
        // empty layer of the regular generator.
        for (size_t i = 1; i < po.support_layers().size(); ++i)
            CHECK(po.support_layers()[i - 1]->print_z < po.support_layers()[i]->print_z);

        // First layer: pillar feet on the bed.
        CHECK(check_layer(po, 0.2).area > 1.);


        for (double ring_z : { 15., 30., 45. }) {
            const LayerCheck at = check_layer(po, ring_z);
            INFO("ring at " << ring_z << " mm: area " << at.area << ", gap " << at.min_gap << ", overlap " << at.overlap);
            CHECK(at.area > 0.1);
            // ...and it is actually printed, not just outlined.
            CHECK(at.paths > 0);
            // The tip ends at the wall: it touches (no visible gap) without printing into it.
            CHECK(at.min_gap < 0.05);
            CHECK(at.overlap < 0.01);
        }
    }

    SECTION("with a tip gap: struts stop short of the wall by the gap")
    {
        PinPrint p;
        slice_pin(p, true, "0.3");
        const PrintObject &po = *p.print.objects().front();
        for (double ring_z : { 15., 30., 45. }) {
            const LayerCheck at = check_layer(po, ring_z);
            INFO("ring at " << ring_z << " mm: area " << at.area << ", gap " << at.min_gap);
            CHECK(at.area > 0.1);
            CHECK(at.min_gap > 0.25);
            CHECK(at.min_gap < 0.4);
        }
    }
}


// What the owner saw in the first hand test: the SLA tree builder's cross-braced pillar cage,
// sliced, printed as a thick faceted wall around the lower two thirds of the pin, with ledges where
// each ring's pillars ended. A stabilizer is a few thin pillars and a pinpoint strut per ring;
// everything else must stay off the part.
TEST_CASE("Stabilizers are thin pillars, not a block around the part", "[Stabilizers]")
{
    auto check = [](const PrintObject &po, const char *label, size_t pillars, double max_volume_ratio) {
        const std::vector<ExPolygons> stab = printed_stabilizers(po);
        const double r      = stabilizers::pillar_radius(po);
        const double pillar = M_PI * r * r;
        const Point  centre = get_extents(po.layers()[po.layers().size() / 2]->lslices).center();

        for (size_t i = 0; i < stab.size(); ++i)
            for (const ExPolygon &island : stab[i]) {
                INFO(label << " z=" << po.layers()[i]->print_z << " island of " << unscaled(unscaled(island.area())) << " mm2");
                // No enclosure: nothing wraps around the part.
                CHECK_FALSE(island.contour.contains(centre));
                // One pillar with its foot, or one pillar with the root of a strut, at most.
                CHECK(unscaled(unscaled(island.area())) < 2.5 * pillar + 4.);
            }

        // Between rings, away from any strut (5 mm, and 20 mm: above the first ring's struts, below
        // the second ring's): just the pillars, each a separate circle of the pillar's size.
        for (double z : { 5., 20. }) {
            size_t i = 0;
            while (i + 1 < po.layers().size() && po.layers()[i]->print_z < z)
                ++i;
            const ExPolygons &s = stab[i];
            INFO(label << " z=" << po.layers()[i]->print_z << ": " << s.size() << " islands, " << total_area(s) << " mm2");
            CHECK(s.size() == pillars);
            CHECK(total_area(s) > 0.85 * pillars * pillar);
            CHECK(total_area(s) < 1.15 * pillars * pillar);
            // ...standing clear of the wall.
            for (const ExPolygon &island : s)
                for (const Point &pt : island.contour.points)
                    for (const ExPolygon &part : po.layers()[i]->lslices)
                        CHECK(distance_to_contour(part.contour, unscaled(pt)) > 0.5);
        }

        // Light: a fraction of the part, not a multiple of it.
        const double ratio = printed_volume(po, stab) / its_volume(part_mesh(po));
        INFO(label << ": stabilizer / part volume " << ratio);
        CHECK(ratio > 0.05);
        CHECK(ratio < max_volume_ratio);
    };

    SECTION("the 6 x 60 mm pin")
    {
        PinPrint p;
        slice_pin(p, true);
        check(*p.print.objects().front(), "pin", 3, 0.4);
    }
    SECTION("the owner's project")
    {
        ProjectPrint p;
        REQUIRE(slice_project(p));
        check(*p.print.objects().front(), "project", 3, 0.75);
    }
}

// FDM lays every layer on the one below. A stabilizer island with nothing under it, or reaching out
// further than half a line past what is under it, prints in the air.
TEST_CASE("Stabilizers print layer on layer", "[Stabilizers]")
{
    auto check = [](const PrintObject &po, const char *label) {
        const std::vector<ExPolygons> stab = printed_stabilizers(po);
        // Something stands on the bed.
        CHECK(total_area(stab.front()) > 1.);
        const Printability pr = printability(po, stab);
        INFO(label << ": " << pr.islands << " islands, " << pr.floating << " floating, worst unsupported "
                   << pr.worst_unsupported << " mm2 at z=" << pr.worst_z);
        CHECK(pr.islands > 100);
        CHECK(pr.floating == 0);
        CHECK(pr.worst_unsupported < 0.02);
    };

    SECTION("the 6 x 60 mm pin")
    {
        PinPrint p;
        slice_pin(p, true);
        check(*p.print.objects().front(), "pin");
    }
    SECTION("the owner's project")
    {
        ProjectPrint p;
        REQUIRE(slice_project(p));
        check(*p.print.objects().front(), "project");
    }
}

// The project as the owner set it up: rings at 15, 30 and 45 mm, a 0.2 mm tip gap, 3 mm pillars.
// Each ring has its tips there, stopping the gap short of the wall, and touches nothing else.
TEST_CASE("Stabilizers on the owner's project reach every ring", "[Stabilizers]")
{
    ProjectPrint p;
    REQUIRE(slice_project(p));
    const PrintObject &po = *p.print.objects().front();
    REQUIRE(po.config().stabilizer_supports.value);
    CHECK_THAT(stabilizers::pillar_radius(po), WithinAbs(1.5, 1e-6));
    CHECK(stabilizers::plan_struts(po).size() == 9);

    const std::vector<ExPolygons> stab = printed_stabilizers(po);
    for (double ring_z : { 15., 30., 45. }) {
        const LayerCheck at = check_layer(po, ring_z);
        INFO("ring at " << ring_z << " mm: area " << at.area << ", gap " << at.min_gap << ", overlap " << at.overlap);
        CHECK(at.area > 0.1);
        CHECK(at.paths > 0);
        CHECK(at.min_gap > 0.15);
        CHECK(at.min_gap < 0.3);
        CHECK(at.overlap < 0.01);
    }
    // Nothing of the stabilizers is ever inside the part.
    for (size_t i = 0; i < stab.size(); ++i)
        CHECK(total_area(intersection_ex(stab[i], po.layers()[i]->lslices)) < 0.01);
}

// ---------------------------------------------------------------------------------------------
// Report (hidden): numbers and pictures for a person. Run with
//   fff_print_tests "[.stabilizers_report]"
// and STAB_PREVIEW_DIR=<dir> to also write SVG previews: side.svg (the layers seen from the front
// and from the side) and top_z*.svg (single layers from above: part orange, stabilizer green, the
// layer below in grey).
// ---------------------------------------------------------------------------------------------
namespace {

void write_side_view(const std::string &path, const PrintObject &po, const std::vector<ExPolygons> &slices)
{
    BoundingBox bb;
    for (size_t i = 0; i < slices.size(); ++i) {
        for (const ExPolygon &e : slices[i])
            bb.merge(get_extents(e.contour));
        for (const ExPolygon &e : po.layers()[i]->lslices)
            bb.merge(get_extents(e.contour));
    }
    const double s = 8., pad = 4.; // px per mm, margin mm
    const double top = po.layers().back()->print_z;
    const double w = unscaled(std::max(bb.size().x(), bb.size().y())) + 2 * pad, h = top + 2 * pad;
    std::ofstream f(path);
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << (2 * w + 2) * s << "' height='" << (h + 4) * s << "'>\n";
    f << "<rect width='100%' height='100%' fill='white'/>\n";
    for (int view = 0; view < 2; ++view) {
        const double x0 = view * (w + 2);
        auto X = [&](coord_t v) { return (x0 + pad + unscaled(v - (view == 0 ? bb.min.x() : bb.min.y()))) * s; };
        auto Z = [&](double z) { return (h - pad - z) * s; };
        auto bar = [&](const ExPolygons &ex, double z, double height, const char *fill) {
            for (const ExPolygon &e : ex) {
                const BoundingBox b = get_extents(e.contour);
                const coord_t lo = view == 0 ? b.min.x() : b.min.y(), hi = view == 0 ? b.max.x() : b.max.y();
                f << "<rect fill='" << fill << "' x='" << X(lo) << "' y='" << Z(z) << "' width='" << (X(hi) - X(lo))
                  << "' height='" << height * s << "'/>\n";
            }
        };
        for (size_t i = 0; i < slices.size(); ++i) {
            const Layer *l = po.layers()[i];
            bar(l->lslices, l->print_z, l->height, "#f0a060");
            bar(slices[i], l->print_z, l->height, "#20a040");
        }
        f << "<text x='" << (x0 + pad) * s << "' y='" << (h + 2) * s << "' font-size='14'>" << (view == 0 ? "front (X)" : "side (Y)")
          << "</text>\n";
    }
    f << "</svg>\n";
}

void write_top_views(const std::string &dir, const PrintObject &po, const std::vector<ExPolygons> &slices,
                     const std::vector<double> &zs)
{
    BoundingBox bb;
    for (const ExPolygons &s : slices)
        for (const ExPolygon &e : s)
            bb.merge(get_extents(e.contour));
    for (const Layer *l : po.layers())
        for (const ExPolygon &e : l->lslices)
            bb.merge(get_extents(e.contour));
    for (double z : zs) {
        size_t best = 0;
        for (size_t i = 0; i < po.layers().size(); ++i)
            if (std::abs(po.layers()[i]->print_z - z) < std::abs(po.layers()[best]->print_z - z))
                best = i;
        char name[64];
        snprintf(name, sizeof(name), "/top_z%05.2f.svg", po.layers()[best]->print_z);
        SVG svg(dir + name, bb, scaled(2.));
        svg.draw(po.layers()[best]->lslices, "#f08020", 0.6f);
        if (best > 0)
            svg.draw(slices[best - 1], "#888888", 0.5f);
        svg.draw(slices[best], "#20a040", 0.7f);
        svg.Close();
    }
}

void report(const char *label, Print &print, const char *subdir)
{
    const PrintObject &po = *print.objects().front();
    const std::vector<stabilizers::Strut> struts = stabilizers::plan_struts(po);
    const std::vector<ExPolygons> slices = printed_stabilizers(po);
    const double vpart = its_volume(part_mesh(po));
    const double vprinted = printed_volume(po, slices);
    WARN(label << ": " << struts.size() << " struts, pillar diameter " << 2. * stabilizers::pillar_radius(po)
               << " mm, printed stabilizer volume " << vprinted << " mm3, part " << vpart << " mm3, ratio "
               << vprinted / vpart);
    std::string per_layer;
    for (size_t i = 0; i < slices.size(); i += std::max<size_t>(1, slices.size() / 40)) {
        const BoundingBox b = slices[i].empty() ? BoundingBox() : get_extents(slices[i]);
        char line[256];
        snprintf(line, sizeof(line), "z=%6.2f islands=%2zu area=%8.2f bbox=%6.1fx%6.1f\n", po.layers()[i]->print_z,
                 slices[i].size(), total_area(slices[i]), slices[i].empty() ? 0. : unscaled(b.size().x()),
                 slices[i].empty() ? 0. : unscaled(b.size().y()));
        per_layer += line;
    }
    WARN(label << " per layer:\n" << per_layer);
    const Printability pr = printability(po, slices);
    WARN(label << ": " << pr.islands << " islands checked, " << pr.floating << " floating, worst unsupported "
               << pr.worst_unsupported << " mm2 at z=" << pr.worst_z);

    if (const char *dir = std::getenv("STAB_PREVIEW_DIR")) {
        const std::string out = std::string(dir) + "/" + subdir;
        boost::filesystem::create_directories(out);
        write_side_view(out + "/side.svg", po, slices);
        write_top_views(out, po, slices, { 0.2, 5., 13., 14., 15., 20., 30., 45. });
    }
}

} // namespace

TEST_CASE("Stabilizer report: pin and the owner's project", "[.stabilizers_report]")
{
    {
        PinPrint p;
        slice_pin(p, true);
        report("PIN", p.print, "pin");
    }
    {
        ProjectPrint p;
        REQUIRE(slice_project(p));
        report("PROJECT", p.print, "project");
    }
}
