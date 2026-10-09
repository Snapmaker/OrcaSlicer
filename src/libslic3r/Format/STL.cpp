#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../Utils.hpp"

#include "STL.hpp"

#include <string>

#include <boost/nowide/fstream.hpp>
#include <boost/nowide/stat.hpp>
#include <boost/format.hpp>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

//Translation
#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

bool load_stl(const char *path, Model *model, const char *object_name_in, ImportstlProgressFn stlFn, int custom_header_length, MemoryConfirmFn memory_confirm_fn)
{
    TriangleMesh mesh;
    std::string design_id;

    // Guard against importing files whose estimated peak memory exceeds the available RAM.
    // admesh peaks at ~4.5x the file size for binary STL (203 B/facet plus repair-phase
    // hash tables: a 2 GiB STL measures ~8.7 GB peak) and ~3x for ASCII STL, so reject
    // early with a readable error instead of letting std::bad_alloc escape from the parser.
    boost::nowide::stat_t st{};
    if (boost::nowide::stat(path, &st) == 0) {
        const uintmax_t file_size = st.st_size;
        // Distinguish binary from ASCII STL (same heuristic as stl_open_count_facets)
        // to pick a sane memory ratio: binary stores 50 B/facet, ASCII ~200 B/facet.
        boost::nowide::ifstream probe(path, std::ios::binary);
        char probe_buf[128] = {};
        probe.read(probe_buf, sizeof(probe_buf));
        bool binary_stl = false;
        for (char c : probe_buf)
            if (static_cast<unsigned char>(c) > 127) { binary_stl = true; break; }
        const uintmax_t estimated_peak_memory = binary_stl ? file_size * 9 / 2 : file_size * 3;
        size_t available_memory = Slic3r::get_available_physical_memory();
        if (available_memory == 0)
            available_memory = Slic3r::total_physical_memory(); // macOS: available-memory query returns 0 by design
        if (available_memory > 0 && estimated_peak_memory > available_memory / 2) {
            // Ask the user (via the GUI callback) whether to continue; abort when no
            // callback is supplied (e.g. CLI) or the user declines.
            if (! (memory_confirm_fn && memory_confirm_fn(estimated_peak_memory, available_memory)))
                throw Slic3r::RuntimeError(
                    (boost::format(_L("The model is too large to import: it requires an estimated %1% MiB of memory, and at least twice that must be free (only %2% MiB is available)."))
                        % (estimated_peak_memory >> 20) % (available_memory >> 20)).str());
        }
    }

    if (!mesh.ReadSTLFile(path, true, stlFn, custom_header_length)) {
        //    die "Failed to open $file\n" if !-e $path;
        return false;
    }
    if (mesh.empty()) {
        // die "This STL file couldn't be read because it's empty.\n"
        return false;
    }

    std::string object_name;
    if (object_name_in == nullptr) {
        const char *last_slash = strrchr(path, DIR_SEPARATOR);
        object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
    } else
       object_name.assign(object_name_in);

    model->add_object(object_name.c_str(), path, std::move(mesh));
    return true;
}

bool store_stl(const char *path, TriangleMesh *mesh, bool binary)
{
    if (binary)
        mesh->write_binary(path);
    else
        mesh->write_ascii(path);
    //FIXME returning false even if write failed.
    return true;
}

bool store_stl(const char *path, ModelObject *model_object, bool binary)
{
    TriangleMesh mesh = model_object->mesh();
    return store_stl(path, &mesh, binary);
}

bool store_stl(const char *path, Model *model, bool binary)
{
    TriangleMesh mesh = model->mesh();
    return store_stl(path, &mesh, binary);
}

}; // namespace Slic3r