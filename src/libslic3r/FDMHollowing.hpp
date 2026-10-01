#pragma once

// Hollowing for FDM prints, on top of the SLA hollowing code (libslic3r/SLA/Hollowing).
//
// The SLA hollower offsets the whole surface inward by a fixed distance in 3D (an OpenVDB distance
// field), so the remaining shell is equally thick everywhere - on sloped and curved faces too,
// where FDM's per-layer top/bottom shell counts leave thin spots. Here the inner surface is
// sliced at the object's layers and cut out of every region's slices, as if it were a negative
// volume: the part prints as a closed shell with an empty cavity inside. The cavity's ceiling is
// bridged like any other overhang.

#include "ExPolygon.hpp"

#include <functional>
#include <vector>

namespace Slic3r {

class PrintObject;

// Cavity outlines per slicing height, in the object's slicing frame. Empty when the object is
// not hollowed or its walls are too thin to leave a cavity.
std::vector<ExPolygons> hollow_cavity_slices(const PrintObject &object, const std::vector<float> &slice_zs,
                                             const std::function<void()> &throw_if_canceled);

} // namespace Slic3r
