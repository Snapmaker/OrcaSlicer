#ifndef slic3r_Format_STL_hpp_
#define slic3r_Format_STL_hpp_

#include <admesh/stl.h>

#include <cstdint>
#include <functional>

namespace Slic3r {

class Model;
class TriangleMesh;
class ModelObject;

// GUI-supplied callback: return true to continue importing a large model despite
// the memory estimate exceeding the guard threshold; false or an empty callback
// aborts the import with an error.
using MemoryConfirmFn = std::function<bool(uintmax_t estimated_memory, uintmax_t available_memory)>;

// Load an STL file into a provided model.
extern bool load_stl(const char *path, Model *model, const char *object_name = nullptr, ImportstlProgressFn stlFn = nullptr, int custom_header_length = 80, MemoryConfirmFn memory_confirm_fn = {});

extern bool store_stl(const char *path, TriangleMesh *mesh, bool binary);
extern bool store_stl(const char *path, ModelObject *model_object, bool binary);
extern bool store_stl(const char *path, Model *model, bool binary);

}; // namespace Slic3r

#endif /* slic3r_Format_STL_hpp_ */
