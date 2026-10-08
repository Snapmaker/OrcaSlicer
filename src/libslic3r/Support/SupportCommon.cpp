#include "../ClipperUtils.hpp"
// #include "../ClipperZUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../Fill/FillBase.hpp"
#include "../MutablePolygon.hpp"
#include "../Geometry.hpp"
#include "../Point.hpp"
#include "clipper/clipper_z.hpp"

#include <cmath>
#include <boost/container/static_vector.hpp>
// Ultra (support groups): LayerCache::nonempty needs 3 + 2*K entries with K groups; small_vector
// keeps the same inline capacity of 5 (so a single-group object still never allocates) and spills
// to the heap beyond it. See the plan's 5.3 experiment.
#include <boost/container/small_vector.hpp>
#include <boost/log/trivial.hpp>

#include <tbb/parallel_for.h>

#include "SupportCommon.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
    #include "../utils.hpp"
    #include "../SVG.hpp"
#endif

#include <cassert>

namespace Slic3r {

// how much we extend support around the actual contact area
//FIXME this should be dependent on the nozzle diameter!
#define SUPPORT_MATERIAL_MARGIN 1.5

//#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 3.
//#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 1.5
#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

void remove_bridges_from_contacts(
    const PrintConfig   &print_config, 
    const Layer         &lower_layer,
    const LayerRegion   &layerm,
    float                fw, 
    Polygons            &contact_polygons)
{
    // compute the area of bridging perimeters
    Polygons bridges;
    {
        // Surface supporting this layer, expanded by 0.5 * nozzle_diameter, as we consider this kind of overhang to be sufficiently supported.
        Polygons lower_grown_slices = expand(lower_layer.lslices,
            //FIXME to mimic the decision in the perimeter generator, we should use half the external perimeter width.
            0.5f * float(scale_(print_config.nozzle_diameter.get_at(layerm.region().config().wall_filament - 1))),
            SUPPORT_SURFACES_OFFSET_PARAMETERS);
        // Collect perimeters of this layer.
        //FIXME split_at_first_point() could split a bridge mid-way
    #if 0
        Polylines overhang_perimeters = layerm.perimeters.as_polylines();
        // workaround for Clipper bug, see Slic3r::Polygon::clip_as_polyline()
        for (Polyline &polyline : overhang_perimeters)
            polyline.points[0].x += 1;
        // Trim the perimeters of this layer by the lower layer to get the unsupported pieces of perimeters.
        overhang_perimeters = diff_pl(overhang_perimeters, lower_grown_slices);
    #else
        Polylines overhang_perimeters = diff_pl(layerm.perimeters.as_polylines(), lower_grown_slices);
    #endif
        
        // only consider straight overhangs
        // only consider overhangs having endpoints inside layer's slices
        // convert bridging polylines into polygons by inflating them with their thickness
        // since we're dealing with bridges, we can't assume width is larger than spacing,
        // so we take the largest value and also apply safety offset to be ensure no gaps
        // are left in between
        Flow perimeter_bridge_flow = layerm.bridging_flow(frPerimeter);
        //FIXME one may want to use a maximum of bridging flow width and normal flow width, as the perimeters are calculated using the normal flow
        // and then turned to bridging flow, thus their centerlines are derived from non-bridging flow and expanding them by a bridging flow
        // may not expand them to the edge of their respective islands.
        const float w = float(0.5 * std::max(perimeter_bridge_flow.scaled_width(), perimeter_bridge_flow.scaled_spacing())) + scaled<float>(0.001);
        for (Polyline &polyline : overhang_perimeters)
            if (polyline.is_straight()) {
                // This is a bridge 
                polyline.extend_start(fw);
                polyline.extend_end(fw);
                // Is the straight perimeter segment supported at both sides?
                Point pts[2]       = { polyline.first_point(), polyline.last_point() };
                bool  supported[2] = { false, false };
                for (size_t i = 0; i < lower_layer.lslices.size() && ! (supported[0] && supported[1]); ++ i)
                    for (int j = 0; j < 2; ++ j)
                        if (! supported[j] && lower_layer.lslices_bboxes[i].contains(pts[j]) && lower_layer.lslices[i].contains(pts[j]))
                            supported[j] = true;
                if (supported[0] && supported[1])
                    // Offset a polyline into a thick line.
                    polygons_append(bridges, offset(polyline, w));
            }
        bridges = union_(bridges);
    }
    // remove the entire bridges and only support the unsupported edges
    //FIXME the brided regions are already collected as layerm.bridged. Use it?
    for (const Surface &surface : layerm.fill_surfaces.surfaces)
        if (surface.surface_type == stBottomBridge && surface.bridge_angle >= 0.0)
            polygons_append(bridges, surface.expolygon);
    //FIXME add the gap filled areas. Extrude the gaps with a bridge flow?
    // Remove the unsupported ends of the bridges from the bridged areas.
    //FIXME add supports at regular intervals to support long bridges!
    bridges = diff(bridges,
            // Offset unsupported edges into polygons.
            offset(layerm.unsupported_bridge_edges, scale_(SUPPORT_MATERIAL_MARGIN), SUPPORT_SURFACES_OFFSET_PARAMETERS));
    // Remove bridged areas from the supported areas.
    contact_polygons = diff(contact_polygons, bridges, ApplySafetyOffset::Yes);

    #ifdef SLIC3R_DEBUG
        static int iRun = 0;
        SVG::export_expolygons(debug_out_path("support-top-contacts-remove-bridges-run%d.svg", iRun ++),
            { { { union_ex(offset(layerm.unsupported_bridge_edges(), scale_(SUPPORT_MATERIAL_MARGIN), SUPPORT_SURFACES_OFFSET_PARAMETERS)) }, { "unsupported_bridge_edges", "orange", 0.5f } },
              { { union_ex(contact_polygons) },            { "contact_polygons",           "blue",   0.5f } },
              { { union_ex(bridges) },                     { "bridges",                    "red",    "black", "", scaled<coord_t>(0.1f), 0.5f } } });
    #endif /* SLIC3R_DEBUG */
}

// Convert some of the intermediate layers into top/bottom interface layers as well as base interface layers.
std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> generate_interface_layers(
    const PrintObjectConfig           &config,
    const SupportParameters           &support_params,
    const SupportGeneratorLayersPtr   &bottom_contacts,
    const SupportGeneratorLayersPtr   &top_contacts,
    // Input / output, will be merged with output. Only provided for Organic supports.
    SupportGeneratorLayersPtr         &top_interface_layers,
    SupportGeneratorLayersPtr         &top_base_interface_layers,
    // Input, will be trimmed with the newly created interface layers.
    SupportGeneratorLayersPtr         &intermediate_layers,
    SupportGeneratorLayerStorage      &layer_storage)
{
    std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> base_and_interface_layers;

    if (! intermediate_layers.empty() && support_params.has_interfaces()) {
        // For all intermediate layers, collect top contact surfaces, which are not further than support_material_interface_layers.
        BOOST_LOG_TRIVIAL(debug) << "PrintObjectSupportMaterial::generate_interface_layers() in parallel - start";
        const bool                 snug_supports          = support_params.support_style == smsSnug;
        const bool                 smooth_supports        = support_params.support_style != smsGrid;
        SupportGeneratorLayersPtr &interface_layers       = base_and_interface_layers.first;
        SupportGeneratorLayersPtr &base_interface_layers  = base_and_interface_layers.second;

        interface_layers.assign(intermediate_layers.size(), nullptr);
        if (support_params.has_base_interfaces())
            base_interface_layers.assign(intermediate_layers.size(), nullptr);
        const auto smoothing_distance    = support_params.support_material_interface_flow.scaled_spacing() * 1.5;
        const auto minimum_island_radius = support_params.support_material_interface_flow.scaled_spacing() / support_params.interface_density;
        const auto closing_distance      = smoothing_distance; // scaled<float>(config.support_material_closing_radius.value);
        // Insert a new layer into base_interface_layers, if intersection with base exists.
        auto insert_layer = [&layer_storage, smooth_supports, closing_distance, smoothing_distance, minimum_island_radius](
                SupportGeneratorLayer &intermediate_layer, Polygons &bottom, Polygons &&top, SupportGeneratorLayer *top_interface_layer,
                const Polygons *subtract, SupporLayerType type) -> SupportGeneratorLayer* {
            bool has_top_interface = top_interface_layer && ! top_interface_layer->polygons.empty();
            assert(! bottom.empty() || ! top.empty() || has_top_interface);
            // Merge top into bottom, unite them with a safety offset.
            append(bottom, std::move(top));
            // Merge top / bottom interfaces. For snug supports, merge using closing distance and regularize (close concave corners).
            bottom = intersection(
                smooth_supports ?
                    smooth_outward(closing(std::move(bottom), closing_distance + minimum_island_radius, closing_distance, SUPPORT_SURFACES_OFFSET_PARAMETERS), smoothing_distance) :
                    union_safety_offset(std::move(bottom)),
                intermediate_layer.polygons);
            if (has_top_interface) {
                // Don't trim the precomputed Organic supports top interface with base layer
                // as the precomputed top interface likely expands over multiple tree tips.
                bottom = union_(std::move(top_interface_layer->polygons), bottom);
                top_interface_layer->polygons.clear();
            }
            if (! bottom.empty()) {
                //FIXME Remove non-printable tiny islands, let them be printed using the base support.
                //bottom = opening(std::move(bottom), minimum_island_radius);
                if (! bottom.empty()) {
                    SupportGeneratorLayer &layer_new = top_interface_layer ? *top_interface_layer : layer_storage.allocate(type);
                    layer_new.polygons   = std::move(bottom);
                    layer_new.print_z    = intermediate_layer.print_z;
                    layer_new.bottom_z   = intermediate_layer.bottom_z;
                    layer_new.height     = intermediate_layer.height;
                    layer_new.bridging   = intermediate_layer.bridging;
                    // Subtract the interface from the base regions.
                    intermediate_layer.polygons = diff(intermediate_layer.polygons, layer_new.polygons);
                    if (subtract)
                        // Trim the base interface layer with the interface layer.
                        layer_new.polygons = diff(std::move(layer_new.polygons), *subtract);
                    //FIXME filter layer_new.polygons islands by a minimum area?
        //                  $interface_area = [ grep abs($_->area) >= $area_threshold, @$interface_area ];
                    return &layer_new;
                }
            }
            return nullptr;
        };
        tbb::parallel_for(tbb::blocked_range<int>(0, int(intermediate_layers.size())),
            [&bottom_contacts, &top_contacts, &top_interface_layers, &top_base_interface_layers, &intermediate_layers, &insert_layer, &support_params,
             snug_supports, &interface_layers, &base_interface_layers](const tbb::blocked_range<int>& range) {
                // Gather the top / bottom contact layers intersecting with num_interface_layers resp. num_interface_layers_only intermediate layers above / below
                // this intermediate layer.
                // Index of the first top contact layer intersecting the current intermediate layer.
                auto idx_top_contact_first        = -1;
                // Index of the first bottom contact layer intersecting the current intermediate layer.
                auto idx_bottom_contact_first     = -1;
                // Index of the first top interface layer intersecting the current intermediate layer.
                auto idx_top_interface_first      = -1;
                // Index of the first top contact interface layer intersecting the current intermediate layer.
                auto idx_top_base_interface_first = -1;
                auto num_intermediate = int(intermediate_layers.size());
                for (int idx_intermediate_layer = range.begin(); idx_intermediate_layer < range.end(); ++ idx_intermediate_layer) {
                    SupportGeneratorLayer &intermediate_layer = *intermediate_layers[idx_intermediate_layer];
                    Polygons polygons_top_contact_projected_interface;
                    Polygons polygons_top_contact_projected_base;
                    Polygons polygons_bottom_contact_projected_interface;
                    Polygons polygons_bottom_contact_projected_base;
                    if (support_params.num_top_interface_layers > 0) {
                        // Top Z coordinate of a slab, over which we are collecting the top / bottom contact surfaces
                        coordf_t top_z              = intermediate_layers[std::min(num_intermediate - 1, idx_intermediate_layer + int(support_params.num_top_interface_layers) - 1)]->print_z;
                        coordf_t top_inteface_z     = std::numeric_limits<coordf_t>::max();
                        if (support_params.num_top_base_interface_layers > 0)
                            // Some top base interface layers will be generated.
                            top_inteface_z = support_params.num_top_interface_layers_only() == 0 ?
                                // Only base interface layers to generate.
                                - std::numeric_limits<coordf_t>::max() :
                                intermediate_layers[std::min(num_intermediate - 1, idx_intermediate_layer + int(support_params.num_top_interface_layers_only()) - 1)]->print_z;
                        // Move idx_top_contact_first up until above the current print_z.
                        idx_top_contact_first = idx_higher_or_equal(top_contacts, idx_top_contact_first, [&intermediate_layer](const SupportGeneratorLayer *layer){ return layer->print_z >= intermediate_layer.print_z; }); //  - EPSILON
                        // Collect the top contact areas above this intermediate layer, below top_z.
                        for (int idx_top_contact = idx_top_contact_first; idx_top_contact < int(top_contacts.size()); ++ idx_top_contact) {
                            const SupportGeneratorLayer &top_contact_layer = *top_contacts[idx_top_contact];
                            //FIXME maybe this adds one interface layer in excess?
                            if (top_contact_layer.bottom_z - EPSILON > top_z)
                                break;
                            polygons_append(top_contact_layer.bottom_z - EPSILON > top_inteface_z ? polygons_top_contact_projected_base : polygons_top_contact_projected_interface,
                                // For snug supports, project the overhang polygons covering the whole overhang, so that they will merge without a gap with support polygons of the other layers.
                                // For grid supports, merging of support regions will be performed by the projection into grid.
                                snug_supports ? *top_contact_layer.overhang_polygons : top_contact_layer.polygons);
                        }
                    }
                    if (support_params.num_bottom_interface_layers > 0) {
                        // Bottom Z coordinate of a slab, over which we are collecting the top / bottom contact surfaces
                        coordf_t bottom_z           = intermediate_layers[std::max(0, idx_intermediate_layer - int(support_params.num_bottom_interface_layers) + 1)]->bottom_z;
                        coordf_t bottom_interface_z = - std::numeric_limits<coordf_t>::max();
                        if (support_params.num_bottom_base_interface_layers > 0)
                            // Some bottom base interface layers will be generated.
                            bottom_interface_z = support_params.num_bottom_interface_layers_only() == 0 ?
                                // Only base interface layers to generate.
                                std::numeric_limits<coordf_t>::max() :
                                intermediate_layers[std::max(0, idx_intermediate_layer - int(support_params.num_bottom_interface_layers_only()))]->bottom_z;
                        // Move idx_bottom_contact_first up until touching bottom_z.
                        idx_bottom_contact_first = idx_higher_or_equal(bottom_contacts, idx_bottom_contact_first, [bottom_z](const SupportGeneratorLayer *layer){ return layer->print_z >= bottom_z - EPSILON; });
                        // Collect the top contact areas above this intermediate layer, below top_z.
                        for (int idx_bottom_contact = idx_bottom_contact_first; idx_bottom_contact < int(bottom_contacts.size()); ++ idx_bottom_contact) {
                            const SupportGeneratorLayer &bottom_contact_layer = *bottom_contacts[idx_bottom_contact];
                            if (bottom_contact_layer.print_z - EPSILON > intermediate_layer.bottom_z)
                                break;
                            polygons_append(bottom_contact_layer.print_z - EPSILON > bottom_interface_z ? polygons_bottom_contact_projected_interface : polygons_bottom_contact_projected_base, bottom_contact_layer.polygons);
                        }
                    }
                    auto resolve_same_layer = [](SupportGeneratorLayersPtr &layers, int &idx, coordf_t print_z) -> SupportGeneratorLayer* {
                        if (! layers.empty()) {
                            idx = idx_higher_or_equal(layers, idx, [print_z](const SupportGeneratorLayer *layer) { return layer->print_z > print_z - EPSILON; });
                            if (idx < int(layers.size()) && layers[idx]->print_z < print_z + EPSILON)
                                return layers[idx];
                        }
                        return nullptr;
                    };
                    SupportGeneratorLayer *top_interface_layer      = resolve_same_layer(top_interface_layers, idx_top_interface_first, intermediate_layer.print_z);
                    SupportGeneratorLayer *top_base_interface_layer = resolve_same_layer(top_base_interface_layers, idx_top_base_interface_first, intermediate_layer.print_z);
                    SupportGeneratorLayer *interface_layer          = nullptr;
                    if (! polygons_bottom_contact_projected_interface.empty() || ! polygons_top_contact_projected_interface.empty() ||
                        (top_interface_layer && ! top_interface_layer->polygons.empty())) {
                        interface_layer = insert_layer(
                            intermediate_layer, polygons_bottom_contact_projected_interface, std::move(polygons_top_contact_projected_interface), top_interface_layer,
                            nullptr, polygons_top_contact_projected_interface.empty() ? SupporLayerType::BottomInterface : SupporLayerType::TopInterface);
                        interface_layers[idx_intermediate_layer] = interface_layer;
                    }
                    if (! polygons_bottom_contact_projected_base.empty() || ! polygons_top_contact_projected_base.empty() ||
                        (top_base_interface_layer && ! top_base_interface_layer->polygons.empty()))
                        base_interface_layers[idx_intermediate_layer] = insert_layer(
                            intermediate_layer, polygons_bottom_contact_projected_base, std::move(polygons_top_contact_projected_base), top_base_interface_layer,
                            interface_layer ? &interface_layer->polygons : nullptr, SupporLayerType::Base);
                }
            });

        // Compress contact_out, remove the nullptr items.
        // The parallel_for above may not have merged all the interface and base_interface layers
        // generated by the Organic supports code, do it here.
        auto merge_remove_empty = [](SupportGeneratorLayersPtr &in1, SupportGeneratorLayersPtr &in2) {
            auto remove_empty = [](SupportGeneratorLayersPtr &vec) {
                vec.erase(
                    std::remove_if(vec.begin(), vec.end(), [](const SupportGeneratorLayer *ptr) { return ptr == nullptr || ptr->polygons.empty(); }),
                    vec.end());
            };
            remove_empty(in1);
            remove_empty(in2);
            if (in2.empty())
                return std::move(in1);
            else if (in1.empty())
                return std::move(in2);
            else {
                SupportGeneratorLayersPtr out(in1.size() + in2.size(), nullptr);
                std::merge(in1.begin(), in1.end(), in2.begin(), in2.end(), out.begin(), [](auto* l, auto* r) { return l->print_z < r->print_z; });
                return out;
            }
        };
        interface_layers      = merge_remove_empty(interface_layers,      top_interface_layers);
        base_interface_layers = merge_remove_empty(base_interface_layers, top_base_interface_layers);
        BOOST_LOG_TRIVIAL(debug) << "PrintObjectSupportMaterial::generate_interface_layers() in parallel - end";
    }

    return base_and_interface_layers;
}

SupportGeneratorLayersPtr generate_raft_base(
    const PrintObject                 &object,
    const SupportParameters           &support_params,
	const SlicingParameters			  &slicing_params,
    const SupportGeneratorLayersPtr   &top_contacts,
    const SupportGeneratorLayersPtr   &interface_layers,
    const SupportGeneratorLayersPtr   &base_interface_layers,
    const SupportGeneratorLayersPtr   &base_layers,
    SupportGeneratorLayerStorage      &layer_storage)
{
    // If there is brim to be generated, calculate the trimming regions.
    Polygons brim;
    if (object.has_brim()) {
        // The object does not have a raft.
        // Calculate the area covered by the brim.
        const BrimType brim_type       = object.config().brim_type;
        const bool     brim_outer      = brim_type == btOuterOnly || brim_type == btOuterAndInner;
        const bool     brim_inner      = brim_type == btInnerOnly || brim_type == btOuterAndInner;
        // BBS: the pattern of raft and brim are the same, thus the brim can be serpated by support raft.
        const auto     brim_object_gap = scaled<float>(object.config().brim_object_gap.value);
        //const auto     brim_object_gap = scaled<float>(object.config().brim_object_gap.value + object.config().brim_width.value);
        for (const ExPolygon &ex : object.layers().front()->lslices) {
            if (brim_outer && brim_inner)
                polygons_append(brim, offset(ex, brim_object_gap));
            else {
                if (brim_outer)
                    polygons_append(brim, offset(ex.contour, brim_object_gap, ClipperLib::jtRound, float(scale_(0.1))));
                else
                    brim.emplace_back(ex.contour);
                if (brim_inner) {
                    Polygons holes = ex.holes;
                    polygons_reverse(holes);
                    holes = shrink(holes, brim_object_gap, ClipperLib::jtRound, float(scale_(0.1)));
                    polygons_reverse(holes);
                    polygons_append(brim, std::move(holes));
                } else
                    polygons_append(brim, ex.holes);
            }
        }
        brim = union_(brim);
    }

    // How much to inflate the support columns to be stable. This also applies to the 1st layer, if no raft layers are to be printed.
    const float inflate_factor_fine      = float(scale_((slicing_params.raft_layers() > 1) ? 0.5 : EPSILON));
    const float inflate_factor_1st_layer = std::max(0.f, float(scale_(object.config().raft_first_layer_expansion)) - inflate_factor_fine);
    SupportGeneratorLayer       *contacts         = top_contacts         .empty() ? nullptr : top_contacts         .front();
    SupportGeneratorLayer       *interfaces       = interface_layers     .empty() ? nullptr : interface_layers     .front();
    SupportGeneratorLayer       *base_interfaces  = base_interface_layers.empty() ? nullptr : base_interface_layers.front();
    SupportGeneratorLayer       *columns_base     = base_layers          .empty() ? nullptr : base_layers          .front();
    if (contacts != nullptr && contacts->print_z > std::max(slicing_params.first_print_layer_height, slicing_params.raft_contact_top_z) + EPSILON)
        // This is not the raft contact layer.
        contacts = nullptr;
    if (interfaces != nullptr && interfaces->bottom_print_z() > slicing_params.raft_interface_top_z + EPSILON)
        // This is not the raft column base layer.
        interfaces = nullptr;
    if (base_interfaces != nullptr && base_interfaces->bottom_print_z() > slicing_params.raft_interface_top_z + EPSILON)
        // This is not the raft column base layer.
        base_interfaces = nullptr;
    if (columns_base != nullptr && columns_base->bottom_print_z() > slicing_params.raft_interface_top_z + EPSILON)
        // This is not the raft interface layer.
        columns_base = nullptr;

    Polygons interface_polygons;
    if (contacts != nullptr && ! contacts->polygons.empty())
        polygons_append(interface_polygons, expand(contacts->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));
    if (interfaces != nullptr && ! interfaces->polygons.empty())
        polygons_append(interface_polygons, expand(interfaces->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));
    if (base_interfaces != nullptr && ! base_interfaces->polygons.empty())
        polygons_append(interface_polygons, expand(base_interfaces->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));

    // Output vector.
    SupportGeneratorLayersPtr raft_layers;

    if (slicing_params.raft_layers() > 1) {
        Polygons base;
        Polygons columns;
        Polygons first_layer;
        if (columns_base != nullptr) {
            if (columns_base->bottom_print_z() > slicing_params.raft_interface_top_z - EPSILON) {
                // Classic supports with colums above the raft interface.
                base = columns_base->polygons;
                columns = base;
                if (! interface_polygons.empty())
                    // Trim the 1st layer columns with the inflated interface polygons.
                    columns = diff(columns, interface_polygons);
            } else {
                // Organic supports with raft on print bed.
                assert(is_approx(columns_base->print_z, slicing_params.first_print_layer_height));
                first_layer = columns_base->polygons;
            }
        }
        if (! interface_polygons.empty()) {
            // Merge the untrimmed columns base with the expanded raft interface, to be used for the support base and interface.
            base = union_(base, interface_polygons);
        }
        // Do not add the raft contact layer, only add the raft layers below the contact layer.
        // Insert the 1st layer.
        {
            SupportGeneratorLayer &new_layer = layer_storage.allocate(slicing_params.base_raft_layers > 0 ? SupporLayerType::RaftBase : SupporLayerType::RaftInterface);
            raft_layers.push_back(&new_layer);
            new_layer.print_z = slicing_params.first_print_layer_height;
            new_layer.height  = slicing_params.first_print_layer_height;
            new_layer.bottom_z = 0.;
            first_layer = union_(std::move(first_layer), base);
            new_layer.polygons = inflate_factor_1st_layer > 0 ? expand(first_layer, inflate_factor_1st_layer) : first_layer;
        }
        // Insert the base layers.
        for (size_t i = 1; i < slicing_params.base_raft_layers; ++ i) {
            coordf_t print_z = raft_layers.back()->print_z;
            SupportGeneratorLayer &new_layer  = layer_storage.allocate_unguarded(SupporLayerType::RaftBase);
            raft_layers.push_back(&new_layer);
            new_layer.print_z  = print_z + slicing_params.base_raft_layer_height;
            new_layer.height   = slicing_params.base_raft_layer_height;
            new_layer.bottom_z = print_z;
            new_layer.polygons = base;
        }
        // Insert the interface layers.
        for (size_t i = 1; i < slicing_params.interface_raft_layers; ++ i) {
            coordf_t print_z = raft_layers.back()->print_z;
            SupportGeneratorLayer &new_layer = layer_storage.allocate_unguarded(SupporLayerType::RaftInterface);
            raft_layers.push_back(&new_layer);
            new_layer.print_z = print_z + slicing_params.interface_raft_layer_height;
            new_layer.height  = slicing_params.interface_raft_layer_height;
            new_layer.bottom_z = print_z;
            new_layer.polygons = interface_polygons;
            //FIXME misusing contact_polygons for support columns.
            new_layer.contact_polygons = std::make_unique<Polygons>(columns);
        }
    } else {
        if (columns_base != nullptr) {
            // Expand the bases of the support columns in the 1st layer.
            Polygons &raft     = columns_base->polygons;
            Polygons  trimming;
            // BBS: if first layer of support is intersected with object island, it must have the same function as brim unless in nobrim mode.
            // brim_object_gap is changed to 0 by default, it's no longer appropriate to use it to determine the gap of first layer support.
            //if (object.has_brim())
            //    trimming = offset(object.layers().front()->lslices, (float)scale_(object.config().brim_object_gap.value), SUPPORT_SURFACES_OFFSET_PARAMETERS);
            //else
                trimming = offset(object.layers().front()->lslices, (float)scale_(support_params.gap_xy_first_layer), SUPPORT_SURFACES_OFFSET_PARAMETERS);
            if (inflate_factor_1st_layer > SCALED_EPSILON) {
                // Inflate in multiple steps to avoid leaking of the support 1st layer through object walls.
                auto  nsteps = std::max(5, int(ceil(inflate_factor_1st_layer / support_params.first_layer_flow.scaled_width())));
                float step   = inflate_factor_1st_layer / nsteps;
                for (int i = 0; i < nsteps; ++ i)
                    raft = diff(expand(raft, step), trimming);
            } else
                raft = diff(raft, trimming);
            if (! interface_polygons.empty())
                columns_base->polygons = diff(columns_base->polygons, interface_polygons);
        }
        if (! brim.empty()) {
            if (columns_base)
                columns_base->polygons = diff(columns_base->polygons, brim);
            if (contacts)
                contacts->polygons = diff(contacts->polygons, brim);
            if (interfaces)
                interfaces->polygons = diff(interfaces->polygons, brim);
            if (base_interfaces)
                base_interfaces->polygons = diff(base_interfaces->polygons, brim);
        }
    }

    return raft_layers;
}

static inline void fill_expolygon_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygon              &&expolygon,
    Fill                    *filler,
    const FillParams        &fill_params,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow)
{
    Surface surface(stInternal, std::move(expolygon));
    Polylines polylines;
    try {
        assert(!fill_params.use_arachne);
        polylines = filler->fill_surface(&surface, fill_params);
    } catch (InfillFailedException &) {
    }
    extrusion_entities_append_paths(
        dst,
        std::move(polylines),
        role,
        flow.mm3_per_mm(), flow.width(), flow.height());
}

