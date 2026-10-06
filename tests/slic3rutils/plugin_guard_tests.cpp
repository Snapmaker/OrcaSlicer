// The network plug-in guards.
//
// EdgeSlicer ships its own clean-room plug-in ("UltraNet") under Bambu's library name, so two
// things have to be decided from the same handful of facts:
//
//  * Account > Login must not open a sign-in page when no agent is loaded - the Google flow ends
//    with the ticket being handed to the plug-in, and with none loaded the user just watches it
//    fail. Offer the plug-in download instead.
//  * Nothing may replace UltraNet. install_plugin() unzips Bambu's CDN package over
//    data_dir/plugins, so every download and update entry point is gated on "is the installed
//    plug-in ours?" - a question the DLL name alone cannot answer, hence the marker file.
//
// The trap the marker exists for: a leftover marker with no library beside it, and a Bambu-original
// library with no marker. Neither is UltraNet, and both must keep the CDN path fully working.
//
// Nothing here touches wx, a config file, the network, or a printer; only the UltraNet-tag case
// near the end writes two scratch files under the temp folder.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PluginGuard.hpp"

#include <fstream>
#include <string>

#include <boost/filesystem.hpp>

using Slic3r::GUI::CameraToolsCopy;
using Slic3r::GUI::LoginGuardAction;
using Slic3r::GUI::bambu_cdn_download_allowed;
using Slic3r::GUI::camera_tools_copy_decision;
using Slic3r::GUI::is_ultranet_plugin;
using Slic3r::GUI::may_overwrite_bambusource;
using Slic3r::GUI::bambusource_needs_refresh;
using Slic3r::GUI::storage_browser_use_lan_url;
using Slic3r::GUI::plugin_guard_decision;
using Slic3r::GUI::PluginSync;
using Slic3r::GUI::plugin_sync_decision;
using Slic3r::GUI::OtaPluginInstall;
using Slic3r::GUI::ota_plugin_install_decision;
using Slic3r::GUI::ota_plugin_staged_names;

TEST_CASE("UltraNet is a library AND a marker, never one alone", "[PluginGuard]")
{
    CHECK(is_ultranet_plugin(/*plugin_present*/ true, /*ultranet_marker*/ true));
    // A Bambu-original (or CDN-updated) plug-in: no marker was written beside it.
    CHECK_FALSE(is_ultranet_plugin(true, false));
    // A marker left behind after the library was removed or replaced by hand.
    CHECK_FALSE(is_ultranet_plugin(false, true));
    // Empty plug-ins folder.
    CHECK_FALSE(is_ultranet_plugin(false, false));
}

TEST_CASE("The Bambu CDN path is disabled exactly when UltraNet is installed", "[PluginGuard]")
{
    CHECK_FALSE(bambu_cdn_download_allowed(true, true));
    // The regression this guards: an empty folder or a Bambu-original plug-in must keep the
    // download and update path working in full.
    CHECK(bambu_cdn_download_allowed(false, false));
    CHECK(bambu_cdn_download_allowed(true, false));
    CHECK(bambu_cdn_download_allowed(false, true));
}

TEST_CASE("A loaded agent is left alone", "[PluginGuard]")
{
    // The running agent is the ground truth - it can exchange the sign-in ticket, so sign-in opens.
    CHECK(plugin_guard_decision(true, true, true, /*agent_loaded*/ true) == LoginGuardAction::ShowLogin);
    CHECK(plugin_guard_decision(true, false, true, true) == LoginGuardAction::ShowLogin);
    // A stale installed_networking=false must not block a session that is already working.
    CHECK(plugin_guard_decision(true, true, /*installed_networking*/ false, true) == LoginGuardAction::ShowLogin);
    CHECK(plugin_guard_decision(false, false, false, true) == LoginGuardAction::ShowLogin);
}

