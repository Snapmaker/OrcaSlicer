#ifndef slic3r_EmbossFreeTransform_hpp_
#define slic3r_EmbossFreeTransform_hpp_

// Rules for the free 3D move and rotate handles of the Text and SVG tools (GLGizmoEmboss,
// GLGizmoSVG; the traced Image uses the SVG tool).
//
// Inside those tools a part could only slide over the surface (surface drag) and turn in its own
// plane (the rotation ring). The handles add a move along the part's own X / Y / Z (or the world
// axes) and a turn around its own X and Y. What that does to the "Use surface" projection, the
// "From surface" distance and the per glyph placement is decided here, free of OpenGL and
// wxWidgets, so it can be unit tested.

#include <optional>

#include "libslic3r/Point.hpp"

namespace Slic3r::GUI::EmbossFreeTransform {

// What a finished handle drag did, seen from the part's surface (its local Z is the emboss
// direction, i.e. the normal of the surface it was placed on).
enum class Kind
{
    None,      // nothing moved
    Tangent,   // moved inside its own plane (along the surface)
    Normal,    // moved off / into the surface (any noticeable part of the move along its Z)
    InPlane,   // turned around its own Z (what the rotation ring does)
    Tilt,      // turned around an axis in its plane: no longer parallel to where it was
};

// Moves shorter than this are no move at all, and a move with a smaller component along the
// part's normal stays a move along the surface [mm].
constexpr double MOVE_EPSILON = 1e-3;
// A turn around an axis closer than this to the part's normal is a turn in its plane (cosine).
constexpr double IN_PLANE_COS = 0.9999;

// `displacement` and `normal` in the same (world) coordinates; `normal` need not be unit length.
Kind classify_move(const Vec3d &displacement, const Vec3d &normal);
// `axis` of the turn and the part's `normal` in the same coordinates; |angle| below 1e-9 is None.
Kind classify_rotation(const Vec3d &axis, const Vec3d &normal, double angle);

// How the part is laid on its object.
struct Projection
{
    bool use_surface = false; // "Use surface": the shape is projected onto the object's surface
    bool per_glyph   = false; // text only: letters placed one by one along the surface
    bool curved      = false; // text only: curved text (its per glyph follows "Use surface")

    bool follows_surface() const { return use_surface || per_glyph; }
    bool operator==(const Projection &o) const
    {
        return use_surface == o.use_surface && per_glyph == o.per_glyph && curved == o.curved;
    }
};

// What the tool does after a finished handle drag.
struct Outcome
{
    // Turn the surface projection off: the part becomes a flat, free placed part. A projection
    // follows the surface again on every update, so it would undo a lift, a push or a tilt.
    bool detach = false;
    // Emboss again (a projection onto the surface has to follow the new place).
    bool reprocess = false;
    // The "From surface" distance is the measured offset of the part along its normal (flat parts).
    bool measure_distance = false;
};

Outcome outcome(const Projection &projection, Kind kind);

// Projection after detaching (`outcome().detach`): surface and per glyph placement off. Curved text
// keeps its curve, now flat in the part's plane (the same as unticking "Use surface").
Projection detached(const Projection &projection);

// The projection a part had before a handle detached it is remembered; the next surface drag of the
// same part puts it back (the drag places the part on the surface again). Returns the projection
// to restore, or nothing when there is nothing to restore.
std::optional<Projection> reattach(const Projection &current, const std::optional<Projection> &remembered);

// How the part sits against the surface under it, for the line shown in the tool.
enum class Placement
{
    Object,    // the part is the whole object: there is no surface to be on
    Projected, // projected onto the surface ("Use surface" or per glyph)
    OnSurface, // flat, its origin on the surface and its normal along the surface normal
    Floating,  // flat, parallel to the surface, at a distance (the "From surface" value)
    Tilted,    // a surface is under it, but the part is not parallel to it
    Free,      // no surface (near enough) under it
};

// Ray from the part's origin along its -Z (both ways) to its object's other parts.
struct SurfaceProbe
{
    double distance  = 0.;  // signed: > 0 the part lies above (outside) the surface [mm]
    double cos_angle = 1.;  // cosine between the part's normal and the surface normal at the hit
};

constexpr double ON_SURFACE_DISTANCE = 0.01;  // [mm]
constexpr double PARALLEL_COS        = 0.9986; // ~3 degrees

// `max_distance` [mm]: farther than this from the surface the part counts as free.
Placement classify_placement(bool is_object, const Projection &projection, const std::optional<SurfaceProbe> &probe, double max_distance);

// --- Geometry of the handle drags ---------------------------------------------------------------

// Volume transformation that moves the part by `world_delta` (a rigid motion in world coordinates)
// when its instance has the transformation `instance`. Every instance shares the volume
// transformation, so the other instances follow in their own frames, as with the Move tool. The
// 3MF fix transformation of an emboss shape does not need special care: it stays right of the
// volume transformation and cancels out.
Transform3d moved_volume_matrix(const Transform3d &instance, const Transform3d &volume, const Transform3d &world_delta);

// Rigid turn by `angle` [rad] around `axis` (world, any length) through `pivot`.
Transform3d rotation_about(const Vec3d &pivot, const Vec3d &axis, double angle);

// Signed distance along `axis` (unit) of the point where the mouse ray (origin `ray_a`, unit
// direction `ray_dir`) passes the drag start `start` (the plane through `start` facing the ray).
// `snap` > 0 rounds to its multiples (SHIFT on the Move tool).
double axis_drag_distance(const Vec3d &start, const Vec3d &axis, const Vec3d &ray_a, const Vec3d &ray_dir, double snap = 0.);

// Difference of two ring angles mapped to (-PI, PI].
double angle_delta(double from, double to);

// An arrow pointing (nearly) at the camera cannot be dragged and would cover the part: hidden when
// |cos| of its angle to the view direction exceeds this.
constexpr double ARROW_HIDE_COS = 0.966; // 15 degrees
bool arrow_visible(const Vec3d &axis, const Vec3d &view_dir);

} // namespace Slic3r::GUI::EmbossFreeTransform

#endif // slic3r_EmbossFreeTransform_hpp_
