#include "GLGizmoEdit.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Jobs/BevelJob.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MeshSculpt.hpp"
// Ultra: voxel_ops_available() gates the "Round all edges..." button in the panel.
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <glad/gl.h>

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Slic3r::GUI {

// The same z-fight offset GLGizmoMeasure uses for its plane overlay, so the two
// gizmos' highlights sit at the same height above the part.
static constexpr float EditHighlightOffset = 0.05f;
// A chain is drawn as a thin triangular prism per segment, in the spirit of the
// Measure gizmo's cylinder-per-segment edges, but built through init_plane_data's
// own vertex layout so there is one rendering path for both kinds of highlight.
// The radius is a fraction of the mesh's bounding-box diagonal so a chain is
// visible on a 5 mm part and not a slab on a 300 mm one.
static constexpr float EditChainRadiusFraction = 0.0025f;

GLGizmoEdit::GLGizmoEdit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

GLGizmoEdit::~GLGizmoEdit() = default;

bool GLGizmoEdit::on_init()
{
    m_shortcut_key = 0;

    const wxString ctrl = _L("Ctrl+");

    m_desc["mode"]              = _L("Select");
    m_desc["mode_face"]         = _L("Face");
    m_desc["mode_smooth_face"]  = _L("Curved face");
    m_desc["mode_chain"]        = _L("Edge chain");
    m_desc["mode_face_hint"]    = _L("Click a face: the whole flat face is selected.");
    m_desc["mode_smooth_hint"]  = _L("Click a face: a gently curved face is selected whole instead of one triangle at a time.");
    m_desc["mode_chain_hint"]   = _L("Hover an edge: the whole edge chain lights up. Select it to chamfer or bevel it.");
    m_desc["feature_angle"]     = _L("Edge angle");
    m_desc["feature_angle_hint"]= _L("An edge counts as an edge when the two faces meeting at it turn by more than this.");
    m_desc["planar_tol"]        = _L("Face tolerance");
    m_desc["smooth_step"]       = _L("Curvature per step");
    m_desc["smooth_cap"]        = _L("Total curvature");
    m_desc["push"]              = _L("Push / pull");
    m_desc["distance"]          = _L("Distance");
    m_desc["snap"]              = _L("Snap");
    m_desc["snap_step"]         = _L("Step");
    m_desc["apply"]             = _L("Apply");
    m_desc["reset_selection"]   = _L("Clear selection");
    m_desc["undo"]              = _L("Undo");
    m_desc["redo"]              = _L("Redo");
    m_desc["undo_caption"]      = ctrl + _L("Z");
    m_desc["redo_caption"]      = ctrl + _L("Y");
    m_desc["no_part"]           = _L("Select a single part to edit it.");
    m_desc["no_selection"]      = _L("Click a face on the part to select it, then drag the arrow or type a distance.");
    m_desc["chain_selected"]    = _L("Edge chain selected. Set a width and press Apply to bevel it.");
    // --- bevel / chamfer (phase 2) ---
    m_desc["bevel"]             = _L("Bevel");
    m_desc["bevel_width"]       = _L("Width");
    m_desc["bevel_segments"]    = _L("Segments");
    m_desc["bevel_profile"]     = _L("Profile");
    m_desc["bevel_chamfer"]     = _L("Chamfer");
    m_desc["bevel_round"]       = _L("Round");
    m_desc["bevel_apply"]       = _L("Apply bevel");
    m_desc["bevel_hint"]        = _L("The selected edges are replaced by a flat band (chamfer) or an arc of the chosen number of segments (round). Where chains meet, a corner patch closes the join.");
    m_desc["bevel_chamfer_hint"]= _L("A chamfer is a single flat band, so the segment count does not apply to it.");
    m_desc["bevel_clamped"]     = _L("Width clamped to %1% mm: the faces next to the selection are too small for the value you asked for.");
    m_desc["bevel_corners"]     = _L("%1% corner patch(es)");
    m_desc["bevel_cleared"]     = _L("A bevel adds triangles, so painted supports, seams, colours and fuzzy skin are cleared when it is applied.");
    m_desc["bevel_err_manifold"]= _L("That selection contains an edge shared by more than two faces, which cannot be bevelled. Repair the part first.");
    m_desc["bevel_err_small"]   = _L("The width does not fit anywhere on this selection. Try a smaller value.");
    m_desc["bevel_err_flat"]    = _L("Those edges are too flat to bevel.");
    m_desc["bevel_err_concave"] = _L("Inside corners are not bevelled yet. Use \"Round all edges\" to fillet them, or select an outside edge.");
    m_desc["bevel_err_failed"]  = _L("The bevel did not produce a valid solid, so nothing was changed. Try a smaller width or fewer segments.");
    // The curved-surface refusal. This is the case that used to hang: the chain
    // runs across a triangulated curved surface, whose "faces" are thousands of
    // tiny facets with no common plane, and the bevel needs planar faces.
    m_desc["bevel_err_curved"]  = _L("This chain runs across %1% faces of a curved surface; bevel needs planar faces.");
    // Which construction produced the result. Said plainly rather than in the
    // module's own words: "geometric" means the exact facets were inserted and the
    // rest of the part is untouched, "voxel" means the band around the chain was
    // rebuilt from a distance field and fine detail inside that band is gone.
    m_desc["bevel_path_geom"]   = _L("Built geometrically: exact facets, the rest of the part untouched.");
    m_desc["bevel_path_voxel"]  = _L("Built by rounding a band around the chain, because this surface is "
                                     "too curved to bevel exactly. Detail inside the band is smoothed.");
    m_desc["bevel_working"]     = _L("Bevelling...");
    m_desc["bevel_cancel"]      = _L("Cancel");
    m_desc["bevel_busy"]        = _L("Another background task is running. Wait for it to finish, then apply the bevel.");
    m_desc["paint_kept"]        = _L("Pushing a face keeps painted supports, seams, colours and fuzzy skin: it moves points and adds no triangles.");
    m_desc["drag_hint"]         = _L("Drag the arrow to push the face along its normal.");
    m_desc["selected_info"]     = _L("Selected: %1% triangles, %2% mm2");
    m_desc["chain_info"]        = _L("Selected: %1% edges%2%");
    m_desc["chain_closed"]      = _L(" (closed loop)");
    m_desc["err_self"]          = _L("That push would make the part intersect itself, so it was not applied. Try a smaller distance.");
    m_desc["err_bounds"]        = _L("That distance is larger than the part, so it was not applied.");
    m_desc["err_whole"]         = _L("That selection is the whole part. Use the Move gizmo to move a whole part.");
    m_desc["err_empty"]         = _L("Nothing is selected.");
    // Ultra: the interim whole-mesh fillet, offered here as well as in the object
    // menu because "round the edges" is the first thing a user reaches for once
    // they are already in the Edit gizmo.
    m_desc["round_all"]         = _L("Round all edges...");
    m_desc["round_all_hint"]    = _L("Fillet every edge of the part at once by a radius. This rebuilds the whole part, so painted data is cleared and the gizmo closes.");
    m_desc["cad_fillet"]        = _L("Exact fillet / chamfer (CAD)...");
    m_desc["cad_fillet_hint"]   = _L("Geometrically exact fillets, chamfers and shells on the part's CAD body. Best for parts imported from STEP; a mesh is converted first.");

    return true;
}

std::string GLGizmoEdit::on_get_name() const { return _u8L("Edit"); }

std::string GLGizmoEdit::get_gizmo_entering_text() const { return _u8L("Entering Edit gizmo"); }
std::string GLGizmoEdit::get_gizmo_leaving_text() const { return _u8L("Leaving Edit gizmo"); }

CommonGizmosDataID GLGizmoEdit::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

bool GLGizmoEdit::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return false;
    // One part at a time, as Sculpt does: the session owns one mesh.
    return selection.get_volume_idxs().size() == 1;
}

// ----------------------------------------------------------------------------
// selection / session
// ----------------------------------------------------------------------------

ModelVolume *GLGizmoEdit::selected_volume(int &object_idx, int &volume_idx, int &mesh_id) const
{
    object_idx = volume_idx = mesh_id = -1;

    const Selection &selection = m_parent.get_selection();
    const Selection::IndicesList &idxs = selection.get_volume_idxs();
    if (idxs.size() != 1)
        return nullptr;
    const GLVolume *gl_volume = selection.get_volume(*idxs.begin());
    if (gl_volume == nullptr)
        return nullptr;

    const GLVolume::CompositeID &cid = gl_volume->composite_id;
    const ModelObjectPtrs &objects = wxGetApp().model().objects;
    if (cid.object_id < 0 || objects.size() <= size_t(cid.object_id))
        return nullptr;
    ModelObject *object = objects[cid.object_id];
    if (cid.volume_id < 0 || object->volumes.size() <= size_t(cid.volume_id))
        return nullptr;
    ModelVolume *mv = object->volumes[cid.volume_id];
    if (!mv->is_model_part())
        return nullptr;

    // The shared Raycaster indexes only the model-part volumes, in order.
    int counter = -1;
    for (int i = 0; i <= cid.volume_id; ++i)
        if (object->volumes[i]->is_model_part())
            ++counter;

    object_idx = cid.object_id;
    volume_idx = cid.volume_id;
    mesh_id    = counter;
    return mv;
}