TEST_CASE("A fresh install with no plug-in is offered the download, not a dead sign-in page", "[PluginGuard]")
{
    // The reported bug: no plugins folder at all, so the loopback callback has nothing to hand the
    // ticket to.
    CHECK(plugin_guard_decision(/*plugin_present*/ false, /*ultranet_marker*/ false,
                                /*installed_networking*/ false, /*agent_loaded*/ false)
          == LoginGuardAction::OfferPluginDownload);
    // Same answer with the preference on: the preference is not what makes sign-in work.
    CHECK(plugin_guard_decision(false, false, true, false) == LoginGuardAction::OfferPluginDownload);
    // A Bambu-original plug-in that failed to load: the download is still the right offer.
    CHECK(plugin_guard_decision(true, false, true, false) == LoginGuardAction::OfferPluginDownload);
    // A leftover marker is not a plug-in.
    CHECK(plugin_guard_decision(false, true, true, false) == LoginGuardAction::OfferPluginDownload);
}

TEST_CASE("UltraNet installed but not loaded asks for a restart, never a download", "[PluginGuard]")
{
    // The dangerous case: offering Bambu's CDN here would unzip their package over ours. The usual
    // cause is the first-run copy landing after the plug-in load point, which a restart fixes.
    CHECK(plugin_guard_decision(true, true, true, /*agent_loaded*/ false) == LoginGuardAction::RestartRequired);
    CHECK(plugin_guard_decision(true, true, /*installed_networking*/ false, false) == LoginGuardAction::RestartRequired);
}

// ---------------------------------------------------------------------------------------------
// The camera component (BambuSource), which is a different thing from the network plug-in.
//
// Live view plays through Bambu's proprietary DirectShow filter. We cannot clean-room it, so
// EdgeSlicer ships a ~9.7 KB placeholder under that name purely so the agent's LoadLibrary probe of
// the plug-ins folder succeeds. The placeholder exports no DllRegisterServer, which is exactly why
// the stock "press Yes to re-register it" path could only ever fail. Whether a given file is the
// placeholder is a filesystem/PE question, tested against real DLLs by hand; what is decided here
// is what to DO once that question has been answered.

TEST_CASE("The stub is never copied over cameratools", "[PluginGuard]")
{
    // The reported bug, second half: plug-ins holds our placeholder and cameratools has nothing.
    // Copying would install a DLL that cannot be registered, so skip and report it missing.
    CHECK(camera_tools_copy_decision(/*plugins_copy_is_stub*/ true,
                                     /*cameratools_has_real_filter*/ false,
                                     /*cameratools_up_to_date*/ false)
          == CameraToolsCopy::SkipStubMissingComponent);
    // Timestamps happening to match changes nothing: the file is still unusable.
    CHECK(camera_tools_copy_decision(true, false, true) == CameraToolsCopy::SkipStubMissingComponent);
}

TEST_CASE("A real filter already in cameratools is never overwritten", "[PluginGuard]")
{
    // The worst case this guards: the user downloads the component, then the next Play stamps our
    // 9.7 KB placeholder over it and live view breaks again.
    CHECK(camera_tools_copy_decision(/*plugins_copy_is_stub*/ true, /*has_real*/ true, false)
          == CameraToolsCopy::KeepExisting);
    // Even a real plug-ins copy does not justify clobbering a real cameratools copy.
    CHECK(camera_tools_copy_decision(false, true, false) == CameraToolsCopy::KeepExisting);
    CHECK(camera_tools_copy_decision(false, true, true) == CameraToolsCopy::KeepExisting);
}

TEST_CASE("Stock copy behaviour survives for a real plug-ins filter", "[PluginGuard]")
{
    // A Bambu-original install, or one where the user installed the component: refresh cameratools
    // when it is stale, leave it alone when it is current.
    CHECK(camera_tools_copy_decision(/*stub*/ false, /*has_real*/ false, /*up_to_date*/ false)
          == CameraToolsCopy::CopyFromPlugins);
    CHECK(camera_tools_copy_decision(false, false, true) == CameraToolsCopy::KeepExisting);
}

