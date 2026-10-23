#ifndef slic3r_Format_Assimp_hpp_
#define slic3r_Format_Assimp_hpp_

#include <string>

namespace Slic3r {

class Model;

// Formats routed through Assimp. Only geometry is imported; materials and
// textures are discarded, matching the current OBJ texture behavior.
bool is_assimp_mesh_file(const std::string& path);

// Load a glTF / GLB / FBX file into the provided model.
// Returns false and fills message with a human readable error on failure.
bool load_assimp_mesh(const char* path, Model* model, std::string& message);

} // namespace Slic3r

#endif // slic3r_Format_Assimp_hpp_
