#ifndef slic3r_BRep_CadBody_hpp_
#define slic3r_BRep_CadBody_hpp_

// The exact B-rep ("CAD body") that stands behind a ModelVolume's triangle mesh.
//
// A ModelVolume is a mesh. A part that came from STEP, or that was converted with
// BRep::cad_body_from_mesh(), can also carry the exact solid its mesh is a tessellation of,
// so that exact operations (fillet, chamfer, shell - see CadEdit.hpp) can be applied to it,
// stacked, and exported to STEP without going through the mesh.
//
// The body is kept honest by a fingerprint of the mesh it belongs to: every tool that edits a
// mesh (cut, simplify, sculpt, remesh, repair, ...) replaces the mesh without knowing about the
// body, and then the fingerprint no longer matches and the body is simply ignored
// (BRep::attached_cad_body() returns null). Only a pure translation of the mesh - which
// ModelVolume::center_geometry_after_creation() does in place - is followed.
//
// This header deliberately has no OpenCASCADE include: Model.hpp includes it, and the
// B-rep is carried as an opaque BinTools blob. CadEdit.hpp holds the operations.

#include "libslic3r/Point.hpp"

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

#include <cstdint>
#include <memory>
#include <string>

struct indexed_triangle_set;

namespace Slic3r { namespace BRep {

// Cheap, translation-aware summary of a triangle mesh. Two meshes with the same fingerprint
// are, for every practical purpose, the same mesh (possibly translated): same counts, same
// volume, same extents and the same vertex centroid and second moments.
struct MeshFingerprint
{
    uint32_t facets   = 0;
    uint32_t vertices = 0;
    double   volume   = 0.;
    Vec3d    bbox_min = Vec3d::Zero();
    Vec3d    bbox_max = Vec3d::Zero();
    Vec3d    centroid = Vec3d::Zero(); // mean of the vertices
    Vec3d    moments  = Vec3d::Zero(); // mean (x-cx)^2, (y-cy)^2, (z-cz)^2 of the vertices
    Vec3d    products = Vec3d::Zero(); // mean (x-cx)(y-cy), (y-cy)(z-cz), (z-cz)(x-cx)

    bool empty() const { return facets == 0; }

    // Does `now` describe this mesh, moved by some translation? On success `shift` receives
    // the translation (now = this + shift). The tolerances absorb float round-off, including a
    // 3MF save/load round trip; any real edit changes the counts, the volume or the extents.
    bool matches(const MeshFingerprint &now, Vec3d &shift) const;

    template<class Archive> void serialize(Archive &ar)
    {
        ar(facets, vertices, volume, bbox_min, bbox_max, centroid, moments, products);
    }
};

MeshFingerprint mesh_fingerprint(const indexed_triangle_set &its);

// Where a body came from, for the UI.
enum class CadBodyOrigin : uint8_t {
    StepFile      = 0, // recovered from the part's source STEP file
    ConvertedMesh = 1, // built from the part's mesh by BRep::mesh_to_brep()
};

// Immutable once built; shared between the volume, its copies and the Undo / Redo stack.
class CadBody
{
public:
    // OCCT BinTools serialization of the shape, without triangulation, in the frame the shape
    // was built in. `shift` moves it into the volume's MESH coordinates.
    std::string     brep;
    Vec3d           shift = Vec3d::Zero();
    // The volume mesh this body is the exact form of.
    MeshFingerprint mesh;
    CadBodyOrigin   origin = CadBodyOrigin::StepFile;
    // Exact operations applied since the body was sourced (fillets, chamfers, shells).
    int             operations = 0;

    // A copy that describes the same solid for a volume mesh that was translated by `delta`.
    std::shared_ptr<const CadBody> translated(const Vec3d &delta, const MeshFingerprint &now) const;

    // Required by the Undo / Redo stack (ImmutableObjectHistory).
    size_t memsize() const { return sizeof(*this) + brep.capacity(); }
    size_t release_optional() { return 0; }
    void   restore_optional() {}

    template<class Archive> void serialize(Archive &ar)
    {
        uint8_t o = uint8_t(origin);
        ar(brep, shift, mesh, o, operations);
        origin = CadBodyOrigin(o);
    }

    // A self-contained blob (header + fields) for the 3MF, and back. from_blob() returns null on
    // anything it does not recognise.
    std::string                            to_blob() const;
    static std::shared_ptr<const CadBody> from_blob(const std::string &blob);
};

}} // namespace Slic3r::BRep

#endif // slic3r_BRep_CadBody_hpp_
