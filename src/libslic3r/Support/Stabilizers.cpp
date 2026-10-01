#include "Stabilizers.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../SLA/IndexedMesh.hpp"
#include "../SLA/SupportTree.hpp"
#include "../TriangleMeshSlicer.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r {

namespace stabilizers {

// Farthest crossing of the ray `from + t * dir` (t > 0) with the island's outer contour, as t.
// Negative when the ray never crosses it.
static double outermost_crossing(const Polygon &contour, const Vec2d &from, const Vec2d &dir)
{
    double best = -1.;
    const size_t n = contour.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2d a = contour[i].cast<double>();
        const Vec2d b = contour[(i + 1) % n].cast<double>();
        const Vec2d e = b - a;
        const double denom = dir.x() * e.y() - dir.y() * e.x();
        if (std::abs(denom) < EPSILON)
            continue;
        const Vec2d  w = a - from;
        const double t = (w.x() * e.y() - w.y() * e.x()) / denom; // along the ray
        const double s = (w.x() * dir.y() - w.y() * dir.x()) / denom; // along the edge
        if (t > 0. && s >= 0. && s <= 1.)
            best = std::max(best, t);
    }
    return best;
}

sla::SupportPoints ring_points(const std::vector<LayerOutline> &layers, const RingParams &params)
{
    sla::SupportPoints out;
    if (layers.empty() || params.ring_spacing <= EPSILON || params.points_per_ring < 1)
        return out;

    const float  top_z       = layers.back().slice_z;
    const float  tip_r       = float(0.5 * params.tip_diameter);
    const double min_area    = sqr(scaled<double>(params.tip_diameter)) * M_PI;
    const double max_width   = scaled<double>(params.max_island_width);
    const int    n           = params.points_per_ring;
    const double step_angle  = 2. * M_PI / n;

    int ring = 0;
    for (double z = params.ring_spacing; z <= double(top_z) - params.top_margin; z += params.ring_spacing, ++ring) {
        // The layer whose slicing plane is nearest to the ring height.
        auto it = std::lower_bound(layers.begin(), layers.end(), float(z),
                                   [](const LayerOutline &l, float zz) { return l.slice_z < zz; });
        if (it == layers.end())
            it = std::prev(it);
        else if (it != layers.begin() && std::abs(std::prev(it)->slice_z - z) < std::abs(it->slice_z - z))
            it = std::prev(it);
        if (it->islands == nullptr)
            continue;

        // Halfway between this layer's slicing plane and the next one: a pinhead whose tip vertices
        // sit exactly on a slicing plane can slice to nothing there, and that is the layer the
        // touch is on.
        const float  point_z = std::next(it) == layers.end() ? it->slice_z : 0.5f * (it->slice_z + std::next(it)->slice_z);
        const double phase = (ring % 2) ? 0.5 * step_angle : 0.;
        for (const ExPolygon &island : *it->islands) {
            if (island.contour.size() < 3 || std::abs(island.contour.area()) < min_area)
                continue;
            if (max_width > 0.) {
                const BoundingBox bb   = get_extents(island.contour);
                const Point       size = bb.size();
                if (double(std::min(size.x(), size.y())) > max_width)
                    continue;
            }
            const Vec2d c = island.contour.centroid().cast<double>();
            for (int i = 0; i < n; ++i) {
                const double a   = phase + i * step_angle;
                const Vec2d  dir(std::cos(a), std::sin(a));
                const double t   = outermost_crossing(island.contour, c, dir);
                if (t <= 0.)
                    continue;
                const Vec2d p = c + dir * t;
                out.emplace_back(Vec3f(float(unscaled(p.x())), float(unscaled(p.y())), point_z), tip_r);
            }
        }
    }
    return out;
}

// The object's printable parts, in the frame its layers were sliced in (slice_z heights).
static indexed_triangle_set object_mesh(const PrintObject &object)
{
    indexed_triangle_set its;
    const Transform3d trafo = object.trafo_centered();
    for (const ModelVolume *volume : object.model_object()->volumes)
        if (volume->is_model_part()) {
            indexed_triangle_set vits = volume->mesh().its;
            its_transform(vits, trafo * volume->get_matrix());
            its_merge(its, vits);
        }
    return its;
}

static sla::SupportTreeConfig tree_config(const PrintObjectConfig &cfg)
{
    sla::SupportTreeConfig tc;
    const double tip_r    = 0.5 * cfg.stabilizer_tip_diameter.value;
    const double pillar_r = std::max(tip_r, 0.5 * cfg.stabilizer_pillar_diameter.value);
    tc.head_front_radius_mm    = tip_r;
    tc.head_back_radius_mm     = pillar_r;
    tc.head_fallback_radius_mm = std::max(tip_r, 0.6 * pillar_r);
    // Just enough bite to fuse with the wall's outer perimeter; the part of the tip inside the
    // wall is clipped away below, so this only shapes the contact patch.
    tc.head_penetration_mm     = std::min(0.2, tip_r);
    // Length of the slanted arm between tip and pillar: it is what keeps the pillar clear of the
    // wall, so never shorter than the support's own XY gap.
    tc.head_width_mm           = std::max({ 2.0, 2. * pillar_r, cfg.support_object_xy_distance.value + pillar_r });
    // FDM parts sit on the bed: no elevation, pillars stand next to the part.
    tc.object_elevation_mm     = 0.;
    tc.ground_facing_only      = true;
    tc.base_radius_mm          = std::max(2.5, 1.5 * pillar_r);
    tc.base_height_mm          = 1.0;
    tc.pillar_base_safety_distance_mm = std::max(1.0, cfg.support_object_xy_distance.value);
    tc.bridge_slope            = M_PI / 4.;
    tc.max_bridge_length_mm    = 10.;
    tc.max_pillar_link_distance_mm = 10.;
    tc.pillar_connection_mode  = sla::PillarConnectionMode::dynamic;
    return tc;
}