TEST_CASE("An upgrade keeps a camera component the user downloaded", "[PluginGuard]")
{
    // The first-run copier re-runs on upgrade. A real filter must outlive it...
    CHECK_FALSE(may_overwrite_bambusource(/*dest_exists*/ true, /*dest_is_real_filter*/ true));
    // ...while our own older placeholder may be refreshed, and an absent file simply written.
    CHECK(may_overwrite_bambusource(true, false));
    CHECK(may_overwrite_bambusource(false, false));
}

TEST_CASE("The sidecar plug-in is authoritative over whatever sits in plugins/", "[PluginGuard]")
{
    // Fresh data dir: install and mark.
    CHECK(plugin_sync_decision(/*sidecar*/ true, /*installed*/ false, /*identical*/ false, /*marker*/ false, /*keep*/ false) == PluginSync::InstallFresh);
    // The reported bug: after an upgrade the older UltraNet stayed (different bytes, so no marker was
    // ever written), the CDN paths came back and Bambu's package replaced it. Now it is updated.
    CHECK(plugin_sync_decision(true, true, false, false, false) == PluginSync::ReplaceForeign);
    // Same with an old marker beside an old DLL: the marker does not make it current.
    CHECK(plugin_sync_decision(true, true, false, true, false) == PluginSync::ReplaceForeign);
    // Bambu's plug-in already replaced ours (or came in with a migrated data dir): take it back.
    CHECK(plugin_sync_decision(true, true, false, false, false) == PluginSync::ReplaceForeign);
    // Already ours and marked: nothing to do on every later start.
    CHECK(plugin_sync_decision(true, true, true, true, false) == PluginSync::Nothing);
    // A 2.3.7.0 install shipped the DLLs without a marker: mark, do not copy.
    CHECK(plugin_sync_decision(true, true, true, false, false) == PluginSync::WriteMarkerOnly);
}

TEST_CASE("No sidecar means no opinion, and the escape hatch keeps a foreign plug-in", "[PluginGuard]")
{
    // A developer tree or a platform without the sidecar must not touch plugins/ at all.
    CHECK(plugin_sync_decision(false, false, false, false, false) == PluginSync::Nothing);
    CHECK(plugin_sync_decision(false, true, false, false, false) == PluginSync::Nothing);
    CHECK(plugin_sync_decision(false, true, false, true, false) == PluginSync::Nothing);
    // ultranet_keep_foreign_plugin: a developer testing another build of the DLL keeps it.
    CHECK(plugin_sync_decision(true, true, false, false, true) == PluginSync::Nothing);
    CHECK(plugin_sync_decision(true, true, false, true, true) == PluginSync::Nothing);
    // The hatch never blocks a fresh install or the marker on an identical copy.
    CHECK(plugin_sync_decision(true, false, false, false, true) == PluginSync::InstallFresh);
    CHECK(plugin_sync_decision(true, true, true, false, true) == PluginSync::WriteMarkerOnly);
}

// ---------------------------------------------------------------------------------------------
// macOS and Linux (the UltraNet port). The copier and the guards run the same decisions there;
// only the file names and the sidecar location differ, and Windows must keep its exact values.

TEST_CASE("The network library carries the name NetworkAgent loads on this platform", "[PluginGuard]")
{
    const std::string name = Slic3r::GUI::network_library_name();
#if defined(_WIN32)
    CHECK(name == "bambu_networking.dll");
    CHECK(std::string(Slic3r::GUI::bambu_source_library_name()) == "BambuSource.dll");
#elif defined(__APPLE__)
    CHECK(name == "libbambu_networking.dylib");
    CHECK(std::string(Slic3r::GUI::bambu_source_library_name()) == "libBambuSource.dylib");
#else
    CHECK(name == "libbambu_networking.so");
    CHECK(std::string(Slic3r::GUI::bambu_source_library_name()) == "libBambuSource.so");
#endif
}

