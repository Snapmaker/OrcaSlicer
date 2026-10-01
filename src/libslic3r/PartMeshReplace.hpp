#ifndef slic3r_PartMeshReplace_hpp_
#define slic3r_PartMeshReplace_hpp_

// Swapping a part's mesh for one edited in another program (the Blender and FreeCAD bridges,
// slic3r/GUI/ExternalEditorBridge.cpp).

namespace Slic3r {

class ModelVolume;
class TriangleMesh;

// Replaces the mesh of `volume` with `mesh`, which must be in the same mesh coordinates (the frame
// ModelVolume::mesh() is in, before the part's own transform and the instance's). The part keeps
// its transform, name, type, extruder, per-part settings and source; the object keeps its
// instances. Painted supports, seams, colours and fuzzy skin are stored per triangle of the old
// mesh, so they are dropped. The part gets a new id, so undo/redo and the scene see the change, and
// an object that was not sunk below the bed is put back on it.
// Returns true when painted data was dropped.
bool replace_part_mesh(ModelVolume &volume, TriangleMesh &&mesh);

} // namespace Slic3r

#endif // slic3r_PartMeshReplace_hpp_
