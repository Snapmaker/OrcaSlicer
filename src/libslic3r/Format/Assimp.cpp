#include "Assimp.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace {

constexpr std::array<const char*, 3> k_supported_extensions = {".fbx", ".gltf", ".glb"};

std::string source_format_name(const std::string& path)
{
    std::string extension = boost::filesystem::path(path).extension().string();
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    boost::algorithm::to_upper(extension);
    return extension.empty() ? "mesh" : extension;
}

float scene_to_millimeters(const std::string& path)
{
    // Assimp converts FBX coordinates to centimetres and glTF/GLB to metres.
    if (boost::algorithm::iends_with(path, ".fbx"))
        return 10.f;
    return 1000.f;
}

// All formats routed through this loader (FBX, glTF, GLB) use a Y-up
// convention and are converted to the slicer's Z-up convention below.
Vec3f printable_position(const aiVector3D& vertex, float scale)
{
    const Vec3f scaled(vertex.x * scale, vertex.y * scale, vertex.z * scale);
    return Vec3f(scaled.x(), -scaled.z(), scaled.y());
}

bool build_mesh(const aiScene& scene, const std::string& input_path, TriangleMesh& out_mesh, std::string& message)
{
    indexed_triangle_set its;
    size_t               vertex_count   = 0;
    size_t               triangle_count = 0;
    for (unsigned mesh_idx = 0; mesh_idx < scene.mNumMeshes; ++mesh_idx) {
        const aiMesh* mesh = scene.mMeshes[mesh_idx];
        if (mesh == nullptr)
            continue;
        vertex_count += mesh->mNumVertices;
        triangle_count += std::count_if(mesh->mFaces, mesh->mFaces + mesh->mNumFaces,
                                        [](const aiFace& face) { return face.mNumIndices == 3; });
    }
    if (vertex_count == 0 || triangle_count == 0 || vertex_count > size_t(std::numeric_limits<int>::max())) {
        message = "The file does not contain a printable triangle mesh.";
        return false;
    }

    its.vertices.reserve(vertex_count);
    its.indices.reserve(triangle_count);

    const float scale = scene_to_millimeters(input_path);
    for (unsigned mesh_idx = 0; mesh_idx < scene.mNumMeshes; ++mesh_idx) {
        const aiMesh* mesh = scene.mMeshes[mesh_idx];
        if (mesh == nullptr)
            continue;
        if (its.vertices.size() > size_t(std::numeric_limits<int>::max()) - mesh->mNumVertices) {
            message = "The imported scene contains too many vertices.";
            return false;
        }

        const int vertex_offset = int(its.vertices.size());
        for (unsigned vertex_idx = 0; vertex_idx < mesh->mNumVertices; ++vertex_idx)
            its.vertices.emplace_back(printable_position(mesh->mVertices[vertex_idx], scale));

        for (unsigned face_idx = 0; face_idx < mesh->mNumFaces; ++face_idx) {
            const aiFace& face = mesh->mFaces[face_idx];
            if (face.mNumIndices != 3)
                continue;
            if (face.mIndices[0] >= mesh->mNumVertices || face.mIndices[1] >= mesh->mNumVertices ||
                face.mIndices[2] >= mesh->mNumVertices) {
                message = "The imported scene contains an invalid triangle index.";
                return false;
            }
            its.indices.emplace_back(vertex_offset + int(face.mIndices[0]), vertex_offset + int(face.mIndices[1]),
                                     vertex_offset + int(face.mIndices[2]));
        }
    }

    out_mesh = TriangleMesh(std::move(its));
    if (out_mesh.empty()) {
        message = "The file does not contain a printable triangle mesh.";
        return false;
    }
    if (out_mesh.volume() < 0)
        out_mesh.flip_triangles();
    return true;
}

} // namespace

bool is_assimp_mesh_file(const std::string& path)
{
    return std::any_of(k_supported_extensions.begin(), k_supported_extensions.end(),
                       [&path](const char* extension) { return boost::algorithm::iends_with(path, extension); });
}

bool load_assimp_mesh(const char* path, Model* model, std::string& message)
{
    if (path == nullptr || model == nullptr)
        return false;

    Assimp::Importer importer;
    constexpr unsigned flags = aiProcess_Triangulate | aiProcess_SortByPType | aiProcess_ValidateDataStructure | aiProcess_FindInvalidData |
                               aiProcess_GenUVCoords | aiProcess_TransformUVCoords | aiProcess_PreTransformVertices;
    const aiScene* scene = importer.ReadFile(path, flags);
    if (scene == nullptr) {
        message = std::string("Unable to import ") + source_format_name(path) + " file: " + importer.GetErrorString();
        return false;
    }

    TriangleMesh mesh;
    if (!build_mesh(*scene, path, mesh, message))
        return false;

    model->add_object(boost::filesystem::path(path).filename().string().c_str(), path, std::move(mesh));
    return true;
}

} // namespace Slic3r