void GLGizmoEdit::attach_to_selection()
{
    int object_idx = -1, volume_idx = -1, mesh_id = -1;
    ModelVolume *mv = selected_volume(object_idx, volume_idx, mesh_id);
    if (mv == nullptr) {
        detach();
        return;
    }
    if (m_volume == mv && m_session && m_volume_id == mv->id())
        return;

    m_volume     = mv;
    m_volume_id  = mv->id();
    m_object_idx = object_idx;
    m_volume_idx = volume_idx;
    m_mesh_id    = mesh_id;
    m_session    = std::make_unique<MeshEdit::EditSession>(mv->mesh().its);
    clear_selection();
}

void GLGizmoEdit::detach()
{
    m_volume     = nullptr;
    m_object_idx = m_volume_idx = m_mesh_id = -1;
    m_session.reset();
    m_dragging   = false;
    clear_selection();
}

void GLGizmoEdit::data_changed(bool /* is_serializing */)
{
    if (m_state != On)
        return;
    attach_to_selection();
}

void GLGizmoEdit::on_set_state()
{
    if (m_state == On) {
        attach_to_selection();
    } else {
        if (m_dragging)
            cancel_drag();
        detach();
    }
}

Transform3d GLGizmoEdit::volume_trafo() const
{
    const Selection &selection = m_parent.get_selection();
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || m_volume == nullptr)
        return Transform3d::Identity();
    const int instance_idx = selection.get_instance_idx();
    if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
        return Transform3d::Identity();
    return mo->instances[instance_idx]->get_transformation().get_matrix() * m_volume->get_matrix();
}

double GLGizmoEdit::mesh_scale() const
{
    // The push distance is a world-space length; the mesh lives in the volume's
    // own space. As in Sculpt, a roughly uniform scale is assumed and the mean of
    // the three factors is used.
    const Vec3d s = Geometry::Transformation(volume_trafo()).get_scaling_factor();
    const double mean = (std::abs(s.x()) + std::abs(s.y()) + std::abs(s.z())) / 3.;
    return mean > EPSILON ? mean : 1.;
}

// ----------------------------------------------------------------------------
// picking
// ----------------------------------------------------------------------------

bool GLGizmoEdit::raycast(const Vec2d &mouse_position, Vec3f &hit, Vec3f &normal, size_t &facet) const
{
    if (m_volume == nullptr || m_mesh_id < 0 || m_c == nullptr || m_c->raycaster() == nullptr)
        return false;
    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    if (size_t(m_mesh_id) >= raycasters.size())
        return false;

    const Camera &camera = wxGetApp().plater()->get_camera();
    return raycasters[m_mesh_id]->unproject_on_mesh(mouse_position, volume_trafo(), camera, hit, normal, nullptr, &facet);
}

MeshEdit::RegionParams GLGizmoEdit::region_params() const
{
    MeshEdit::RegionParams p;
    if (m_pick_mode == PickMode::SmoothFace) {
        p.mode           = MeshEdit::RegionMode::Smooth;
        p.step_angle_deg = m_smooth_step;
        p.angle_tol_deg  = m_smooth_cap;
    } else {
        p.mode          = MeshEdit::RegionMode::Planar;
        p.angle_tol_deg = m_planar_tol;
    }
    return p;
}

void GLGizmoEdit::update_hover(const Vec2d &mouse_position)
{
    m_last_mouse = mouse_position;
    if (!m_session || m_volume == nullptr) {
        m_hover_valid = false;
        return;
    }

    Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
    size_t facet = 0;
    m_hover_valid = raycast(mouse_position, hit, normal, facet);
    if (!m_hover_valid) {
        m_hover_region = MeshEdit::FaceRegion{};
        m_hover_chain  = MeshEdit::EdgeChain{};
        m_hover_region_key = m_hover_chain_key = -1;
        return;
    }
    m_hover_point = hit;
    m_hover_facet = facet;

    if (m_pick_mode == PickMode::EdgeChain) {
        m_hover_region = MeshEdit::FaceRegion{};
        m_hover_region_key = -1;
        const int edge = m_session->nearest_edge(facet, hit);
        if (edge != m_hover_chain_key) {
            m_hover_chain     = m_session->grow_chain(edge, m_feature_angle, m_chain_continuation);
            m_hover_chain_key = m_hover_chain.empty() ? -1 : edge;
            m_hover_chain_model.reset();
        }
    } else {
        m_hover_chain = MeshEdit::EdgeChain{};
        m_hover_chain_key = -1;
        // The cache key is the seed facet, so a hover that stays on the same
        // facet rebuilds nothing. A hover that moves to a different facet of the
        // SAME region does re-grow it - the grow is bounded and cheap, and
        // keying on the region itself would need a facet -> region map that
        // phase 1 does not otherwise want.
        if (int(facet) != m_hover_region_key) {
            m_hover_region     = m_session->grow_region(facet, region_params());
            m_hover_region_key = m_hover_region.empty() ? -1 : int(facet);
            m_hover_region_model.reset();
        }
    }
}

void GLGizmoEdit::commit_hover_to_selection()
{
    if (!m_hover_valid)
        return;
    if (m_pick_mode == PickMode::EdgeChain) {
        if (m_hover_chain.empty())
            return;
        m_selected_chain       = m_hover_chain;
        m_selected_chain_key   = m_hover_chain_key;
        m_selected_region      = MeshEdit::FaceRegion{};
        m_selection_is_chain   = true;
    } else {
        if (m_hover_region.empty())
            return;
        m_selected_region      = m_hover_region;
        m_selected_region_key  = m_hover_region_key;
        m_selected_chain       = MeshEdit::EdgeChain{};
        m_selection_is_chain   = false;
    }
    m_has_selection    = true;
    m_push_distance    = 0.f;
    m_show_last_status = false;
    m_selected_region_model.reset();
    m_selected_chain_model.reset();
}

void GLGizmoEdit::clear_selection()
{
    m_has_selection      = false;
    m_selection_is_chain = false;
    m_selected_region    = MeshEdit::FaceRegion{};
    m_selected_chain     = MeshEdit::EdgeChain{};
    m_hover_region       = MeshEdit::FaceRegion{};
    m_hover_chain        = MeshEdit::EdgeChain{};
    m_push_distance      = 0.f;
    m_show_last_status   = false;
    // A bevel preview belongs to a selection; dropping the selection has to drop
    // the preview too, or the renderer keeps showing a bevel of edges that are no
    // longer selected.
    m_show_bevel_status = false;
    clear_bevel_preview();
    invalidate_highlight_models();
}

void GLGizmoEdit::invalidate_highlight_models()
{
    m_hover_region_model.reset();
    m_selected_region_model.reset();
    m_hover_chain_model.reset();
    m_selected_chain_model.reset();
    m_hover_region_key = m_selected_region_key = -1;
    m_hover_chain_key  = m_selected_chain_key  = -1;
}

// ----------------------------------------------------------------------------
// push / pull
// ----------------------------------------------------------------------------

// The push axis in WORLD space: the region's normal carried through the volume
// transform. A non-uniform scale skews normals, hence the inverse-transpose.
static Vec3d push_axis_world(const Transform3d &trafo, const Vec3f &normal_mesh)
{
    const Matrix3d nm = trafo.matrix().block(0, 0, 3, 3).inverse().transpose();
    Vec3d axis = nm * normal_mesh.cast<double>();
    const double l = axis.norm();
    return l > EPSILON ? Vec3d(axis / l) : Vec3d::UnitZ();
}

bool GLGizmoEdit::drag_distance(const Vec2d &mouse_position, float &out) const
{
    // Project the mouse ray onto the LINE through the drag anchor along the push
    // axis, and report how far along that line the closest approach is. That is
    // the standard "drag along an axis" solve the Move gizmo uses, and unlike
    // projecting onto a camera-facing plane it stays stable when the axis points
    // nearly at the camera.
    const Linef3 ray = m_parent.mouse_ray(Point(int(mouse_position.x()), int(mouse_position.y())));
    const Vec3d  rd  = ray.b - ray.a;
    const Vec3d &ad  = m_drag_axis_world;

    const double a = ad.dot(ad);
    const double b = ad.dot(rd);
    const double c = rd.dot(rd);
    const Vec3d  w = m_drag_anchor_world - ray.a;
    const double d = ad.dot(w);
    const double e = rd.dot(w);

    const double denom = a * c - b * b;
    if (std::abs(denom) < 1e-12)
        return false;
    // Root cause of the reversed push/pull direction: this is the standard
    // closest-point-between-two-lines solve (line 1 = the mouse ray, from
    // ray.a along rd; line 2 = the drag axis, from m_drag_anchor_world along
    // ad), solved for t in Q(t) = m_drag_anchor_world + t*ad. Cramer's rule on
    // the 2x2 system built from a,b,c,d,e above gives t = (b*e - c*d) / denom.
    // The previous (d*c - b*e)/denom is exactly the negation of that - it
    // reports how far BACK along the axis the ray's closest approach is,
    // rather than how far forward - so every drag produced a distance with
    // the opposite sign of the true one: dragging along the arrow's drawn
    // direction (which points along +m_drag_axis_world, the outward normal)
    // measured as a NEGATIVE t, which read as "push inward" instead of "pull
    // outward". Since MeshEdit::translate_region moves the region by
    // `region.normal * params.d` (positive d = outward, confirmed by
    // reading translate_region directly), the fix belongs here in the
    // projection, not in the mesh translate or the arrow's drawn direction -
    // both of those already agree with each other and with the intended
    // convention.
    const double t = (b * e - c * d) / denom;   // distance along the axis, world mm
    out = float(t);
    return true;
}