static inline void fill_expolygons_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygons             &&expolygons,
    Fill                    *filler,
    const FillParams        &fill_params,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow)
{
    for (ExPolygon &expoly : expolygons)
        fill_expolygon_generate_paths(dst, std::move(expoly), filler, fill_params, density, role, flow);
}

static inline void fill_expolygons_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygons             &&expolygons,
    Fill                    *filler,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow)
{
    FillParams fill_params;
    fill_params.density     = density;
    fill_params.dont_adjust = true;
    fill_expolygons_generate_paths(dst, std::move(expolygons), filler, fill_params, density, role, flow);
}

static Polylines draw_perimeters(const ExPolygon &expoly, double clip_length)
{
    // Draw the perimeters.
    Polylines polylines;
    polylines.reserve(expoly.holes.size() + 1);
    for (size_t i = 0; i <= expoly.holes.size();  ++ i) {
        Polyline pl(i == 0 ? expoly.contour.points : expoly.holes[i - 1].points);
        pl.points.emplace_back(pl.points.front());
        if (i > 0)
            // It is a hole, reverse it.
            pl.reverse();
        // so that all contours are CCW oriented.
        pl.clip_end(clip_length);
        polylines.emplace_back(std::move(pl));
    }
    return polylines;
}

void tree_supports_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    const Polygons          &polygons,
    const Flow              &flow,
    const SupportParameters &support_params)
{
    // Offset expolygon inside, returns number of expolygons collected (0 or 1).
    // Vertices of output paths are marked with Z = source contour index of the expoly.
    // Vertices at the intersection of source contours are marked with Z = -1.
    auto shrink_expolygon_with_contour_idx = [](const Slic3r::ExPolygon &expoly, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib_Z::Paths &out) -> int
    {
        assert(delta > 0);
        auto append_paths_with_z = [](ClipperLib::Paths &src, coord_t contour_idx, ClipperLib_Z::Paths &dst) {
            dst.reserve(next_highest_power_of_2(dst.size() + src.size()));
            for (const ClipperLib::Path &contour : src) {
                ClipperLib_Z::Path tmp;
                tmp.reserve(contour.size());
                for (const Point &p : contour)
                    tmp.emplace_back(p.x(), p.y(), contour_idx);
                dst.emplace_back(std::move(tmp));
            }
        };

        // 1) Offset the outer contour.
        ClipperLib_Z::Paths contours;
        {
            ClipperLib::ClipperOffset co;
            if (joinType == jtRound)
                co.ArcTolerance = miterLimit;
            else
                co.MiterLimit = miterLimit;
            co.ShortestEdgeLength = double(delta * 0.005);
            co.AddPath(expoly.contour.points, joinType, ClipperLib::etClosedPolygon);
            ClipperLib::Paths contours_raw;
            co.Execute(contours_raw, - delta);
            if (contours_raw.empty())
                // No need to try to offset the holes.
                return 0;
            append_paths_with_z(contours_raw, 0, contours);
        }

        if (expoly.holes.empty()) {
            // No need to subtract holes from the offsetted expolygon, we are done.
            append(out, std::move(contours));
        } else {
            // 2) Offset the holes one by one, collect the offsetted holes.
            ClipperLib_Z::Paths holes;
            {
                for (const Polygon &hole : expoly.holes) {
                    ClipperLib::ClipperOffset co;
                    if (joinType == jtRound)
                        co.ArcTolerance = miterLimit;
                    else
                        co.MiterLimit = miterLimit;
                    co.ShortestEdgeLength = double(delta * 0.005);
                    co.AddPath(hole.points, joinType, ClipperLib::etClosedPolygon);
                    ClipperLib::Paths out2;
                    // Execute reorients the contours so that the outer most contour has a positive area. Thus the output
                    // contours will be CCW oriented even though the input paths are CW oriented.
                    // Offset is applied after contour reorientation, thus the signum of the offset value is reversed.
                    co.Execute(out2, delta);
                    append_paths_with_z(out2, 1 + (&hole - expoly.holes.data()), holes);
                }
            }

            // 3) Subtract holes from the contours.
            if (holes.empty()) {
                // No hole remaining after an offset. Just copy the outer contour.
                append(out, std::move(contours));
            } else {
                // Negative offset. There is a chance, that the offsetted hole intersects the outer contour.
                // Subtract the offsetted holes from the offsetted contours.
                ClipperLib_Z::Clipper clipper;
                clipper.ZFillFunction([](const ClipperLib_Z::IntPoint &e1bot, const ClipperLib_Z::IntPoint &e1top, const ClipperLib_Z::IntPoint &e2bot, const ClipperLib_Z::IntPoint &e2top, ClipperLib_Z::IntPoint &pt) {
                        //pt.z() = std::max(std::max(e1bot.z(), e1top.z()), std::max(e2bot.z(), e2top.z()));
                        // Just mark the intersection.
                        pt.z() = -1;
                    });
                clipper.AddPaths(contours, ClipperLib_Z::ptSubject, true);
                clipper.AddPaths(holes,    ClipperLib_Z::ptClip,    true);
                ClipperLib_Z::Paths output;
                clipper.Execute(ClipperLib_Z::ctDifference, output, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
                if (! output.empty()) {
                    append(out, std::move(output));
                } else {
                    // The offsetted holes have eaten up the offsetted outer contour.
                    return 0;
                }
            }
        }

        return 1;
    };

    const double spacing = flow.scaled_spacing();
    // Clip the sheath path to avoid the extruder to get exactly on the first point of the loop.
    const double clip_length = spacing * 0.15;
    const double anchor_length = spacing * 6.;
    ClipperLib_Z::Paths anchor_candidates;
    for (ExPolygon& expoly : closing_ex(polygons, float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5 * flow.scaled_width()))) {
        std::unique_ptr<ExtrusionEntityCollection> eec;
        ExPolygons                                 regions_to_draw_inner_wall{expoly};
        if (support_params.tree_branch_diameter_double_wall_area_scaled > 0)
            if (double area = expoly.area(); area > support_params.tree_branch_diameter_double_wall_area_scaled) {
                BOOST_LOG_TRIVIAL(debug)<< "TreeSupports: double wall area: " << area<< " > " << support_params.tree_branch_diameter_double_wall_area_scaled;
                eec = std::make_unique<ExtrusionEntityCollection>();
                // Don't reorder internal / external loops of the same island, always start with the internal loop.
                eec->no_sort = true;
                // Make the tree branch stable by adding another perimeter.
                ExPolygons level2 = offset2_ex({expoly}, -1.5 * flow.scaled_width(), 0.5 * flow.scaled_width());
                if (level2.size() > 0) {
                    regions_to_draw_inner_wall = level2;
                    extrusion_entities_append_paths(eec->entities, draw_perimeters(expoly, clip_length), ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height(),
                            // Disable reversal of the path, always start with the anchor, always print CCW.
                            false);
                    expoly = level2.front();
                }
            }
        for (ExPolygon &expoly : regions_to_draw_inner_wall)
        {
            // Try to produce one more perimeter to place the seam anchor.
            // First genrate a 2nd perimeter loop as a source for anchor candidates.
            // The anchor candidate points are annotated with an index of the source contour or with -1 if on intersection.
            anchor_candidates.clear();
            shrink_expolygon_with_contour_idx(expoly, flow.scaled_width(), DefaultJoinType, 1.2, anchor_candidates);
            // Orient all contours CW.
            for (auto &path : anchor_candidates)
                if (ClipperLib_Z::Area(path) > 0) std::reverse(path.begin(), path.end());

            // Draw the perimeters.
            Polylines polylines;
            polylines.reserve(expoly.holes.size() + 1);
            for (int idx_loop = 0; idx_loop < int(expoly.num_contours()); ++idx_loop) {
                // Open the loop with a seam.
                const Polygon &loop = expoly.contour_or_hole(idx_loop);
                Polyline       pl(loop.points);
                // Orient all contours CW, because the anchor will be added to the end of polyline while we want to start a loop with the anchor.
                if (idx_loop == 0)
                    // It is an outer contour.
                    pl.reverse();
                pl.points.emplace_back(pl.points.front());
                pl.clip_end(clip_length);
                if (pl.size() < 2) continue;
                // Find the foot of the seam point on anchor_candidates. Only pick an anchor point that was created by offsetting the source contour.
                ClipperLib_Z::Path *closest_contour = nullptr;
                Vec2d               closest_point;
                int                 closest_point_idx = -1;
                double              closest_point_t   = 0.;
                double              d2min             = std::numeric_limits<double>::max();
                Vec2d               seam_pt           = pl.back().cast<double>();
                for (ClipperLib_Z::Path &path : anchor_candidates)
                    for (int i = 0; i < int(path.size()); ++i) {
                        int j = next_idx_modulo(i, path);
                        if (path[i].z() == idx_loop || path[j].z() == idx_loop) {
                            Vec2d pi(path[i].x(), path[i].y());
                            Vec2d pj(path[j].x(), path[j].y());
                            Vec2d v  = pj - pi;
                            Vec2d w  = seam_pt - pi;
                            auto  l2 = v.squaredNorm();
                            auto  t  = std::clamp((l2 == 0) ? 0 : v.dot(w) / l2, 0., 1.);
                            if ((path[i].z() == idx_loop || t > EPSILON) && (path[j].z() == idx_loop || t < 1. - EPSILON)) {
                                // Closest point.
                                Vec2d  fp = pi + v * t;
                                double d2 = (fp - seam_pt).squaredNorm();
                                if (d2 < d2min) {
                                    d2min             = d2;
                                    closest_contour   = &path;
                                    closest_point     = fp;
                                    closest_point_idx = i;
                                    closest_point_t   = t;
                                }
                            }
                        }
                    }
                if (d2min < sqr(flow.scaled_width() * 3.)) {
                    // Try to cut an anchor from the closest_contour.
                    // Both closest_contour and pl are CW oriented.
                    pl.points.emplace_back(closest_point.cast<coord_t>());
                    const ClipperLib_Z::Path &path             = *closest_contour;
                    double                    remaining_length = anchor_length - (seam_pt - closest_point).norm();
                    int                       i                = closest_point_idx;
                    int                       j                = next_idx_modulo(i, *closest_contour);
                    Vec2d                     pi(path[i].x(), path[i].y());
                    Vec2d                     pj(path[j].x(), path[j].y());
                    Vec2d                     v = pj - pi;
                    double                    l = v.norm();
                    if (remaining_length < (1. - closest_point_t) * l) {
                        // Just trim the current line.
                        pl.points.emplace_back((closest_point + v * (remaining_length / l)).cast<coord_t>());
                    } else {
                        // Take the rest of the current line, continue with the other lines.
                        pl.points.emplace_back(path[j].x(), path[j].y());
                        pi = pj;
                        for (i = j; path[i].z() == idx_loop && remaining_length > 0; i = j, pi = pj) {
                            j  = next_idx_modulo(i, path);
                            pj = Vec2d(path[j].x(), path[j].y());
                            v  = pj - pi;
                            l  = v.norm();
                            if (i == closest_point_idx) {
                                // Back at the first segment. Most likely this should not happen and we may end the anchor.
                                break;
                            }
                            if (remaining_length <= l) {
                                pl.points.emplace_back((pi + v * (remaining_length / l)).cast<coord_t>());
                                break;
                            }
                            pl.points.emplace_back(path[j].x(), path[j].y());
                            remaining_length -= l;
                        }
                    }
                }
                // Start with the anchor.
                pl.reverse();
                polylines.emplace_back(std::move(pl));
            }

            ExtrusionEntitiesPtr &out = eec ? eec->entities : dst;
            extrusion_entities_append_paths(out, std::move(polylines), ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height(),
                                            // Disable reversal of the path, always start with the anchor, always print CCW.
                                            false);
        }
        if (eec) {
            std::reverse(eec->entities.begin(), eec->entities.end());
            dst.emplace_back(eec.release());
        }
    }
}

