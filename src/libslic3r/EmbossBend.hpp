#ifndef slic3r_EmbossBend_hpp_
#define slic3r_EmbossBend_hpp_

// Curved (arc) text: an in-plane bend of the 2D text outlines around the emboss direction.
//
// Geometry (text space, y up, the reference line is y = 0 through the volume origin):
//   s     = +1 for arch (text outside the circle), -1 for smile (text inside the circle)
//   theta = x / R          (arc length along the reference circle equals the straight x)
//   rho   = R + s * y
//   X     = rho * sin(theta)
//   Y     = s * (rho * cos(theta) - R)
// The origin maps onto itself, the arc centre is at (0, -s * R) and the map keeps the
// orientation of polygons (its Jacobian determinant is rho / R > 0).
//
// Everything here is a pure function of the 2D shapes, so extrusion, surface cut,
// volume types and the 3MF mesh bake downstream stay unchanged. With "Use surface" the bent
// outlines are projected along the text's -z onto the object like straight text.

#include <optional>
#include <vector>
#include "Point.hpp"
#include "Polygon.hpp"
#include "ExPolygon.hpp"
#include "EmbossShape.hpp" // EmbossBend, ExPolygonsWithIds
#include "Emboss.hpp"      // GlyphAdvances

namespace Slic3r::Emboss {

// Largest allowed span of the longest line, a closed 360 degree ring is not supported
constexpr double BEND_MAX_ANGLE_DEG = 359.;
// Allowed deviation of a densified straight edge from the true arc / spiral [mm]
constexpr double BEND_TOLERANCE_MM = 0.01;
// Same with "Use surface". The surface cut projects every straight 2D edge exactly (as the section
// of the surface with the plane through the edge and the projection direction), so the density only
// has to follow the arc, not the surface. A coarser sagitta keeps the CGAL cut cheaper; 0.02 mm is
// still far below a printed line width.
constexpr double BEND_SURFACE_TOLERANCE_MM = 0.02;
// The reference radius must exceed the glyph extent facing the arc centre by this ratio
constexpr double BEND_MIN_RADIUS_RATIO = 1.05;

// Resolved bend in shape units
struct BendSpec
{
    // radius of the reference line (y = 0) [shape units]; <= 0 means no bend
    double radius = 0.;
    // smile: text inside the circle
    bool inside = false;
    // maximal sagitta of one densified segment [shape units]
    double tolerance = 0.;

    bool is_active() const { return radius > 0.; }
    double side() const { return inside ? -1. : 1.; }
    // arc centre in text space [shape units]
    Vec2d center() const { return Vec2d(0., -side() * radius); }
};

// Measured text needed to resolve a bend [shape units]
struct BendInput
{
    // width of the widest line (advance boxes united with outlines)
    double width = 0.;
    // vertical extent of all outlines
    double y_min = 0.;
    double y_max = 0.;
    bool   is_valid() const { return width > 0. && y_max >= y_min; }
};

struct BendResult
{
    BendSpec spec;
    // resulting span of the widest line [deg]
    double angle_deg = 0.;
    // resulting radius of the reference circle [mm]
    double radius_mm = 0.;
    // radius was raised so the glyphs facing the centre fit (angle mode: span got smaller)
    bool limited_by_height = false;
    // radius was raised so the text fits into BEND_MAX_ANGLE_DEG (radius mode)
    bool limited_by_length = false;

    bool is_active() const { return spec.is_active(); }
};

// Width of the widest line and vertical extent. Lines are separated by ENTER_UNICODE ids.
// advances: optional advance boxes from text2vshapes (same size as shapes), else outlines only.
BendInput measure_bend_input(const ExPolygonsWithIds &shapes, const GlyphAdvances *advances = nullptr);

// Resolve user parameters into a radius in shape units, with clamping.
// shape_scale = mm per shape unit (EmbossShape::scale)
// tolerance_mm = allowed sagitta of a densified edge, see bend_tolerance_mm()
BendResult resolve_bend(const EmbossBend &bend, const BendInput &input, double shape_scale,
                        double tolerance_mm = BEND_TOLERANCE_MM);

// Map one point of text space onto the arc
Vec2d bend_point(const Vec2d &p, const BendSpec &spec);

// Warp (bent letters): densify edges, then map every point. Inactive spec returns the input.
Polygon    bend_polygon(const Polygon &polygon, const BendSpec &spec);
ExPolygons bend_expolygons(const ExPolygons &shape, const BendSpec &spec);
void       bend_shapes(ExPolygonsWithIds &shapes, const BendSpec &spec);

// x of the pivot of every shape: centre of its advance box, else the centre of its outline.
// Shapes without outline get NaN.
std::vector<double> glyph_pivots(const ExPolygonsWithIds &shapes, const GlyphAdvances *advances = nullptr);

// Rigid letters: every glyph keeps its shape, it is rotated about (pivot_x, 0) by the tangent
// angle and moved so the pivot lands on the reference circle. pivots_x as from glyph_pivots().
void place_glyphs_on_arc(ExPolygonsWithIds &shapes, const std::vector<double> &pivots_x, const BendSpec &spec);

// Measure, resolve and apply (bent or rigid). Inactive bend leaves shapes untouched.
BendResult apply_bend(ExPolygonsWithIds &shapes, const EmbossBend &bend, double shape_scale,
                      const GlyphAdvances *advances = nullptr, double tolerance_mm = BEND_TOLERANCE_MM);

// Densify tolerance for flat text or for text projected onto the surface [mm]
inline double bend_tolerance_mm(bool use_surface) { return use_surface ? BEND_SURFACE_TOLERANCE_MM : BEND_TOLERANCE_MM; }

// ---- Placing the arc on an object (text plane coordinates [mm], z towards the text) ----

struct RoundOutline
{
    Vec2d  center = Vec2d::Zero();
    double radius = 0.;
};

// Algebraic (Kasa) circle fit through rim points. Empty with fewer than 12 points, when the points
// are not round (rms deviation above 1 % of the radius) or when they cover only a part of the
// circle (an angular gap above 45 degrees).
std::optional<RoundOutline> fit_round_outline(const std::vector<Vec2d> &rim);

struct SurfaceRound
{
    // circle of the outline of the target seen along the projection direction
    Vec2d  center = Vec2d::Zero();
    double radius = 0.;
    // outermost radius where the surface facing the text is not steeper than max_slope_deg, walking
    // inward from the outline (a hemisphere gives radius * sin(max_slope), a flat cap the radius)
    double usable_radius = 0.;
};

// Round outline of a curved target (dome, sphere, lid, cylinder end) seen along the text's
// projection direction (-z). points: vertices of the target in text coordinates.
// Empty when the outline is not round.
std::optional<SurfaceRound> surface_round_area(const std::vector<Vec3d> &points, double max_slope_deg = 45.);

} // namespace Slic3r::Emboss

#endif // slic3r_EmbossBend_hpp_
