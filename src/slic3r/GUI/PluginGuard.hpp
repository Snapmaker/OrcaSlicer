#ifndef slic3r_GUI_PluginGuard_hpp_
#define slic3r_GUI_PluginGuard_hpp_

#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI {

// EdgeSlicer ships its own clean-room network plug-in ("UltraNet"), a drop-in for Bambu's
// bambu_networking.dll. Two things then have to be kept straight, and both are decided here so the
// decision can be tested without wx, a config file, a network or a printer:
//
//  1. Sign-in needs SOME plug-in. Bambu cloud sign-in with Google goes login webview -> system
//     browser -> our loopback on 13650 -> ticket exchange through the plug-in. With no plug-in
//     loaded the ticket has nowhere to go, so opening the sign-in page at all is a dead end;
//     offer the download instead.
//
//  2. UltraNet must never be overwritten. install_plugin() unzips Bambu's CDN package straight
//     over data_dir/plugins, and the "plug-in needs updating" path calls into the same code, so
//     every download entry point has to be shut off while our own plug-in is the one installed.
//
// UltraNet is recognised by a marker file written next to the DLLs (see kUltraNetMarkerName): the
// DLL name is Bambu's, so the DLL alone cannot tell the two apart. A folder holding
// bambu_networking.dll and no marker is a Bambu-original (or CDN-updated) plug-in and keeps the
// stock behaviour in full.
extern const char *const kUltraNetMarkerName; // "ultranet.txt"

// What the app should do when the user asks to sign in to a Bambu account.
enum class LoginGuardAction {
    // A plug-in is loaded and usable: open the sign-in page as before.
    ShowLogin,
    // No agent. UltraNet is installed, so the download dialog would be wrong (and dangerous) -
    // the plug-in is there and simply did not load, e.g. it needs the restart after a first-run
    // install. Tell the user to restart rather than offering Bambu's CDN.
    RestartRequired,
    // No agent and no UltraNet: offer the Bambu network plug-in download.
    OfferPluginDownload,
};

// The single decision behind every guarded entry point.
//
//   plugin_present      - data_dir/plugins holds a bambu_networking library
//   ultranet_marker     - ... and our marker file sits beside it
//   installed_networking- the app_config preference
//   agent_loaded        - wxGetApp().getAgent() != nullptr
LoginGuardAction plugin_guard_decision(bool plugin_present,
                                       bool ultranet_marker,
                                       bool installed_networking,
                                       bool agent_loaded);

// True when the installed plug-in is ours. This is the one that gates every Bambu CDN download and
// update prompt: the home-page banner command, the Device-tab install link, the Preferences
// checkbox's download offer, the wizard's offer, and m_networking_need_update handling.
// A marker with no DLL beside it is a leftover, not an installation.
bool is_ultranet_plugin(bool plugin_present, bool ultranet_marker);

// True when the Bambu CDN download/update path may run. It is the exact complement of the above,
// so an empty plug-in folder or a Bambu-original plug-in keeps working untouched.
bool bambu_cdn_download_allowed(bool plugin_present, bool ultranet_marker);

// What the start-up copier does with <data_dir>/plugins, given the sidecar folder ("ultranet"
// beside the exe) that a released build ships. Decided from four facts so it is testable.
//
// The old rule was "copy only if absent - never clobber a user/CDN-updated copy". That left two
// holes a user actually fell into: (1) after an EdgeSlicer upgrade the older UltraNet stayed in
// place and, since it no longer matched the sidecar byte for byte, got no marker - so every Bambu
// CDN entry point came back to life and the "plug-in needs updating" prompt unzipped Bambu's
// package over ours; (2) a data dir migrated from Snapmaker Orca arrived with Bambu's plug-in
// already in plugins/, which our host cannot use on Windows, and ours was never installed at all.
// There is no supported way to run a Bambu-original plug-in in EdgeSlicer, so the sidecar copy is
// authoritative: anything else in that slot is replaced, unless the user sets the escape hatch
// (app config `ultranet_keep_foreign_plugin`, for developers testing another build of the DLL).
enum class PluginSync {
    Nothing,         // no sidecar, or an identical copy already marked, or the escape hatch is set
    InstallFresh,    // plugins/ holds no network library: copy ours in and write the marker
    ReplaceForeign,  // plugins/ holds a different library (older UltraNet, Bambu's, or a hand copy):
                     // replace it with ours and (re)write the marker
    WriteMarkerOnly, // byte-identical to ours but unmarked (a 2.3.7.0 install): just mark it
};

PluginSync plugin_sync_decision(bool sidecar_present,
                                bool installed_present,
                                bool installed_identical,
                                bool marker_present,
                                bool keep_foreign);

// The network plug-in's file name on this platform - the name NetworkAgent loads from
// <data_dir>/plugins and the name the sidecar ships it under: bambu_networking.dll on Windows,
// libbambu_networking.dylib on macOS, libbambu_networking.so on Linux.
const char *network_library_name();

// Where a released build keeps the bundled plug-in (the "sidecar"), given the folder of the
// running executable. <exe dir>/ultranet on Windows and Linux (the AppImage's bin/ultranet);
// Contents/Resources/ultranet inside the macOS app bundle, because Contents/MacOS may hold only
// code - a text file there (the marker) makes the bundle unsignable.
boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir);
// The same rule with the layout spelled out, so both can be tested on any platform.
boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir, bool macos_app_bundle);

