#ifndef slic3r_MeshThumbnail_hpp_
#define slic3r_MeshThumbnail_hpp_

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

// A picture of a mesh file for the Home tab's Library (STL, OBJ and AMF carry no preview of their
// own, unlike a 3MF). The file is read into a bare triangle list, without building a Model, and drawn
// by a small software renderer: no OpenGL, so it runs on the scan's worker thread. Kept free of wx
// and libslic3r so it can be tested on its own (tests/slic3rutils/library_index_tests.cpp). STEP is
// read by Utils/StepMesh, with OpenCASCADE.
namespace Slic3r {
namespace Library {

// Triangles as 9 floats each (three corners, x y z), Z up.
using Triangles = std::vector<float>;

struct MeshLimits
{
    size_t max_bytes { 256u * 1024 * 1024 }; // a bigger file is not read
    size_t max_triangles { 8u * 1000 * 1000 };
};
// STEP is tessellated first, which is far slower per byte than reading a mesh.
inline MeshLimits step_limits() { return {64u * 1024 * 1024, 4u * 1000 * 1000}; }

// false when the file cannot be read, is not of that format, is too big or has no triangles.
bool read_stl(const std::string& path, Triangles& out, const MeshLimits& limits = {});
bool read_obj(const std::string& path, Triangles& out, const MeshLimits& limits = {});
bool read_amf(const std::string& path, Triangles& out, const MeshLimits& limits = {}); // plain or zipped
// The same from the file's bytes, for the tests.
bool parse_stl(const std::string& bytes, Triangles& out, const MeshLimits& limits = {});
bool parse_obj(const std::string& text, Triangles& out, const MeshLimits& limits = {});
bool parse_amf(const std::string& xml, Triangles& out, const MeshLimits& limits = {});

// RGBA pixels (size x size, row 0 at the top) of the triangles seen from the front-right and a
// little above, shaded, on a transparent background. Empty for no triangles.
std::vector<unsigned char> render_rgba(const Triangles& tris, int size);
std::string                encode_png(const std::vector<unsigned char>& rgba, int size);

// The PNG for a Library file of type "stl", "obj", "amf" or "step"; "" for anything else or on
// failure. `cancel` stops a STEP file's tessellation.
std::string mesh_thumbnail_png(const std::string& path, const std::string& type, int size = 256,
                               const std::atomic<bool>* cancel = nullptr);

} // namespace Library
} // namespace Slic3r

#endif
