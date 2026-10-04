#include "CustomModels.hpp"

#include "Format/bbs_3mf.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"
#include "TriangleSelector.hpp"
#include "UntrustedInput.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <set>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace custom_models {

const char *library_folder_name() { return "custom_models"; }

fs::path library_dir(const std::string &data_dir) { return fs::path(data_dir) / library_folder_name(); }

FileType file_type(const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".3mf")
        return FileType::Project3mf;
    if (ext == ".stl" || ext == ".obj" || ext == ".step" || ext == ".stp")
        return FileType::Mesh;
    return FileType::Unsupported;
}

// ---- ordering ------------------------------------------------------------------------------------

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

char fold(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }

// -1, 0, 1: case-insensitive, digit runs compared as numbers.
int natural_compare(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && is_digit(a[ie]))
                ++ie;
            while (je < b.size() && is_digit(b[je]))
                ++je;
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0')
                ++is;
            while (js + 1 < je && b[js] == '0')
                ++js;
            const size_t la = ie - is, lb = je - js;
            if (la != lb)
                return la < lb ? -1 : 1;
            const int c = a.compare(is, la, b, js, lb);
            if (c != 0)
                return c < 0 ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        const char ca = fold(a[i]), cb = fold(b[j]);
        if (ca != cb)
            return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return 0;
}

} // namespace

bool natural_less(const std::string &a, const std::string &b)
{
    const int c = natural_compare(a, b);
    if (c != 0)
        return c < 0;
    return a < b;
}

// ---- scanning ------------------------------------------------------------------------------------

std::size_t Folder::file_count() const
{
    std::size_t n = files.size();
    for (const Folder &f : folders)
        n += f.file_count();
    return n;
}

namespace {

struct ScanState
{
    const ScanLimits &limits;
    std::size_t       listed    = 0;
    bool              truncated = false;
};

std::string name_of(const fs::path &p) { return p.filename().string(); }

bool skipped_name(const std::string &name)
{
    // Hidden entries, and the "~$" lock files office tools leave beside a document.
    return name.empty() || name.front() == '.' || boost::algorithm::starts_with(name, "~$");
}

void scan_into(const fs::path &dir, int depth, ScanState &state, Folder &out)
{
    boost::system::error_code ec;
    fs::directory_iterator    it(dir, ec), end;
    if (ec)
        return;

    std::vector<fs::path> files, dirs;
    for (; it != end; it.increment(ec)) {
        if (ec)
            break;
        const std::string name = name_of(it->path());
        if (skipped_name(name))
            continue;
        boost::system::error_code sec;
        const fs::file_status     st = fs::status(it->path(), sec);
        if (sec)
            continue;
        if (fs::is_directory(st)) {
            if (depth < state.limits.max_depth)
                dirs.push_back(it->path());
        } else if (fs::is_regular_file(st) && file_type(it->path()) != FileType::Unsupported)
            files.push_back(it->path());
    }
    const auto by_name = [](const fs::path &a, const fs::path &b) { return natural_less(name_of(a), name_of(b)); };
    std::sort(files.begin(), files.end(), by_name);
    std::sort(dirs.begin(), dirs.end(), by_name);

    for (const fs::path &f : files) {
        if (state.listed >= state.limits.max_files) {
            state.truncated = true;
            break;
        }
        Entry e;
        e.path  = f;
        e.type  = file_type(f);
        e.label = f.stem().string();
        out.files.push_back(std::move(e));
        ++state.listed;
    }
    // "cube.stl" beside "cube.3mf": the label alone would be ambiguous, so say which is which.
    for (size_t i = 0; i < out.files.size(); ++i) {
        bool clash = false;
        for (size_t j = 0; j < out.files.size() && !clash; ++j)
            // Compare the file stems, not the labels: a label already extended for an earlier clash
            // would no longer match its partner.
            clash = j != i && boost::algorithm::iequals(out.files[i].path.stem().string(), out.files[j].path.stem().string());
        if (clash) {
            std::string ext = out.files[i].path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            out.files[i].label += " (" + ext + ")";
        }
    }

    for (const fs::path &d : dirs) {
        if (state.listed >= state.limits.max_files) {
            state.truncated = true;
            break;
        }
        Folder sub;
        sub.label = name_of(d);
        sub.path  = d;
        scan_into(d, depth + 1, state, sub);
        if (!sub.empty())
            out.folders.push_back(std::move(sub));
    }
}

} // namespace

Folder scan_library(const fs::path &root, const ScanLimits &limits)
{
    Folder result;
    result.path = root;
    boost::system::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return result;
    ScanState state{limits};
    scan_into(root, 0, state, result);
    result.truncated = state.truncated;
    return result;
}

// ---- saving --------------------------------------------------------------------------------------

std::string library_file_name(const std::string &typed_name)
{
    std::string text = typed_name;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    size_t lead = 0;
    while (lead < text.size() && std::isspace(static_cast<unsigned char>(text[lead])))
        ++lead;
    text = text.substr(lead);
    if (text.size() >= 4 && boost::algorithm::iends_with(text, ".3mf"))
        text.resize(text.size() - 4);

    const std::string stem = untrusted::sanitize_download_filename(text);
    if (stem.empty())
        return std::string();
    return untrusted::sanitize_download_filename(stem + ".3mf");
}