bool GLGizmoEdit::begin_drag(const Vec2d &mouse_position)
{
    if (!m_session || !m_has_selection || m_selection_is_chain || m_selected_region.empty())
        return false;

    m_drag_base_mesh = m_session->mesh();
    m_drag_region    = m_selected_region;
    const Transform3d trafo = volume_trafo();
    m_drag_axis_world  = push_axis_world(trafo, m_drag_region.normal);
    m_drag_anchor_world = trafo * m_drag_region.center.cast<double>();

    float t = 0.f;
    if (!drag_distance(mouse_position, t))
        return false;
    m_drag_start_distance = t;
    m_drag_distance       = 0.f;
    m_dragging            = true;
    m_show_last_status    = false;
    return true;
}

void GLGizmoEdit::update_drag(const Vec2d &mouse_position)
{
    if (!m_dragging)
        return;
    float t = 0.f;
    if (!drag_distance(mouse_position, t))
        return;
    float d = t - m_drag_start_distance;
    if (m_push_snap)
        d = MeshEdit::snap_to_step(d, m_push_step);
    if (d == m_drag_distance)
        return;
    m_drag_distance = d;
    m_push_distance = d;
    // Preview only: no self-intersection test (too slow per tick), no undo
    // entry, no commit to the Model. The drag is absolute, so the whole
    // displacement is re-applied to the pre-drag mesh every tick.
    apply_push(d, /* final_apply */ false);
}

void GLGizmoEdit::end_drag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    const float d = m_drag_distance;
    // Put the working mesh back to where the drag began, then apply once for
    // real: the guarded path takes the undo snapshot, runs the self-intersection
    // test and commits to the volume.
    m_session->set_mesh(indexed_triangle_set(m_drag_base_mesh));
    if (d == 0.f) {
        refresh_render_volume();
        return;
    }
    apply_push(d, /* final_apply */ true);
    m_drag_base_mesh = indexed_triangle_set();
}

void GLGizmoEdit::cancel_drag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    if (m_session && !m_drag_base_mesh.indices.empty())
        m_session->set_mesh(indexed_triangle_set(m_drag_base_mesh));
    m_drag_base_mesh = indexed_triangle_set();
    m_drag_distance  = 0.f;
    m_push_distance  = 0.f;
    refresh_render_volume();
    m_parent.set_as_dirty();
}

void GLGizmoEdit::apply_push(float distance, bool final_apply)
{
    if (!m_session || m_volume == nullptr)
        return;

    // Distances are world millimetres; the mesh lives in the volume's own space.
    const float d_mesh = float(double(distance) / mesh_scale());

    MeshEdit::TranslateParams tp;
    tp.d = d_mesh;
    // A push/pull is allowed to intersect: MeshEdit::self_intersects() only
    // tests the moved mesh against ITSELF (MeshBoolean::cgal::does_self_
    // intersect on the single indexed_triangle_set - see MeshEdit.cpp,
    // translate_region()). It never sees the other volumes/objects in the
    // Model, so it was never a "does this collide with another part" guard -
    // it was flagging a face that, once moved, passes through other geometry
    // of the SAME part. Unlike a bevel's self-intersection check (which
    // guards the validity of geometry translate_region just constructed -
    // a folded chamfer/fillet band IS a broken, non-manifold-in-effect
    // result), a translated region is still a perfectly well-formed
    // indexed_triangle_set: no facets or indices changed, only vertex
    // positions, so a self-crossing surface here is a legitimate (if
    // unusual) shape rather than corrupt output. The owner wants a part
    // allowed to intersect itself or another part, so the push is never
    // refused for it; CGAL's does_self_intersect is also an expensive
    // whole-mesh test to pay for on every apply just to show an informational
    // notice, so it is skipped entirely rather than run-but-ignored.
    tp.check_self_intersection = false;

    if (!final_apply) {
        // Preview: translate the PRE-DRAG mesh and show the result without
        // touching the session's history.
        const MeshEdit::MeshTopology topo = MeshEdit::build_topology(m_drag_base_mesh);
        MeshEdit::TranslateResult res = MeshEdit::translate_region(m_drag_base_mesh, topo, m_drag_region, tp);
        m_last_status = res.status;
        if (!res.ok())
            return;
        if (res.status == MeshEdit::TranslateStatus::Ok)
            m_session->set_mesh(std::move(res.mesh));
        refresh_render_volume();
        m_parent.set_as_dirty();
        return;
    }

    const MeshEdit::FaceRegion &region = m_dragging || !m_drag_region.empty() ? m_drag_region : m_selected_region;
    MeshEdit::TranslateResult res = m_session->apply_translate(region.empty() ? m_selected_region : region, tp);
    m_last_status      = res.status;
    m_show_last_status = res.status != MeshEdit::TranslateStatus::Ok && res.status != MeshEdit::TranslateStatus::NoOp;

    if (res.status != MeshEdit::TranslateStatus::Ok) {
        // A refusal leaves the working mesh exactly as it was; put the on-screen
        // preview back in step with it.
        refresh_render_volume();
        m_parent.set_as_dirty();
        if (res.status == MeshEdit::TranslateStatus::SelfIntersects)
            wxGetApp().plater()->get_notification_manager()->push_notification(
                NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
                into_u8(m_desc.at("err_self")));
        return;
    }

    commit_to_volume();
    // The selection is still the same facets, but their positions moved, so the
    // region's normal, area and centre are stale - re-grow it against the new
    // mesh so a second push starts from the right place.
    if (!m_selected_region.empty()) {
        const int seed = m_selected_region.facets.front();
        m_selected_region = m_session->grow_region(size_t(seed), region_params());
        m_selected_region_model.reset();
    }
    m_push_distance = 0.f;
    m_drag_distance = 0.f;
}

void GLGizmoEdit::apply_push_from_field()
{
    if (!m_session || !m_has_selection || m_selection_is_chain || m_selected_region.empty())
        return;
    float d = m_push_distance;
    if (m_push_snap)
        d = MeshEdit::snap_to_step(d, m_push_step);
    if (d == 0.f)
        return;
    m_drag_region = m_selected_region;
    apply_push(d, /* final_apply */ true);
    m_drag_region = MeshEdit::FaceRegion{};
}

void GLGizmoEdit::commit_to_volume()
{
    if (!m_session || m_volume == nullptr)
        return;

    // One undo step per APPLIED operation, taken before the volume is touched -
    // the discipline GLGizmoSculpt::end_stroke() follows.
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Edit"), UndoRedo::SnapshotType::GizmoAction);

    indexed_triangle_set edited = m_session->mesh();
    // Deliberately NO plater->clear_before_change_mesh(): a translate changes no
    // indices, so every painted annotation on this volume stays valid. That is
    // the whole reason phase 1 is translation-only.
    // commit_sculpted_mesh() refuses outright unless indices_match(), so this
    // can never silently misalign existing paint.
    const bool committed = Sculpt::commit_sculpted_mesh(*m_volume, std::move(edited), /* ensure_on_bed */ true);
    assert(committed);
    if (!committed) {
        // Belt and braces: if the contract were ever broken, do nothing rather
        // than commit a mesh whose paint no longer lines up.
        return;
    }

    plater->changed_mesh(m_object_idx);
    wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);

    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    // set_new_unique_id() bumped the volume id; keep the cache in step instead of
    // throwing the session away.
    m_volume_id = m_volume->id();
    invalidate_highlight_models();
}

// ----------------------------------------------------------------------------
// Bevel / chamfer - phase 2
// ----------------------------------------------------------------------------

MeshEdit::BevelParams GLGizmoEdit::bevel_params() const
{
    MeshEdit::BevelParams p;
    // The panel works in WORLD millimetres, the mesh in its own units; a scaled
    // volume needs the width pulled back the way the push distance is.
    const double s = mesh_scale();
    p.width    = float(double(m_bevel_width) / (s > 1e-9 ? s : 1.));
    p.segments = std::clamp(m_bevel_segments, 1, MeshEdit::BevelMaxSegments);
    p.profile  = m_bevel_profile == 0 ? MeshEdit::BevelProfile::Chamfer : MeshEdit::BevelProfile::Round;
    // The preview must stay interactive, so the self-intersection guard is left
    // to Apply. update_bevel_preview() passes false; apply_bevel() turns it on.
    p.check_self_intersection = false;
    return p;
}

void GLGizmoEdit::clear_bevel_preview()
{
    if (!m_bevel_preview_valid)
        return;
    m_bevel_preview_valid = false;
    m_bevel_preview_mesh.clear();
    m_bevel_preview_width    = -1.f;
    m_bevel_preview_segments = -1;
    m_bevel_preview_profile  = -1;
    m_bevel_preview_seed     = -1;
    // Put the real mesh back under the renderer. When the session has already gone
    // (detach() resets it before clearing the selection) refresh_render_volume()
    // has nothing to restore from - but the volume is being dropped anyway, and
    // the plater rebuilds its own volumes on the next data_changed(), so there is
    // nothing stale left to show.
    refresh_render_volume();
    m_parent.set_as_dirty();
}

