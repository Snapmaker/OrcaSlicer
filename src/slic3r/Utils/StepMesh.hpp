#ifndef slic3r_StepMesh_hpp_
#define slic3r_StepMesh_hpp_

#include <atomic>
#include <string>

#include "MeshThumbnail.hpp"

// A STEP file as a bare triangle list, for the Library's previews (Utils/MeshThumbnail). Kept apart
// from MeshThumbnail.cpp because it is the one reader that needs OpenCASCADE, whose Handle() macro
// must not meet CGAL's headers in a unity build.
namespace Slic3r {
namespace Library {

// A coarse tessellation, enough for a picture: the chord tolerance follows the part's size.
// `cancel` stops the reading and the meshing. false when the file cannot be read, is too big or has
// no faces.
bool read_step(const std::string& path, Triangles& out, const MeshLimits& limits = step_limits(),
               const std::atomic<bool>* cancel = nullptr);

} // namespace Library
} // namespace Slic3r

#endif
