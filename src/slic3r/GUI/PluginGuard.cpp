#include "PluginGuard.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>

namespace Slic3r { namespace GUI {

const char *const kUltraNetMarkerName = "ultranet.txt";

bool is_ultranet_plugin(bool plugin_present, bool ultranet_marker)
{
    // The marker alone proves nothing - a stale ultranet.txt left behind after the user replaced
    // the plug-in with Bambu's must not lock the CDN path out. Both, or it is not ours.
    return plugin_present && ultranet_marker;
}

bool bambu_cdn_download_allowed(bool plugin_present, bool ultranet_marker)
{
    return ! is_ultranet_plugin(plugin_present, ultranet_marker);
}

PluginSync plugin_sync_decision(bool sidecar_present,
                                bool installed_present,
                                bool installed_identical,
                                bool marker_present,
                                bool keep_foreign)
{
    // A build without the sidecar (a developer tree, or a platform we do not ship it for yet) has
    // nothing to install and no opinion about what is there.
    if (! sidecar_present)
        return PluginSync::Nothing;
    if (! installed_present)
        return PluginSync::InstallFresh;
    if (installed_identical)
        return marker_present ? PluginSync::Nothing : PluginSync::WriteMarkerOnly;
    // Different bytes. An older UltraNet after an upgrade, Bambu's package after a CDN download,
    // or a migrated data dir - none of them is what this build was tested with. The marker does
    // not change the answer: an old marker beside an old DLL still means "update it".
    return keep_foreign ? PluginSync::Nothing : PluginSync::ReplaceForeign;
}

const char *network_library_name()
{
#if defined(_WIN32)
    return "bambu_networking.dll";
#elif defined(__APPLE__)
    return "libbambu_networking.dylib";
#else
    return "libbambu_networking.so";
#endif
}

boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir, bool macos_app_bundle)
{
    // <App>.app/Contents/MacOS/<exe> -> <App>.app/Contents/Resources/ultranet
    if (macos_app_bundle)
        return exe_dir.parent_path() / "Resources" / "ultranet";
    return exe_dir / "ultranet";
}

boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir)
{
#if defined(__APPLE__)
    return ultranet_sidecar_dir(exe_dir, true);
#else
    return ultranet_sidecar_dir(exe_dir, false);
#endif
}

LoginGuardAction plugin_guard_decision(bool plugin_present,
                                       bool ultranet_marker,
                                       bool installed_networking,
                                       bool agent_loaded)
{
    // An agent is loaded: the ticket exchange has somewhere to go. Nothing to guard. The
    // preference is deliberately not consulted here - the running agent is the ground truth, and a
    // stale `installed_networking=false` must not block a session that is already working.
    if (agent_loaded)
        return LoginGuardAction::ShowLogin;

    // No agent, but our plug-in is sitting in the plug-ins folder. Offering Bambu's CDN download
    // here would overwrite it, and it would not help anyway: the usual cause is the first-run copy
    // landing after the plug-in load point, which a restart fixes.
    if (is_ultranet_plugin(plugin_present, ultranet_marker))
        return LoginGuardAction::RestartRequired;

    // No agent and no plug-in of ours. Either networking is switched off, or the folder is empty,
    // or a Bambu-original plug-in failed to load - the download dialog is the right answer to all
    // three, and it is also where the user turns the preference back on.
    (void) installed_networking;
    return LoginGuardAction::OfferPluginDownload;
}

// ---------------------------------------------------------------------------------------------
// Camera component (BambuSource).

const char *bambu_source_library_name()
{
#if defined(_WIN32)
    return "BambuSource.dll";
#elif defined(__APPLE__)
    return "libBambuSource.dylib";
#else
    return "libBambuSource.so";
#endif
}

namespace {

// Little-endian scalar reads out of a byte buffer, bounds-checked. The file is attacker-adjacent
// (it can be anything the user dropped in the plug-ins folder), so every read is guarded and a
// malformed image simply answers "not a real filter".
template<typename T> bool read_le(const std::vector<char> &buf, std::size_t off, T &out)
{
    if (off > buf.size() || buf.size() - off < sizeof(T))
        return false;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(buf[off + i])) << (8 * i);
    out = static_cast<T>(v);
    return true;
}