// ---------------------------------------------------------------------------------------------
// The camera component (BambuSource), which is a different thing from the network plug-in.
//
// Live view on a Bambu printer is played through BambuSource, a proprietary DirectShow source
// filter registered under CLSID {233E64FB-...}. It is NOT part of UltraNet and we cannot clean-room
// it: EdgeSlicer ships only a ~9.7 KB stub named BambuSource.dll so NetworkAgent's LoadLibrary
// probe of the plug-ins folder succeeds. The stub exports no DllRegisterServer, so the stock
// "press Yes to re-register it" path can only ever fail with "the entry-point DllRegisterServer
// was not found" - the reported bug.
//
// Everything here therefore has to tell the stub from the real filter, and must do so without
// running the DLL: the probe reads the PE export directory off the file on disk, never LoadLibrary
// (which would run DllMain of an untrusted binary and pin the file).

// The name of the DirectShow filter, per platform.
const char *bambu_source_library_name();

// True when `dll` is a real Bambu camera filter: it exists and its PE export table (or the
// platform equivalent) exports DllRegisterServer. Missing file, unreadable file, or a malformed
// image all answer false - the caller then treats it as "no real component", which is safe.
bool exports_dll_register_server(const boost::filesystem::path &dll);

// True when `dll` is EdgeSlicer's placeholder rather than Bambu's filter: our marker file sits in
// the same folder AND the library does not export DllRegisterServer. Both halves are required - a
// real filter dropped into our plug-ins folder keeps the marker beside it and must still be
// recognised as real, and a stray unexporting DLL somewhere without a marker is not ours to judge.
bool is_ultranet_bambusource_stub(const boost::filesystem::path &dll);

// What start_stream_service() should do about <data_dir>/cameratools/BambuSource.dll, decided from
// three facts so it can be tested without a filesystem.
enum class CameraToolsCopy {
    // cameratools has no usable filter and the plug-ins copy is only our stub: copying would
    // install a DLL that cannot be registered. Skip it and let the caller report the component as
    // missing.
    SkipStubMissingComponent,
    // cameratools already holds a real filter. Never overwrite it - in particular never with the
    // stub, which is the second half of the reported bug.
    KeepExisting,
    // Stock behaviour: the plug-ins copy is a real filter and cameratools is absent or stale.
    CopyFromPlugins,
};

CameraToolsCopy camera_tools_copy_decision(bool plugins_copy_is_stub,
                                           bool cameratools_has_real_filter,
                                           bool cameratools_up_to_date);

// What copy_network_if_available() does at startup with a plug-in staged in <data_dir>/ota by the
// Bambu plug-in update check (`update_network_plugin` = true in the app config). Stock code copies
// ota/bambu_networking.dll, BambuSource and live555 straight over <data_dir>/plugins - over UltraNet,
// whose library carries the same name. That must never happen unless the user deliberately keeps a
// foreign plug-in (`ultranet_keep_foreign_plugin`).
enum class OtaPluginInstall {
    NothingStaged, // the flag is not set: nothing to do (stock)
    Install,       // stock copy: no UltraNet installed, or the user keeps a foreign plug-in on purpose
    Refuse,        // UltraNet is installed: clear the flag, remove the staged files by exact name, log
};

OtaPluginInstall ota_plugin_install_decision(bool update_flag, bool ultranet_installed, bool keep_foreign);

// The exact file names in <data_dir>/ota that a refused install removes: the network library,
// BambuSource and live555 for this platform, and the network_plugins.json that described them.
// Never a pattern and never a path outside ota/.
const char *const *ota_plugin_staged_names(size_t &count);

// Whether the first-run/upgrade copier may write BambuSource over `dest`. A real filter the user
// obtained from Bambu must survive an EdgeSlicer upgrade, so the sidecar stub never replaces it.
bool may_overwrite_bambusource(bool dest_exists, bool dest_is_real_filter);

// macOS/Linux have no DllRegisterServer to look for, so there the placeholder identifies itself
// instead: every camera module UltraNet builds for those platforms carries this string (as an
// exported constant), and Bambu's own libBambuSource never does.
extern const char *const kUltraNetModuleTag; // "UltraNet-module"

// True when the file at `lib` contains kUltraNetModuleTag. Reads the file; never loads it. A
// missing, unreadable or oversized file answers false.
bool carries_ultranet_module_tag(const boost::filesystem::path &lib);

// The copier's "is what is already in plugins/ a real camera component?" question, per platform:
// a PE exporting DllRegisterServer on Windows (exports_dll_register_server, unchanged), and on
// macOS/Linux any existing library that does not carry the UltraNet tag - i.e. Bambu's.
bool is_real_camera_component(const boost::filesystem::path &lib);
// Whether start-up should copy the sidecar's BambuSource over <data_dir>/plugins/BambuSource.dll.
// BambuSource is checked on its own rather than only when the network plug-in is replaced: since
// it carries the storage browser it can change while bambu_networking.dll stays byte-identical,
// and a copy that failed once (the file was in use) must be retried on the next start. Never over
// Bambu's real camera filter, and not while the user keeps a foreign plug-in on purpose.
bool bambusource_needs_refresh(bool bundled_present,
                               bool installed_present,
                               bool identical,
                               bool installed_is_real_filter,
                               bool keep_foreign);

// ---------------------------------------------------------------------------------------------
// The Device tab's storage browser (MediaFilePanel::fetchUrl): whether to hand the tunnel library
// the printer's LAN address (bambu:///local/<ip>...) rather than ask the network agent for a
// cloud relay URL.
//
// Stock rule: the printer is in LAN-only mode or offers no remote file protocol, it offers the
// local one, and its IP is known. A tunnel library that can only reach printers on the LAN
// (PrinterFileSystem::TunnelIsLanOnly, EdgeSlicer's) has no other route at all, so it also takes
// the LAN address for a cloud-bound printer - whenever the IP and the access code are both known.
bool storage_browser_use_lan_url(bool lan_mode,
                                 bool local_proto,
                                 bool remote_proto,
                                 bool has_ip,
                                 bool has_access_code,
                                 bool lan_only_tunnel);

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginGuard_hpp_