// The support layer at `layer`'s print_z, inserting one when the support generators made none.
static SupportLayer *support_layer_at(PrintObject &object, const Layer &layer, bool &inserted)
{
    SupportLayerPtrs &sls = object.support_layers();
    auto it = std::lower_bound(sls.begin(), sls.end(), layer.print_z - EPSILON,
                               [](const SupportLayer *l, double z) { return l->print_z < z; });
    if (it != sls.end() && std::abs((*it)->print_z - layer.print_z) < EPSILON)
        return *it;
    it = object.insert_support_layer(it, 0, 0, layer.height, layer.print_z, layer.slice_z);
    (*it)->support_type = stInnerTree;
    inserted = true;
    return *it;
}

} // namespace stabilizers

void generate_stabilizer_supports(PrintObject &object, const std::function<void()> &throw_if_canceled)
{
    const PrintObjectConfig &cfg = object.config();
    if (!cfg.stabilizer_supports.value || object.layers().size() < 2)
        return;

    indexed_triangle_set mesh = stabilizers::object_mesh(object);
    if (mesh.empty())
        return;

    std::vector<stabilizers::LayerOutline> outlines;
    outlines.reserve(object.layers().size());
    for (const Layer *layer : object.layers())
        outlines.push_back({ float(layer->slice_z), &layer->lslices });

    stabilizers::RingParams rp;
    rp.ring_spacing     = cfg.stabilizer_ring_spacing.value;
    rp.points_per_ring  = cfg.stabilizer_points_per_ring.value;
    rp.tip_diameter     = cfg.stabilizer_tip_diameter.value;
    rp.max_island_width = cfg.stabilizer_max_island_width.value;
    sla::SupportPoints points = stabilizers::ring_points(outlines, rp);
    if (points.empty())
        return;

    // Snap every point onto the mesh: the layer outline is the slice polygon, which can sit a few
    // microns off the triangles, and the pinhead search starts from the surface.
    sla::IndexedMesh imesh(mesh);
    for (sla::SupportPoint &sp : points) {
        int   idx = -1;
        Vec3d closest;
        imesh.squared_distance(sp.pos.cast<double>(), idx, closest);
        if (idx >= 0)
            sp.pos = closest.cast<float>();
    }
    throw_if_canceled();

    sla::JobController ctl;
    ctl.cancelfn = throw_if_canceled;
    sla::SupportableMesh       sm(imesh, points, stabilizers::tree_config(cfg));
    sla::SupportTree::UPtr     tree = sla::SupportTree::create(sm, ctl);
    const indexed_triangle_set &tree_mesh = tree->retrieve_mesh(sla::MeshType::Support);
    BOOST_LOG_TRIVIAL(debug) << "Stabilizers: " << points.size() << " contact points, tree mesh with "
                             << tree_mesh.indices.size() << " triangles";
    if (tree_mesh.empty())
        return;
    throw_if_canceled();

    std::vector<ExPolygons> slices = slice_mesh_ex(tree_mesh, zs_from_layers(object.layers()), float(SCALED_EPSILON), throw_if_canceled);

    // Specks left by clipping at the wall, too small to print a line into.
    const double min_island_area = sqr(scaled<double>(0.2));

    bool inserted = false;
    for (size_t i = 0; i < object.layers().size() && i < slices.size(); ++i) {
        const Layer &layer = *object.layers()[i];
        // Touch, don't fuse: clip the struts at the part's outline, so the tip's footprint ends
        // exactly where the outer wall begins.
        ExPolygons islands = diff_ex(slices[i], layer.lslices);
        if (islands.empty())
            continue;

        const Flow flow = i == 0 ? support_material_1st_layer_flow(&object, float(layer.height)) :
                                   support_material_flow(&object, float(layer.height));
        const float half_w  = 0.5f * float(flow.scaled_width());
        const float spacing = float(flow.scaled_spacing());

        auto *coll = new ExtrusionEntityCollection();
        for (const ExPolygon &island : islands) {
            if (island.area() < min_island_area)
                continue;
            // Solid concentric loops, outside in: a pillar a few lines wide is all wall. An island
            // thinner than one line (the very tip) is traced along its own outline instead, which
            // is what makes the contact.
            Polygons loop = offset(island, -half_w);
            if (loop.empty())
                loop = to_polygons(island);
            while (!loop.empty()) {
                Polygons next = offset(loop, -spacing);
                extrusion_entities_append_loops(coll->entities, std::move(loop), erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
                loop = std::move(next);
            }
        }
        if (coll->entities.empty()) {
            delete coll;
            continue;
        }

        SupportLayer *sl = stabilizers::support_layer_at(object, layer, inserted);
        sl->support_fills.entities.push_back(coll);
        expolygons_append(sl->support_islands, islands);
        if (sl->support_type == stInnerTree)
            expolygons_append(sl->lslices, islands);
    }

    if (inserted) {
        // Keep ids equal to positions and the neighbour links intact after the insertions.
        SupportLayerPtrs &sls = object.support_layers();
        for (size_t i = 0; i < sls.size(); ++i) {
            sls[i]->set_id(i);
            sls[i]->lower_layer = i > 0 ? sls[i - 1] : nullptr;
            sls[i]->upper_layer = i + 1 < sls.size() ? sls[i + 1] : nullptr;
        }
    }
}

} // namespace Slic3r