void fill_expolygons_with_sheath_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    const Polygons          &polygons,
    Fill                    *filler,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow,
    const SupportParameters& support_params,
    bool                     with_sheath,
    bool                     no_sort)
{
    if (polygons.empty())
        return;

    if (with_sheath) {
        if (density == 0) {
            tree_supports_generate_paths(dst, polygons, flow, support_params);
            return;
        }
    }
    else {
        fill_expolygons_generate_paths(dst, closing_ex(polygons, float(SCALED_EPSILON)), filler, density, role, flow);
        return;
    }

    FillParams fill_params;
    fill_params.density     = density;
    fill_params.dont_adjust = true;

    const double spacing = flow.scaled_spacing();
    // Clip the sheath path to avoid the extruder to get exactly on the first point of the loop.
    const double clip_length = spacing * 0.15;

    for (ExPolygon &expoly : closing_ex(polygons, float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5*flow.scaled_width()))) {
        // Don't reorder the skirt and its infills.
        std::unique_ptr<ExtrusionEntityCollection> eec;
        if (no_sort) {
            eec = std::make_unique<ExtrusionEntityCollection>();
            eec->no_sort = true;
        }
        ExtrusionEntitiesPtr &out = no_sort ? eec->entities : dst;
        extrusion_entities_append_paths(out, draw_perimeters(expoly, clip_length), ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
        // Fill in the rest.
        fill_expolygons_generate_paths(out, offset_ex(expoly, float(-0.4 * spacing)), filler, fill_params, density, role, flow);
        if (no_sort && ! eec->empty())
            dst.emplace_back(eec.release());
    }
}

// Support layers, partially processed.
struct SupportGeneratorLayerExtruded
{
    SupportGeneratorLayerExtruded& operator=(SupportGeneratorLayerExtruded &&rhs) {
        this->layer = rhs.layer;
        this->extrusions = std::move(rhs.extrusions);
        m_polygons_to_extrude = std::move(rhs.m_polygons_to_extrude);
        rhs.layer = nullptr;
        return *this;
    }

    bool empty() const {
        return layer == nullptr || layer->polygons.empty();
    }

    void set_polygons_to_extrude(Polygons &&polygons) {
        if (m_polygons_to_extrude == nullptr)
            m_polygons_to_extrude = std::make_unique<Polygons>(std::move(polygons));
        else
            *m_polygons_to_extrude = std::move(polygons);
    }
    Polygons& polygons_to_extrude() { return (m_polygons_to_extrude == nullptr) ? layer->polygons : *m_polygons_to_extrude; }
    const Polygons& polygons_to_extrude() const { return (m_polygons_to_extrude == nullptr) ? layer->polygons : *m_polygons_to_extrude; }

    bool could_merge(const SupportGeneratorLayerExtruded &other) const {
        return ! this->empty() && ! other.empty() &&
            std::abs(this->layer->height - other.layer->height) < EPSILON &&
            this->layer->bridging == other.layer->bridging;
    }

    // Merge regions, perform boolean union over the merged polygons.
    void merge(SupportGeneratorLayerExtruded &&other) {
        assert(this->could_merge(other));
        // 1) Merge the rest polygons to extrude, if there are any.
        if (other.m_polygons_to_extrude != nullptr) {
            if (m_polygons_to_extrude == nullptr) {
                // This layer has no extrusions generated yet, if it has no m_polygons_to_extrude (its area to extrude was not reduced yet).
                assert(this->extrusions.empty());
                m_polygons_to_extrude = std::make_unique<Polygons>(this->layer->polygons);
            }
            Slic3r::polygons_append(*m_polygons_to_extrude, std::move(*other.m_polygons_to_extrude));
            *m_polygons_to_extrude = union_safety_offset(*m_polygons_to_extrude);
            other.m_polygons_to_extrude.reset();
        } else if (m_polygons_to_extrude != nullptr) {
            assert(other.m_polygons_to_extrude == nullptr);
            // The other layer has no extrusions generated yet, if it has no m_polygons_to_extrude (its area to extrude was not reduced yet).
            assert(other.extrusions.empty());
            Slic3r::polygons_append(*m_polygons_to_extrude, other.layer->polygons);
            *m_polygons_to_extrude = union_safety_offset(*m_polygons_to_extrude);
        }
        // 2) Merge the extrusions.
        this->extrusions.insert(this->extrusions.end(), other.extrusions.begin(), other.extrusions.end());
        other.extrusions.clear();
        // 3) Merge the infill polygons.
        Slic3r::polygons_append(this->layer->polygons, std::move(other.layer->polygons));
        this->layer->polygons = union_safety_offset(this->layer->polygons);
        other.layer->polygons.clear();
    }

    void polygons_append(Polygons &dst) const {
        if (layer != NULL && ! layer->polygons.empty())
            Slic3r::polygons_append(dst, layer->polygons);
    }

    // The source layer. It carries the height and extrusion type (bridging / non bridging, extrusion height).
    SupportGeneratorLayer  *layer { nullptr };
    // Collect extrusions. They will be exported sorted by the bottom height.
    ExtrusionEntitiesPtr                  extrusions;

private:
    // In case the extrusions are non-empty, m_polygons_to_extrude may contain the rest areas yet to be filled by additional support.
    // This is useful mainly for the loop interfaces, which are generated before the zig-zag infills.
    std::unique_ptr<Polygons>             m_polygons_to_extrude;
};

typedef std::vector<SupportGeneratorLayerExtruded*> SupportGeneratorLayerExtrudedPtrs;

struct LoopInterfaceProcessor
{
    LoopInterfaceProcessor(coordf_t circle_r) :
        n_contact_loops(0),
        circle_radius(circle_r),
        circle_distance(circle_r * 3.)
    {
        // Shape of the top contact area.
        circle.points.reserve(6);
        for (size_t i = 0; i < 6; ++ i) {
            double angle = double(i) * M_PI / 3.;
            circle.points.push_back(Point(circle_radius * cos(angle), circle_radius * sin(angle)));
        }
    }

    // Generate loop contacts at the top_contact_layer,
    // trim the top_contact_layer->polygons with the areas covered by the loops.
    void generate(SupportGeneratorLayerExtruded &top_contact_layer, const Flow &interface_flow_src) const;

    int         n_contact_loops;
    coordf_t    circle_radius;
    coordf_t    circle_distance;
    Polygon     circle;
};

