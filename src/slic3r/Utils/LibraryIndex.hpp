#ifndef slic3r_LibraryIndex_hpp_
#define slic3r_LibraryIndex_hpp_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The Home tab's Library: every model file under the folders the user picked, each folder optionally
// tagged with a Category and a Vendor. This is the index behind it, kept free of wx so it can be
// tested on its own (tests/slic3rutils/library_index_tests.cpp):
//   * the folder list as app_config stores it (JSON),
//   * which files count (3MF, STL, STEP, OBJ, AMF) and what is read from a 3MF without loading it
//     (cover thumbnail, plate count and names, title and designer),
//   * the scan: incremental (a file whose size and time are unchanged is not opened again), never
//     following directory links, skipping hidden folders, bounded in depth and count, and keeping
//     the last known files of a folder that cannot be reached (a network share that is offline),
//   * the cache on disk: <cache>/index.json and <cache>/thumbs/<id>.png.
namespace Slic3r {
namespace Library {

struct Folder
{
    std::string path;
    bool        recursive { true };
    std::string category;
    std::string vendor;
};

// The app_config value (a JSON array) <-> folders. Bad entries are dropped, paths are trimmed and a
// path listed twice is kept once (the first).
std::vector<Folder> folders_from_json(const std::string& text);
std::string         folders_to_json(const std::vector<Folder>& folders);

// "3mf", "stl", "step", "obj", "amf", or "" for a file the Library does not list. Case-insensitive;
// ".stp" is "step". A sliced 3MF (".gcode.3mf") is a "3mf" too (the entry says it is sliced).
std::string file_type(const std::string& file_name);

// A stable id for a path (the same on every run and platform): 16 hex digits of FNV-1a 64.
std::string entry_id(const std::string& path);

// What is read from a 3MF without loading it.
struct ThreeMfInfo
{
    bool                     ok { false };     // it opened as a zip
    int                      plates { 0 };
    std::vector<std::string> plate_names;      // by plate, "" where it has none; may be shorter than plates
    std::string              title;
    std::string              designer;
    std::string              thumbnail_png;    // the cover, "" when there is none
    bool                     sliced { false }; // it carries G-code (a .gcode.3mf, or Metadata/plate_N.gcode)
};
ThreeMfInfo read_3mf(const std::string& path);
// The same pieces from the raw texts, for the tests: which file _rels/.rels names as the package
// thumbnail ("" = none), and the title / designer in the head of 3D/3dmodel.model.
std::string rels_thumbnail(const std::string& rels_xml);
std::string model_metadata(const std::string& model_xml_head, const std::string& name);
// Plate names from Metadata/model_settings.config, by plate index (1-based ids in the file).
std::vector<std::string> plate_names(const std::string& model_settings_xml);

struct Entry
{
    std::string id;
    std::string path;
    std::string name;       // file name
    std::string type;       // file_type()
    std::string root;       // the Library folder it was found under
    std::string rel_dir;    // its folder relative to root ("" = the root itself), '/'-separated
    std::string category;   // from the folder
    std::string vendor;
    int64_t     size { 0 };
    int64_t     mtime { 0 };   // unix seconds
    int64_t     added { 0 };   // when this index first saw it
    int         plates { 0 };
    std::vector<std::string> plate_names;
    std::string title, designer;
    bool        sliced { false };
    bool        has_thumbnail { false };
};

nlohmann::json entry_to_json(const Entry& e);   // the cache's form (path included)
// What the Home page gets for one entry: everything it shows, its Library folder and the folder
// inside it, but not the file's full path (the host acts on ids).
nlohmann::json page_item(const Entry& e);
Entry          entry_from_json(const nlohmann::json& j);

struct FolderState
{
    std::string path;
    bool        online { true };
    int         files { 0 };
};

struct Index
{
    int64_t                  scanned_at { 0 };
    std::vector<Entry>       entries;
    std::vector<FolderState> folders;
};

// <cache>/index.json. A missing or unreadable file is an empty index.
Index load_index(const std::string& cache_dir);
bool  save_index(const std::string& cache_dir, const Index& index);
std::string thumbnail_path(const std::string& cache_dir, const std::string& id);

struct ScanLimits
{
    int    max_depth { 16 };
    size_t max_files { 100000 };
};

// Walk `folders` and return the new index. An entry whose path, size and time match one in
// `previous` is taken from it without opening the file. New or changed 3MF files are read and their
// cover written to <cache>/thumbs/<id>.png; thumbnails of files that are gone are removed. A file
// under two Library folders belongs to the deepest one (its tags win). `hidden` paths are left
// out. `progress` is told the number of files seen so far now and then; `cancel` stops the walk
// (the index returned is then partial and should not be saved). `now` is the time new entries are
// stamped with as added.
Index scan(const std::vector<Folder>& folders, const Index& previous, const std::set<std::string>& hidden,
           const std::string& cache_dir, int64_t now, const std::atomic<bool>& cancel,
           const std::function<void(size_t)>& progress = {}, const ScanLimits& limits = {});

} // namespace Library
} // namespace Slic3r

#endif
