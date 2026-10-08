#ifndef slic3r_EmbossPicking_hpp_
#define slic3r_EmbossPicking_hpp_

// Picking rules that make text and SVG parts (emboss volumes) easy to grab in the 3D scene.
//
// Two problems made these parts hard to pick:
//  1. Plain closest-hit picking lets the part's own object win whenever the text is a little
//     sunk into the surface (flat text on a curved face, "use surface" text, thin relief), so the
//     click lands on the object behind the text.
//  2. While the Text / SVG tool is open, a drag only started on the glyph strokes themselves.
//     A press between the letters hovered the object behind, which closed the tool and
//     started moving the whole object instead.
//
// The rules live here, free of OpenGL and wxWidgets, so they can be unit tested.

#include <cstddef>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r::GUI::EmbossPicking {

// One volume the picking ray hit: its closest front-facing hit.
struct VolumeHit
{
    // Distance of the hit from the camera, along the view direction [mm].
    double depth{ 0. };
    // Model object and instance the volume belongs to (-1 for wipe towers and other helpers).
    int object_id{ -1 };
    int instance_id{ -1 };
    // >= 0 for text and SVG parts: how far behind the closest hit of their own object they may lie
    // and still be the one picked [mm]. Negative for every other volume.
    double prefer_tolerance{ -1. };

    bool is_text_or_svg() const { return prefer_tolerance >= 0.; }
};

// Smallest and largest depth tolerance given to a text or SVG part [mm].
constexpr double MIN_PREFER_TOLERANCE = 0.1;
constexpr double MAX_PREFER_TOLERANCE = 3.0;

// Depth tolerance of a text or SVG part whose emboss depth (in world mm) is given: the part wins
// while it is hidden by no more than its own thickness, clamped to [MIN, MAX]_PREFER_TOLERANCE.
double prefer_tolerance_from_depth(double emboss_depth_world);

// `closest` is the index (into `hits`) of the hit that plain closest-hit picking chose.
// Returns the index of the hit to report instead: a text or SVG part of the same object instance
// that lies no more than its tolerance behind the closest hit. The nearest such part wins. If the
// closest hit already is a text or SVG part, or none qualifies, `closest` is returned unchanged.
size_t prefer_text_or_svg(const std::vector<VolumeHit> &hits, size_t closest);

// What the mouse hovers while the Text or SVG tool edits a volume.
enum class HoverOwner
{
    Nothing,          // empty space or the print bed
    EditedVolume,     // the volume being edited
    SameObjectPart,   // another part, modifier or negative volume of the edited volume's object instance
    OtherTextOrSvg,   // another text or SVG volume (a press on it reselects that one)
    OtherObject,      // a volume of another object or instance
    GizmoHandle,      // a grabber of the tool itself
};

// May the edited volume's on-screen footprint take the hover (and so the drag) over from `owner`?
bool footprint_takes_over(HoverOwner owner);

// Padding added around a volume's on-screen footprint [logical pixels].
constexpr double FOOTPRINT_PADDING_PX = 6.0;

// True when `point` lies inside the convex polygon `hull` (screen pixels), or no further than
// `padding` from its outline. Degenerate hulls (a point or a segment) are padded the same way.
bool footprint_contains(const Polygon &hull, const Vec2d &point, double padding);

} // namespace Slic3r::GUI::EmbossPicking

#endif // slic3r_EmbossPicking_hpp_