void LoopInterfaceProcessor::generate(SupportGeneratorLayerExtruded &top_contact_layer, const Flow &interface_flow_src) const
{
    if (n_contact_loops == 0 || top_contact_layer.empty())
        return;

    Flow flow = interface_flow_src.with_height(top_contact_layer.layer->height);

    Polygons overhang_polygons;
    if (top_contact_layer.layer->overhang_polygons != nullptr)
        overhang_polygons = std::move(*top_contact_layer.layer->overhang_polygons);

    // Generate the outermost loop.
    // Find centerline of the external loop (or any other kind of extrusions should the loop be skipped)
    ExPolygons top_contact_expolygons = offset_ex(union_ex(top_contact_layer.layer->polygons), - 0.5f * flow.scaled_width());

    // Grid size and bit shifts for quick and exact to/from grid coordinates manipulation.
    coord_t circle_grid_resolution = 1;
    coord_t circle_grid_powerof2 = 0;
    {
        // epsilon to account for rounding errors
        coord_t circle_grid_resolution_non_powerof2 = coord_t(2. * circle_distance + 3.);
        while (circle_grid_resolution < circle_grid_resolution_non_powerof2) {
            circle_grid_resolution <<= 1;
            ++ circle_grid_powerof2;
        }
    }

    struct PointAccessor {
        const Point* operator()(const Point &pt) const { return &pt; }
    };
    typedef ClosestPointInRadiusLookup<Point, PointAccessor> ClosestPointLookupType;

    Polygons loops0;
    {
        // find centerline of the external loop of the contours
        // Only consider the loops facing the overhang.
        Polygons external_loops;
        // Holes in the external loops.
        Polygons circles;
        Polygons overhang_with_margin = offset(union_ex(overhang_polygons), 0.5f * flow.scaled_width());
        for (ExPolygons::iterator it_contact_expoly = top_contact_expolygons.begin(); it_contact_expoly != top_contact_expolygons.end(); ++ it_contact_expoly) {
            // Store the circle centers placed for an expolygon into a regular grid, hashed by the circle centers.
            ClosestPointLookupType circle_centers_lookup(coord_t(circle_distance - SCALED_EPSILON));
            Points circle_centers;
            Point  center_last;
            // For each contour of the expolygon, start with the outer contour, continue with the holes.
            for (size_t i_contour = 0; i_contour <= it_contact_expoly->holes.size(); ++ i_contour) {
                Polygon     &contour = (i_contour == 0) ? it_contact_expoly->contour : it_contact_expoly->holes[i_contour - 1];
                const Point *seg_current_pt = nullptr;
                coordf_t     seg_current_t  = 0.;
                if (! intersection_pl(contour.split_at_first_point(), overhang_with_margin).empty()) {
                    // The contour is below the overhang at least to some extent.
                    //FIXME ideally one would place the circles below the overhang only.
                    // Walk around the contour and place circles so their centers are not closer than circle_distance from each other.
                    if (circle_centers.empty()) {
                        // Place the first circle.
                        seg_current_pt = &contour.points.front();
                        seg_current_t  = 0.;
                        center_last    = *seg_current_pt;
                        circle_centers_lookup.insert(center_last);
                        circle_centers.push_back(center_last);
                    }
                    for (Points::const_iterator it = contour.points.begin() + 1; it != contour.points.end(); ++it) {
                        // Is it possible to place a circle on this segment? Is it not too close to any of the circles already placed on this contour?
                        const Point &p1 = *(it-1);
                        const Point &p2 = *it;
                        // Intersection of a ray (p1, p2) with a circle placed at center_last, with radius of circle_distance.
                        const Vec2d v_seg(coordf_t(p2(0)) - coordf_t(p1(0)), coordf_t(p2(1)) - coordf_t(p1(1)));
                        const Vec2d v_cntr(coordf_t(p1(0) - center_last(0)), coordf_t(p1(1) - center_last(1)));
                        coordf_t a = v_seg.squaredNorm();
                        coordf_t b = 2. * v_seg.dot(v_cntr);
                        coordf_t c = v_cntr.squaredNorm() - circle_distance * circle_distance;
                        coordf_t disc = b * b - 4. * a * c;
                        if (disc > 0.) {
                            // The circle intersects a ray. Avoid the parts of the segment inside the circle.
                            coordf_t t1 = (-b - sqrt(disc)) / (2. * a);
                            coordf_t t2 = (-b + sqrt(disc)) / (2. * a);
                            coordf_t t0 = (seg_current_pt == &p1) ? seg_current_t : 0.;
                            // Take the lowest t in <t0, 1.>, excluding <t1, t2>.
                            coordf_t t;
                            if (t0 <= t1)
                                t = t0;
                            else if (t2 <= 1.)
                                t = t2;
                            else {
                                // Try the following segment.
                                seg_current_pt = nullptr;
                                continue;
                            }
                            seg_current_pt = &p1;
                            seg_current_t  = t;
                            center_last    = Point(p1(0) + coord_t(v_seg(0) * t), p1(1) + coord_t(v_seg(1) * t));
                            // It has been verified that the new point is far enough from center_last.
                            // Ensure, that it is far enough from all the centers.
                            std::pair<const Point*, coordf_t> circle_closest = circle_centers_lookup.find(center_last);
                            if (circle_closest.first != nullptr) {
                                -- it;
                                continue;
                            }
                        } else {
                            // All of the segment is outside the circle. Take the first point.
                            seg_current_pt = &p1;
                            seg_current_t  = 0.;
                            center_last    = p1;
                        }
                        // Place the first circle.
                        circle_centers_lookup.insert(center_last);
                        circle_centers.push_back(center_last);
                    }
                    external_loops.push_back(std::move(contour));
                    for (const Point &center : circle_centers) {
                        circles.push_back(circle);
                        circles.back().translate(center);
                    }
                }
            }
        }
        // Apply a pattern to the external loops.
        loops0 = diff(external_loops, circles);
    }

    Polylines loop_lines;
    {
        // make more loops
        Polygons loop_polygons = loops0;
        for (int i = 1; i < n_contact_loops; ++ i)
            polygons_append(loop_polygons,
                opening(
                    loops0,
                    i * flow.scaled_spacing() + 0.5f * flow.scaled_spacing(),
                    0.5f * flow.scaled_spacing()));
        // Clip such loops to the side oriented towards the object.
        // Collect split points, so they will be recognized after the clipping.
        // At the split points the clipped pieces will be stitched back together.
        loop_lines.reserve(loop_polygons.size());
        std::unordered_map<Point, int, PointHash> map_split_points;
        for (Polygons::const_iterator it = loop_polygons.begin(); it != loop_polygons.end(); ++ it) {
            assert(map_split_points.find(it->first_point()) == map_split_points.end());
            map_split_points[it->first_point()] = -1;
            loop_lines.push_back(it->split_at_first_point());
        }
        loop_lines = intersection_pl(loop_lines, expand(overhang_polygons, scale_(SUPPORT_MATERIAL_MARGIN)));
        // Because a closed loop has been split to a line, loop_lines may contain continuous segments split to 2 pieces.
        // Try to connect them.
        for (int i_line = 0; i_line < int(loop_lines.size()); ++ i_line) {
            Polyline &polyline = loop_lines[i_line];
            auto it = map_split_points.find(polyline.first_point());
            if (it != map_split_points.end()) {
                // This is a stitching point.
                // If this assert triggers, multiple source polygons likely intersected at this point.
                assert(it->second != -2);
                if (it->second < 0) {
                    // First occurence.
                    it->second = i_line;
                } else {
                    // Second occurence. Join the lines.
                    Polyline &polyline_1st = loop_lines[it->second];
                    assert(polyline_1st.first_point() == it->first || polyline_1st.last_point() == it->first);
                    if (polyline_1st.first_point() == it->first)
                        polyline_1st.reverse();
                    polyline_1st.append(std::move(polyline));
                    it->second = -2;
                }
                continue;
            }
            it = map_split_points.find(polyline.last_point());
            if (it != map_split_points.end()) {
                // This is a stitching point.
                // If this assert triggers, multiple source polygons likely intersected at this point.
                assert(it->second != -2);
                if (it->second < 0) {
                    // First occurence.
                    it->second = i_line;
                } else {
                    // Second occurence. Join the lines.
                    Polyline &polyline_1st = loop_lines[it->second];
                    assert(polyline_1st.first_point() == it->first || polyline_1st.last_point() == it->first);
                    if (polyline_1st.first_point() == it->first)
                        polyline_1st.reverse();
                    polyline.reverse();
                    polyline_1st.append(std::move(polyline));
                    it->second = -2;
                }
            }
        }
        // Remove empty lines.
        remove_degenerate(loop_lines);
    }

    // add the contact infill area to the interface area
    // note that growing loops by $circle_radius ensures no tiny
    // extrusions are left inside the circles; however it creates
    // a very large gap between loops and contact_infill_polygons, so maybe another
    // solution should be found to achieve both goals
    // Store the trimmed polygons into a separate polygon set, so the original infill area remains intact for
    // "modulate by layer thickness".
    top_contact_layer.set_polygons_to_extrude(diff(top_contact_layer.layer->polygons, offset(loop_lines, float(circle_radius * 1.1))));

    // Transform loops into ExtrusionPath objects.
    extrusion_entities_append_paths(
        top_contact_layer.extrusions,
        std::move(loop_lines),
        ExtrusionRole::erSupportMaterialInterface, flow.mm3_per_mm(), flow.width(), flow.height());
}

#ifdef SLIC3R_DEBUG
static std::string dbg_index_to_color(int idx)
{
    if (idx < 0)
        return "yellow";
    idx = idx % 3;
    switch (idx) {
        case 0: return "red";
        case 1: return "green";
        default: return "blue";
    }
}
#endif /* SLIC3R_DEBUG */

// When extruding a bottom interface layer over an object, the bottom interface layer is extruded in a thin air, therefore
// it is being extruded with a bridging flow to not shrink excessively (the die swell effect).
// Tiny extrusions are better avoided and it is always better to anchor the thread to an existing support structure if possible.
// Therefore the bottom interface spots are expanded a bit. The expanded regions may overlap with another bottom interface layers,
// leading to over extrusion, where they overlap. The over extrusion is better avoided as it often makes the interface layers
// to stick too firmly to the object.
//
// Modulate thickness (increase bottom_z) of extrusions_in_out generated for this_layer
// if they overlap with overlapping_layers, whose print_z is above this_layer.bottom_z() and below this_layer.print_z.
static void modulate_extrusion_by_overlapping_layers(
    // Extrusions generated for this_layer.
    ExtrusionEntitiesPtr                               &extrusions_in_out,
    const SupportGeneratorLayer          &this_layer,
    // Multiple layers overlapping with this_layer, sorted bottom up.
    const SupportGeneratorLayersPtr      &overlapping_layers)
{
    size_t n_overlapping_layers = overlapping_layers.size();
    if (n_overlapping_layers == 0 || extrusions_in_out.empty())
        // The extrusions do not overlap with any other extrusion.
        return;

    // Get the initial extrusion parameters.
    ExtrusionPath *extrusion_path_template = dynamic_cast<ExtrusionPath*>(extrusions_in_out.front());
    assert(extrusion_path_template != nullptr);
    ExtrusionRole extrusion_role  = extrusion_path_template->role();
    float         extrusion_width = extrusion_path_template->width;

    struct ExtrusionPathFragment
    {
        ExtrusionPathFragment() : mm3_per_mm(-1), width(-1), height(-1) {};
        ExtrusionPathFragment(double mm3_per_mm, float width, float height) : mm3_per_mm(mm3_per_mm), width(width), height(height) {};

        Polylines       polylines;
        double          mm3_per_mm;
        float           width;
        float           height;
    };

    // Split the extrusions by the overlapping layers, reduce their extrusion rate.
    // The last path_fragment is from this_layer.
    std::vector<ExtrusionPathFragment> path_fragments(
        n_overlapping_layers + 1,
        ExtrusionPathFragment(extrusion_path_template->mm3_per_mm, extrusion_path_template->width, extrusion_path_template->height));
    // Don't use it, it will be released.
    extrusion_path_template = nullptr;

#ifdef SLIC3R_DEBUG
    static int iRun = 0;
    ++ iRun;
    BoundingBox bbox;
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        bbox.merge(get_extents(overlapping_layer.polygons));
    }
    for (ExtrusionEntitiesPtr::const_iterator it = extrusions_in_out.begin(); it != extrusions_in_out.end(); ++ it) {
        ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(*it);
        assert(path != nullptr);
        bbox.merge(get_extents(path->polyline));
    }
    SVG svg(debug_out_path("support-fragments-%d-%lf.svg", iRun, this_layer.print_z).c_str(), bbox);
    const float transparency = 0.5f;
    // Filled polygons for the overlapping regions.
    svg.draw(union_ex(this_layer.polygons), dbg_index_to_color(-1), transparency);
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        svg.draw(union_ex(overlapping_layer.polygons), dbg_index_to_color(int(i_overlapping_layer)), transparency);
    }
    // Contours of the overlapping regions.
    svg.draw(to_polylines(this_layer.polygons), dbg_index_to_color(-1), scale_(0.2));
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        svg.draw(to_polylines(overlapping_layer.polygons), dbg_index_to_color(int(i_overlapping_layer)), scale_(0.1));
    }
    // Fill extrusion, the source.
    for (ExtrusionEntitiesPtr::const_iterator it = extrusions_in_out.begin(); it != extrusions_in_out.end(); ++ it) {
        ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(*it);
        std::string color_name;
        switch ((it - extrusions_in_out.begin()) % 9) {
            case 0: color_name = "magenta"; break;
            case 1: color_name = "deepskyblue"; break;
            case 2: color_name = "coral"; break;
            case 3: color_name = "goldenrod"; break;
            case 4: color_name = "orange"; break;
            case 5: color_name = "olivedrab"; break;
            case 6: color_name = "blueviolet"; break;
            case 7: color_name = "brown"; break;
            default: color_name = "orchid"; break;
        }
        svg.draw(path->polyline, color_name, scale_(0.2));
    }
#endif /* SLIC3R_DEBUG */

    // End points of the original paths.
    std::vector<std::pair<Point, Point>> path_ends;
    // Collect the paths of this_layer.
    {
        Polylines &polylines = path_fragments.back().polylines;
        for (ExtrusionEntity *ee : extrusions_in_out) {
            ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(ee);
            assert(path != nullptr);
            polylines.emplace_back(Polyline(std::move(path->polyline)));
            path_ends.emplace_back(std::pair<Point, Point>(polylines.back().points.front(), polylines.back().points.back()));
            delete path;
        }
    }
    // Destroy the original extrusion paths, their polylines were moved to path_fragments already.
    // This will be the destination for the new paths.
    extrusions_in_out.clear();

    // Fragment the path segments by overlapping layers. The overlapping layers are sorted by an increasing print_z.
    // Trim by the highest overlapping layer first.
    for (int i_overlapping_layer = int(n_overlapping_layers) - 1; i_overlapping_layer >= 0; -- i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        ExtrusionPathFragment &frag = path_fragments[i_overlapping_layer];
        Polygons polygons_trimming = offset(union_ex(overlapping_layer.polygons), float(scale_(0.5*extrusion_width)));
        frag.polylines = intersection_pl(path_fragments.back().polylines, polygons_trimming);
        path_fragments.back().polylines = diff_pl(path_fragments.back().polylines, polygons_trimming);
        // Adjust the extrusion parameters for a reduced layer height and a non-bridging flow (nozzle_dmr = -1, does not matter).
        assert(this_layer.print_z > overlapping_layer.print_z);
        frag.height = float(this_layer.print_z - overlapping_layer.print_z);
        frag.mm3_per_mm = Flow(frag.width, frag.height, -1.f).mm3_per_mm();
#ifdef SLIC3R_DEBUG
        svg.draw(frag.polylines, dbg_index_to_color(i_overlapping_layer), scale_(0.1));
#endif /* SLIC3R_DEBUG */
    }

#ifdef SLIC3R_DEBUG
    svg.draw(path_fragments.back().polylines, dbg_index_to_color(-1), scale_(0.1));
    svg.Close();
#endif /* SLIC3R_DEBUG */

    // Now chain the split segments using hashing and a nearly exact match, maintaining the order of segments.
    // Create a single ExtrusionPath or ExtrusionEntityCollection per source ExtrusionPath.
    // Map of fragment start/end points to a pair of <i_overlapping_layer, i_polyline_in_layer>
    // Because a non-exact matching is used for the end points, a multi-map is used.
    // As the clipper library may reverse the order of some clipped paths, store both ends into the map.
    struct ExtrusionPathFragmentEnd
    {
        ExtrusionPathFragmentEnd(size_t alayer_idx, size_t apolyline_idx, bool ais_start) :
            layer_idx(alayer_idx), polyline_idx(apolyline_idx), is_start(ais_start) {}
        size_t layer_idx;
        size_t polyline_idx;
        bool   is_start;
    };
    class ExtrusionPathFragmentEndPointAccessor {
    public:
        ExtrusionPathFragmentEndPointAccessor(const std::vector<ExtrusionPathFragment> &path_fragments) : m_path_fragments(path_fragments) {}
        // Return an end point of a fragment, or nullptr if the fragment has been consumed already.
        const Point* operator()(const ExtrusionPathFragmentEnd &fragment_end) const {
            const Polyline &polyline = m_path_fragments[fragment_end.layer_idx].polylines[fragment_end.polyline_idx];
            return polyline.points.empty() ? nullptr :
                (fragment_end.is_start ? &polyline.points.front() : &polyline.points.back());
        }
    private:
        ExtrusionPathFragmentEndPointAccessor& operator=(const ExtrusionPathFragmentEndPointAccessor&) {
            return *this;
        }

        const std::vector<ExtrusionPathFragment> &m_path_fragments;
    };
    const coord_t search_radius = 7;
    ClosestPointInRadiusLookup<ExtrusionPathFragmentEnd, ExtrusionPathFragmentEndPointAccessor> map_fragment_starts(
        search_radius, ExtrusionPathFragmentEndPointAccessor(path_fragments));
    for (size_t i_overlapping_layer = 0; i_overlapping_layer <= n_overlapping_layers; ++ i_overlapping_layer) {
        const Polylines &polylines = path_fragments[i_overlapping_layer].polylines;
        for (size_t i_polyline = 0; i_polyline < polylines.size(); ++ i_polyline) {
            // Map a starting point of a polyline to a pair of <layer, polyline>
            if (polylines[i_polyline].points.size() >= 2) {
                map_fragment_starts.insert(ExtrusionPathFragmentEnd(i_overlapping_layer, i_polyline, true));
                map_fragment_starts.insert(ExtrusionPathFragmentEnd(i_overlapping_layer, i_polyline, false));
            }
        }
    }

    // For each source path:
    for (size_t i_path = 0; i_path < path_ends.size(); ++ i_path) {
        const Point &pt_start = path_ends[i_path].first;
        const Point &pt_end   = path_ends[i_path].second;
        Point pt_current = pt_start;
        // Find a chain of fragments with the original / reduced print height.
        ExtrusionMultiPath multipath;
        for (;;) {
            // Find a closest end point to pt_current.
            std::pair<const ExtrusionPathFragmentEnd*, coordf_t> end_and_dist2 = map_fragment_starts.find(pt_current);
            // There may be a bug in Clipper flipping the order of two last points in a fragment?
            // assert(end_and_dist2.first != nullptr);
            assert(end_and_dist2.first == nullptr || end_and_dist2.second < search_radius * search_radius);
            if (end_and_dist2.first == nullptr) {
                // New fragment connecting to pt_current was not found.
                // Verify that the last point found is close to the original end point of the unfragmented path.
                //const double d2 = (pt_end - pt_current).cast<double>.squaredNorm();
                //assert(d2 < coordf_t(search_radius * search_radius));
                // End of the path.
                break;
            }
            const ExtrusionPathFragmentEnd &fragment_end_min = *end_and_dist2.first;
            // Fragment to consume.
            ExtrusionPathFragment &frag = path_fragments[fragment_end_min.layer_idx];
            Polyline              &frag_polyline = frag.polylines[fragment_end_min.polyline_idx];
            // Path to append the fragment to.
            ExtrusionPath         *path = multipath.paths.empty() ? nullptr : &multipath.paths.back();
            if (path != nullptr) {
                // Verify whether the path is compatible with the current fragment.
                assert(this_layer.layer_type == SupporLayerType::BottomContact || path->height != frag.height || path->mm3_per_mm != frag.mm3_per_mm);
                if (path->height != frag.height || path->mm3_per_mm != frag.mm3_per_mm) {
                    path = nullptr;
                }
                // Merging with the previous path. This can only happen if the current layer was reduced by a base layer, which was split into a base and interface layer.
            }
            if (path == nullptr) {
                // Allocate a new path.
                multipath.paths.push_back(ExtrusionPath(extrusion_role, frag.mm3_per_mm, frag.width, frag.height));
                path = &multipath.paths.back();
            }
            // The Clipper library may flip the order of the clipped polylines arbitrarily.
            // Reverse the source polyline, if connecting to the end.
            if (! fragment_end_min.is_start)
                frag_polyline.reverse();
            // Enforce exact overlap of the end points of successive fragments.
            assert(frag_polyline.points.front() == pt_current);
            frag_polyline.points.front() = pt_current;
            // Don't repeat the first point.
            if (! path->polyline.points.empty())
                path->polyline.points.pop_back();
            // Consume the fragment's polyline, remove it from the input fragments, so it will be ignored the next time.
            path->polyline.append(std::move(frag_polyline));
            frag_polyline.points.clear();
            pt_current = path->polyline.points.back();
            if (pt_current == pt_end) {
                // End of the path.
                break;
            }
        }
        if (!multipath.paths.empty()) {
            if (multipath.paths.size() == 1) {
                // This path was not fragmented.
                extrusions_in_out.push_back(new ExtrusionPath(std::move(multipath.paths.front())));
            } else {
                // This path was fragmented. Copy the collection as a whole object, so the order inside the collection will not be changed
                // during the chaining of extrusions_in_out.
                extrusions_in_out.push_back(new ExtrusionMultiPath(std::move(multipath)));
            }
        }
    }
    // If there are any non-consumed fragments, add them separately.
    //FIXME this shall not happen, if the Clipper works as expected and all paths split to fragments could be re-connected.
    for (auto it_fragment = path_fragments.begin(); it_fragment != path_fragments.end(); ++ it_fragment)
        extrusion_entities_append_paths(extrusions_in_out, std::move(it_fragment->polylines), extrusion_role, it_fragment->mm3_per_mm, it_fragment->width, it_fragment->height);
}

