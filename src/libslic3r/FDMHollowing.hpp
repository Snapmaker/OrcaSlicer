#pragma once

// Hollowing for FDM prints, on top of the SLA hollowing code (libslic3r/SLA/Hollowing).
//
// The SLA hollower offsets the whole surface inward by a fixed distance in 3D (an OpenVDB distance
// field), so the remaining shell is equally thick everywhere - on sloped and curved faces too,
// where FDM's per-layer top/bottom shell counts leave thin spots. Here the inner surface is
// sliced at the object's layers and cut out of the part's slices, as if it were a negative
// volume: the part prints as a closed shell with an empty cavity inside. The cavity's ceiling is
// bridged like any other overhang.
//
// hollow_interior and hollow_shell_thickness are region settings, so a model part may override
// the object's values. Every model part is hollowed on its own, with its own settings, and its
// cavity is cut only out of that part's slices: a hollow part next to a solid one in the same
// object stays next to a solid one. Negative volumes and modifiers are never hollowed.

#include "ExPolygon.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

class ModelVolume;
class PrintObject;
class PrintRegionConfig;
struct VolumeSlices;

// The region settings a model part is sliced with (the object's, overridden by the part's own),
// or null when the part has no region in this object.
const PrintRegionConfig *hollowing_config(const PrintObject &object, const ModelVolume &volume);

// Cuts the cavity of every hollowed model part out of that part's own slices in `volume_slices`
// (one VolumeSlices per sliced volume, at `slice_zs` - see PrintObject::slice_volumes()). Runs
// before the slices are split into regions, so modifiers and the part's other regions see the
// cavity too. Returns one message per part that asked to be hollowed and could not be, or only
// partly: to be shown to the user as slicing warnings.
std::vector<std::string> hollow_volume_slices(const PrintObject &object, const std::vector<float> &slice_zs,
                                              std::vector<VolumeSlices> &volume_slices,
                                              const std::function<void()> &throw_if_canceled);

// How deep the inside of a sliced part reaches, mm: the largest r such that some point of it is at
// least r from its surface, measured with a cylinder of radius r and height 2r (so a little less
// than the true inscribed sphere). `slices` are the part's slices at `slice_zs`. Exposed for tests.
double hollowing_depth(const std::vector<ExPolygons> &slices, const std::vector<float> &slice_zs);

// The fillet the hollower keeps between the shell and the cavity, mm: a cavity forms only where
// the part is deeper than the shell thickness plus this.
constexpr double HOLLOWING_CLOSING_DISTANCE = 2.;

} // namespace Slic3r
