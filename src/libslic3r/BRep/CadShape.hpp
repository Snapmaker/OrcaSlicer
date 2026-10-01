#ifndef slic3r_BRep_CadShape_hpp_
#define slic3r_BRep_CadShape_hpp_

// OpenCASCADE-typed side of CadBody / CadEdit: reading and writing the shape a CadBody carries.
// Kept out of CadEdit.hpp so that GUI code does not need OCCT headers.

#include "CadBody.hpp"

#include "libslic3r/TriangleMesh.hpp"

#include <TopoDS_Shape.hxx>

#include <memory>

namespace Slic3r { namespace BRep {

// The body's shape in the volume's MESH coordinates (its shift applied). Throws
// Slic3r::RuntimeError when the blob cannot be read.
TopoDS_Shape cad_body_shape(const CadBody &body);

// A body for `shape` (already in mesh coordinates), fingerprinted to `mesh`.
std::shared_ptr<CadBody> make_cad_body(const TopoDS_Shape &shape, const indexed_triangle_set &mesh, CadBodyOrigin origin,
                                       int operations);

// Triangulate like STEP import does and weld the shared edge nodes, so the result is a closed
// mesh when the shape is a closed solid.
indexed_triangle_set tessellate_cad_shape(const TopoDS_Shape &shape, double linear_deflection, double angular_deflection);

}} // namespace Slic3r::BRep

#endif // slic3r_BRep_CadShape_hpp_