// Support layer that is covered by some form of dense interface.
static constexpr const std::initializer_list<SupporLayerType> support_types_interface{
    SupporLayerType::RaftInterface, SupporLayerType::BottomContact, SupporLayerType::BottomInterface, SupporLayerType::TopContact, SupporLayerType::TopInterface
};

SupportGeneratorLayersPtr generate_support_layers(
    PrintObject                         &object,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers)
{
    // Install support layers into the object.
    // A support layer installed on a PrintObject has a unique print_z.
    SupportGeneratorLayersPtr layers_sorted;
    layers_sorted.reserve(raft_layers.size() + bottom_contacts.size() + top_contacts.size() + intermediate_layers.size() + interface_layers.size() + base_interface_layers.size());
    append(layers_sorted, raft_layers);
    append(layers_sorted, bottom_contacts);
    append(layers_sorted, top_contacts);
    append(layers_sorted, intermediate_layers);
    append(layers_sorted, interface_layers);
    append(layers_sorted, base_interface_layers);
    // remove dupliated layers
    std::sort(layers_sorted.begin(), layers_sorted.end());
    layers_sorted.erase(std::unique(layers_sorted.begin(), layers_sorted.end()), layers_sorted.end());

    // Sort the layers lexicographically by a raising print_z and a decreasing height.
    std::sort(layers_sorted.begin(), layers_sorted.end(), [](auto *l1, auto *l2) { return *l1 < *l2; });
    int layer_id = 0;
    int layer_id_interface = 0;
    assert(object.support_layers().empty());
    for (size_t i = 0; i < layers_sorted.size();) {
        // Find the last layer with roughly the same print_z, find the minimum layer height of all.
        // Due to the floating point inaccuracies, the print_z may not be the same even if in theory they should.
        size_t j = i + 1;
        coordf_t zmax = layers_sorted[i]->print_z + EPSILON;
        for (; j < layers_sorted.size() && layers_sorted[j]->print_z <= zmax; ++j) ;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        coordf_t zavg = 0.5 * (layers_sorted[i]->print_z + layers_sorted[j - 1]->print_z);
        coordf_t height_min = layers_sorted[i]->height;
        bool     empty = true;
        // For snug supports, layers where the direction of the support interface shall change are accounted for.
        size_t   num_interfaces = 0;
        size_t   num_top_contacts = 0;
        double   top_contact_bottom_z = 0;
        for (size_t u = i; u < j; ++u) {
            SupportGeneratorLayer &layer = *layers_sorted[u];
            if (! layer.polygons.empty()) {
                empty             = false;
                num_interfaces   += one_of(layer.layer_type, support_types_interface);
                if (layer.layer_type == SupporLayerType::TopContact) {
                    ++ num_top_contacts;
                    assert(num_top_contacts <= 1);
                    // All top contact layers sharing this print_z shall also share bottom_z.
                    //assert(num_top_contacts == 1 || (top_contact_bottom_z - layer.bottom_z) < EPSILON);
                    top_contact_bottom_z = layer.bottom_z;
                }
            }
            layer.print_z = zavg;
            height_min = std::min(height_min, layer.height);
        }
        if (! empty) {
            // Here the upper_layer and lower_layer pointers are left to null at the support layers,
            // as they are never used. These pointers are candidates for removal.
            bool   this_layer_contacts_only = num_top_contacts > 0 && num_top_contacts == num_interfaces;
            size_t this_layer_id_interface  = layer_id_interface;
            if (this_layer_contacts_only) {
                // Find a supporting layer for its interface ID.
                for (auto it = object.support_layers().rbegin(); it != object.support_layers().rend(); ++ it)
                    if (const SupportLayer &other_layer = **it; std::abs(other_layer.print_z - top_contact_bottom_z) < EPSILON) {
                        // other_layer supports this top contact layer. Assign a different support interface direction to this layer
                        // from the layer that supports it.
                        this_layer_id_interface = other_layer.interface_id() + 1;
                    }
            }
            object.add_support_layer(layer_id ++, this_layer_id_interface, height_min, zavg);
            if (num_interfaces && ! this_layer_contacts_only)
                ++ layer_id_interface;
        }
        i = j;
    }
    return layers_sorted;
}

size_t support_group_object_layer_index(const SupportGeneratorLayer &layer, bool use_above,
                                        const std::vector<coordf_t> &object_layer_zs)
{
    const size_t idx = use_above ? layer.idx_object_layer_above : layer.idx_object_layer_below;
    if (idx != size_t(-1) && idx < object_layer_zs.size())
        return idx;
    // merge_contact_layers (SupportMaterial.cpp) merges contacts closer than support_layer_height_min
    // and SupportGeneratorLayer::merge does not merge the object layer indices, so a merged layer can
    // arrive here with none; the organic tree's precomputed roof layers never carry one at all. Fall
    // back to the object layer the contact is there FOR - above for a top contact, below for a bottom
    // one. Stage 4 changed this from "the nearest": see support_group_object_layer_index_at().
    return support_group_object_layer_index_at(use_above ? layer.print_z : layer.bottom_z, object_layer_zs, use_above);
}

size_t support_group_object_layer_index_at(coordf_t z, const std::vector<coordf_t> &object_layer_zs, bool above)
{
    if (object_layer_zs.empty())
        return size_t(-1);
    // Round TOWARDS the object the surface is there for, never to the nearest layer. A part
    // floating 10 mm above the bed has no footprint at all at the layers where its own support roof
    // is printed, so the nearest layer below is empty and the group's claim there is empty with it -
    // support_group_piece() then gives the whole roof to the default group and the group appears to
    // do nothing. Measured on the two-part organic-tree fixture: 61 % of the object-wide yardstick
    // with nearest, and the classic-tree roof came out bit-identical to the ungrouped control.
    auto it = std::lower_bound(object_layer_zs.begin(), object_layer_zs.end(), z - EPSILON);
    if (above) {
        if (it == object_layer_zs.end())
            return object_layer_zs.size() - 1;
        return size_t(it - object_layer_zs.begin());
    }
    if (it == object_layer_zs.end() || (it != object_layer_zs.begin() && *it > z + EPSILON))
        -- it;
    return size_t(it - object_layer_zs.begin());
}

std::vector<coordf_t> object_layer_print_zs(const PrintObject &object)
{
    std::vector<coordf_t> zs;
    zs.reserve(object.layers().size());
    for (const Layer *layer : object.layers())
        zs.emplace_back(layer->print_z);
    return zs;
}

Polygons support_group_piece(const Polygons &src, const std::vector<Polygons> *claim,
                             size_t idx_object_layer, size_t g)
{
    if (claim == nullptr || idx_object_layer == size_t(-1) || idx_object_layer >= claim->size())
        // No mask for this layer: the default group keeps everything, the others get nothing. That
        // is the conservative direction - geometry is never dropped, only attributed to group 0.
        return g == 0 ? src : Polygons();
    const Polygons &c = (*claim)[idx_object_layer];
    if (g == 0)
        return c.empty() ? src : diff(src, c);
    return c.empty() ? Polygons() : intersection(src, c);
}

// Ultra (support groups, plan 2026-09-02 Stage 3 3.5): the config the SHARED SupportParameters is
// built from. It is the object's own, with the interface layer counts raised to the max over all
// groups. Reason: SupportParameters collapses support_material_interface_flow into
// support_material_flow when support_interface_top_layers == 0, and
// support_material_bottom_interface_flow - which decides the bottom-contact layer HEIGHTS, i.e.
// shared geometry - is derived from it. Taking the max keeps the interface flow alive whenever ANY
// group wants interfaces, and equals the object's own value when there is a single group, which is
// what keeps the off-mode output where it is. support_interface_filament deliberately stays the
// OBJECT's, so a group's filament choice can never move a bottom-contact layer height.
// Stage 4a: the organic-tree generator reads the same config to size its roof, so it uses this too.
PrintObjectConfig support_shared_config(const PrintObject &object,
                                        const std::vector<PrintObject::SupportGroup> &groups)
{
    PrintObjectConfig config = object.config();
    if (groups.size() <= 1)
        return config;
    // -1 in support_interface_bottom_layers means "same as top"; resolve it before taking a max.
    auto resolved_bottom = [](const PrintObjectConfig &c) {
        return c.support_interface_bottom_layers.value < 0 ? std::max(0, c.support_interface_top_layers.value)
                                                           : c.support_interface_bottom_layers.value;
    };
    int top    = config.support_interface_top_layers.value;
    int bottom = resolved_bottom(config);
    for (const PrintObject::SupportGroup &group : groups) {
        top    = std::max(top,    group.config.support_interface_top_layers.value);
        bottom = std::max(bottom, resolved_bottom(group.config));
    }
    config.support_interface_top_layers.value    = top;
    config.support_interface_bottom_layers.value = bottom;
    return config;
}

// Ultra (support groups, plan Stage 3 3.5 / R3.1): fill the per-group masks and turn them into
// DISJOINT per-object-layer claims. Every group's footprint is expanded by `reach` - how far a
// contact layer can sit outside the outline of the part it supports - and the result is then cut
// against (a) every OTHER group's own part, so a wide claim can never reach across a neighbour and
// take the contacts that belong to it, and (b) every LOWER group's claim, so the claims stay
// disjoint and the split is a hard clip along that boundary. claims[0] holds the union of all of
// them: group 0 takes what is left of any shared polygon, so the K pieces cover it exactly.
// `reach` and the group order are the two knobs behind the seam R3.1 describes.
std::vector<std::vector<Polygons>> support_group_claims(const PrintObject &object,
                                                        std::vector<PrintObject::SupportGroup> &groups,
                                                        const SupportParameters &shared_params)
{
    std::vector<std::vector<Polygons>> claims;
    if (groups.size() <= 1)
        // Off mode - every project that carries no support_group data. Not one extra mesh slice,
        // not one extra boolean; the whole of the off-mode guarantee is this early return.
        return claims;

    object.support_group_masks(groups);

    // Ultra (support groups, Stage 4): a TREE's roof carries no object-layer index of its own, and
    // it is not printed at the part's own layers either - it sits under the part, from the Z gap
    // down through however many roof layers there are. A part floating above the bed therefore has
    // an EMPTY footprint at every layer its own roof is printed at, so a claim built layer by layer
    // is empty exactly where the tree needs it and support_group_piece() hands the whole roof to the
    // default group. Measured before this projection: the classic-tree two-part fixture came out
    // BIT-IDENTICAL to the ungrouped control, and the organic single-part fixture delivered 61 % of
    // what the same value set object-wide delivers.
    //
    // So project each group's footprint DOWNWARD over the layers its roof can occupy before the
    // claims are cut. The claims stay disjoint by construction, because everything below - the
    // other-group block and the cut against lower groups - is computed from the projected masks too.
    // The normal generator projects nothing: its contacts carry idx_object_layer_above and land on
    // the right layer exactly, so Stage 3's output is untouched.
    if (is_tree(object.config().support_type.value) && ! object.layers().empty()) {
        const double layer_height = std::max(EPSILON, object.config().layer_height.value);
        const size_t project_down = size_t(shared_params.num_top_interface_layers) +
            size_t(std::ceil(object.config().support_top_z_distance.value / layer_height)) + 2;
        for (PrintObject::SupportGroup &group : groups) {
            if (group.mask.empty())
                continue;
            std::vector<Polygons> projected(group.mask.size());
            tbb::parallel_for(tbb::blocked_range<size_t>(0, group.mask.size()),
                [&group, &projected, project_down](const tbb::blocked_range<size_t> &range) {
                    for (size_t i = range.begin(); i < range.end(); ++ i) {
                        Polygons acc;
                        for (size_t j = i; j <= i + project_down && j < group.mask.size(); ++ j)
                            append(acc, group.mask[j]);
                        projected[i] = acc.empty() ? Polygons() : union_(acc);
                    }
                });
            group.mask = std::move(projected);
        }
    }

    // How far a contact layer can sit OUTSIDE the outline of the part it supports. Three terms, and
    // the second one is the whole reason a group used to barely change anything:
    //
    //  - the XY gap plus one interface extrusion width, the margin the contact is inflated by;
    //  - for grid supports, one grid cell. fill_contact_layer() runs every contact through
    //    SupportGridPattern, which STRETCHES each island onto a grid of
    //    support_base_pattern_spacing + support_material_flow.spacing() (SupportGridParams,
    //    SupportMaterial.cpp - 5.4 mm with the stock 5 mm spacing) before it is sliced. A narrow
    //    overhang - the crescent a slanted wall sheds on each layer - therefore becomes a band of
    //    grid cells straddling the part outline, most of it well beyond the part's own footprint.
    //    Claiming only footprint + 0.6 mm left the rest to the default group: measured on the
    //    user's two slanted cylinders, the group claimed 814 mm2 of 7506 mm2 of contact area
    //    (11 %) and its five interface layers changed that part's interface by 1 %, where the
    //    same value set object-wide changed it by 34 %. See the plan's 2c.
    //  - for TREE supports (Stage 4), one branch radius. A tree's roof is not the overhang either:
    //    it is drawn on top of branches whose tips are placed inside the overhang but whose circles
    //    reach tree_support_branch_diameter/2 beyond it, and organic trees additionally smooth the
    //    roof outward. The same lesson as the grid cell, with the generator's own dimension.
    //
    // Reaching this far is only safe because of the second cut below: no claim may take area
    // sitting over ANOTHER group's part - `block`, the part outline plus the gap no support may
    // enter anyway.
    // docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2c, Stage 4.
    const PrintObjectConfig &object_config = object.config();
    const float block = float(scale_(shared_params.gap_xy));
    float       reach = block + float(shared_params.support_material_interface_flow.scaled_width());
    if (shared_params.support_style == smsGrid)
        reach += float(scale_(object_config.support_base_pattern_spacing.value +
                              shared_params.support_material_flow.spacing()));
    else if (is_tree(object_config.support_type.value))
        reach += float(scale_(0.5 * object_config.tree_support_branch_diameter.value));

    const size_t num_layers = object.layers().size();
    claims.assign(groups.size(), std::vector<Polygons>(num_layers));
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers),
        [&groups, &claims, reach, block](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++ i) {
                Polygons taken;
                for (size_t g = 1; g < groups.size(); ++ g) {
                    Polygons c;
                    if (i < groups[g].mask.size() && ! groups[g].mask[i].empty())
                        c = expand(groups[g].mask[i], reach);
                    if (! c.empty()) {
                        // Another group's part, plus the gap no support may enter: off limits.
                        Polygons blocked;
                        for (size_t h = 0; h < groups.size(); ++ h)
                            if (h != g && i < groups[h].mask.size() && ! groups[h].mask[i].empty())
                                append(blocked, expand(groups[h].mask[i], block));
                        if (! blocked.empty()) {
                            // ... except over this group's OWN part. Two parts that touch would
                            // otherwise let the neighbour's gap eat a strip of the group's own
                            // footprint, which is never the right answer.
                            blocked = union_(blocked);
                            if (! groups[g].mask[i].empty())
                                blocked = diff(blocked, groups[g].mask[i]);
                            if (! blocked.empty())
                                c = diff(c, blocked);
                        }
                    }
                    if (! c.empty() && ! taken.empty())
                        c = diff(c, taken);
                    if (! c.empty())
                        append(taken, c);
                    claims[g][i] = std::move(c);
                }
                claims[0][i] = taken.empty() ? Polygons() : union_(taken);
            }
        });
    BOOST_LOG_TRIVIAL(info) << "Support generator - " << groups.size() << " support groups";
    return claims;
}

