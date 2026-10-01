#ifndef slic3r_BRep_CadEdit_hpp_
#define slic3r_BRep_CadEdit_hpp_

// Exact B-rep edits on a part's CAD body: fillet and chamfer of picked edges, and shell
// (hollow to a wall thickness, opening picked faces), with OpenCASCADE's BRepFilletAPI /
// BRepOffsetAPI. The UI side is GLGizmoCadFillet.
//
// The fillet / chamfer / shell calls follow Orca-Cad's GeometryEngine::apply_fillet /
// apply_chamfer and CadDocument's Shell feature (github.com/tommasobbianchi/Orca-Cad, branch
// cad-mainline, AGPL-3.0): one BRepFilletAPI_MakeFillet / MakeChamfer per operation over the
// picked edges, and MakeThickSolidByJoin with a negative offset for an inward shell, failing
// with a reason rather than handing back the input. EdgeSlicer additions: edges and faces are
// addressed by their index in the shape's de-duplicated TopExp maps (stable for a given
// serialized body, which is what lets the UI pick on one copy and a worker thread operate on
// another), every result is checked with BRepCheck_Analyzer and must stay a solid, and the
// body travels with the ModelVolume (CadBody.hpp) so operations stack and STEP export writes
// the exact result.
//
// This header has no OpenCASCADE include, so GUI code can use it; the OCCT-typed helpers are
// in CadShape.hpp.

#include "CadBody.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class ModelVolume;

namespace BRep {

// How a body is turned into the part's mesh. The defaults are STEP import's own
// (AppConfig "linear_defletion" / "angle_defletion"), so a filleted STEP part is tessellated
// exactly as finely as the part it came from.
struct TessellationParams
{
    double linear_deflection  = 0.003; // mm
    double angular_deflection = 0.5;   // rad
};

// ---- sourcing -------------------------------------------------------------------------------

// The body attached to the volume, when the volume's mesh is still the one the body was made
// for (allowing a pure translation, which is folded into the returned body's shift). Null when
// there is none or the mesh has been edited since. Cheap: no OCCT work.
std::shared_ptr<const CadBody> attached_cad_body(const ModelVolume &volume);

// Recover the exact body of a volume imported from STEP by re-reading its source file (see
// step_source_brep() in STEPExport.hpp: the mesh must still be the file's tessellation).
// Slow (reads the file). Null with a reason otherwise.
std::shared_ptr<const CadBody> cad_body_from_step_source(const ModelVolume &volume, std::string *why_not = nullptr);
// The same from a copy of what it reads, safe on a worker thread.
struct StepSourceRef
{
    std::string input_file;
    std::string volume_name;
    Vec3d       mesh_offset = Vec3d::Zero();
};
StepSourceRef                  step_source_ref(const ModelVolume &volume);
bool                           is_step_file(const std::string &path);
std::shared_ptr<const CadBody> cad_body_from_step_source(const StepSourceRef &source, const indexed_triangle_set &mesh,
                                                         std::string *why_not = nullptr);

// Convert a closed triangle mesh into a CAD body (BRep::mesh_to_brep: coplanar triangles merged
// into planar faces). Refused - null with a reason - for an open or non-manifold mesh, which
// would become a shell that can be neither filleted nor printed as a solid.
struct MeshConversionReport
{
    int         triangles = 0;
    int         faces     = 0; // B-rep faces after the coplanar merge
    int         edges     = 0;
    double      seconds   = 0.;
    std::string error;
};
std::shared_ptr<const CadBody> cad_body_from_mesh(const indexed_triangle_set &its, MeshConversionReport &report);

// Triangle counts above which "Convert to CAD body" warns, and above which it is refused.
constexpr int ConvertWarnTriangles = 5000;
constexpr int ConvertMaxTriangles  = 50000;

// ---- topology for picking and drawing ----------------------------------------------------------

// Edges and faces are numbered 0..n-1 in the order of the shape's de-duplicated TopExp maps.
struct CadTopology
{
    // A tessellation for hit-testing (NOT the part's mesh): triangle t belongs to face triangle_face[t].
    indexed_triangle_set mesh;
    std::vector<int>     triangle_face;
    int                  num_faces = 0;
    int                  num_edges = 0;
    BoundingBoxf3        bbox;

