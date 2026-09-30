#pragma once

#include "Point.hpp"

#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;

// A face of an object's convex hull that the object can rest on. These are the faces the
// "Lay on Face" gizmo offers and the ones the CLI --ground-* options choose from.
//
// Frames: "object" coordinates have the volume transformations applied but not the instance
// transformation. "Instance" coordinates additionally have the instance rotation, scale and
// mirror applied, but not its offset.
struct LayOnFacePlane
{
    Vec3d       normal;         // outward unit normal, object coordinates
    Vec3d       center;         // centroid of the outline, object coordinates; on the face's mean plane
    float       area;           // mm², instance coordinates
    Pointf3s    outline;        // convex outline in the plane frame, where the face is horizontal
    Transform3d to_plane_frame; // rotation from instance coordinates to the plane frame
};

// Candidate faces of the object's model parts, largest first. The instance transformation
// (without offset) is applied before measuring, so faces too small to rest on are dropped
// by their printed size: under 5 mm², a side under 1 mm, or an inner angle under 1°.
std::vector<LayOnFacePlane> lay_on_face_planes(const ModelObject &object, const Transform3d &instance_matrix_no_offset);

// Candidate faces of a single model part, measured the same way. Normals are still in object coordinates,
// so the planes can be passed to the functions below like the object's.
std::vector<LayOnFacePlane> lay_on_face_planes(const ModelVolume &volume, const Transform3d &instance_matrix_no_offset);

// Index of the largest plane, or -1 if `planes` is empty. Of planes with the same area, such as
// the top and bottom of a box, the one already facing down the most wins, so flat parts stay put.
int find_largest_plane(const std::vector<LayOnFacePlane> &planes);

// Index of the plane whose normal is closest to `direction` (object coordinates),
// or -1 if `planes` is empty.
int find_plane_by_normal(const std::vector<LayOnFacePlane> &planes, const Vec3d &direction);

// Index of the plane whose face contains `point` (object coordinates) within `tolerance` mm, or -1
// if there is none. `instance_matrix_no_offset` is the one the planes were computed with.
int find_plane_at_point(const std::vector<LayOnFacePlane> &planes, const Transform3d &instance_matrix_no_offset,
                        const Vec3d &point, double tolerance);

// Rotates the instance so that `normal` (object coordinates) points down, the same rotation as
// the gizmo applies, then drops the instance so its lowest point is at z = 0.
void lay_on_face(ModelObject &object, size_t instance_idx, const Vec3d &normal);

// The part's new volume matrix after turning it about its own center so that `normal` (object coordinates,
// from the part's planes) points down in the world, and then sliding it along world Z so that face lies at the
// bed level z = 0 (its XY position is unchanged). `volume_matrix` is the part's current volume matrix and
// `instance_matrix` (with its offset) places the object in the world; only the part moves.
Transform3d lay_part_on_face_matrix(const ModelVolume &volume, const Transform3d &volume_matrix, const Transform3d &instance_matrix,
                                    const Vec3d &normal);

// The z translation that puts the lowest point of the object's instance on the bed (z = 0), measured on the
// parts' exact geometry like lay_on_face() does. Zero when it is on the bed already (within a nanometre).
double bed_drop_shift(const ModelObject &object, size_t instance_idx);

// Puts the lowest point of every instance of the object on the bed. Used after a part has been turned: the
// parts share the instances, so all of them drop.
void drop_object_to_bed(ModelObject &object);

// Rotates one part of the object with lay_part_on_face_matrix(), which puts the picked face on the bed, then raises
// the object if another part is left below z = 0. The other parts and the instance rotation stay as they were.
void lay_part_on_face(ModelObject &object, size_t instance_idx, size_t volume_idx, const Vec3d &normal);

} // namespace Slic3r