// Ultra (support groups, plan Stage 3 3.5): a sibling of every contact layer holding only the part
// of it group `g` owns. The clones are input to generate_interface_layers and are NEVER added to
// the layer graph, so generate_support_layers' assert(num_top_contacts <= 1) is untouched - which
// is exactly why the plan splits contacts at extrusion time and not here.
SupportGeneratorLayersPtr clone_contacts_masked(
    const SupportGeneratorLayersPtr &contacts,
    size_t                           g,
    const std::vector<Polygons>     &claim,
    const std::vector<coordf_t>     &object_layer_zs,
    SupportGeneratorLayerStorage    &layer_storage,
    bool                             use_above)
{
    SupportGeneratorLayersPtr out;
    out.reserve(contacts.size());
    for (SupportGeneratorLayer *src : contacts) {
        if (src == nullptr)
            continue;
        const size_t idx = support_group_object_layer_index(*src, use_above, object_layer_zs);
        Polygons polygons = support_group_piece(src->polygons, &claim, idx, g);
        if (polygons.empty())
            continue;
        SupportGeneratorLayer &dst = layer_storage.allocate(src->layer_type);
        dst.print_z                = src->print_z;
        dst.bottom_z               = src->bottom_z;
        dst.height                 = src->height;
        dst.bridging               = src->bridging;
        dst.idx_object_layer_above = src->idx_object_layer_above;
        dst.idx_object_layer_below = src->idx_object_layer_below;
        dst.support_group          = uint16_t(g);
        dst.polygons               = std::move(polygons);
        // Snug supports project *overhang_polygons rather than polygons, and dereference it
        // unconditionally, so every optional polygon set travels with the clone.
        auto mask_ptr = [&claim, idx, g](const std::unique_ptr<Polygons> &s, std::unique_ptr<Polygons> &d) {
            if (s)
                d = std::make_unique<Polygons>(support_group_piece(*s, &claim, idx, g));
        };
        mask_ptr(src->contact_polygons,  dst.contact_polygons);
        mask_ptr(src->overhang_polygons, dst.overhang_polygons);
        mask_ptr(src->enforcer_polygons, dst.enforcer_polygons);
        out.emplace_back(&dst);
    }
    return out;
}