void GLGizmoEdit::update_bevel_preview()
{
    if (!m_session || !m_has_selection || !m_selection_is_chain || m_selected_chain.empty()) {
        clear_bevel_preview();
        return;
    }

    // Rebuild only when something the preview depends on actually moved: a slider
    // that has not changed redraws the same mesh many times a second otherwise.
    const int seed = m_selected_chain.edges.empty() ? -1 : m_selected_chain.edges.front();
    if (m_bevel_preview_valid && m_bevel_preview_width == m_bevel_width &&
        m_bevel_preview_segments == m_bevel_segments && m_bevel_preview_profile == m_bevel_profile &&
        m_bevel_preview_seed == seed)
        return;

    const MeshEdit::BevelResult r = m_session->preview_bevel(m_selected_chain.edges, bevel_params());

    m_bevel_status         = r.status;
    // The preview runs bevel_edges() only, so this is Geometric or None - it never
    // reports the voxel path, because the preview never takes it. That is the
    // honest thing to show while the user is still dragging: the panel says the
    // chain is too curved to bevel, and Apply is what runs the round instead.
    m_bevel_path           = r.path;
    m_bevel_applied_width  = float(double(r.min_width) * mesh_scale());
    m_bevel_clamped        = r.clamped;
    m_bevel_corner_patches = r.corner_patches;
    m_bevel_dropped_concave = r.dropped_concave;
    m_bevel_curved_facets  = r.curved_side_facets;
    m_bevel_curved_spread  = r.curved_side_spread_deg;
    m_show_bevel_status    = true;

    if (r.status != MeshEdit::BevelStatus::Ok || r.mesh.indices.empty()) {
        // A refusal shows the UNBEVELLED part rather than nothing: the panel says
        // why, and the user can see what they still have.
        clear_bevel_preview();
        return;
    }

    m_bevel_preview_mesh     = r.mesh;
    m_bevel_preview_valid    = true;
    m_bevel_preview_width    = m_bevel_width;
    m_bevel_preview_segments = m_bevel_segments;
    m_bevel_preview_profile  = m_bevel_profile;
    m_bevel_preview_seed     = seed;

    // Show it. The session's own mesh is untouched, so cancelling is free.
    if (m_object_idx >= 0 && m_volume_idx >= 0) {
        GLVolumeCollection &volumes = m_parent.get_volumes();
        for (GLVolume *v : volumes.volumes) {
            if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
                continue;
            v->model.reset();
            v->model.init_from(m_bevel_preview_mesh);
        }
    }
    m_parent.set_as_dirty();
}

void GLGizmoEdit::commit_bevelled_mesh(indexed_triangle_set &&its)
{
    if (m_volume == nullptr)
        return;

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Bevel edges"), UndoRedo::SnapshotType::GizmoAction);

    // THE DIFFERENCE FROM A PUSH, and the reason this is a separate function
    // rather than a flag on commit_to_volume(): a bevel inserts vertices and
    // facets, so every facet index changes and the painted annotations no longer
    // refer to the triangles they were painted on. They have to be dropped, which
    // is what clear_before_change_mesh() does - the same call Subdivide, Simplify
    // and Remesh make, for the same reason.
    plater->clear_before_change_mesh(m_object_idx);

    m_volume->set_mesh(std::move(its));
    m_volume->calculate_convex_hull();
    m_volume->invalidate_convex_hull_2d();
    m_volume->set_new_unique_id();
    if (ModelObject *obj = m_volume->get_object(); obj != nullptr) {
        obj->invalidate_bounding_box();
        obj->ensure_on_bed();
    }

    plater->changed_mesh(m_object_idx);
    wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);

    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    m_volume_id = m_volume->id();
    invalidate_highlight_models();
}

void GLGizmoEdit::apply_bevel()
{
    if (!m_session || !m_has_selection || !m_selection_is_chain || m_selected_chain.empty())
        return;
    if (m_bevel_job_running)
        return;                     // one at a time

    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    Worker &worker = plater->get_ui_job_worker();
    if (!worker.is_idle()) {
        // Something else (an arrange, a slice) owns the worker. Say so rather
        // than appearing to ignore the button.
        m_bevel_status      = MeshEdit::BevelStatus::Failed;
        m_show_bevel_status = true;
        wxGetApp().plater()->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
            into_u8(m_desc.at("bevel_busy")));
        return;
    }

    // The preview skipped the self-intersection guard to stay interactive; the
    // real apply pays for it once.
    MeshEdit::BevelParams p = bevel_params();
    p.check_self_intersection = true;

    // THE FIX for the freeze: the solve runs on the worker, against a COPY of the
    // session's mesh, and comes back through on_bevel_done() on the UI thread.
    // Nothing here blocks, so the canvas keeps drawing and the panel's Cancel is
    // reachable for the whole run.
    m_bevel_job_running = true;
    m_show_bevel_status = false;

    indexed_triangle_set  mesh_copy = m_session->mesh();
    std::vector<int>      edges     = m_selected_chain.edges;

    replace_job(worker, std::make_unique<BevelJob>(
        std::move(mesh_copy), std::move(edges), p,
        [this](const MeshEdit::BevelResult &r) { this->on_bevel_done(r); }));
}

void GLGizmoEdit::cancel_bevel_job()
{
    if (!m_bevel_job_running)
        return;
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        plater->get_ui_job_worker().cancel();
}

void GLGizmoEdit::on_bevel_done(const MeshEdit::BevelResult &r)
{
    m_bevel_job_running = false;

    m_bevel_status          = r.status;
    m_bevel_path            = r.path;
    m_bevel_applied_width   = float(double(r.min_width) * mesh_scale());
    m_bevel_clamped         = r.clamped;
    m_bevel_corner_patches  = r.corner_patches;
    m_bevel_dropped_concave = r.dropped_concave;
    m_bevel_curved_facets   = r.curved_side_facets;
    m_bevel_curved_spread   = r.curved_side_spread_deg;
    m_show_bevel_status     = r.status != MeshEdit::BevelStatus::Cancelled;

    if (r.status != MeshEdit::BevelStatus::Ok || r.mesh.indices.empty()) {
        // Nothing changed; the panel says why. Put the un-bevelled part back under
        // the renderer in case a preview was up.
        clear_bevel_preview();
        m_parent.set_as_dirty();
        return;
    }

    // The worker built the mesh from a copy, so the session has to be told about
    // the result explicitly - this is the undo entry the inline path used to take
    // inside apply_bevel().
    if (!m_session)
        return;
    m_session->adopt_mesh(indexed_triangle_set(r.mesh));

    // Drop the preview bookkeeping FIRST - the preview is now the real thing -
    // then commit and re-sync.
    m_bevel_preview_valid = false;
    m_bevel_preview_mesh.clear();
    m_bevel_preview_width = -1.f;
    m_bevel_preview_seed  = -1;

    indexed_triangle_set committed = r.mesh;
    commit_bevelled_mesh(std::move(committed));

    // Every index changed, so the old selection means nothing on the new mesh.
    clear_selection();
    refresh_render_volume();
    m_parent.set_as_dirty();
}

void GLGizmoEdit::refresh_render_volume()
{
    if (m_object_idx < 0 || m_volume_idx < 0 || !m_session)
        return;
    GLVolumeCollection &volumes = m_parent.get_volumes();
    for (GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
            continue;
        v->model.reset();
        v->model.init_from(m_session->mesh());
    }
    invalidate_highlight_models();
}

// ----------------------------------------------------------------------------
// undo
// ----------------------------------------------------------------------------