TEST_CASE("The sidecar sits beside the exe, or in the app bundle's Resources on macOS", "[PluginGuard]")
{
    namespace fs = boost::filesystem;
    using Slic3r::GUI::ultranet_sidecar_dir;
    // Windows install dir and the Linux AppImage's bin/: <exe dir>/ultranet, as it always was.
    CHECK(ultranet_sidecar_dir(fs::path("C:/Program Files/EdgeSlicer"), false) ==
          fs::path("C:/Program Files/EdgeSlicer") / "ultranet");
    CHECK(ultranet_sidecar_dir(fs::path("/tmp/.mount_EdgeS/bin"), false) == fs::path("/tmp/.mount_EdgeS/bin/ultranet"));
    // macOS: Contents/MacOS may hold only code, so the plug-in and its marker live in Resources.
    CHECK(ultranet_sidecar_dir(fs::path("/Applications/EdgeSlicer.app/Contents/MacOS"), true) ==
          fs::path("/Applications/EdgeSlicer.app/Contents/Resources/ultranet"));
#if defined(_WIN32)
    // The platform default is unchanged on Windows.
    CHECK(ultranet_sidecar_dir(fs::path("C:/EdgeSlicer")) == fs::path("C:/EdgeSlicer") / "ultranet");
#endif
}