SaveResult save_objects(const std::vector<SourceObject> &objects, const fs::path &file)
{
    SaveResult result;
    if (file.empty() || objects.empty()) {
        result.error = "nothing to save";
        return result;
    }

    fs::path scratch;
    try {
        Model tmp;
        for (const SourceObject &src : objects) {
            if (src.object == nullptr)
                continue;
            ModelObject *copy = tmp.add_object(*src.object);
            if (copy->instances.empty())
                copy->add_instance();
            // One object, once: the library item is the object, not the copies the plate holds.
            const std::size_t keep = src.instance_idx < copy->instances.size() ? src.instance_idx : 0;
            for (std::size_t i = copy->instances.size(); i-- > 0;)
                if (i != keep)
                    copy->delete_instance(i);
            copy->invalidate_bounding_box();
        }
        if (tmp.objects.empty()) {
            result.error = "nothing to save";
            return result;
        }
        // Centred on the origin, so the file does not remember where on the plate it was.
        tmp.center_instances_around_point(Vec2d::Zero());

        fs::create_directories(file.parent_path());
        scratch = file.parent_path() / (".saving-" + file.filename().string());

        // The project settings of the file are written empty: no printer, filament or process
        // preset travels with a custom model. The writer wants a config object, not nullptr.
        DynamicPrintConfig no_settings;
        StoreParams        params;
        const std::string  scratch_path = scratch.string();
        params.path                     = scratch_path.c_str();
        params.model                    = &tmp;
        params.config                   = &no_settings;
        params.strategy                 = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        if (!store_bbs_3mf(params)) {
            result.error = "the 3MF writer failed";
            boost::system::error_code ec;
            fs::remove(scratch, ec);
            return result;
        }

        boost::system::error_code ec;
        fs::remove(file, ec); // an existing file is replaced only now that the new one is complete
        fs::rename(scratch, file);
        result.ok = true;
    } catch (const std::exception &e) {
        result.error = e.what();
        if (!scratch.empty()) {
            boost::system::error_code ec;
            fs::remove(scratch, ec);
        }
    }
    return result;
}

// ---- importing -----------------------------------------------------------------------------------

const std::vector<std::string> &filament_index_keys()
{
    // Every per-object / per-part option that names a filament by number. 0 means "the object's
    // own / the default" for all of them and is never clamped.
    static const std::vector<std::string> keys = {"extruder",         "sparse_infill_filament", "wall_filament",
                                                  "outer_wall_filament", "solid_infill_filament", "support_filament",
                                                  "support_interface_filament", "wipe_tower_filament"};
    return keys;
}

namespace {

// Removes from `config` the keys a file we did not write may not set; returns how many.
std::size_t strip_untrusted(ModelConfig &config)
{
    if (config.empty())
        return 0;
    // Nothing is trusted here: no preset of the user's has a per-object post-processing script.
    const std::vector<untrusted::UntrustedSetting> found =
        untrusted::find_untrusted_settings(config.get(), "object settings", untrusted::TrustedValues());
    std::size_t removed = 0;
    for (const untrusted::UntrustedSetting &s : found)
        if (config.erase(s.key))
            ++removed;
    return removed;
}

void clamp_filament_keys(ModelConfig &config, int filament_count, ImportReport &report)
{
    for (const std::string &key : filament_index_keys()) {
        const ConfigOptionInt *opt = config.has(key) ? dynamic_cast<const ConfigOptionInt *>(config.option(key)) : nullptr;
        if (opt == nullptr || opt->value <= 0)
            continue;
        report.highest_filament_requested = std::max(report.highest_filament_requested, opt->value);
        if (opt->value > filament_count) {
            config.set_key_value(key, new ConfigOptionInt(1));
            ++report.filament_settings_clamped;
        }
    }
}

} // namespace

ImportReport prepare_imported_objects(const std::vector<ModelObject *> &objects, std::size_t filament_count)
{
    ImportReport report;
    const int    count = int(std::max<std::size_t>(filament_count, 1));

    // Painted regions above the limit become filament 1 (not "unpainted"): the state map sends
    // every filament number past the limit to 1.
    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = i == 0 ? EnforcerBlockerType::NONE : EnforcerBlockerType(int(i) > count ? 1 : int(i));

    for (ModelObject *object : objects) {
        if (object == nullptr)
            continue;
        report.untrusted_settings_removed += strip_untrusted(object->config);
        clamp_filament_keys(object->config, count, report);
        for (auto &range : object->layer_config_ranges) {
            report.untrusted_settings_removed += strip_untrusted(range.second);
            clamp_filament_keys(range.second, count, report);
        }
        for (ModelVolume *volume : object->volumes) {
            if (volume == nullptr)
                continue;
            report.untrusted_settings_removed += strip_untrusted(volume->config);
            clamp_filament_keys(volume->config, count, report);

            bool painted_over = false;
            for (const size_t zero_based : volume->get_extruders_from_multi_material_painting()) {
                const int filament = int(zero_based) + 1;
                report.highest_filament_requested = std::max(report.highest_filament_requested, filament);
                painted_over |= filament > count;
            }
            if (painted_over) {
                volume->remap_extruder_ids(size_t(count), state_map);
                ++report.painted_volumes_clamped;
            }
        }
    }
    if (report.untrusted_settings_removed > 0)
        BOOST_LOG_TRIVIAL(warning) << "Custom model: removed " << report.untrusted_settings_removed
                                   << " untrusted setting(s) from the object settings of a library file";
    return report;
}

} // namespace custom_models
} // namespace Slic3r
