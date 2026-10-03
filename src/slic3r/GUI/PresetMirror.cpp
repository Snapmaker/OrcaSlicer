#include "PresetMirror.hpp"
#include "PresetMirrorCore.hpp"

#include "libslic3r/libslic3r.h"   // Slic3r::data_dir()
#include "libslic3r/Utils.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <ctime>

namespace bfs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

// ---- helpers ----------------------------------------------------------------

long long read_updated_time(const bfs::path& info_path)
{
    // .info is a simple "key = value" text file; return updated_time or 0.
    boost::system::error_code ec;
    if (!bfs::exists(info_path, ec)) return 0;
    std::ifstream in(info_path.string());
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        // trim
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key == "updated_time") {
            std::string val = line.substr(pos + 1);
            try { return std::stoll(val); } catch (...) { return 0; }
        }
    }
    return 0;
}

// Copy an .info, forcing sync_info blank so the mirrored preset is inert to the fork's cloud
// delete/upload gates (keeps user_id + setting_id + base_id).
void copy_info_inert(const bfs::path& src_info, const bfs::path& dst_info)
{
    boost::system::error_code ec;
    if (!bfs::exists(src_info, ec)) return;
    std::ifstream in(src_info.string());
    std::vector<std::string> lines;
    std::string line;
    bool had_sync = false;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.rfind("sync_info", 0) == 0) { lines.push_back("sync_info = "); had_sync = true; }
        else lines.push_back(line);
    }
    if (!had_sync) lines.push_back("sync_info = ");
    std::ofstream out(dst_info.string(), std::ios::binary | std::ios::trunc);
    for (auto& l : lines) out << l << "\n";
}

