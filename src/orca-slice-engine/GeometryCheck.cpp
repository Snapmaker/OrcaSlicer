#include "GeometryCheck.hpp"
#include "Types.hpp"

#include <boost/log/trivial.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/MeshBoolean.hpp"

namespace {

// Helper: build a defect Issue for a given volume
Issue make_issue(const std::string& level, int plate_id,
                 const std::string& object_name, const std::string& code,
                 const std::string& message)
{
    Issue issue;
    issue.level       = level;
    issue.plate_id    = plate_id;
    issue.object_name = object_name;
    issue.z_height    = -1.0;
    issue.code        = code;
    issue.message     = message;
    return issue;
}

void check_non_manifold(const Slic3r::ModelVolume& volume,
                        const std::string& obj_name, int plate_id,
                        std::vector<Issue>& out)
{
    size_t open_edges = Slic3r::its_num_open_edges(volume.mesh().its);
    if (open_edges > 0) {
        out.push_back(make_issue(
            "error", plate_id, obj_name, "GEOM_NON_MANIFOLD",
            "Non-manifold mesh: " + std::to_string(open_edges) +
                " open edge(s) detected. The mesh is not watertight and may cause slicing artifacts."));
    }
}

void check_degenerate_faces(Slic3r::ModelVolume& volume,
                            const std::string& obj_name, int plate_id,
                            std::vector<Issue>& out)
{
    // Create a mutable copy because its_remove_degenerate_faces modifies the ITS.
    // We use a const-cast on the shared_ptr, then perform a copy-on-write check.
    // Actually, we can't modify the volume's mesh directly. Instead copy the ITS.
    indexed_triangle_set its_copy = volume.mesh().its;
    size_t removed = Slic3r::its_remove_degenerate_faces(its_copy);
    if (removed > 0) {
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_DEGENERATE",
            "Degenerate faces: " + std::to_string(removed) +
                " zero-area triangle(s) found. These may cause slicing issues."));
    }
}

bool check_self_intersect(const Slic3r::ModelVolume& volume,
                          const std::string& obj_name, int plate_id,
                          std::vector<Issue>& out)
{
    try {
        if (Slic3r::MeshBoolean::cgal::does_self_intersect(volume.mesh())) {
            out.push_back(make_issue(
                "error", plate_id, obj_name, "GEOM_SELF_INTERSECT",
                "Self-intersecting mesh: faces penetrate each other. "
                "This will cause incorrect slicing results."));
            return true;
        }
    } catch (const std::exception& e) {
        // CGAL may fail on degenerate input; log and continue
        BOOST_LOG_TRIVIAL(warning)
            << "GeometryCheck: CGAL self-intersect check failed for \""
            << obj_name << "\": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning)
            << "GeometryCheck: CGAL self-intersect check failed for \""
            << obj_name << "\" (unknown error)";
    }
    return false;
}

void check_multi_component(const Slic3r::ModelVolume& volume,
                           const std::string& obj_name, int plate_id,
                           std::vector<Issue>& out)
{
    size_t patches = Slic3r::its_number_of_patches(volume.mesh().its);
    if (patches > 1) {
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_MULTI_COMPONENT",
            "Disconnected mesh: " + std::to_string(patches) +
                " separate component(s). Consider splitting the model."));
    }
}

void check_inverted_normals(const Slic3r::ModelVolume& volume,
                            const std::string& obj_name, int plate_id,
                            std::vector<Issue>& out)
{
    double vol = Slic3r::its_volume(volume.mesh().its);
    if (vol < 0.0) {
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_INVERTED",
            "Inverted normals: mesh volume is negative (" +
                std::to_string(vol) + " mm³). Face winding may be reversed."));
    }
}

void check_empty(const Slic3r::ModelVolume& volume,
                 const std::string& obj_name, int plate_id,
                 std::vector<Issue>& out)
{
    if (volume.mesh().its.indices.empty()) {
        out.push_back(make_issue(
            "error", plate_id, obj_name, "GEOM_EMPTY",
            "Empty mesh: no triangles in model volume."));
    }
}

} // namespace

std::vector<Issue> run_geometry_checks(const Slic3r::Model& model, int plate_id)
{
    std::vector<Issue> issues;

    for (Slic3r::ModelObject* obj : model.objects) {
        if (obj == nullptr)
            continue;

        for (size_t vi = 0; vi < obj->volumes.size(); ++vi) {
            Slic3r::ModelVolume* vol = obj->volumes[vi];
            if (vol == nullptr || !vol->is_model_part())
                continue;

            std::string name = obj->name;
            if (obj->volumes.size() > 1)
                name += "[" + std::to_string(vi) + "]";

            check_empty(*vol, name, plate_id, issues);
            check_non_manifold(*vol, name, plate_id, issues);
            check_degenerate_faces(*vol, name, plate_id, issues);
            check_self_intersect(*vol, name, plate_id, issues);
            check_multi_component(*vol, name, plate_id, issues);
            check_inverted_normals(*vol, name, plate_id, issues);
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Geometry check completed: "
        << issues.size() << " defect(s) found";

    return issues;
}
