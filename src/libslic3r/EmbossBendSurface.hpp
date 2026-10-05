#ifndef slic3r_EmbossBendSurface_hpp_
#define slic3r_EmbossBendSurface_hpp_

// Curved text placed letter by letter on a surface ("Curve" + "Use surface" + per letter).
//
// The flat arc of EmbossBend is carried over to the surface with geodesic polar coordinates:
//   - the text origin O lies on the surface; the arc centre C is reached by walking along the
//     surface from O towards the centre side (-y for arch, +y for smile) a geodesic distance rho,
//   - the reference "circle" is the set of points a geodesic distance rho from C (walking out of C
//     in every direction), so on a developable surface (plane, cylinder, cone) it is exactly the
//     flat arc rolled onto the surface,
//   - the glyphs are spaced by arc length measured along that curve on the surface, so letters do
//     not spread out or bunch on spheres and cylinders,
//   - every glyph gets its own frame: origin on the surface under its pivot, z along the local
//     surface normal, x along the curve in reading direction. The glyph is then projected onto the
//     surface along its own normal.
// Radius mode keeps rho = the requested radius (measured along the surface). Angle mode solves
// rho so that the widest line spans the requested angle around C.
//
// Geodesics are traced by a "straightest walk": small steps in the tangent plane, each step
// projected back onto the mesh (closest point) and the direction re-projected into the new tangent
// plane. Normals are interpolated from per-corner normals that only smooth across edges under 45
// degrees, so sharp edges stay sharp.

#include <memory>
#include <optional>
#include <vector>

#include "Point.hpp"
#include "ExPolygon.hpp"
#include "admesh/stl.h"
#include "EmbossBend.hpp"

namespace Slic3r {
class AABBMesh;
}

namespace Slic3r::Emboss {

// Triangle mesh the text is placed on, in text coordinates [mm]
class BendSurface
{
public:
    explicit BendSurface(indexed_triangle_set its);
    ~BendSurface();
    BendSurface(const BendSurface &) = delete;
    BendSurface &operator=(const BendSurface &) = delete;

    bool empty() const;

    struct Point
    {
        Vec3d position = Vec3d::Zero();
        Vec3d normal   = Vec3d::UnitZ();
    };
    // Closest point of the surface with the smooth normal there
    std::optional<Point> closest(const Vec3d &p) const;

    struct Walk
    {
        Point  end;
        Vec3d  direction = Vec3d::UnitX(); // tangent at the end, unit
        double length    = 0.;             // walked length (shorter than asked when stuck)
        bool   complete  = false;
    };
    // Straightest walk from `start` along the tangent `direction` for `length` [mm].
    // step <= 0: automatic (length / 48 within 0.05 .. 0.5 mm)
    Walk walk(const Point &start, const Vec3d &direction, double length, double step = 0.) const;

    // Flip all normals (text placed on the inside of a shell)
    void set_flip_normals(bool flip) { m_flip = flip; }

private:
    std::unique_ptr<indexed_triangle_set> m_its;
    std::unique_ptr<AABBMesh>             m_tree;
    // per face corner, smoothed across edges under 45 degrees
    std::vector<std::array<Vec3d, 3>>     m_corner_normals;
    bool                                  m_flip = false;
};

struct SurfaceArcParams
{
    // resolved flat radius of the reference line [mm] (angle mode: width / angle)
    double radius = 0.;
    bool   inside = false; // smile
    // angle mode: requested span of the widest line [rad]; empty in radius mode
    std::optional<double> span;
    // widest line [mm]
    double width = 0.;
    // glyphs facing the centre must fit: lower limit of the geodesic radius [mm]
    double min_radius = 0.;
    // largest span of the widest line [rad]
    double max_span = 359. * M_PI / 180.;
};

// Reference curve of the last placement, for the gizmo overlay (text coordinates [mm])
struct SurfaceArcPreview
{
    bool   valid    = false;
    Vec3d  center   = Vec3d::Zero(); // arc centre on the surface
    Vec3d  normal   = Vec3d::UnitZ(); // surface normal there
    double radius   = 0.;            // geodesic radius [mm]
    double span_deg = 0.;            // span of the widest line around the centre
    bool   limited  = false;         // radius / angle had to be changed to fit
    // closed loop around the centre; points where the walk got stuck are left out (not ok)
    std::vector<Vec3d> circle;
    std::vector<Vec3d> circle_normal;
    std::vector<bool>  circle_ok;
};

struct SurfaceArc
{
    SurfaceArcPreview preview;
    // per pivot: glyph frame -> text coordinates (origin on the surface under the pivot,
    // z = surface normal, x = reading direction along the curve); empty for missing pivots
    std::vector<std::optional<Transform3d>> frames;
    // per pivot: radius of curvature of the curve in the glyph's tangent plane [mm]
    // (for the local warp of bent letters); 0 = straight
    std::vector<double> curvature_radius;
};

// pivots_mm: x of every glyph pivot along the reference line [mm] (NaN = no glyph).
// x_min / x_max: extent of the widest line [mm] (used for the span).
SurfaceArc place_on_surface_arc(const BendSurface &surface, const std::vector<double> &pivots_mm,
                                double x_min, double x_max, const SurfaceArcParams &params);

// Glyph layout of a text for the letter by letter placement
struct SurfaceGlyphLayout
{
    std::vector<double> pivots;    // [shape units], NaN for glyphs without outline
    std::vector<double> pivots_mm; // the same in mm
    double              x_min = 0., x_max = 0.; // extent of the text [mm]
    SurfaceArcParams    params;
};
// Empty when the bend is not active or there is nothing to place
std::optional<SurfaceGlyphLayout> surface_glyph_layout(const ExPolygonsWithIds &shapes, const GlyphAdvances *advances,
                                                       const EmbossBend &bend, double shape_scale);

// Outline of a glyph in its own frame: pivot at the origin, the reference line on the x axis.
// Bent letters get the local warp with the curvature of the curve on the surface.
ExPolygons surface_glyph_shape(const ExPolygons &glyph, double pivot, double curvature_radius_mm, const EmbossBend &bend,
                               double shape_scale);

} // namespace Slic3r::Emboss

#endif // slic3r_EmbossBendSurface_hpp_