void generate_support_toolpaths(
    SupportLayerPtrs                    &support_layers,
    const PrintObjectConfig             &config,
    const SupportParameters             &support_params,
    const SlicingParameters             &slicing_params,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers,
    const std::vector<SupportGroupToolpaths> *groups,
    const std::vector<coordf_t>              *object_layer_zs)
{
    // Ultra (support groups): everything below that reads `group_mode` is dead for a single-group
    // object, which is every project that carries no support_group data - see the plan's 3.4 step 5.
    const size_t num_groups = groups == nullptr ? 0 : groups->size();
    const bool   group_mode = num_groups > 1 && object_layer_zs != nullptr;
    // loop_interface_processor with a given circle radius.
    LoopInterfaceProcessor loop_interface_processor(1.5 * support_params.support_material_interface_flow.scaled_width());
    loop_interface_processor.n_contact_loops = config.support_interface_loop_pattern.value ? 1 : 0;

    std::vector<float>      angles { support_params.base_angle };
    if (config.support_base_pattern == smpRectilinearGrid)
        angles.push_back(support_params.interface_angle);

    BoundingBox bbox_object(Point(-scale_(1.), -scale_(1.0)), Point(scale_(1.), scale_(1.)));

//    const coordf_t link_max_length_factor = 3.;
    const coordf_t link_max_length_factor = 0.;

    // Insert the raft base layers.
    auto n_raft_layers = std::min<size_t>(support_layers.size(), std::max(0, int(slicing_params.raft_layers()) - 1));

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n_raft_layers),
        [&support_layers, &raft_layers, &intermediate_layers, &config, &support_params, &slicing_params,
            &bbox_object, link_max_length_factor]
            (const tbb::blocked_range<size_t>& range) {
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id)
        {
            assert(support_layer_id < raft_layers.size());
            SupportLayer               &support_layer = *support_layers[support_layer_id];
            assert(support_layer.support_fills.entities.empty());
            SupportGeneratorLayer      &raft_layer    = *raft_layers[support_layer_id];

            std::unique_ptr<Fill> filler_interface = std::unique_ptr<Fill>(Fill::new_from_type(support_params.raft_interface_fill_pattern));
            std::unique_ptr<Fill> filler_support   = std::unique_ptr<Fill>(Fill::new_from_type(support_params.base_fill_pattern));
            filler_interface->set_bounding_box(bbox_object);
            filler_support->set_bounding_box(bbox_object);

            // Print the tree supports cutting through the raft with the exception of the 1st layer, where a full support layer will be printed below
            // both the raft and the trees.
            // Trim the raft layers with the tree polygons.
            const Polygons &tree_polygons =
                support_layer_id > 0 && support_layer_id < intermediate_layers.size() && is_approx(intermediate_layers[support_layer_id]->print_z, support_layer.print_z) ?
                intermediate_layers[support_layer_id]->polygons : Polygons();

            // Print the support base below the support columns, or the support base for the support columns plus the contacts.
            if (support_layer_id > 0) {
                const Polygons &to_infill_polygons = (support_layer_id < slicing_params.base_raft_layers) ?
                    raft_layer.polygons :
                    //FIXME misusing contact_polygons for support columns.
                    ((raft_layer.contact_polygons == nullptr) ? Polygons() : *raft_layer.contact_polygons);
                // Trees may cut through the raft layers down to a print bed.
                Flow flow(float(support_params.support_material_flow.width()), float(raft_layer.height), support_params.support_material_flow.nozzle_diameter());
                assert(!raft_layer.bridging);
                if (! to_infill_polygons.empty()) {
                    Fill *filler = filler_support.get();
                    filler->angle = support_params.raft_angle_base;
                    filler->spacing = support_params.support_material_flow.spacing();
                    filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / support_params.support_density));
                    fill_expolygons_with_sheath_generate_paths(
                        // Destination
                        support_layer.support_fills.entities,
                        // Regions to fill
                        tree_polygons.empty() ? to_infill_polygons : diff(to_infill_polygons, tree_polygons),
                        // Filler and its parameters
                        filler, float(support_params.support_density),
                        // Extrusion parameters
                        ExtrusionRole::erSupportMaterial, flow,
                        support_params, support_params.with_sheath, false);
                }
                if (! tree_polygons.empty())
                    tree_supports_generate_paths(support_layer.support_fills.entities, tree_polygons, flow, support_params);
            }

            Fill *filler = filler_interface.get();
            Flow  flow = support_params.first_layer_flow;
            float density = 0.f;
            if (support_layer_id == 0) {
                // Base flange.
                filler->angle = support_params.raft_angle_1st_layer;
                filler->spacing = support_params.first_layer_flow.spacing();
                density       = float(config.raft_first_layer_density.value * 0.01);
            } else if (support_layer_id >= slicing_params.base_raft_layers) {
                filler->angle = support_params.raft_interface_angle(support_layer.interface_id());
                // We don't use $base_flow->spacing because we need a constant spacing
                // value that guarantees that all layers are correctly aligned.
                filler->spacing = support_params.support_material_flow.spacing();
                assert(! raft_layer.bridging);
                flow          = Flow(float(support_params.raft_interface_flow.width()), float(raft_layer.height), support_params.raft_interface_flow.nozzle_diameter());
                density       = float(support_params.raft_interface_density);
            } else
                continue;
            filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / density));
            fill_expolygons_with_sheath_generate_paths(
                // Destination
                support_layer.support_fills.entities,
                // Regions to fill
                tree_polygons.empty() ? raft_layer.polygons : diff(raft_layer.polygons, tree_polygons),
                // Filler and its parameters
                filler, density,
                // Extrusion parameters
                (support_layer_id < slicing_params.base_raft_layers) ? ExtrusionRole::erSupportMaterial : ExtrusionRole::erSupportMaterialInterface, flow,
                // sheath at first layer
                support_params, support_layer_id == 0, support_layer_id == 0);
        }
    });

    struct LayerCacheItem {
        LayerCacheItem(SupportGeneratorLayerExtruded *layer_extruded = nullptr) : layer_extruded(layer_extruded) {}
        SupportGeneratorLayerExtruded         *layer_extruded;
        std::vector<SupportGeneratorLayer*>    overlapping;
    };
    // Ultra (support groups): interface extrusions belonging to a group that pins its own interface
    // filament. They are height-modulated exactly like the layer they came from, then moved into
    // SupportLayer::interface_by_extruder instead of support_fills - which is what makes
    // ToolOrdering schedule the tool change (ToolOrdering.cpp collect_extruders).
    struct GroupInterfaceItem {
        const SupportGeneratorLayerExtruded *parent   { nullptr };  // to copy the overlapping list from
        SupportGeneratorLayer               *layer    { nullptr };
        unsigned                             extruder { 0 };        // 0-based, as the map wants
        ExtrusionEntitiesPtr                 extrusions;
        std::vector<SupportGeneratorLayer*>  overlapping;
    };
    struct LayerCache {
        SupportGeneratorLayerExtruded                                     bottom_contact_layer;
        SupportGeneratorLayerExtruded                                     top_contact_layer;
        SupportGeneratorLayerExtruded                                     base_layer;
        SupportGeneratorLayerExtruded                                     interface_layer;
        SupportGeneratorLayerExtruded                                     base_interface_layer;
        // Ultra (support groups): the sibling interface layers of groups 1..K-1 at this print_z.
        // Empty for a single-group object. unique_ptr because SupportGeneratorLayerExtruded declares
        // a move assignment and therefore has no implicit move constructor.
        std::vector<std::unique_ptr<SupportGeneratorLayerExtruded>>       extra_interface_layers;
        std::vector<std::unique_ptr<SupportGeneratorLayerExtruded>>       extra_base_interface_layers;
        std::vector<GroupInterfaceItem>                                   group_interfaces;
        // Ultra (support groups): 5 fixed slots plus up to 2 extra interface layers per group.
        // small_vector keeps the same inline capacity, so a single-group object allocates nothing.
        boost::container::small_vector<LayerCacheItem, 5>  nonempty;

        float    ironing_angle;
        Polygons polys_to_iron;
        // Ultra (support groups, Stage 5): ironing follows the group. A top contact layer is
        // SHARED between the groups, so it is split by the same claim the interface is split by,
        // and each piece is remembered with its own group index and its own interface angle; the
        // pattern, the spacing and the flow that fill it are that group's too. A group whose
        // support_ironing is off contributes no piece at all, which is exactly what leaves its
        // interface unironed while its neighbour's is ironed. Empty for a single-group object,
        // where polys_to_iron above is still what drives the one object-wide pass.
        struct GroupIroningItem {
            size_t   group { 0 };
            float    angle { 0.f };
            Polygons polys;
        };
        std::vector<GroupIroningItem> group_irons;

        void add_nonempty_and_sort() {
            for (SupportGeneratorLayerExtruded *item : { &bottom_contact_layer, &top_contact_layer, &interface_layer, &base_interface_layer, &base_layer })
                if (! item->empty())
                    this->nonempty.emplace_back(item);
            for (auto &vec : { &this->extra_interface_layers, &this->extra_base_interface_layers })
                for (auto &item : *vec)
                    if (! item->empty())
                        this->nonempty.emplace_back(item.get());
            // Sort the layers with the same print_z coordinate by their heights, thickest first.
            std::stable_sort(this->nonempty.begin(), this->nonempty.end(), [](const LayerCacheItem &lc1, const LayerCacheItem &lc2) { return lc1.layer_extruded->layer->height > lc2.layer_extruded->layer->height; });
        }
    };
    std::vector<LayerCache>             layer_caches(support_layers.size());

    tbb::parallel_for(tbb::blocked_range<size_t>(n_raft_layers, support_layers.size()),
        [&config, &slicing_params, &support_params, &support_layers, &bottom_contacts, &top_contacts, &intermediate_layers, &interface_layers, &base_interface_layers, &layer_caches, &loop_interface_processor,
            &bbox_object, &angles, n_raft_layers, link_max_length_factor, groups, object_layer_zs, num_groups, group_mode]
            (const tbb::blocked_range<size_t>& range) {
        // Indices of the 1st layer in their respective container at the support layer height.
        size_t idx_layer_bottom_contact   = size_t(-1);
        size_t idx_layer_top_contact      = size_t(-1);
        size_t idx_layer_intermediate     = size_t(-1);
        size_t idx_layer_interface        = size_t(-1);
        size_t idx_layer_base_interface   = size_t(-1);
        const auto fill_type_first_layer  = ipRectilinear;
        auto filler_interface       = std::unique_ptr<Fill>(Fill::new_from_type(support_params.contact_fill_pattern));
        // Filler for the 1st layer interface, if different from filler_interface.
        auto filler_first_layer_ptr = std::unique_ptr<Fill>(range.begin() == 0 && support_params.contact_fill_pattern != fill_type_first_layer ? Fill::new_from_type(fill_type_first_layer) : nullptr);
        // Pointer to the 1st layer interface filler.
        auto filler_first_layer     = filler_first_layer_ptr ? filler_first_layer_ptr.get() : filler_interface.get();
        // Filler for the 1st layer interface, if different from filler_interface.
        auto filler_raft_contact_ptr = std::unique_ptr<Fill>(range.begin() == n_raft_layers && config.support_interface_top_layers.value == 0 ?
            Fill::new_from_type(support_params.raft_interface_fill_pattern) : nullptr);
        // Pointer to the 1st layer interface filler.
        auto filler_raft_contact     = filler_raft_contact_ptr ? filler_raft_contact_ptr.get() : filler_interface.get();
        // Filler for the base interface (to be used for soluble interface / non soluble base, to produce non soluble interface layer below soluble interface layer).
        auto filler_base_interface  = std::unique_ptr<Fill>(base_interface_layers.empty() ? nullptr :
            Fill::new_from_type(support_params.interface_density > 0.95 || support_params.with_sheath ? ipRectilinear : ipSupportBase));
        auto filler_support         = std::unique_ptr<Fill>(Fill::new_from_type(support_params.base_fill_pattern));
        // Ultra (support groups): one interface filler per group, so each group's interface is laid
        // down with its own contact_fill_pattern. Empty for a single-group object.
        std::vector<std::unique_ptr<Fill>> filler_interface_groups;
        if (group_mode) {
            filler_interface_groups.reserve(num_groups);
            for (const SupportGroupToolpaths &grp : *groups) {
                filler_interface_groups.emplace_back(Fill::new_from_type(grp.params->contact_fill_pattern));
                filler_interface_groups.back()->set_bounding_box(bbox_object);
            }
        }
        filler_interface->set_bounding_box(bbox_object);
        if (filler_first_layer_ptr)
            filler_first_layer_ptr->set_bounding_box(bbox_object);
        if (filler_raft_contact_ptr)
            filler_raft_contact_ptr->set_bounding_box(bbox_object);
        if (filler_base_interface)
            filler_base_interface->set_bounding_box(bbox_object);
        filler_support->set_bounding_box(bbox_object);
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id)
        {
            SupportLayer &support_layer = *support_layers[support_layer_id];
            LayerCache   &layer_cache   = layer_caches[support_layer_id];
            const float   support_interface_angle = (support_params.support_style == smsGrid || config.support_interface_pattern == smipRectilinear) ?
                support_params.interface_angle : support_params.raft_interface_angle(support_layer.interface_id());

            // Find polygons with the same print_z.
            SupportGeneratorLayerExtruded &bottom_contact_layer = layer_cache.bottom_contact_layer;
            SupportGeneratorLayerExtruded &top_contact_layer    = layer_cache.top_contact_layer;
            SupportGeneratorLayerExtruded &base_layer           = layer_cache.base_layer;
            SupportGeneratorLayerExtruded &interface_layer      = layer_cache.interface_layer;
            SupportGeneratorLayerExtruded &base_interface_layer = layer_cache.base_interface_layer;
            // Increment the layer indices to find a layer at support_layer.print_z.
            {
                auto fun = [&support_layer](const SupportGeneratorLayer *l){ return l->print_z >= support_layer.print_z - EPSILON; };
                idx_layer_bottom_contact  = idx_higher_or_equal(bottom_contacts,     idx_layer_bottom_contact,  fun);
                idx_layer_top_contact     = idx_higher_or_equal(top_contacts,        idx_layer_top_contact,     fun);
                idx_layer_intermediate    = idx_higher_or_equal(intermediate_layers, idx_layer_intermediate,    fun);
                idx_layer_interface       = idx_higher_or_equal(interface_layers,    idx_layer_interface,       fun);
                idx_layer_base_interface  = idx_higher_or_equal(base_interface_layers, idx_layer_base_interface,fun);
            }
            // Copy polygons from the layers.
            if (idx_layer_bottom_contact < bottom_contacts.size() && bottom_contacts[idx_layer_bottom_contact]->print_z < support_layer.print_z + EPSILON)
                bottom_contact_layer.layer = bottom_contacts[idx_layer_bottom_contact];
            if (idx_layer_top_contact < top_contacts.size() && top_contacts[idx_layer_top_contact]->print_z < support_layer.print_z + EPSILON)
                top_contact_layer.layer = top_contacts[idx_layer_top_contact];
            if (idx_layer_interface < interface_layers.size() && interface_layers[idx_layer_interface]->print_z < support_layer.print_z + EPSILON)
                interface_layer.layer = interface_layers[idx_layer_interface];
            if (idx_layer_base_interface < base_interface_layers.size() && base_interface_layers[idx_layer_base_interface]->print_z < support_layer.print_z + EPSILON)
                base_interface_layer.layer = base_interface_layers[idx_layer_base_interface];
            // Ultra (support groups): with K groups there are up to K interface layers at one
            // print_z - one per group, produced by K calls to generate_interface_layers. The lowest
            // group's stays in layer_cache.interface_layer so every merge below behaves exactly as
            // it does today; the siblings are extruded alongside it with their own parameters.
            if (group_mode) {
                auto collect_siblings = [&support_layer](const SupportGeneratorLayersPtr &src, size_t idx_first,
                                                         std::vector<std::unique_ptr<SupportGeneratorLayerExtruded>> &dst) {
                    if (idx_first >= src.size() || src[idx_first]->print_z >= support_layer.print_z + EPSILON)
                        return;
                    for (size_t k = idx_first + 1; k < src.size() && src[k]->print_z < support_layer.print_z + EPSILON; ++ k) {
                        auto item = std::make_unique<SupportGeneratorLayerExtruded>();
                        item->layer = src[k];
                        dst.emplace_back(std::move(item));
                    }
                };
                collect_siblings(interface_layers,      idx_layer_interface,      layer_cache.extra_interface_layers);
                collect_siblings(base_interface_layers, idx_layer_base_interface, layer_cache.extra_base_interface_layers);
            }
            if (idx_layer_intermediate < intermediate_layers.size() && intermediate_layers[idx_layer_intermediate]->print_z < support_layer.print_z + EPSILON)
                base_layer.layer = intermediate_layers[idx_layer_intermediate];

            // This layer is a raft contact layer. Any contact polygons at this layer are raft contacts.
            bool raft_layer = slicing_params.interface_raft_layers && top_contact_layer.layer && is_approx(top_contact_layer.layer->print_z, slicing_params.raft_contact_top_z);
            if (config.support_interface_top_layers == 0) {
                // If no top interface layers were requested, we treat the contact layer exactly as a generic base layer.
                // Don't merge the raft contact layer though.
                if (support_params.can_merge_support_regions && ! raft_layer) {
                    if (base_layer.could_merge(top_contact_layer))
                        base_layer.merge(std::move(top_contact_layer));
                    else if (base_layer.empty())
                        base_layer = std::move(top_contact_layer);
                }
            } else {
                if (group_mode && ! top_contact_layer.empty()) {
                    // Ultra (support groups, Stage 5): the same split the interface gets, applied
                    // to the surface that is about to be ironed. It has to be taken HERE, before
                    // loop_interface_processor.generate() and before the interface layer is merged
                    // in, for the same reason the object-wide capture below is taken here: this is
                    // the last point at which the polygons are the contact's own.
                    const Polygons src = top_contact_layer.polygons_to_extrude();
                    if (! src.empty()) {
                        const size_t idx_object_layer = support_group_object_layer_index(
                            *top_contact_layer.layer, true, *object_layer_zs);
                        for (size_t g = 0; g < num_groups; ++ g) {
                            const SupportGroupToolpaths &grp = (*groups)[g];
                            if (! grp.params->ironing)
                                continue;
                            Polygons piece = support_group_piece(src, grp.claim, idx_object_layer, g);
                            if (piece.empty())
                                continue;
                            const float angle_g = (grp.params->support_style == smsGrid || grp.config->support_interface_pattern == smipRectilinear) ?
                                grp.params->interface_angle : grp.params->raft_interface_angle(support_layer.interface_id());
                            LayerCache::GroupIroningItem gi;
                            gi.group = g;
                            gi.angle = angle_g;
                            gi.polys = std::move(piece);
                            layer_cache.group_irons.push_back(std::move(gi));
                        }
                    }
                } else if (support_params.ironing && !top_contact_layer.empty()) {
                    // Orca: save the top surface to be ironed later
                    layer_cache.ironing_angle = support_interface_angle; // TODO: should we rotate 90 degrees?
                    layer_cache.polys_to_iron = top_contact_layer.polygons_to_extrude();
                }

                loop_interface_processor.generate(top_contact_layer, support_params.support_material_interface_flow);
                // If no loops are allowed, we treat the contact layer exactly as a generic interface layer.
                // Merge interface_layer into top_contact_layer, as the top_contact_layer is not synchronized and therefore it will be used
                // to trim other layers.
                if (top_contact_layer.could_merge(interface_layer) && ! raft_layer)
                    top_contact_layer.merge(std::move(interface_layer));
            }
            if ((config.support_interface_top_layers == 0 || config.support_interface_bottom_layers == 0) && support_params.can_merge_support_regions) {
                if (base_layer.could_merge(bottom_contact_layer))
                    base_layer.merge(std::move(bottom_contact_layer));
                else if (base_layer.empty() && ! bottom_contact_layer.empty() && ! bottom_contact_layer.layer->bridging)
                    base_layer = std::move(bottom_contact_layer);
            } else if (bottom_contact_layer.could_merge(top_contact_layer) && ! raft_layer)
                top_contact_layer.merge(std::move(bottom_contact_layer));
            else if (bottom_contact_layer.could_merge(interface_layer))
                bottom_contact_layer.merge(std::move(interface_layer));

#if 0
            if ( ! interface_layer.empty() && ! base_layer.empty()) {
                // turn base support into interface when it's contained in our holes
                // (this way we get wider interface anchoring)
                //FIXME The intention of the code below is unclear. One likely wanted to just merge small islands of base layers filling in the holes
                // inside interface layers, but the code below fills just too much, see GH #4570
                Polygons islands = top_level_islands(interface_layer.layer->polygons);
                polygons_append(interface_layer.layer->polygons, intersection(base_layer.layer->polygons, islands));
                base_layer.layer->polygons = diff(base_layer.layer->polygons, islands);
            }
#endif

            // Top and bottom contacts, interface layers.
            enum class InterfaceLayerType { TopContact, BottomContact, RaftContact, Interface, InterfaceAsBase };
            // Ultra (support groups, plan Stage 3 3.6 item 3): the SHARED contact layers are split
            // between the groups here, at EXTRUSION time - never in the layer graph, where K sibling
            // TopContact layers at one print_z would trip assert(num_top_contacts <= 1) in
            // generate_support_layers (see the plan's 5.2 experiment). Interface layers were already
            // produced per group by generate_interface_layers and carry their own support_group tag,
            // so they are used whole. Returns false when there is nothing group-specific to do, in
            // which case the caller runs today's single-config code unchanged.
            auto extrude_interface_grouped = [&](SupportGeneratorLayerExtruded &layer_ex, InterfaceLayerType interface_layer_type) -> bool {
                if (! group_mode || interface_layer_type == InterfaceLayerType::RaftContact)
                    return false;
                const bool interface_as_base = interface_layer_type == InterfaceLayerType::InterfaceAsBase;
                const SupporLayerType lt     = layer_ex.layer->layer_type;
                const bool shared            = lt == SupporLayerType::TopContact || lt == SupporLayerType::BottomContact;
                const size_t idx_object_layer = shared ?
                    support_group_object_layer_index(*layer_ex.layer, lt == SupporLayerType::TopContact, *object_layer_zs) :
                    size_t(-1);
                const Polygons src = layer_ex.polygons_to_extrude();
                for (size_t g = 0; g < num_groups; ++ g) {
                    const SupportGroupToolpaths &grp = (*groups)[g];
                    Polygons piece = shared ? support_group_piece(src, grp.claim, idx_object_layer, g) :
                                              (size_t(layer_ex.layer->support_group) == g ? src : Polygons());
                    if (piece.empty())
                        continue;
                    Fill *filler = filler_interface_groups[g].get();
                    auto interface_flow = layer_ex.layer->bridging ?
                        Flow::bridging_flow(layer_ex.layer->height, grp.params->support_material_bottom_interface_flow.nozzle_diameter()) :
                        (interface_as_base ? &grp.params->support_material_flow : &grp.params->support_material_interface_flow)
                            ->with_height(float(layer_ex.layer->height));
                    const float group_interface_angle = (grp.params->support_style == smsGrid || grp.config->support_interface_pattern == smipRectilinear) ?
                        grp.params->interface_angle : grp.params->raft_interface_angle(support_layer.interface_id());
                    filler->angle   = interface_as_base ? angles[support_layer_id % angles.size()] : group_interface_angle;
                    double density  = interface_as_base ? grp.params->support_density : grp.params->interface_density;
                    filler->spacing = interface_as_base ? grp.params->support_material_flow.spacing() : grp.params->support_material_interface_flow.spacing();
                    filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / density));
                    // A group that pins its own interface filament emits into its own bucket, which
                    // becomes SupportLayer::interface_by_extruder after the height modulation - the
                    // same storage the Chameleon pass uses, and the reason the two are interlocked.
                    ExtrusionEntitiesPtr *dst = &layer_ex.extrusions;
                    if (! interface_as_base && grp.interface_filament > 0 && grp.interface_filament != config.support_interface_filament.value) {
                        const unsigned extruder = unsigned(grp.interface_filament - 1);
                        size_t slot = size_t(-1);
                        for (size_t i = 0; i < layer_cache.group_interfaces.size(); ++ i)
                            if (layer_cache.group_interfaces[i].parent == &layer_ex && layer_cache.group_interfaces[i].extruder == extruder) {
                                slot = i;
                                break;
                            }
                        if (slot == size_t(-1)) {
                            GroupInterfaceItem gi;
                            gi.parent   = &layer_ex;
                            gi.layer    = layer_ex.layer;
                            gi.extruder = extruder;
                            layer_cache.group_interfaces.push_back(std::move(gi));
                            slot = layer_cache.group_interfaces.size() - 1;
                        }
                        dst = &layer_cache.group_interfaces[slot].extrusions;
                    }
                    fill_expolygons_generate_paths(
                        *dst,
                        union_safety_offset_ex(piece),
                        filler, float(density),
                        interface_as_base ? ExtrusionRole::erSupportMaterial : ExtrusionRole::erSupportMaterialInterface, interface_flow);
                }
                return true;
            };
            auto extrude_interface = [&](SupportGeneratorLayerExtruded &layer_ex, InterfaceLayerType interface_layer_type) {
                if (! layer_ex.empty() && ! layer_ex.polygons_to_extrude().empty()) {
                    if (extrude_interface_grouped(layer_ex, interface_layer_type))
                        return;
                    bool interface_as_base = interface_layer_type == InterfaceLayerType::InterfaceAsBase;
                    bool raft_contact      = interface_layer_type == InterfaceLayerType::RaftContact;
                    //FIXME Bottom interfaces are extruded with the briding flow. Some bridging layers have its height slightly reduced, therefore
                    // the bridging flow does not quite apply. Reduce the flow to area of an ellipse? (A = pi * a * b)
                    auto *filler = raft_contact ? filler_raft_contact : filler_interface.get();
                    auto interface_flow = layer_ex.layer->bridging ?
                        Flow::bridging_flow(layer_ex.layer->height, support_params.support_material_bottom_interface_flow.nozzle_diameter()) :
                        (raft_contact ? &support_params.raft_interface_flow :
                         interface_as_base ? &support_params.support_material_flow : &support_params.support_material_interface_flow)
                            ->with_height(float(layer_ex.layer->height));
                    filler->angle = interface_as_base ?
                            // If zero interface layers are configured, use the same angle as for the base layers.
                            angles[support_layer_id % angles.size()] :
                            // Use interface angle for the interface layers.
                            raft_contact ?
                                support_params.raft_interface_angle(support_layer.interface_id()) :
                                support_interface_angle;
                    double density = raft_contact ? support_params.raft_interface_density : interface_as_base ? support_params.support_density : support_params.interface_density;
                    filler->spacing = raft_contact ? support_params.raft_interface_flow.spacing() :
                        interface_as_base ? support_params.support_material_flow.spacing() : support_params.support_material_interface_flow.spacing();
                    filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / density));
                    fill_expolygons_generate_paths(
                        // Destination
                        layer_ex.extrusions,
                        // Regions to fill
                        union_safety_offset_ex(layer_ex.polygons_to_extrude()),
                        // Filler and its parameters
                        filler, float(density),
                        // Extrusion parameters
                        interface_as_base ? ExtrusionRole::erSupportMaterial : ExtrusionRole::erSupportMaterialInterface, interface_flow);
                }
            };
            const bool top_interfaces = support_params.num_top_interface_layers > 0;
            const bool bottom_interfaces = top_interfaces && support_params.num_bottom_interface_layers > 0;
            extrude_interface(top_contact_layer,    raft_layer ? InterfaceLayerType::RaftContact : top_interfaces ? InterfaceLayerType::TopContact : InterfaceLayerType::InterfaceAsBase);
            extrude_interface(bottom_contact_layer, bottom_interfaces ? InterfaceLayerType::BottomContact : InterfaceLayerType::InterfaceAsBase);
            extrude_interface(interface_layer,      top_interfaces ? InterfaceLayerType::Interface : InterfaceLayerType::InterfaceAsBase);
            // Ultra (support groups): the sibling interface layers produced for the other groups at
            // this print_z. Each carries its own support_group tag, so extrude_interface_grouped
            // fills it with that group's pattern, density, flow and angle. Empty at K == 1.
            if (group_mode)
                for (auto &extra : layer_cache.extra_interface_layers)
                    extrude_interface(*extra, top_interfaces ? InterfaceLayerType::Interface : InterfaceLayerType::InterfaceAsBase);

            // Base interface layers under soluble interfaces
            auto extrude_base_interface = [&](SupportGeneratorLayerExtruded &layer_ex) {
                if (layer_ex.empty() || layer_ex.polygons_to_extrude().empty())
                    return;
                Fill *filler = filler_base_interface.get();
                //FIXME Bottom interfaces are extruded with the briding flow. Some bridging layers have its height slightly reduced, therefore
                // the bridging flow does not quite apply. Reduce the flow to area of an ellipse? (A = pi * a * b)
                assert(! layer_ex.layer->bridging);
                // Ultra (support groups): a base interface layer belongs wholly to the group that
                // produced it - it is never shared - so it simply uses that group's parameters.
                // At K == 1 params_g IS support_params and this is the code that was here before.
                const SupportParameters &params_g = (group_mode && size_t(layer_ex.layer->support_group) < num_groups) ?
                    *(*groups)[layer_ex.layer->support_group].params : support_params;
                Flow interface_flow = params_g.support_material_flow.with_height(float(layer_ex.layer->height));
                filler->angle   = support_interface_angle;
                filler->spacing = params_g.support_material_interface_flow.spacing();
                filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / params_g.interface_density));
                fill_expolygons_generate_paths(
                    // Destination
                    layer_ex.extrusions,
                    // Regions to fill
                    union_safety_offset_ex(layer_ex.polygons_to_extrude()),
                    // Filler and its parameters
                    filler, float(params_g.interface_density),
                    // Extrusion parameters
                    ExtrusionRole::erSupportMaterial, interface_flow);
            };
            extrude_base_interface(base_interface_layer);
            if (group_mode)
                for (auto &extra : layer_cache.extra_base_interface_layers)
                    extrude_base_interface(*extra);

            // Base support or flange.
            if (! base_layer.empty() && ! base_layer.polygons_to_extrude().empty()) {
                Fill             *filler          = filler_support.get();
                filler->angle = angles[support_layer_id % angles.size()];
                // We don't use $base_flow->spacing because we need a constant spacing
                // value that guarantees that all layers are correctly aligned.
                assert(! base_layer.layer->bridging);
                auto flow = support_params.support_material_flow.with_height(float(base_layer.layer->height));
                filler->spacing = support_params.support_material_flow.spacing();
                filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / support_params.support_density));
                float density = float(support_params.support_density);
                bool  sheath  = support_params.with_sheath;
                bool  no_sort = false;
                bool  done    = false;
                if (base_layer.layer->bottom_z < EPSILON) {
                    // Base flange (the 1st layer).
                    filler = filler_first_layer;
                    filler->angle = Geometry::deg2rad(float(config.support_angle.value + 90.));
                    density = float(config.raft_first_layer_density.value * 0.01);
                    flow = support_params.first_layer_flow;
                    // use the proper spacing for first layer as we don't need to align
                    // its pattern to the other layers
                    //FIXME When paralellizing, each thread shall have its own copy of the fillers.
                    filler->spacing = flow.spacing();
                    filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / density));
                    sheath  = true;
                    no_sort = true;
                } else if (support_params.support_style == SupportMaterialStyle::smsTreeOrganic) {
                    // if the tree supports are too tall, use double wall to make it stronger
                    SupportParameters support_params2 = support_params;
                    if (support_layer.print_z > 100.0)
                        support_params2.tree_branch_diameter_double_wall_area_scaled = 0.1;
                    tree_supports_generate_paths(base_layer.extrusions, base_layer.polygons_to_extrude(), flow, support_params2);
                    done = true;
                }
                if (! done)
                    fill_expolygons_with_sheath_generate_paths(
                        // Destination
                        base_layer.extrusions,
                        // Regions to fill
                        base_layer.polygons_to_extrude(),
                        // Filler and its parameters
                        filler, density,
                        // Extrusion parameters
                        ExtrusionRole::erSupportMaterial, flow,
                        support_params, sheath, no_sort);
            }

            // Merge base_interface_layers to base_layers to avoid unneccessary retractions
            if (! base_layer.empty() && ! base_interface_layer.empty() && ! base_layer.polygons_to_extrude().empty() && ! base_interface_layer.polygons_to_extrude().empty() &&
                base_layer.could_merge(base_interface_layer))
                base_layer.merge(std::move(base_interface_layer));

            layer_cache.add_nonempty_and_sort();

            // Collect the support areas with this print_z into islands, as there is no need
            // for retraction over these islands.
            Polygons polys;
            // Collect the extrusions, sorted by the bottom extrusion height.
            for (LayerCacheItem &layer_cache_item : layer_cache.nonempty) {
                // Collect islands to polys.
                layer_cache_item.layer_extruded->polygons_append(polys);
                // The print_z of the top contact surfaces and bottom_z of the bottom contact surfaces are "free"
                // in a sense that they are not synchronized with other support layers. As the top and bottom contact surfaces
                // are inflated to achieve a better anchoring, it may happen, that these surfaces will at least partially
                // overlap in Z with another support layers, leading to over-extrusion.
                // Mitigate the over-extrusion by modulating the extrusion rate over these regions.
                // The print head will follow the same print_z, but the layer thickness will be reduced
                // where it overlaps with another support layer.
                //FIXME When printing a briging path, what is an equivalent height of the squished extrudate of the same width?
                // Collect overlapping top/bottom surfaces.
                layer_cache_item.overlapping.reserve(20);
                coordf_t bottom_z = layer_cache_item.layer_extruded->layer->bottom_print_z() + EPSILON;
                auto add_overlapping = [&layer_cache_item, bottom_z](const SupportGeneratorLayersPtr &layers, size_t idx_top) {
                    for (int i = int(idx_top) - 1; i >= 0 && layers[i]->print_z > bottom_z; -- i)
                        layer_cache_item.overlapping.push_back(layers[i]);
                };
                add_overlapping(top_contacts, idx_layer_top_contact);
                if (layer_cache_item.layer_extruded->layer->layer_type == SupporLayerType::BottomContact) {
                    // Bottom contact layer may overlap with a base layer, which may be changed to interface layer.
                    add_overlapping(intermediate_layers,   idx_layer_intermediate);
                    add_overlapping(interface_layers,      idx_layer_interface);
                    add_overlapping(base_interface_layers, idx_layer_base_interface);
                }
                // Order the layers by lexicographically by an increasing print_z and a decreasing layer height.
                std::stable_sort(layer_cache_item.overlapping.begin(), layer_cache_item.overlapping.end(), [](auto *l1, auto *l2) { return *l1 < *l2; });
            }
            // Ultra (support groups): a group's own-filament interface came out of one of the layers
            // above, so it is modulated against exactly that layer's overlapping set.
            for (GroupInterfaceItem &gi : layer_cache.group_interfaces)
                for (const LayerCacheItem &item : layer_cache.nonempty)
                    if (item.layer_extruded == gi.parent) {
                        gi.overlapping = item.overlapping;
                        break;
                    }
            assert(support_layer.support_islands.empty());
            if (! polys.empty()) {
                support_layer.support_islands = union_ex(polys);
                // support_layer.support_islands_bboxes.reserve(support_layer.support_islands.size());
                // for (const ExPolygon &expoly : support_layer.support_islands)
                //     support_layer.support_islands_bboxes.emplace_back(get_extents(expoly).inflated(SCALED_EPSILON));
            }
        } // for each support_layer_id
    });

    // Now modulate the support layer height in parallel.
    tbb::parallel_for(tbb::blocked_range<size_t>(n_raft_layers, support_layers.size()),
        [&support_layers, &layer_caches, &support_params, &bbox_object, &config, groups]
            (const tbb::blocked_range<size_t>& range) {
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id) {
            SupportLayer &support_layer = *support_layers[support_layer_id];
            LayerCache   &layer_cache   = layer_caches[support_layer_id];
            // For all extrusion types at this print_z, ordered by decreasing layer height:
            for (LayerCacheItem &layer_cache_item : layer_cache.nonempty) {
                // Trim the extrusion height from the bottom by the overlapping layers.
                modulate_extrusion_by_overlapping_layers(layer_cache_item.layer_extruded->extrusions, *layer_cache_item.layer_extruded->layer, layer_cache_item.overlapping);
                support_layer.support_fills.append(std::move(layer_cache_item.layer_extruded->extrusions));
            }

            // Ultra (support groups): the height modulation is applied first, exactly as the plan's
            // 3.6 item 4 asks, and only then does the group's interface leave support_fills for
            // SupportLayer::interface_by_extruder. From there ToolOrdering::collect_extruders
            // registers the extruder on this layer and GCode::process_layer emits it after the
            // toolchange - nothing new is needed in ToolOrdering, the wipe tower or the G-code writer.
            for (GroupInterfaceItem &gi : layer_cache.group_interfaces) {
                if (gi.extrusions.empty())
                    continue;
                modulate_extrusion_by_overlapping_layers(gi.extrusions, *gi.layer, gi.overlapping);
                support_layer.interface_by_extruder[gi.extruder].append(std::move(gi.extrusions));
            }

            // Orca: Generate iron toolpath for contact layer
            // Ultra (support groups, Stage 5): one routine, called once for a single-group object
            // and once per group otherwise. Everything inside it but the destination, the angle and
            // the SupportParameters is the code that was here before.
            auto generate_ironing = [&support_layers, &support_layer, &bbox_object, support_layer_id]
                                    (Polygons &src, float angle, const SupportParameters &params,
                                     ExtrusionEntitiesPtr &dst) {
                if (src.empty())
                    return;
                auto f = std::unique_ptr<Fill>(Fill::new_from_type(params.ironing_pattern));
                f->set_bounding_box(bbox_object);
                f->layer_id        = support_layer.id();
                f->z               = support_layer.print_z;
                f->overlap         = 0;
                f->angle           = angle;
                f->spacing         = params.ironing_spacing;
                f->link_max_length = (coord_t) scale_(3. * f->spacing);

                ExPolygons polys_to_iron = union_safety_offset_ex(src);
                src.clear();

                // Find the layer above that directly overlaps current layer, clip the overlapped part
                if (support_layer_id < support_layers.size() - 1) {
                    const auto& upper_layer = support_layers[support_layer_id + 1];
                    if (!upper_layer->support_islands.empty() && upper_layer->bottom_z() <= support_layer.print_z + EPSILON) {
                        polys_to_iron = diff_ex(polys_to_iron, upper_layer->support_islands);
                    }
                }

                fill_expolygons_generate_paths(
                    // Destination
                    dst,
                    // Regions to fill
                    std::move(polys_to_iron),
                    // Filler and its parameters
                    f.get(), 1.f,
                    // Extrusion parameters
                    ExtrusionRole::erIroning, params.ironing_flow);
            };
            generate_ironing(layer_cache.polys_to_iron, layer_cache.ironing_angle, support_params,
                             support_layer.support_fills.entities);
            for (LayerCache::GroupIroningItem &gi : layer_cache.group_irons) {
                const SupportGroupToolpaths &grp = (*groups)[gi.group];
                // A group that pins its own interface filament irons its own interface with that
                // filament: the ironed surface IS that interface, and interface_by_extruder is
                // where the G-code writer looks for it (GCode.cpp extrudes erIroning out of that
                // map in a second pass, which is how the Chameleon pass already gets ironing to
                // follow the interface it belongs to). Every other group irons in support_fills,
                // i.e. with the object's own support filament, exactly as before.
                ExtrusionEntitiesPtr *dst = &support_layer.support_fills.entities;
                if (grp.interface_filament > 0 && grp.interface_filament != config.support_interface_filament.value)
                    dst = &support_layer.interface_by_extruder[unsigned(grp.interface_filament - 1)].entities;
                generate_ironing(gi.polys, gi.angle, *grp.params, *dst);
            }
        }
    });