std::string read_file(const bfs::path& p)
{
    std::ifstream in(p.string(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every option this fork declares nullable, i.e. where a literal "nil" is a legal value. Any other
// key carrying "nil" makes ConfigOption::deserialize() throw, and PresetCollection::load_presets()
// DELETES (.json + .info) every preset it fails to parse - so such a preset must never be copied in.
const std::vector<std::string>& nullable_option_keys()
{
    static const std::vector<std::string> keys = []() {
        std::vector<std::string> k;
        for (const auto& kv : print_config_def.options)
            if (kv.second.nullable)
                k.push_back(kv.first);
        return k;
    }();
    return keys;
}

// A source preset is usable if this fork can already read it, or if the only thing stopping it is
// per-extruder "nil" padding we can collapse without changing the preset's meaning.
bool source_is_usable(const std::string& text, const std::vector<std::string>& nullable)
{
    if (mirror::preset_is_parseable(text, nullable))
        return true;
    std::string fixed;
    return mirror::sanitize_nil_arrays(text, nullable, &fixed)
        && mirror::preset_is_parseable(fixed, nullable);
}

// Find the Bambu Studio user\<uid> dir. Prefer the logged-in uid; prefer stable over Beta; else
// the most-recently-modified numeric dir. Returns empty path if none.
bfs::path find_bambu_user_dir(const std::string& logged_in_uid)
{
    // data_dir() is %APPDATA%\<fork>; BambuStudio is a sibling under %APPDATA%.
    bfs::path appdata = bfs::path(Slic3r::data_dir()).parent_path();
    const char* roots[] = { "BambuStudio", "BambuStudioBeta" };

    std::vector<mirror::UidCandidate> cands;
    std::vector<bfs::path>            paths;
    for (const char* r : roots) {
        bfs::path user = appdata / r / "user";
        boost::system::error_code ec;
        if (!bfs::is_directory(user, ec)) continue;
        for (bfs::directory_iterator it(user, ec), end; it != end && !ec; it.increment(ec)) {
            if (!bfs::is_directory(it->status())) continue;
            std::string name = it->path().filename().string();
            if (name.empty() || name == "default") continue;
            if (name.find_first_not_of("0123456789") != std::string::npos) continue;
            mirror::UidCandidate c;
            c.root  = r;
            c.uid   = name;
            c.mtime = (long long) bfs::last_write_time(it->path(), ec);
            cands.push_back(c);
            paths.push_back(it->path());
        }
    }
    int pick = mirror::choose_uid(cands, logged_in_uid);
    return pick < 0 ? bfs::path() : paths[(size_t) pick];
}

// Enumerate the Bambu source tree. `ok` stays false unless every directory we touched was read
// without error - a partial or failed listing must never be mistaken for "the user deleted things".
mirror::SourceListing list_source(const bfs::path& src_uid)
{
    mirror::SourceListing out;
    boost::system::error_code ec;
    if (!bfs::is_directory(src_uid, ec)) return out;   // ok == false

    const char* types[] = { "filament", "process", "machine" };
    const auto& nullable = nullable_option_keys();

    for (const char* typ : types) {
        bfs::path sdir = src_uid / typ;
        if (!bfs::is_directory(sdir, ec)) continue;   // a type the user simply has none of

        // 1) base\ cache (full inheritance copies; no .info, use mtime)
        bfs::path sbase = sdir / "base";
        if (bfs::is_directory(sbase, ec)) {
            boost::system::error_code it_ec;
            bfs::directory_iterator it(sbase, it_ec), end;
            if (it_ec) return mirror::SourceListing();   // unreadable -> whole run is not ok
            for (; it != end; it.increment(it_ec)) {
                if (it_ec) return mirror::SourceListing();
                if (it->path().extension() != ".json") continue;
                mirror::SourceFile f;
                f.rel = std::string(typ) + "/base/" + it->path().filename().string();
                f.t   = (long long) bfs::last_write_time(it->path(), ec);
                f.parseable = source_is_usable(read_file(it->path()), nullable);
                out.files.push_back(f);
            }
        }

        // 2) top-level user presets (sparse override diffs; use .info updated_time)
        {
            boost::system::error_code it_ec;
            bfs::directory_iterator it(sdir, it_ec), end;
            if (it_ec) return mirror::SourceListing();
            for (; it != end; it.increment(it_ec)) {
                if (it_ec) return mirror::SourceListing();
                if (!bfs::is_regular_file(it->status())) continue;
                if (it->path().extension() != ".json") continue;
                mirror::SourceFile f;
                f.rel = std::string(typ) + "/" + it->path().filename().string();
                bfs::path info = it->path(); info.replace_extension(".info");
                f.t = read_updated_time(info);
                if (f.t == 0) f.t = (long long) bfs::last_write_time(it->path(), ec);
                f.parseable = source_is_usable(read_file(it->path()), nullable);
                out.files.push_back(f);
            }
        }
    }

    out.ok = true;
    return out;
}

} // namespace

int mirror_bambu_user_presets(const std::string& logged_in_uid)
{
    try {
        bfs::path src_uid = find_bambu_user_dir(logged_in_uid);
        if (src_uid.empty()) { BOOST_LOG_TRIVIAL(info) << "[preset-mirror] no Bambu Studio user dir found; skipping"; return 0; }

        bfs::path dst_root = bfs::path(Slic3r::data_dir()) / "user" / "default";
        boost::system::error_code ec;
        bfs::create_directories(dst_root, ec);

        bfs::path man_path = dst_root / ".bs_mirror_manifest.json";
        auto manifest = mirror::parse_manifest(read_file(man_path));

        // Enumerate first. Nothing destructive happens before we know the listing is good.
        mirror::SourceListing src = list_source(src_uid);
        if (!src.ok) {
            BOOST_LOG_TRIVIAL(warning) << "[preset-mirror] could not enumerate " << src_uid.string()
                                       << "; skipping this run (no presets touched)";
            return 0;
        }

        // Which of our tracked/listed files are on disk right now.
        mirror::DestState dst;
        auto note_present = [&](const std::string& rel) {
            bfs::path p = dst_root / bfs::path(rel);
            dst.present[rel] = bfs::exists(p, ec);
        };
        for (const auto& f : src.files) note_present(f.rel);
        for (const auto& kv : manifest) note_present(kv.first);

        auto plan = mirror::build_plan(src, manifest, dst);

        // Index the source files so we can find what to copy.
        std::map<std::string, const mirror::SourceFile*> by_rel;
        for (const auto& f : src.files) by_rel[f.rel] = &f;

        int copied = 0, uptodate = 0, native_protected = 0, respected = 0, skipped = 0, retired = 0, errors = 0, sanitized = 0;

        for (const auto& item : plan) {
            switch (item.action) {
            case mirror::Action::Copy: {
                if (by_rel.find(item.rel) == by_rel.end()) { ++errors; break; }
                bfs::path srcp = src_uid / bfs::path(item.rel);
                bfs::path dstp = dst_root / bfs::path(item.rel);
                bfs::create_directories(dstp.parent_path(), ec);

                // Collapse Bambu's per-extruder "nil" padding where it is unambiguous, so the
                // preset loads here instead of being rejected (and then deleted) by the loader.
                std::string text = read_file(srcp);
                std::string fixed;
                int         collapsed = 0;
                if (!mirror::preset_is_parseable(text, nullable_option_keys())
                    && mirror::sanitize_nil_arrays(text, nullable_option_keys(), &fixed, &collapsed)) {
                    std::ofstream o(dstp.string(), std::ios::binary | std::ios::trunc);
                    o << fixed;
                    if (!o) {
                        BOOST_LOG_TRIVIAL(warning) << "[preset-mirror] write failed " << dstp.string();
                        ++errors;
                        break;
                    }
                    sanitized += collapsed ? 1 : 0;
                } else {
                    bfs::copy_file(srcp, dstp, bfs::copy_option::overwrite_if_exists, ec);
                    if (ec) {
                        BOOST_LOG_TRIVIAL(warning) << "[preset-mirror] copy failed " << srcp.string() << ": " << ec.message();
                        ++errors;
                        break;
                    }
                }
                // base\ entries have no .info; only the top-level user presets carry one.
                if (item.rel.find("/base/") == std::string::npos) {
                    bfs::path si = srcp; si.replace_extension(".info");
                    bfs::path di = dstp; di.replace_extension(".info");
                    copy_info_inert(si, di);
                }
                ++copied;
                break;
            }
            case mirror::Action::UpToDate:        ++uptodate;         break;
            case mirror::Action::ProtectNative:   ++native_protected; break;
            case mirror::Action::RespectDelete:   ++respected;        break;
            case mirror::Action::SkipUnparseable: ++skipped;          break;
            case mirror::Action::Retire:          ++retired;          break;
            }
        }

        auto updated = mirror::apply_plan(manifest, plan);
        try {
            std::ofstream out(man_path.string(), std::ios::binary | std::ios::trunc);
            out << mirror::dump_manifest(updated);
        } catch (...) {}

        BOOST_LOG_TRIVIAL(info) << "[preset-mirror] from " << src_uid.string()
            << " -> copied=" << copied << " uptodate=" << uptodate
            << " fork_native_protected=" << native_protected << " user_deletions_respected=" << respected
            << " skipped_unparseable=" << skipped << " retired=" << retired
            << " nil_collapsed=" << sanitized
            << " errors=" << errors;
        if (skipped > 0)
            BOOST_LOG_TRIVIAL(info) << "[preset-mirror] " << skipped << " Bambu preset(s) use per-extruder 'nil' "
                                       "values this slicer cannot read; they stay in Bambu Studio only";
        return copied;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[preset-mirror] failed: " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[preset-mirror] failed (unknown)";
    }
    return 0;
}

int repull_mirrored_presets()
{
    // Recovery affordance: clear every "deleted" flag so the next sync re-pulls anything that was
    // removed from this slicer but that Bambu Studio still has.
    try {
        bfs::path man_path = bfs::path(Slic3r::data_dir()) / "user" / "default" / ".bs_mirror_manifest.json";
        auto manifest = mirror::parse_manifest(read_file(man_path));
        int cleared = 0;
        for (auto& kv : manifest)
            if (kv.second.deleted) { kv.second.deleted = false; kv.second.t = 0; ++cleared; }
        if (cleared > 0) {
            std::ofstream out(man_path.string(), std::ios::binary | std::ios::trunc);
            out << mirror::dump_manifest(manifest);
        }
        BOOST_LOG_TRIVIAL(info) << "[preset-mirror] re-pull requested: cleared " << cleared << " deletion flag(s)";
        return cleared;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[preset-mirror] re-pull failed: " << e.what();
    }
    return 0;
}

}} // namespace Slic3r::GUI