// Map a PE relative virtual address to a file offset using the section table.
bool rva_to_offset(const std::vector<char> &buf,
                   std::size_t              section_table,
                   std::uint16_t            num_sections,
                   std::uint32_t            rva,
                   std::size_t &            out)
{
    for (std::uint16_t i = 0; i < num_sections; ++i) {
        const std::size_t sh = section_table + std::size_t(i) * 40; // IMAGE_SECTION_HEADER
        std::uint32_t virt_size = 0, virt_addr = 0, raw_size = 0, raw_ptr = 0;
        if (! read_le(buf, sh + 8, virt_size) || ! read_le(buf, sh + 12, virt_addr) ||
            ! read_le(buf, sh + 16, raw_size) || ! read_le(buf, sh + 20, raw_ptr))
            return false;
        // A section's in-memory size may exceed its raw size; only the raw part exists in the file.
        const std::uint32_t span = virt_size > raw_size ? raw_size : virt_size;
        if (span == 0 || rva < virt_addr || rva >= virt_addr + span)
            continue;
        const std::uint64_t off = std::uint64_t(raw_ptr) + (rva - virt_addr);
        if (off >= buf.size())
            return false;
        out = std::size_t(off);
        return true;
    }
    return false;
}

// Does the PE image in `buf` export the symbol `want`?
bool pe_exports(const std::vector<char> &buf, const char *want)
{
    std::uint16_t mz = 0;
    if (! read_le(buf, 0, mz) || mz != 0x5A4D) // "MZ"
        return false;
    std::uint32_t e_lfanew = 0;
    if (! read_le(buf, 0x3C, e_lfanew))
        return false;
    const std::size_t pe = e_lfanew;
    std::uint32_t sig = 0;
    if (! read_le(buf, pe, sig) || sig != 0x00004550) // "PE\0\0"
        return false;

    std::uint16_t num_sections = 0, opt_size = 0, magic = 0;
    if (! read_le(buf, pe + 6, num_sections) || ! read_le(buf, pe + 20, opt_size))
        return false;
    const std::size_t opt = pe + 24;
    if (! read_le(buf, opt, magic))
        return false;
    // The export directory is data directory 0; it sits at a different offset in PE32 vs PE32+.
    const std::size_t dd0 = opt + (magic == 0x20B ? 112 : 96);
    std::uint32_t export_rva = 0, export_size = 0;
    if (! read_le(buf, dd0, export_rva) || ! read_le(buf, dd0 + 4, export_size))
        return false;
    if (export_rva == 0 || export_size == 0)
        return false; // no export table at all - this is what our stub looks like

    const std::size_t section_table = opt + opt_size;
    std::size_t exp_off = 0;
    if (! rva_to_offset(buf, section_table, num_sections, export_rva, exp_off))
        return false;

    std::uint32_t num_names = 0, names_rva = 0;
    if (! read_le(buf, exp_off + 24, num_names) || ! read_le(buf, exp_off + 32, names_rva))
        return false;
    if (num_names == 0 || names_rva == 0)
        return false;
    std::size_t names_off = 0;
    if (! rva_to_offset(buf, section_table, num_sections, names_rva, names_off))
        return false;

    const std::size_t want_len = std::strlen(want);
    // Cap the walk: a corrupt header could claim a huge name count.
    const std::uint32_t cap = num_names > 65536u ? 65536u : num_names;
    for (std::uint32_t i = 0; i < cap; ++i) {
        std::uint32_t name_rva = 0;
        if (! read_le(buf, names_off + std::size_t(i) * 4, name_rva))
            return false;
        std::size_t name_off = 0;
        if (! rva_to_offset(buf, section_table, num_sections, name_rva, name_off))
            continue;
        // Compare in place, refusing to run off the end of the buffer.
        if (buf.size() - name_off <= want_len)
            continue;
        if (std::memcmp(buf.data() + name_off, want, want_len) == 0 && buf[name_off + want_len] == '\0')
            return true;
    }
    return false;
}

} // namespace

bool exports_dll_register_server(const boost::filesystem::path &dll)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(dll, ec) || ec)
        return false;
    const boost::uintmax_t size = boost::filesystem::file_size(dll, ec);
    // Guard both ends: too small to be a PE at all, or too large to slurp (the real filter is ~5 MB;
    // 256 MB is far past anything legitimate).
    if (ec || size < 64 || size > 256ull * 1024 * 1024)
        return false;

    std::vector<char> buf;
    try {
        std::ifstream f(dll.string().c_str(), std::ios::binary);
        if (! f)
            return false;
        buf.resize(std::size_t(size));
        f.read(buf.data(), std::streamsize(buf.size()));
        if (std::size_t(f.gcount()) != buf.size())
            return false;
    } catch (...) {
        return false;
    }

    // The exported-symbol question is only meaningful for a PE. On macOS/Linux the shipped stub is
    // likewise a placeholder with no such entry point; a Mach-O/ELF is not a PE, so pe_exports()
    // answers false and the caller treats the file as "not a real filter", which is the behaviour
    // we want there too (the download path is Windows-only for now).
    return pe_exports(buf, "DllRegisterServer");
}

