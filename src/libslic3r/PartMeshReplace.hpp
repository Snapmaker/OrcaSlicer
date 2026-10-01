#ifndef slic3r_PartMeshReplace_hpp_
#define slic3r_PartMeshReplace_hpp_

// Swapping a part's mesh for one edited in another program (the Blender and FreeCAD bridges,
// slic3r/GUI/ExternalEditorBridge.cpp).

#include <memory>

namespace Slic3r {

class ModelVolume;
class TriangleMesh;
namespace BRep { class CadBody; }

// Replaces the mesh of `volume` with `mesh`, which must be in the same mesh coordinates (the frame
// ModelVolume::mesh() is in, before the part's own transform and the instance's). The part keeps
// its transform, name, type, extruder, per-part settings and source; the object keeps its
// instances. Painted supports, seams, colours and fuzzy skin are stored per triangle of the old
// mesh, so they are dropped. The part gets a new id, so undo/redo and the scene see the change, and
// an object that was not sunk below the bed is put back on it.
// `cad_body`, when given, is the exact solid the new mesh is the tessellation of (in the same mesh
// coordinates); it is attached, re-fingerprinted to the mesh the volume ends up with, so the part
// stays exact for STEP export and the CAD tools. Without one, a body the old mesh had stays on the
// volume; BRep::attached_cad_body() ignores it unless the new mesh is the same one.
// Returns true when painted data was dropped.
bool replace_part_mesh(ModelVolume &volume, TriangleMesh &&mesh, std::shared_ptr<const BRep::CadBody> cad_body = nullptr);

} // namespace Slic3r

#endif // slic3r_PartMeshReplace_hpp_