    // Per edge.
    std::vector<std::vector<Vec3f>> edge_polylines;
    std::vector<std::array<int, 2>> edge_faces;      // the two faces it bounds, -1 when fewer
    // An edge can be filleted or chamfered: it bounds exactly two different faces, meeting at
    // an angle (not tangent), and is neither degenerate nor a seam.
    std::vector<uint8_t>            edge_selectable;
    // Selectable edges that continue this one smoothly (tangent) through a shared vertex:
    // what "tangent chain" grows along.
    std::vector<std::vector<int>>   edge_tangent_neighbours;

    // Per face.
    std::vector<std::vector<int>> face_edges;
    std::vector<uint8_t>          face_planar;
};

// Throws Slic3r::RuntimeError when the body cannot be read.
CadTopology cad_topology(const CadBody &body, double linear_deflection, double angular_deflection);

// The edge, plus every selectable edge reachable from it through tangent continuations.
std::vector<int> tangent_chain(const CadTopology &topo, int edge);
// The selectable edges of a face.
std::vector<int> face_selectable_edges(const CadTopology &topo, int face);

// ---- operations -------------------------------------------------------------------------------

enum class EdgeFeature { Fillet, Chamfer };

enum class CadOpStatus {
    Ok,
    NothingSelected, // no edge / face given
    BadSize,         // radius, distance or thickness <= 0
    BadIndex,        // an index is out of range, or the edge cannot take the feature
    Failed,          // OpenCASCADE could not build it
    InvalidResult,   // it built something, but not a valid solid
};

struct CadOpResult
{
    CadOpStatus                    status = CadOpStatus::Failed;
    std::string                    error;  // why, in plain words, when status != Ok
    std::shared_ptr<const CadBody> body;   // Ok: the new body, fingerprinted to `mesh`
    indexed_triangle_set           mesh;   // Ok: its tessellation, vertices welded
    int                            faces_before  = 0;
    int                            faces_after   = 0;
    double                         volume_before = 0.;
    double                         volume_after  = 0.;
    // A fillet / chamfer that failed: the largest size that does build for the same edges (found
    // by bisection, 0 when even small sizes fail), and the shortest selected edge. Mesh units.
    double                         largest_size  = 0.;
    double                         shortest_edge = 0.;
    double                         seconds       = 0.;

    bool ok() const { return status == CadOpStatus::Ok; }
};

// Fillet (radius) or chamfer (equal distance on both faces) the given edges, all at once.
CadOpResult fillet_edges(const CadBody &body, EdgeFeature feature, double size, const std::vector<int> &edges,
                         const TessellationParams &tess = {});

// Hollow the solid to `thickness` (wall inward from the outer surface), leaving the given
// faces open. At least one face must be opened.
CadOpResult shell_solid(const CadBody &body, const std::vector<int> &open_faces, double thickness,
                        const TessellationParams &tess = {});

// Sizes are typed in world millimetres; the body lives in the volume's mesh space. The factor
// between the two for `volume_to_world` (instance * volume matrix): the mean of the three scale
// factors (a roughly uniform scale is assumed). mesh size = world size / mean_scale().
double mean_scale(const Transform3d &volume_to_world);

// Swap the volume's mesh for `result.mesh` and attach `result.body` (re-fingerprinted to the
// mesh the volume actually ends up with). The volume keeps its transformation, name, settings
// and source; painted supports, seams, colours and fuzzy skin are cleared because they are
// stored per triangle of the old mesh. Returns true when painted data was cleared.
// The caller updates the object (bounding box, on-bed) and takes the undo snapshot.
bool apply_cad_result(ModelVolume &volume, const CadOpResult &result);

}} // namespace Slic3r::BRep

#endif // slic3r_BRep_CadEdit_hpp_