bool GLGizmoEdit::do_undo()
{
    if (!m_session || !m_session->can_undo())
        return false;
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Undo edit"), UndoRedo::SnapshotType::GizmoAction);
    if (!m_session->undo())
        return false;
    indexed_triangle_set restored = m_session->mesh();
    if (!Sculpt::commit_sculpted_mesh(*m_volume, std::move(restored), /* ensure_on_bed */ true))
        return false;
    plater->changed_mesh(m_object_idx);
    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    m_volume_id = m_volume->id();
    clear_selection();
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoEdit::do_redo()
{
    if (!m_session || !m_session->can_redo())
        return false;
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Redo edit"), UndoRedo::SnapshotType::GizmoAction);
    if (!m_session->redo())
        return false;
    indexed_triangle_set restored = m_session->mesh();
    if (!Sculpt::commit_sculpted_mesh(*m_volume, std::move(restored), /* ensure_on_bed */ true))
        return false;
    plater->changed_mesh(m_object_idx);
    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    m_volume_id = m_volume->id();
    clear_selection();
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoEdit::on_edit_char(int key_code, bool shift_down, bool ctrl_down)
{
    if (m_state != On || !m_session)
        return false;

    // Esc drops the selection rather than closing the gizmo, the way the Cut
    // gizmo's Draw mode claims Esc to clear its line. Only when there IS a
    // selection to drop - otherwise Esc keeps its usual "close the gizmo"
    // meaning.
    if (key_code == WXK_ESCAPE && !ctrl_down) {
        if (m_dragging) {
            cancel_drag();
            return true;
        }
        if (m_has_selection) {
            clear_selection();
            m_parent.set_as_dirty();
            return true;
        }
        return false;
    }

    if (!ctrl_down)
        return false;

    // wx delivers Ctrl+letter as the control character (1-26) in on_char, so
    // both spellings are accepted.
    const bool is_z = key_code == 'z' || key_code == 'Z' || key_code == 26;
    const bool is_y = key_code == 'y' || key_code == 'Y' || key_code == 25;

    if (is_z && !shift_down)
        return do_undo();
    if (is_y || (is_z && shift_down))
        return do_redo();
    return false;
}

// ----------------------------------------------------------------------------
// mouse
// ----------------------------------------------------------------------------

bool GLGizmoEdit::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mouse_pos(double(mouse_event.GetX()), double(mouse_event.GetY()));

    if (m_volume == nullptr || !m_session)
        return false;

    if (mouse_event.Moving()) {
        // Over the handle there is nothing to re-pick: keep the selection as it
        // is so the arrow does not fight the region highlight under it.
        if (m_hover_id == PushHandleId)
            m_hover_valid = false;
        else
            update_hover(mouse_pos);
        m_parent.set_as_dirty();
        // Hovering must not swallow the event: the canvas still wants it for its
        // own tooltip and hover handling.
        return false;
    }

    if (mouse_event.LeftDown()) {
        // A click on the push arrow starts a drag; a click on the part picks a
        // new selection. The arrow is tested FIRST, and it is tested against the
        // scene raycaster's answer (m_hover_id), not against a fresh raycast of
        // the mesh: the handle stands off the surface, so a mesh raycast never
        // reports it and the drag could never begin.
        if (m_hover_id == PushHandleId && begin_drag(mouse_pos)) {
            m_parent.set_as_dirty();
            return true;
        }
        update_hover(mouse_pos);
        if (m_hover_valid) {
            commit_hover_to_selection();
            m_parent.set_as_dirty();
            return true;
        }
        return false;
    }

    if (mouse_event.Dragging()) {
        if (m_dragging) {
            update_drag(mouse_pos);
            return true;
        }
        return false;
    }

    if (mouse_event.LeftUp()) {
        if (m_dragging) {
            end_drag();
            m_parent.set_as_dirty();
            return true;
        }
        return false;
    }

    if (mouse_event.RightDown() && m_dragging) {
        cancel_drag();
        return true;
    }

    if (mouse_event.Leaving()) {
        m_hover_valid = false;
        m_parent.set_as_dirty();
        return false;
    }

    return false;
}

// ----------------------------------------------------------------------------
// rendering
// ----------------------------------------------------------------------------

void GLGizmoEdit::rebuild_region_model(const MeshEdit::FaceRegion &region, GLModel &model, int &cached_key, int key) const
{
    if (region.empty() || !m_session)
        return;
    if (model.is_initialized() && cached_key == key)
        return;
    model.reset();
    // The same builder the Measure gizmo's plane overlay uses, with the same
    // z-fight offset: one rendering path for every facet-list highlight in the
    // application.
    model.init_from(init_plane_data(m_session->mesh(), region.facets, EditHighlightOffset));
    cached_key = key;
}

void GLGizmoEdit::rebuild_chain_model(const MeshEdit::EdgeChain &chain, GLModel &model, int &cached_key, int key) const
{
    if (chain.empty() || !m_session)
        return;
    if (model.is_initialized() && cached_key == key)
        return;
    model.reset();

    const indexed_triangle_set &its  = m_session->mesh();
    const MeshEdit::MeshTopology &topo = m_session->topology();

    // Radius as a fraction of the part, so a chain reads the same on any size.
    Vec3f lo = its.vertices.empty() ? Vec3f::Zero() : its.vertices.front();
    Vec3f hi = lo;
    for (const Vec3f &v : its.vertices) { lo = lo.cwiseMin(v); hi = hi.cwiseMax(v); }
    const float radius = std::max(1e-4f, (hi - lo).norm() * EditChainRadiusFraction);

    GLModel::Geometry data;
    data.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
    unsigned int base = 0;
    constexpr int Sides = 6;

    for (int e : chain.edges) {
        if (e < 0 || e >= topo.num_edges)
            continue;
        const Vec2i32 &ev = topo.edge_vertices[size_t(e)];
        if (ev(0) < 0 || ev(1) < 0)
            continue;
        const Vec3f a = its.vertices[size_t(ev(0))];
        const Vec3f b = its.vertices[size_t(ev(1))];
        Vec3f axis = b - a;
        const float len = axis.norm();
        if (len < 1e-9f)
            continue;
        axis /= len;
        // Any two vectors perpendicular to the edge; the choice is arbitrary
        // because the tube is round.
        Vec3f u = std::abs(axis.z()) < 0.9f ? axis.cross(Vec3f::UnitZ()) : axis.cross(Vec3f::UnitX());
        u.normalize();
        const Vec3f v = axis.cross(u);

        for (int k = 0; k < Sides; ++k) {
            const float ang = 2.f * float(M_PI) * float(k) / float(Sides);
            const Vec3f n   = u * std::cos(ang) + v * std::sin(ang);
            data.add_vertex(Vec3f(a + n * radius), n);
            data.add_vertex(Vec3f(b + n * radius), n);
        }
        for (int k = 0; k < Sides; ++k) {
            const unsigned int a0 = base + unsigned(2 * k);
            const unsigned int b0 = base + unsigned(2 * k + 1);
            const unsigned int a1 = base + unsigned(2 * ((k + 1) % Sides));
            const unsigned int b1 = base + unsigned(2 * ((k + 1) % Sides) + 1);
            data.add_triangle(a0, b0, b1);
            data.add_triangle(a0, b1, a1);
        }
        base += unsigned(2 * Sides);
    }

    if (data.is_empty())
        return;
    model.init_from(std::move(data));
    cached_key = key;
}

void GLGizmoEdit::render_highlights()
{
    if (!m_session || m_volume == nullptr)
        return;

    // Build whatever is missing. Not const, so it cannot go in on_render's const
    // path - hence on_render() is not const either, matching GLGizmoBase.
    if (m_pick_mode == PickMode::EdgeChain) {
        rebuild_chain_model(m_hover_chain, m_hover_chain_model, m_hover_chain_key, m_hover_chain_key);
        rebuild_chain_model(m_selected_chain, m_selected_chain_model, m_selected_chain_key, m_selected_chain_key);
    } else {
        rebuild_region_model(m_hover_region, m_hover_region_model, m_hover_region_key, m_hover_region_key);
    }
    if (!m_selection_is_chain)
        rebuild_region_model(m_selected_region, m_selected_region_model, m_selected_region_key, m_selected_region_key);

    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();

    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d trafo  = volume_trafo();
    const Transform3d view   = camera.get_view_matrix();
    shader->set_uniform("view_model_matrix", view * trafo);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix",
                        (Matrix3d) (view.matrix().block(0, 0, 3, 3) * trafo.matrix().block(0, 0, 3, 3).inverse().transpose()));

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));

    // Hover is the paler of the two, selection the solid one - the same
    // convention GLGizmoMeasure's render_glmodel(..., hover) uses.
    const ColorRGBA hover_color(0.30f, 0.70f, 0.95f, 0.35f);
    const ColorRGBA sel_color  (1.00f, 0.60f, 0.10f, 0.60f);

    auto draw = [&](GLModel &m, const ColorRGBA &c) {
        if (!m.is_initialized())
            return;
        m.set_color(c);
        m.render();
    };

    // Selection under hover, so hovering a different face of the same part still
    // reads clearly.
    draw(m_selected_region_model, sel_color);
    draw(m_selected_chain_model, sel_color);
    if (!(m_has_selection && m_hover_region_key == m_selected_region_key))
        draw(m_hover_region_model, hover_color);
    if (!(m_has_selection && m_hover_chain_key == m_selected_chain_key))
        draw(m_hover_chain_model, hover_color);

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

// The arrow's proportions, as fractions of its total length. A head that is a
// quarter of the arrow and three times the stem's radius is the proportion the
// Move gizmo's stilized_arrow uses, and it is what makes the thing read as a
// grabbable arrow rather than as the bare stick the first cut drew.
static constexpr double EditHandleHeadFraction   = 0.28;
static constexpr double EditHandleStemRadiusFrac = 0.022;
static constexpr double EditHandleHeadRadiusFrac = 0.072;

double GLGizmoEdit::handle_length() const
{
    if (!m_session)
        return 1.;
    const indexed_triangle_set &its = m_session->mesh();
    if (its.vertices.empty())
        return 1.;
    Vec3f lo = its.vertices.front(), hi = lo;
    for (const Vec3f &v : its.vertices) { lo = lo.cwiseMin(v); hi = hi.cwiseMax(v); }
    // Mesh space -> world, so the arrow keeps its on-screen size on a scaled volume.
    const double diag = double((hi - lo).norm()) * mesh_scale();
    return std::max(2.0, diag * 0.25);
}

