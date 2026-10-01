#ifndef slic3r_Format_STEPExport_hpp_
#define slic3r_Format_STEPExport_hpp_

// STEP (ISO 10303, AP214) export of model objects as B-rep solids, in millimetres.
//
// Each model part becomes one STEP shape named after the part; an object with several parts
// becomes an assembly named after the object (one per exported instance), and the part's
// filament colour is written as its surface colour. Geometry is placed in world coordinates
// (instance matrix * volume matrix, plus StepExportParams::world_offset).
//
// A part that carries a CAD body (BRep/CadBody.hpp: filleted, chamfered, shelled or converted
// with the exact CAD tools) whose mesh is unedited is exported with that body.
// A part imported from STEP is exported with its EXACT original geometry when the source
// file can still be read and the part's mesh is still the tessellation of it (see
// step_source_brep()); any other part is converted from its mesh by BRep::mesh_to_brep(),
// with coplanar triangles merged into planar faces. Negative volumes, modifiers and blockers
// are not exported.

#include "libslic3r/BRep/MeshToBRep.hpp"
#include "libslic3r/Point.hpp"

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace Slic3r {

class Model;
class ModelObject;
class ModelVolume;

struct StepExportItem
{
    const ModelObject *object       = nullptr;
    int                instance_idx = -1; // -1: every instance of the object
};

struct StepExportParams
{
    // "#RRGGBB" per extruder; a part printed with extruder N gets extruder_colours[N - 1].
    std::vector<std::string> extruder_colours;
    // Added to every world coordinate, e.g. minus a plate's origin.
    Vec3d                    world_offset = Vec3d::Zero();
    BRep::MeshToBRepParams   mesh;
    // Look for the exact B-rep of parts imported from STEP (re-reads the source file).
    bool                     use_source_brep = true;
    // Name of the root assembly when more than one object is exported.
    std::string              product_name = "EdgeSlicer export";
};

struct StepExportReport
{
    int    objects          = 0; // exported object instances
    int    parts            = 0; // exported model parts
    int    exact_parts      = 0; // written from the source STEP's B-rep
    int    mesh_parts       = 0; // converted from the mesh
    int    open_parts       = 0; // written as a surface (shell) because the mesh is open
    int    skipped_negative = 0; // negative volumes (not subtracted, not exported)
    double seconds          = 0.;
    std::vector<std::string> warnings;
    std::string              error;
};

bool store_step(const std::string &path, const std::vector<StepExportItem> &items, const StepExportParams &params, StepExportReport &report);
// Every instance of every object of the model.
bool store_step(const std::string &path, const Model &model, const StepExportParams &params, StepExportReport &report);

// One part on its own, in its MESH coordinates (the frame ModelVolume::mesh() is in: before the
// part's own and the instance's transforms), for a round trip through a CAD program (the FreeCAD
// bridge). A shape whose geometry comes back in the same frame can replace the part's mesh while
// the part keeps its placement. The exact B-rep is written when step_source_brep() finds it;
// otherwise the mesh is converted, or, with `exact_only`, nothing is written and report.error says
// why the part has no exact B-rep.
bool store_step_part(const std::string &path, const ModelVolume &volume, const StepExportParams &params, StepExportReport &report, bool exact_only = false);

// The exact B-rep of a volume imported from STEP, in the volume's MESH coordinates (the
// frame ModelVolume::mesh() is in), or a null shape when the source file is gone or
// unreadable, or when the volume's mesh is no longer the tessellation of that B-rep (the part
// was cut, simplified, remeshed, baked, ...). `why_not` receives the reason for a null result.
TopoDS_Shape step_source_brep(const ModelVolume &volume, std::string *why_not = nullptr);
// The same from the pieces of the volume it reads (its source file, name, mesh and the importer's
// centring shift), for callers that must not touch the Model from a worker thread.
TopoDS_Shape step_source_brep(const std::string &input_file, const std::string &volume_name, const indexed_triangle_set &mesh,
                              const Vec3d &mesh_offset, std::string *why_not = nullptr);

} // namespace Slic3r

#endif // slic3r_Format_STEPExport_hpp_