bool is_ultranet_bambusource_stub(const boost::filesystem::path &dll)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(dll, ec) || ec)
        return false; // nothing there is "missing", not "our stub"
    // The marker is what makes this folder ours. Without it we must not claim the file is a stub.
    if (! boost::filesystem::exists(dll.parent_path() / kUltraNetMarkerName, ec) || ec)
        return false;
    return ! exports_dll_register_server(dll);
}

CameraToolsCopy camera_tools_copy_decision(bool plugins_copy_is_stub,
                                           bool cameratools_has_real_filter,
                                           bool cameratools_up_to_date)
{
    // A real filter already in cameratools is the best thing present - never overwrite it, and in
    // particular never with the stub (the second half of the reported bug: the user downloads the
    // component, then the next Play stamps our 9.7 KB placeholder over it).
    if (cameratools_has_real_filter)
        return CameraToolsCopy::KeepExisting;
    if (plugins_copy_is_stub)
        return CameraToolsCopy::SkipStubMissingComponent;
    return cameratools_up_to_date ? CameraToolsCopy::KeepExisting : CameraToolsCopy::CopyFromPlugins;
}

OtaPluginInstall ota_plugin_install_decision(bool update_flag, bool ultranet_installed, bool keep_foreign)
{
    if (! update_flag)
        return OtaPluginInstall::NothingStaged;
    // The staged package is Bambu's: with UltraNet installed it would replace ours. Only somebody who
    // keeps a foreign plug-in on purpose gets the stock copy.
    if (ultranet_installed && ! keep_foreign)
        return OtaPluginInstall::Refuse;
    return OtaPluginInstall::Install;
}

const char *const *ota_plugin_staged_names(size_t &count)
{
#if defined(_WIN32)
    static const char *const names[] = { "bambu_networking.dll", "BambuSource.dll", "live555.dll", "network_plugins.json" };
#elif defined(__APPLE__)
    static const char *const names[] = { "libbambu_networking.dylib", "libBambuSource.dylib", "liblive555.dylib", "network_plugins.json" };
#else
    static const char *const names[] = { "libbambu_networking.so", "libBambuSource.so", "liblive555.so", "network_plugins.json" };
#endif
    count = sizeof(names) / sizeof(names[0]);
    return names;
}

bool may_overwrite_bambusource(bool dest_exists, bool dest_is_real_filter)
{
    // Upgrades re-run the first-run copier. A real filter the user fetched from Bambu must outlive
    // that; anything else (absent, or our own stub from a previous version) may be refreshed.
    return ! (dest_exists && dest_is_real_filter);
}

const char *const kUltraNetModuleTag = "UltraNet-module";

bool carries_ultranet_module_tag(const boost::filesystem::path &lib)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(lib, ec) || ec)
        return false;
    const boost::uintmax_t size = boost::filesystem::file_size(lib, ec);
    if (ec || size == 0 || size > 256ull * 1024 * 1024)
        return false;
    try {
        std::ifstream f(lib.string().c_str(), std::ios::binary);
        if (! f)
            return false;
        // Stream it in chunks, keeping a tag-sized overlap so a match across a boundary is found.
        const std::string tag(kUltraNetModuleTag);
        std::string       window;
        std::vector<char> chunk(64 * 1024);
        while (f) {
            f.read(chunk.data(), std::streamsize(chunk.size()));
            const std::streamsize n = f.gcount();
            if (n <= 0)
                break;
            window.append(chunk.data(), std::size_t(n));
            if (window.find(tag) != std::string::npos)
                return true;
            if (window.size() > tag.size())
                window.erase(0, window.size() - tag.size());
        }
    } catch (...) {
    }
    return false;
}

bool is_real_camera_component(const boost::filesystem::path &lib)
{
#if defined(_WIN32)
    return exports_dll_register_server(lib);
#else
    boost::system::error_code ec;
    return boost::filesystem::exists(lib, ec) && ! ec && ! carries_ultranet_module_tag(lib);
#endif
}

bool bambusource_needs_refresh(bool bundled_present,
                               bool installed_present,
                               bool identical,
                               bool installed_is_real_filter,
                               bool keep_foreign)
{
    if (! bundled_present || keep_foreign)
        return false;
    if (installed_present && identical)
        return false;
    return may_overwrite_bambusource(installed_present, installed_is_real_filter);
}

bool storage_browser_use_lan_url(bool lan_mode,
                                 bool local_proto,
                                 bool remote_proto,
                                 bool has_ip,
                                 bool has_access_code,
                                 bool lan_only_tunnel)
{
    if ((lan_mode || ! remote_proto) && local_proto && has_ip)
        return true; // stock
    return lan_only_tunnel && has_ip && has_access_code;
}

} } // namespace Slic3r::GUI