void GLGizmoEdit::init_handle_models()
{
    if (!m_handle_stem.model.is_initialized()) {
        indexed_triangle_set its = its_make_cylinder(1.0, 1.0);
        m_handle_stem.model.init_from(its);
        m_handle_stem.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    if (!m_handle_head.model.is_initialized()) {
        indexed_triangle_set its = its_make_cone(1.0, 1.0, double(PI) / 12.0);
        m_handle_head.model.init_from(its);
        m_handle_head.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
}

bool GLGizmoEdit::handle_transforms(Transform3d &stem, Transform3d &head) const
{
    if (!m_has_selection || m_selection_is_chain || m_selected_region.empty() || !m_session)
        return false;

    const Transform3d trafo = volume_trafo();
    // The arrow must follow the face while the face is being dragged, otherwise
    // the head slides out from under the cursor mid-drag.
    const MeshEdit::FaceRegion &region = (m_dragging && !m_drag_region.empty()) ? m_drag_region : m_selected_region;
    const Vec3d axis_world = push_axis_world(trafo, region.normal);

    Vec3d origin_world = trafo * region.center.cast<double>();
    if (m_dragging)
        // m_drag_distance is a world-space length along the push axis.
        origin_world += axis_world * double(m_drag_distance);

    // Rotation taking +Z onto the push axis.
    Transform3d rot = Transform3d::Identity();
    const Vec3d z = Vec3d::UnitZ();
    const Vec3d c = z.cross(axis_world);
    const double s = c.norm(), dp = z.dot(axis_world);
    if (s > EPSILON)
        rot.rotate(Eigen::AngleAxisd(std::atan2(s, dp), c.normalized()));
    else if (dp < 0.)
        rot.rotate(Eigen::AngleAxisd(M_PI, Vec3d::UnitX()));

    const double len         = handle_length();
    const double head_len    = len * EditHandleHeadFraction;
    const double stem_len    = len - head_len;
    const double stem_radius = len * EditHandleStemRadiusFrac;
    const double head_radius = len * EditHandleHeadRadiusFrac;

    const Transform3d base = Geometry::assemble_transform(origin_world) * rot;
    // its_make_cylinder / its_make_cone are built around the origin growing along
    // +Z, so each only needs a translation up the axis and a scale.
    stem = base * Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(),
                                               Vec3d(stem_radius, stem_radius, stem_len));
    head = base * Geometry::assemble_transform(stem_len * Vec3d::UnitZ(), Vec3d::Zero(),
                                               Vec3d(head_radius, head_radius, head_len));
    return true;
}

void GLGizmoEdit::update_handle_raycasters()
{
    if (m_handle_raycasters.size() < 2)
        return;
    Transform3d stem, head;
    if (!handle_transforms(stem, head)) {
        // Nothing to pick: park the raycasters at a degenerate scale so a stale
        // selection cannot leave an invisible grabbable volume behind.
        const Transform3d nowhere = Geometry::assemble_transform(Vec3d(0., 0., -1.e6), Vec3d::Zero(), Vec3d(1.e-6, 1.e-6, 1.e-6));
        m_handle_raycasters[0]->set_transform(nowhere);
        m_handle_raycasters[1]->set_transform(nowhere);
        return;
    }
    m_handle_raycasters[0]->set_transform(stem);
    m_handle_raycasters[1]->set_transform(head);
}

void GLGizmoEdit::on_register_raycasters_for_picking()
{
    // The arrow is drawn over the part, so the picker must prefer it.
    m_parent.set_raycaster_gizmos_on_top(true);
    init_handle_models();
    if (!m_handle_raycasters.empty())
        return;
    // Both pieces answer with the SAME picking id: grabbing the stem and grabbing
    // the head are the same gesture, and the brief asks for the line itself to be
    // grab-able too.
    m_handle_raycasters.emplace_back(m_parent.add_raycaster_for_picking(
        SceneRaycaster::EType::Gizmo, PushHandleId, *m_handle_stem.mesh_raycaster, Transform3d::Identity()));
    m_handle_raycasters.emplace_back(m_parent.add_raycaster_for_picking(
        SceneRaycaster::EType::Gizmo, PushHandleId, *m_handle_head.mesh_raycaster, Transform3d::Identity()));
    update_handle_raycasters();
}

void GLGizmoEdit::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.set_raycaster_gizmos_on_top(false);
    m_handle_raycasters.clear();
}

void GLGizmoEdit::render_push_axis() const
{
    Transform3d stem, head;
    if (!handle_transforms(stem, head))
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    // Lit rather than flat: an unlit cone is a flat silhouette and reads as the
    // same "stick" the defect reported.
    const Camera &camera = wxGetApp().plater()->get_camera();
    const bool hovered = m_hover_id == PushHandleId;
    const ColorRGBA color = hovered ? ColorRGBA(1.0f, 0.9f, 0.4f, 1.0f)
                                    : ColorRGBA(1.0f, 0.75f, 0.2f, 1.0f);

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("emission_factor", 0.1f);

    GLModel &stem_model = const_cast<GLModel &>(m_handle_stem.model);
    GLModel &head_model = const_cast<GLModel &>(m_handle_head.model);

    for (int i = 0; i < 2; ++i) {
        const Transform3d &m = i == 0 ? stem : head;
        GLModel &gm          = i == 0 ? stem_model : head_model;
        if (!gm.is_initialized())
            continue;
        const Transform3d view_model = camera.get_view_matrix() * m;
        shader->set_uniform("view_model_matrix", view_model);
        shader->set_uniform("normal_matrix", (Matrix3d) view_model.matrix().block(0, 0, 3, 3).inverse().transpose());
        gm.set_color(color);
        gm.render();
    }
    shader->stop_using();
}

void GLGizmoEdit::on_render()
{
    if (m_volume == nullptr || !m_session)
        return;
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_highlights();
    init_handle_models();
    // The handle follows the selection and the live drag, so its pickable volumes
    // have to be re-placed every frame - exactly as GLGizmoCut re-places its
    // connector raycasters.
    update_handle_raycasters();
    render_push_axis();
}

// ----------------------------------------------------------------------------
// panel
// ----------------------------------------------------------------------------

float GLGizmoEdit::compute_label_width() const
{
    // Recomputed when the font scaling moves (a DPI change, or the user changing
    // the UI scale), otherwise reused - calc_text_size over a dozen labels every
    // frame would be wasted work.
    const float scaling = m_imgui->get_style_scaling();
    if (m_label_width > 0.f && std::abs(scaling - m_label_width_scaling) < 0.001f)
        return m_label_width;

    // Every label that is drawn in the left column of a row. The mode row's
    // "Select" is in here too, so the combo lines up with the sliders below it.
    static const char *label_keys[] = {"mode",     "feature_angle", "planar_tol", "smooth_step",
                                       "smooth_cap", "distance",    "snap_step",  "bevel_width",
                                       "bevel_segments", "bevel_profile"};
    float width = 0.f;
    for (const char *key : label_keys) {
        const auto it = m_desc.find(key);
        if (it != m_desc.end())
            width = std::max(width, m_imgui->calc_text_size(it->second).x);
    }

    m_label_width         = width + m_imgui->scaled(1.5f);
    m_label_width_scaling = scaling;
    return m_label_width;
}

float GLGizmoEdit::compute_mode_combo_width() const
{
    // THE BUG THIS FIXES: the Select combo was sized to whatever the panel had
    // left over (wrap_width - label_col), and the panel's own width was derived
    // from the LABEL column alone. So the combo came out about as wide as the word
    // "Face" - the first entry, and the shortest - and "Curved face" and "Edge
    // chain" were clipped inside it.
    //
    // The fix is the one the Cut gizmo's combos use: measure EVERY entry, take the
    // widest, and add the room the combo itself needs around the text - the arrow
    // button on the right plus the frame padding on both sides. Then the panel is
    // made at least that wide (see on_render_input_window), so the measurement is
    // not immediately undone by a narrow window.
    const float scaling = m_imgui->get_style_scaling();
    if (m_mode_combo_width > 0.f && std::abs(scaling - m_mode_combo_scaling) < 0.001f)
        return m_mode_combo_width;

    static const char *mode_keys[] = {"mode_face", "mode_smooth_face", "mode_chain"};
    float              widest = 0.f;
    for (const char *key : mode_keys) {
        const auto it = m_desc.find(key);
        if (it != m_desc.end())
            widest = std::max(widest, m_imgui->calc_text_size(it->second).x);
    }

    // The arrow square is one frame height, and the text sits inside the frame
    // padding on either side. GetFrameHeight() is the arrow, 2x FramePadding.x the
    // text inset, and a little slack so the longest entry is not flush against the
    // arrow.
    const ImGuiStyle &style = ImGui::GetStyle();
    m_mode_combo_width   = widest + ImGui::GetFrameHeight() + 2.f * style.FramePadding.x +
                         m_imgui->scaled(0.5f);
    m_mode_combo_scaling = scaling;
    return m_mode_combo_width;
}