// This one writes two small files under the temp folder (per-process unique names).
TEST_CASE("macOS/Linux tell our camera placeholder from Bambu's by the UltraNet tag", "[PluginGuard]")
{
    namespace fs = boost::filesystem;
    using Slic3r::GUI::carries_ultranet_module_tag;
    using Slic3r::GUI::kUltraNetModuleTag;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("plugin_guard_tag_%%%%-%%%%-%%%%");
    fs::create_directories(dir);
    const fs::path ours = dir / "ours.bin", theirs = dir / "theirs.bin", edge = dir / "edge.bin";
    {
        // The tag buried in the middle of a larger image, as it sits in .rodata.
        std::ofstream f(ours.string(), std::ios::binary);
        f << std::string(100000, '\x7f') << kUltraNetModuleTag << std::string(5000, '\0');
    }
    {
        std::ofstream f(theirs.string(), std::ios::binary);
        f << std::string(200000, 'B') << "Bambu_Create" << std::string(10, '\0');
    }
    {
        // Straddling the 64 KB read boundary.
        std::ofstream f(edge.string(), std::ios::binary);
        f << std::string(64 * 1024 - 5, 'x') << kUltraNetModuleTag;
    }
    CHECK(carries_ultranet_module_tag(ours));
    CHECK(carries_ultranet_module_tag(edge));
    CHECK_FALSE(carries_ultranet_module_tag(theirs));
    CHECK_FALSE(carries_ultranet_module_tag(dir / "missing.bin"));
#if !defined(_WIN32)
    // Off Windows this is the copier's "keep it" question: only Bambu's module is kept.
    CHECK(Slic3r::GUI::is_real_camera_component(theirs));
    CHECK_FALSE(Slic3r::GUI::is_real_camera_component(ours));
    CHECK_FALSE(Slic3r::GUI::is_real_camera_component(dir / "missing.bin"));
#endif
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("BambuSource is refreshed on its own, never over Bambu's camera filter", "[PluginGuard]")
{
    // The storage-browser tunnel changed BambuSource while the network plug-in may stay identical:
    // an older placeholder is replaced even then.
    CHECK(bambusource_needs_refresh(/*bundled*/ true, /*installed*/ true, /*identical*/ false, /*real*/ false, /*keep*/ false));
    // A fresh data dir gets it; an identical copy is left alone.
    CHECK(bambusource_needs_refresh(true, false, false, false, false));
    CHECK_FALSE(bambusource_needs_refresh(true, true, true, false, false));
    // Bambu's real filter (live view) always survives.
    CHECK_FALSE(bambusource_needs_refresh(true, true, false, true, false));
    // No sidecar, or the developer escape hatch: hands off.
    CHECK_FALSE(bambusource_needs_refresh(false, true, false, false, false));
    CHECK_FALSE(bambusource_needs_refresh(true, true, false, false, true));
}

TEST_CASE("Storage browser route: stock rule, plus the LAN address for a LAN-only tunnel", "[PluginGuard]")
{
    // Stock: LAN mode (or no remote file protocol) + a local protocol + a known IP.
    CHECK(storage_browser_use_lan_url(/*lan*/ true, /*local*/ true, /*remote*/ true, /*ip*/ true, /*code*/ true, /*lan_only*/ false));
    CHECK(storage_browser_use_lan_url(false, true, false, true, true, false));
    // Stock: a cloud-bound printer with a relay goes to the cloud, whatever is known locally.
    CHECK_FALSE(storage_browser_use_lan_url(false, true, true, true, true, false));
    CHECK_FALSE(storage_browser_use_lan_url(true, true, true, false, true, false));
    // A LAN-only tunnel (EdgeSlicer's FTPS one) takes the LAN address for that cloud-bound printer...
    CHECK(storage_browser_use_lan_url(false, true, true, true, true, true));
    // ...even when the printer advertises no local port-6000 protocol, which FTPS does not use...
    CHECK(storage_browser_use_lan_url(false, false, true, true, true, true));
    // ...but only with both the IP and the access code; otherwise the stock flow decides.
    CHECK_FALSE(storage_browser_use_lan_url(false, true, true, false, true, true));
    CHECK_FALSE(storage_browser_use_lan_url(false, true, true, true, false, true));
}

// copy_network_if_available(): a Bambu package staged in ota/ by the plug-in update check must never
// be copied over UltraNet (same library file name) unless the user keeps a foreign plug-in.
TEST_CASE("A staged Bambu plug-in is never installed over UltraNet", "[PluginGuard]")
{
    // No flag: nothing staged, whatever is installed.
    CHECK(ota_plugin_install_decision(false, true, false) == OtaPluginInstall::NothingStaged);
    CHECK(ota_plugin_install_decision(false, false, false) == OtaPluginInstall::NothingStaged);
    CHECK(ota_plugin_install_decision(false, true, true) == OtaPluginInstall::NothingStaged);
    // UltraNet installed: refused (flag cleared, staged files removed).
    CHECK(ota_plugin_install_decision(true, true, false) == OtaPluginInstall::Refuse);
    // ...unless the user keeps a foreign plug-in on purpose.
    CHECK(ota_plugin_install_decision(true, true, true) == OtaPluginInstall::Install);
    // No UltraNet (a self-built tree, or Bambu's plug-in): the stock copy.
    CHECK(ota_plugin_install_decision(true, false, false) == OtaPluginInstall::Install);
    CHECK(ota_plugin_install_decision(true, false, true) == OtaPluginInstall::Install);
}

TEST_CASE("A refused ota install removes only exact file names inside ota/", "[PluginGuard]")
{
    size_t n = 0;
    const char *const *names = ota_plugin_staged_names(n);
    REQUIRE(n == 4);
    bool has_library = false, has_changelog = false;
    for (size_t i = 0; i < n; ++i) {
        const std::string name = names[i];
        CHECK_FALSE(name.empty());
        // A bare file name: no directory part, no parent reference, no wildcard.
        CHECK(name.find('/') == std::string::npos);
        CHECK(name.find('\\') == std::string::npos);
        CHECK(name.find("..") == std::string::npos);
        CHECK(name.find('*') == std::string::npos);
        has_library   = has_library || name == Slic3r::GUI::network_library_name();
        has_changelog = has_changelog || name == "network_plugins.json";
    }
    CHECK(has_library);
    CHECK(has_changelog);
}