#ifndef NDEBUG
    struct Test {
        static bool verify_nonempty(const ExtrusionEntityCollection *collection) {
            for (const ExtrusionEntity *ee : collection->entities) {
                if (const ExtrusionPath *path = dynamic_cast<const ExtrusionPath*>(ee))
                    assert(! path->empty());
                else if (const ExtrusionMultiPath *multipath = dynamic_cast<const ExtrusionMultiPath*>(ee))
                    assert(! multipath->empty());
                else if (const ExtrusionEntityCollection *eecol = dynamic_cast<const ExtrusionEntityCollection*>(ee)) {
                    assert(! eecol->empty());
                    return verify_nonempty(eecol);
                } else
                    assert(false);
            }
            return true;
        }
    };
    for (const SupportLayer *support_layer : support_layers)
        assert(Test::verify_nonempty(&support_layer->support_fills));
#endif // NDEBUG
}

/*
void PrintObjectSupportMaterial::clip_by_pillars(
    const PrintObject   &object,
    LayersPtr           &bottom_contacts,
    LayersPtr           &top_contacts,
    LayersPtr           &intermediate_contacts);

{
    // this prevents supplying an empty point set to BoundingBox constructor
    if (top_contacts.empty())
        return;

    coord_t pillar_size    = scale_(PILLAR_SIZE);
    coord_t pillar_spacing = scale_(PILLAR_SPACING);

    // A regular grid of pillars, filling the 2D bounding box.
    Polygons grid;
    {
        // Rectangle with a side of 2.5x2.5mm.
        Polygon pillar;
        pillar.points.push_back(Point(0, 0));
        pillar.points.push_back(Point(pillar_size, 0));
        pillar.points.push_back(Point(pillar_size, pillar_size));
        pillar.points.push_back(Point(0, pillar_size));

        // 2D bounding box of the projection of all contact polygons.
        BoundingBox bbox;
        for (LayersPtr::const_iterator it = top_contacts.begin(); it != top_contacts.end(); ++ it)
            bbox.merge(get_extents((*it)->polygons));
        grid.reserve(size_t(ceil(bb.size()(0) / pillar_spacing)) * size_t(ceil(bb.size()(1) / pillar_spacing)));
        for (coord_t x = bb.min(0); x <= bb.max(0) - pillar_size; x += pillar_spacing) {
            for (coord_t y = bb.min(1); y <= bb.max(1) - pillar_size; y += pillar_spacing) {
                grid.push_back(pillar);
                for (size_t i = 0; i < pillar.points.size(); ++ i)
                    grid.back().points[i].translate(Point(x, y));
            }
        }
    }

    // add pillars to every layer
    for my $i (0..n_support_z) {
        $shape->[$i] = [ @$grid ];
    }

    // build capitals
    for my $i (0..n_support_z) {
        my $z = $support_z->[$i];

        my $capitals = intersection(
            $grid,
            $contact->{$z} // [],
        );

        // work on one pillar at time (if any) to prevent the capitals from being merged
        // but store the contact area supported by the capital because we need to make
        // sure nothing is left
        my $contact_supported_by_capitals = [];
        foreach my $capital (@$capitals) {
            // enlarge capital tops
            $capital = offset([$capital], +($pillar_spacing - $pillar_size)/2);
            push @$contact_supported_by_capitals, @$capital;

            for (my $j = $i-1; $j >= 0; $j--) {
                my $jz = $support_z->[$j];
                $capital = offset($capital, -$self->interface_flow->scaled_width/2);
                last if !@$capitals;
                push @{ $shape->[$j] }, @$capital;
            }
        }

        // Capitals will not generally cover the whole contact area because there will be
        // remainders. For now we handle this situation by projecting such unsupported
        // areas to the ground, just like we would do with a normal support.
        my $contact_not_supported_by_capitals = diff(
            $contact->{$z} // [],
            $contact_supported_by_capitals,
        );
        if (@$contact_not_supported_by_capitals) {
            for (my $j = $i-1; $j >= 0; $j--) {
                push @{ $shape->[$j] }, @$contact_not_supported_by_capitals;
            }
        }
    }
}

sub clip_with_shape {
    my ($self, $support, $shape) = @_;

    foreach my $i (keys %$support) {
        // don't clip bottom layer with shape so that we
        // can generate a continuous base flange
        // also don't clip raft layers
        next if $i == 0;
        next if $i < $self->object_config->raft_layers;
        $support->{$i} = intersection(
            $support->{$i},
            $shape->[$i],
        );
    }
}
*/

} // namespace Slic3r