void GLGizmoEdit::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info() || !m_c->selection_info()->model_object())
        return;

    // ROOT CAUSE of the combo staying "Face"-width even after compute_mode_
    // combo_width() was added: this window-width math reads
    // ImGui::GetStyle().WindowPadding, but push_toolbar_style() - which
    // overrides WindowPadding to (20,10)*scale, versus ImGui's own default of
    // (8,8) - was not pushed yet. So window_width was budgeted with an ~8px
    // padding assumption while the panel that actually opened used ~20px
    // *scale* of padding per side. The resulting wrap_width (measured after
    // push_toolbar_style, inside the real window) came out narrower than
    // window_width assumed by exactly that padding gap, so the row's
    // `wrap_width - label_col` clamp in the combo below silently cut the
    // combo back down under mode_combo_w - undoing the fix compute_mode_
    // combo_width() was supposed to provide. Pushing the toolbar style
    // BEFORE doing the width math (instead of after, right before
    // GizmoImguiBegin) makes GetStyle() report the padding that will really
    // be in effect, so window_width and wrap_width agree.
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());

    // A fixed width, as Sculpt learnt to use: an auto-sizing panel resizes out
    // from under the pointer whenever a conditional row appears.
    //
    // The width is derived from the widest label the panel can show rather than
    // being a flat number: at 150% DPI "Curvature per step" and "Face tolerance"
    // are wider than the old scaled(18) allowed for, and the label column then
    // ate the whole row and clipped. Taking the real measurement keeps every row
    // inside the panel at any scale and in any translation.
    const float label_col     = compute_label_width();
    // Wide enough for the label column plus the widest control in it. The mode
    // combo is the widest - it has to hold "Curved face" - and sizing the window
    // to the label column alone is what clipped it; scaled(9) was a guess at what
    // a control needs and the combo needs more than that at several DPI scales and
    // in most translations.
    const float mode_combo_w  = compute_mode_combo_width();
    const float window_width  = std::max({m_imgui->scaled(18.0f),
                                          label_col + m_imgui->scaled(9.0f),
                                          label_col + mode_combo_w +
                                              2.f * ImGui::GetStyle().WindowPadding.x});
    const float approx_height = m_imgui->scaled(20.f);
    y = std::min(y, bottom_limit - approx_height);

    dock_setup_next_window(x, y, bottom_limit, window_width);

    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar));

    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    const float wrap_width = ImGui::GetContentRegionAvail().x;

    if (m_volume == nullptr || !m_session) {
        m_imgui->text_wrapped(m_desc.at("no_part"), wrap_width);
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    const float space_size        = m_imgui->get_style_scaling() * 8;
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    const float sliders_width     = std::max(m_imgui->scaled(3.0f),
                                             wrap_width - label_col - 1.5f * slider_icon_width - space_size);
    const float drag_left         = ImGui::GetStyle().WindowPadding.x + label_col + sliders_width - space_size;

    // --- pick mode ---
    // Three radio buttons on one line ran past the panel edge and the last one
    // ("Edge chain") was clipped; a combo takes one line whatever the labels are.
    // render_combo() is the base's own label+combo row, so this picks up the
    // combo styling (and the label column) every other gizmo uses.
    {
        // Positional - the order must match PickMode.
        const std::vector<std::string> mode_labels = {into_u8(m_desc.at("mode_face")),
                                                      into_u8(m_desc.at("mode_smooth_face")),
                                                      into_u8(m_desc.at("mode_chain"))};
        int mode = int(m_pick_mode);
        // The measured width, not the leftovers. It is clamped to what the row
        // actually has so a very long translation cannot push the combo off the
        // panel edge - but the window above was sized from the same measurement,
        // so in the normal case this is exactly mode_combo_w.
        const float combo_w = std::min(std::max(mode_combo_w, m_imgui->scaled(3.0f)),
                                       std::max(m_imgui->scaled(3.0f), wrap_width - label_col));
        if (render_combo(into_u8(m_desc.at("mode")), mode_labels, mode, label_col, combo_w) &&
            mode != int(m_pick_mode)) {
            m_pick_mode = PickMode(mode);
            clear_selection();
            update_hover(m_last_mouse);
        }
    }

    switch (m_pick_mode) {
    case PickMode::Face:       m_imgui->text_wrapped(m_desc.at("mode_face_hint"), wrap_width); break;
    case PickMode::SmoothFace: m_imgui->text_wrapped(m_desc.at("mode_smooth_hint"), wrap_width); break;
    case PickMode::EdgeChain:  m_imgui->text_wrapped(m_desc.at("mode_chain_hint"), wrap_width); break;
    }

    ImGui::Separator();

    // --- selection thresholds, whichever mode is live ---
    // Every slider row: label, slider, and a numeric field at the end.
    //
    // The slider draws its own value from `fmt` (BBLSliderFloat renders the format
    // string onto the track), so these rows were already readable. What was NOT was
    // the field at the end: it carried a hard-coded "%.1f" and no bounds, so an
    // angle row showed "30 deg" on the slider and a bare "30.0" in the box, and the
    // box would accept a value the slider could never reach. It now takes the row's
    // own format and the row's own range, so the two always agree.
    auto slider_row = [&](const wxString &label, const char *id, float *value, float lo, float hi,
                          const char *fmt, float step = 0.1f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(label_col);
        ImGui::PushItemWidth(sliders_width);
        const bool changed = m_imgui->bbl_slider_float_style(id, value, lo, hi, fmt, 1.0f, true);
        ImGui::SameLine(drag_left);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        const bool typed = ImGui::BBLDragFloat((std::string(id) + "_input").c_str(), value, step,
                                               lo, hi, fmt);
        *value = std::clamp(*value, lo, hi);
        return changed || typed;
    };

    bool selection_params_changed = false;
    if (m_pick_mode == PickMode::EdgeChain) {
        selection_params_changed |= slider_row(m_desc.at("feature_angle"), "##edit_feature_angle", &m_feature_angle, 1.f, 179.f, "%.0f deg");
        m_imgui->text_wrapped(m_desc.at("feature_angle_hint"), wrap_width);
    } else if (m_pick_mode == PickMode::SmoothFace) {
        selection_params_changed |= slider_row(m_desc.at("smooth_step"), "##edit_smooth_step", &m_smooth_step, 0.5f, 45.f, "%.1f deg");
        selection_params_changed |= slider_row(m_desc.at("smooth_cap"), "##edit_smooth_cap", &m_smooth_cap, 1.f, 90.f, "%.0f deg");
    } else {
        selection_params_changed |= slider_row(m_desc.at("planar_tol"), "##edit_planar_tol", &m_planar_tol, 0.05f, 20.f, "%.2f deg");
    }
    if (selection_params_changed) {
        // A threshold change invalidates every cached grow, hover and selection
        // alike - the selection was grown with the OLD numbers.
        m_hover_region_key = m_hover_chain_key = -1;
        invalidate_highlight_models();
        update_hover(m_last_mouse);
        m_parent.set_as_dirty();
    }

    ImGui::Separator();

    // --- what is selected ---
    if (!m_has_selection) {
        m_imgui->text_wrapped(m_desc.at("no_selection"), wrap_width);
    } else if (m_selection_is_chain) {
        m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at("chain_info"),
                                                m_selected_chain.edges.size(),
                                                m_selected_chain.closed ? m_desc.at("chain_closed") : wxString()),
                              wrap_width);
        m_imgui->text_wrapped(m_desc.at("chain_selected"), wrap_width);
    } else {
        m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at("selected_info"),
                                                m_selected_region.facets.size(),
                                                wxString::Format("%.2f", double(m_selected_region.area))),
                              wrap_width);
    }

    // --- push/pull ---
    if (m_has_selection && !m_selection_is_chain) {
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("push"));
        m_imgui->text_wrapped(m_desc.at("drag_hint"), wrap_width);

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("distance"));
        ImGui::SameLine(label_col);
        ImGui::PushItemWidth(sliders_width);
        ImGui::BBLDragFloat("##edit_distance", &m_push_distance, 0.1f, 0.0f, 0.0f, "%.2f mm");

        m_imgui->bbl_checkbox(m_desc.at("snap"), m_push_snap);
        if (m_push_snap) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("snap_step"));
            ImGui::SameLine(label_col);
            ImGui::PushItemWidth(sliders_width);
            ImGui::BBLDragFloat("##edit_snap_step", &m_push_step, 0.05f, 0.0f, 0.0f, "%.2f mm");
            m_push_step = std::clamp(m_push_step, PushStepMin, PushStepMax);
        }

        if (m_imgui->button(m_desc.at("apply")))
            apply_push_from_field();
        ImGui::SameLine();
        if (m_imgui->button(m_desc.at("reset_selection")))
            clear_selection();
    }

    // --- bevel / chamfer (phase 2) ---
    //
    // Offered only for a CHAIN selection: a bevel acts on edges, and a face region
    // has none of its own. That is also why the two sections are mutually
    // exclusive rather than both always visible - the panel shows the tool that
    // applies to what is actually selected.
    if (m_has_selection && m_selection_is_chain) {
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("bevel"));

        bool bevel_changed = false;

        // WIDTH: a slider over the useful range with the exact field beside it.
        // It used to be a bare drag field, which meant the only way to find out
        // what widths this chain would take was to drag and watch - a slider shows
        // the range at a glance, and the field is still there for "exactly 1.25".
        //
        // The two are ONE VALUE (&m_bevel_width), so they cannot disagree: whichever
        // control moved wrote it, and the other draws from it on the same frame.
        // The 0..10 mm range is the brief's; a width past 10 mm is possible on a big
        // part and is what the numeric field is for, so it is clamped to
        // BevelWidthMax rather than to the slider's top.
        {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("bevel_width"));
            ImGui::SameLine(label_col);
            ImGui::PushItemWidth(sliders_width);
            if (m_imgui->bbl_slider_float_style("##edit_bevel_width", &m_bevel_width,
                                                BevelWidthSliderMin, BevelWidthSliderMax,
                                                "%.2f mm", 1.0f, true))
                bevel_changed = true;
            ImGui::SameLine(drag_left);
            ImGui::PushItemWidth(1.5f * slider_icon_width);
            // Same format as the slider, and the same 0.1 step the brief asks for.
            // Bounds 0..BevelWidthMax rather than the slider's, so a value the
            // slider cannot reach can still be typed.
            if (ImGui::BBLDragFloat("##edit_bevel_width_input", &m_bevel_width, BevelWidthStep,
                                    BevelWidthMin, BevelWidthMax, "%.2f mm"))
                bevel_changed = true;
            m_bevel_width = std::clamp(m_bevel_width, BevelWidthMin, BevelWidthMax);
        }

        // SEGMENTS: the slider now shows its value ("%d"), and carries a number at
        // the end like every other row. It was the one unlabelled control in the
        // panel - a bare track with no way to read the count off it.
        {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("bevel_segments"));
            ImGui::SameLine(label_col);
            ImGui::PushItemWidth(sliders_width);
            static const int seg_min = 1;
            static const int seg_max = MeshEdit::BevelMaxSegments;
            int              n       = m_bevel_segments;
            if (ImGui::BBLSliderScalar("##edit_bevel_segments", ImGuiDataType_S32, &n, &seg_min,
                                       &seg_max, "%d")) {
                n = std::max(seg_min, std::min(seg_max, n));
                if (n != m_bevel_segments) {
                    m_bevel_segments = n;
                    bevel_changed    = true;
                }
            }
            ImGui::SameLine(drag_left);
            ImGui::PushItemWidth(1.5f * slider_icon_width);
            int typed = m_bevel_segments;
            if (ImGui::BBLDragScalar("##edit_bevel_segments_input", ImGuiDataType_S32, &typed, 1.f,
                                     &seg_min, &seg_max, "%d")) {
                typed = std::max(seg_min, std::min(seg_max, typed));
                if (typed != m_bevel_segments) {
                    m_bevel_segments = typed;
                    bevel_changed    = true;
                }
            }
        }

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("bevel_profile"));
        // Two short labels, so the radio pair still fits on one line - but put it
        // in the same label column as every other row rather than starting a new
        // line, so the panel reads as one grid.
        ImGui::SameLine(label_col);
        {
            int profile = m_bevel_profile;
            const bool picked =
                ImGui::RadioButton(into_u8(m_desc.at("bevel_chamfer")).c_str(), &profile, 0) ||
                (ImGui::SameLine(), ImGui::RadioButton(into_u8(m_desc.at("bevel_round")).c_str(), &profile, 1));
            if (picked && profile != m_bevel_profile) {
                m_bevel_profile = profile;
                bevel_changed   = true;
            }
        }
        // A chamfer IS the one-segment case, so the segment control means nothing
        // for it. Say so rather than letting a live-looking slider do nothing.
        if (m_bevel_profile == 0)
            m_imgui->text_wrapped(m_desc.at("bevel_chamfer_hint"), wrap_width);

        // The live preview: re-run whenever a control moved, and once on entry so
        // the user sees the result before touching anything.
        if (bevel_changed || !m_bevel_preview_valid)
            update_bevel_preview();

        m_imgui->text_wrapped(m_desc.at("bevel_hint"), wrap_width);

        // What the solve decided, before Apply is pressed.
        if (m_show_bevel_status) {
            if (m_bevel_clamped && m_bevel_applied_width > 0.f)
                m_imgui->warning_text(GUI::format_wxstr(m_desc.at("bevel_clamped"),
                                                        wxString::Format("%.2f", double(m_bevel_applied_width))));
            if (m_bevel_corner_patches > 0)
                m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at("bevel_corners"), m_bevel_corner_patches),
                                      wrap_width);
            const wxString *err = nullptr;
            switch (m_bevel_status) {
            case MeshEdit::BevelStatus::NonManifold:    err = &m_desc.at("bevel_err_manifold"); break;
            case MeshEdit::BevelStatus::WidthTooSmall:  err = &m_desc.at("bevel_err_small"); break;
            // An empty result has two quite different causes, and telling them
            // apart is the difference between "nothing to do" and "use the other
            // tool", so the concave case gets its own line.
            case MeshEdit::BevelStatus::EmptyChain:
                err = m_bevel_dropped_concave > 0 ? &m_desc.at("bevel_err_concave")
                                                  : &m_desc.at("bevel_err_flat");
                break;
            case MeshEdit::BevelStatus::Failed:         err = &m_desc.at("bevel_err_failed"); break;
            default: break;
            }
            if (err != nullptr)
                m_imgui->warning_text(*err);
            // The curved-surface refusal carries a number, so it is formatted
            // rather than taken from the table as-is. It is only REACHED now when
            // the voxel fallback could not run either (no OpenVDB in this build, or
            // the round itself failed) - a chain the geometry cannot build is
            // normally rounded rather than refused.
            if (m_bevel_status == MeshEdit::BevelStatus::CurvedSurface)
                m_imgui->warning_text(GUI::format_wxstr(m_desc.at("bevel_err_curved"),
                                                        m_bevel_curved_facets));

            // WHICH PATH RAN. The two are different operations with different
            // consequences - the geometric one inserts facets and leaves the rest
            // of the mesh alone, the voxel one re-extracts the surface in a band
            // and loses detail below its voxel size there - so the panel says which
            // one produced what the user is looking at rather than letting them
            // guess from the shape.
            if (m_bevel_status == MeshEdit::BevelStatus::Ok) {
                if (m_bevel_path == MeshEdit::BevelPath::VoxelFallback)
                    m_imgui->text_wrapped(m_desc.at("bevel_path_voxel"), wrap_width);
                else if (m_bevel_path == MeshEdit::BevelPath::Geometric)
                    m_imgui->text_wrapped(m_desc.at("bevel_path_geom"), wrap_width);
            }
        }

        // While the worker runs, Apply is replaced by a progress line and a Cancel.
        // The bevel is no longer on the UI thread, so the panel stays live for the
        // whole solve and this button is actually reachable - which is the point.
        if (m_bevel_job_running) {
            m_imgui->text(m_desc.at("bevel_working"));
            ImGui::SameLine();
            if (m_imgui->button(m_desc.at("bevel_cancel")))
                cancel_bevel_job();
        } else {
            if (m_imgui->button(m_desc.at("bevel_apply")))
                apply_bevel();
            ImGui::SameLine();
            if (m_imgui->button(m_desc.at("reset_selection"))) {
                clear_bevel_preview();
                clear_selection();
            }
        }

        // The painted-data warning, stated where the decision is taken rather than
        // only in the note at the bottom - which is about a PUSH and says the
        // opposite.
        m_imgui->text_wrapped(m_desc.at("bevel_cleared"), wrap_width);
    }

    // --- undo / redo ---
    ImGui::Separator();
    {
        // This imgui build has no BeginDisabled/EndDisabled, so the greying
        // goes through ImGuiWrapper::button(label, size, enable), which is what
        // the rest of the application uses for a conditionally-live button.
        const bool can_undo = m_session->can_undo();
        const bool can_redo = m_session->can_redo();
        if (m_imgui->button(m_desc.at("undo"), ImVec2(0.f, 0.f), can_undo))
            do_undo();
        ImGui::SameLine();
        if (m_imgui->button(m_desc.at("redo"), ImVec2(0.f, 0.f), can_redo))
            do_redo();
        ImGui::SameLine();
        m_imgui->text(m_desc.at("undo_caption") + " / " + m_desc.at("redo_caption"));
    }

    // --- status ---
    if (m_show_last_status) {
        const wxString *msg = nullptr;
        switch (m_last_status) {
        case MeshEdit::TranslateStatus::SelfIntersects: msg = &m_desc.at("err_self"); break;
        case MeshEdit::TranslateStatus::OutOfBounds:    msg = &m_desc.at("err_bounds"); break;
        case MeshEdit::TranslateStatus::WholeMesh:      msg = &m_desc.at("err_whole"); break;
        case MeshEdit::TranslateStatus::EmptyRegion:    msg = &m_desc.at("err_empty"); break;
        default: break;
        }
        if (msg != nullptr) {
            ImGui::Separator();
            m_imgui->warning_text(*msg);
        }
    }

    ImGui::Separator();
    m_imgui->text_wrapped(m_desc.at("paint_kept"), wrap_width);

    // --- the exact B-rep tool ---
    // The bevel above works on triangles; for a CAD part the exact tool is the better answer,
    // so it is offered here where users look for it. CallAfter for the same reason as below:
    // opening it closes this gizmo, which must not happen inside this gizmo's own frame.
    ImGui::Separator();
    if (m_imgui->button(m_desc.at("cad_fillet")))
        wxGetApp().CallAfter([]() { wxGetApp().obj_list()->open_cad_fillet(/*from_gizmo*/ true); });
    m_imgui->text_wrapped(m_desc.at("cad_fillet_hint"), wrap_width);

    // --- the interim whole-mesh fillet ---
    //
    // Deliberately last and visually separated: unlike everything above it, this is
    // NOT a local edit. It rebuilds the entire part from its distance field, which
    // renumbers every facet, clears painted data and invalidates this session's
    // topology cache and undo stack outright - so it closes the gizmo rather than
    // trying to keep a session alive over a mesh that no longer matches it. The
    // ObjectList entry point does the closing, because a gizmo cannot reach
    // GLGizmosManager from here.
    //
    // Hidden without OpenVDB, matching the object menu.
    if (voxel_ops_available()) {
        ImGui::Separator();
        // CallAfter, exactly as GLGizmoSculpt's Quad-remesh button does: the handler
        // closes this gizmo, and destroying the gizmo's state from inside its own
        // ImGui frame is not survivable. Deferring to the next idle runs it once the
        // frame is finished.
        if (m_imgui->button(m_desc.at("round_all")))
            wxGetApp().CallAfter([]() { wxGetApp().obj_list()->round_all_edges(/*close_gizmos*/ true); });
        m_imgui->text_wrapped(m_desc.at("round_all_hint"), wrap_width);
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
