#ifndef slic3r_BRep_MeshToBRep_hpp_
#define slic3r_BRep_MeshToBRep_hpp_

// Triangle mesh <-> OpenCASCADE B-rep conversion.
//
// mesh_to_brep() follows the approach of Orca-Cad's GeometryEngine::mesh_to_brep
// (github.com/tommasobbianchi/Orca-Cad, AGPL-3.0, itself a port of
// github.com/tommasobbianchi/mesh2step): vertices and edges are SHARED between triangles at
// construction time, so the topology is sewn by construction instead of by a
// BRepBuilderAPI_Sewing pass, and watertightness falls out of the edge-usage counts.
// EdgeSlicer additions: connected components become separate solids, closed inward-facing
// components become cavities of the solid that contains them, open components become shells
// with a warning, all sub-shape tolerances are set from the mesh's float precision so the
// coplanar merge yields a valid B-rep, and the straight edges left collinear on a merged face's
// boundary are joined by a linear pass (UnifySameDomain's own edge merge is far from linear).

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace Slic3r { namespace BRep {

struct MeshToBRepParams
{
    // Weld distance (mm): vertices closer than this become one vertex, and a triangle whose
    // longest edge is shorter is dropped as noise. It is raised to the float precision of
    // the mesh's coordinates when that is coarser. Never used as a sewing tolerance.
    double tolerance       = 1e-4;
    // Neighbouring coplanar faces whose normals differ by less than this are merged into one
    // planar face (a 12-triangle cube becomes 6 faces). <= 0 keeps one face per triangle.
    double merge_angle_deg = 0.1;
};

struct MeshToBRepStats
{
    int    input_triangles      = 0;
    int    kept_triangles       = 0;
    int    degenerate_collapsed = 0; // fewer than 3 distinct vertices after welding
    int    degenerate_sliver    = 0; // 3 distinct vertices, but collinear
    int    faces_failed         = 0;
    int    unique_edges         = 0;
    int    boundary_edges       = 0; // used by one triangle: the mesh is open there
    int    nonmanifold_edges    = 0; // used by three or more triangles
    int    flipped_edges        = 0; // used twice in the same direction: inconsistent winding
    int    components           = 0;
    int    solids               = 0;
    int    cavities             = 0; // closed inward-facing components placed inside a solid
    int    inverted_solids      = 0; // closed inward-facing components with no container: flipped
    int    open_shells          = 0;
    int    faces_before_merge   = 0;
    int    faces_final          = 0;
    bool   watertight           = false; // every edge used exactly twice, in opposite directions
    bool   is_solid             = false; // the result is made of solids only (no open shells)
    double volume               = 0.;    // sum of the solids' volumes (mm^3)
    double tolerance_used       = 0.;
    double seconds_build        = 0.;
    double seconds_merge        = 0.;
    std::vector<std::string> warnings;
};

// Convert a triangle mesh into a B-rep: a solid per closed component (with cavities), a shell
// per open component, combined into a compound when there is more than one. Throws
// Slic3r::RuntimeError when the mesh has no usable triangle.
TopoDS_Shape mesh_to_brep(const indexed_triangle_set &its, const MeshToBRepParams &params, MeshToBRepStats &stats);
inline TopoDS_Shape mesh_to_brep(const indexed_triangle_set &its, const MeshToBRepParams &params = {})
{
    MeshToBRepStats stats;
    return mesh_to_brep(its, params, stats);
}

// Measurements of a B-rep, used by the exporter's checks and by the tests.
struct ShapeInfo
{
    int           solids       = 0;
    int           shells       = 0; // all shells, including those inside solids
    int           free_shells  = 0; // shells that are not part of a solid
    int           faces        = 0;
    int           planar_faces = 0;
    int           edges        = 0;
    double        volume       = 0.; // of the solids
    double        area         = 0.;
    BoundingBoxf3 bbox;              // tight (exact geometry, not the triangulation)
    bool          valid        = false; // BRepCheck_Analyzer, only when check_validity
};
ShapeInfo shape_info(const TopoDS_Shape &shape, bool check_validity = false);

// Triangulate a B-rep (BRepMesh) and return the triangles in the shape's coordinates, wound
// outwards. The shape keeps the triangulation it was given.
indexed_triangle_set brep_to_its(const TopoDS_Shape &shape, double linear_deflection, double angular_deflection);

}} // namespace Slic3r::BRep

#endif // slic3r_BRep_MeshToBRep_hpp_
