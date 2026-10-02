#ifndef slic3r_GUI_FFDiagnostics_hpp_
#define slic3r_GUI_FFDiagnostics_hpp_

#include <string>
#include <vector>

// The decisions behind the FlashForge LAN "Test connection" and "Diagnostics" buttons, kept free of
// wx, the filesystem, the network and FlashNetwork.dll so they can be tested on their own.
//
// Why this file exists at all: the FlashForge device tab drives printers through FlashForge's
// closed FlashNetwork.dll. Everything it can tell us about a failure arrives as a bare integer -
// the FNET_* codes in FlashNetwork.h - and every one of those integers reaches a user who has no
// way to look them up. A C5 owner who types one wrong character of the check code gets 1001, which
// says nothing; "serial number or check code rejected" ends the support thread on the spot. The
// map below is the whole point of the connection test, so it is the part that gets tested.
//
// The diagnostics zip has the same shape of problem from the other side: whatever we put in it is
// what the owner has to work from, and one entry - the check code - must never be in it. The
// manifest is built here, as data, so that "the check code never ships" is a unit test and not a
// code-reading exercise.

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------------------------
// FNET error codes
// ---------------------------------------------------------------------------------------------

// The name of an FNET_* constant, e.g. "FNET_VERIFY_LAN_DEV_FAILED", or "FNET_UNKNOWN" for a code
// FlashNetwork.h does not define. Kept separate from the human text so a bug report can quote the
// symbol the header actually uses.
std::string fnet_error_name(int code);

// A sentence a printer owner can act on. Never empty - an unmapped code still gets a usable line
// that quotes the number, because a new DLL may return codes this build has never seen.
std::string fnet_error_text(int code);

// "FNET_VERIFY_LAN_DEV_FAILED (1001): serial number or check code rejected". What the Test
// connection button shows on failure, and what goes into the diagnostics zip.
std::string fnet_error_description(int code);

// FNET_OK is the only success. Notably FNET_ERROR is -1, so a plain "ret < 0" test would let the
// positive failures (busy, aborted, gcode-not-found) through as success.
bool fnet_succeeded(int code);

// ---------------------------------------------------------------------------------------------
// The result of a LAN connection test
// ---------------------------------------------------------------------------------------------

// What fnet_getLanDevProduct / fnet_getLanDevDetail told us about one printer. Filled in by the
// GUI; rendered and summarised here.
struct FFConnectionTestResult
{
    // What was tried.
    std::string serial_number;
    std::string ip;
    unsigned short port = 8898;

    // fnet_getLanDevProduct - reachability and the control capabilities the device advertises.
    bool product_attempted = false;
    int  product_code      = 0;

    // fnet_getLanDevDetail - the descriptive fields.
    bool detail_attempted = false;
    int  detail_code      = 0;

    std::string product_name;     // detail->name, the device's own name
    std::string machine_type;     // nozzle model / measure, as the tab shows it
    std::string firmware_version; // detail->firmwareVersion
    std::string mac_address;
    std::string status;           // "ready", "printing", ...

    // Set when the test could not run at all, e.g. FlashNetwork.dll never loaded. Then neither
    // call was attempted and the codes mean nothing.
    std::string unavailable_reason;
};

// True when the device answered the product call. A detail failure after a product success still
// counts as reachable - the printer is there, it just refused or could not describe itself.
bool ff_test_reachable(const FFConnectionTestResult &result);

// True when both calls succeeded, i.e. the credentials are good and the device is fully described.
bool ff_test_fully_ok(const FFConnectionTestResult &result);

// The one line the button puts in front of the user. Reachable/not, then either the product name
// and firmware or the failing call's error code and its meaning.
std::string ff_test_summary(const FFConnectionTestResult &result);

// The multi-line block written into the diagnostics zip and shown under the button. Contains the
// serial number and IP, never the check code.
std::string ff_test_report(const FFConnectionTestResult &result);

// ---------------------------------------------------------------------------------------------
// The diagnostics zip manifest
// ---------------------------------------------------------------------------------------------

// One entry in the zip: where it lands inside the archive, and where it came from. A source_path
// entry is copied from disk; an inline entry carries its own text (the test report, the summary).
struct FFDiagnosticsEntry
{
    std::string zip_path;    // path inside the archive, forward slashes
    std::string source_path; // absolute path on disk, empty for inline entries
    std::string inline_text; // content for inline entries, empty for copied files
    bool is_inline = false;
};

// What the caller knows when it builds the zip. Paths are absolute; the vectors hold the files the
// caller already found on disk.
struct FFDiagnosticsInput
{
    std::string app_version;          // SLIC3R_VERSION
    std::string app_build;            // build id / commit, may be empty
    std::string os_description;
    std::string flashnetwork_version; // fnet_getVersion(), empty when the DLL never loaded
    std::string flashnetwork_dll_path;
    bool        flashnetwork_loaded = false;

    FFConnectionTestResult test;

    std::vector<std::string> flashnetwork_logs; // absolute paths, <datadir>/FlashNetwork/*
    std::vector<std::string> app_logs;          // absolute paths, <datadir>/log/debug_*
};

// The file name the zip gets, e.g. "EdgeSlicer_FlashForge_diagnostics_2026-09-11.zip". The date is
// passed in rather than read from the clock so the name is testable.
std::string ff_diagnostics_file_name(const std::string &date_yyyy_mm_dd);

// The manifest: every file the zip will contain, in order. Log files keep their base names under
// flashnetwork/ and log/; the report and the device entry are written inline.
//
// The invariant this function exists to hold: no entry, inline or copied, carries the check code.
// The caller never passes it in - FFDiagnosticsInput has no field for it - which is the mechanism,
// and the manifest is checked for it anyway.
std::vector<FFDiagnosticsEntry> ff_diagnostics_manifest(const FFDiagnosticsInput &input);

// The top-level summary.txt written into the zip: app version, OS, DLL path and version, and the
// connection test report.
std::string ff_diagnostics_summary(const FFDiagnosticsInput &input);

// True when no entry of the manifest contains the given string. Used by the caller (and the tests)
// to assert that the check code never reaches the archive.
bool ff_diagnostics_manifest_is_free_of(const std::vector<FFDiagnosticsEntry> &entries,
                                        const std::string                    &secret);

// ---------------------------------------------------------------------------------------------
// Where FlashNetwork.dll is looked for
// ---------------------------------------------------------------------------------------------

// The library is FlashForge's and closed, so we redistribute it rather than build it. It is
// installed beside the executable by the FLASHNETWORK_BIN_DIR CMake option; a user who has to
// place it by hand (an old install, or a build made without that option) drops it in the data
// directory instead, which needs no administrator rights. Both are searched, in that order.
//
// Returns the candidate paths in search order. The caller takes the first that exists.
std::vector<std::string> ff_flashnetwork_search_paths(const std::string &exe_dir,
                                                      const std::string &data_dir);

// The message the Device tab shows when no candidate existed, listing every path that was tried so
// a user can say where they put the file. Never blank.
std::string ff_flashnetwork_missing_text(const std::vector<std::string> &searched);

// The message shown when the file was found but would not load - almost always a 32-bit DLL next
// to a 64-bit build, or a truncated download.
std::string ff_flashnetwork_load_failed_text(const std::string &path);

// ---------------------------------------------------------------------------------------------
// Why FlashNetwork did not come up
// ---------------------------------------------------------------------------------------------

// fnet_initlize() takes the path of FlashForge's server-settings file (FLASHNETWORK9.DAT for the
// 3.4.x library). It is not optional: given a path that does not exist, or a file that is empty or
// from another library generation (FLASHNETWORK7.DAT), the call returns FNET_ERROR and the whole
// FlashForge stack stays down. EdgeSlicer 2.4.2.0 shipped the DLL without it, and every install
// failed exactly there with nothing in the log but "initalize FlashNetwork failed".
//
// Candidate locations for that file, in search order, duplicates dropped:
//   1. <resources>/data/<dat>  - where the installer puts it, and where FlashForge's own client
//                                looks (resources/data beside the executable).
//   2. <dll dir>/<dat>         - beside the library, for a hand-placed pair in <data dir>/plugins.
//   3. <dll dir>/resources/data/<dat> - an Orca-Flashforge install picked with Locate, which
//                                keeps the file in its own resources/data.
//   4. <data dir>/plugins/<dat> - hand-placed next to a hand-placed DLL that was found elsewhere.
// An empty argument contributes nothing. The caller takes the first that exists.
std::vector<std::string> ff_flashnetwork_dat_search_paths(const std::string &resources_dir,
                                                          const std::string &dll_path,
                                                          const std::string &data_dir,
                                                          const std::string &dat_name);

// The step at which bringing FlashNetwork up failed. Each has its own log line and its own
// sentence, because "failed to initialize" alone is what made the 2.4.2.0 failure undiagnosable.
enum class FFNetInitStage
{
    LoadFailed,    // LoadLibrary/dlopen refused the file; code = GetLastError() (0 on dlopen)
    MissingSymbol, // the library lacks an export this build binds; detail = the symbol
    InitFailed,    // fnet_initlize returned non-zero; code = its return, detail = the DAT path
    BadVersion,    // the library reports a version this build does not speak; detail = it
};

// One line for the log and the Device tab, naming the step, the path and the code, e.g.
//   "fnet_initlize returned -1 (FNET_ERROR) with server settings C:/.../FLASHNETWORK9.DAT"
// For InitFailed, dat_found says whether the settings file existed; when it did not the line says
// so, since that is the one cause a user (or the packager) can fix.
std::string ff_flashnetwork_init_error(FFNetInitStage stage, const std::string &library_path,
                                       const std::string &detail, long code, bool dat_found = true);

// A known Windows LoadLibrary error code turned into its likely cause ("a dependency is missing",
// "not a 64-bit library"), or an empty string for codes with no useful gloss.
std::string ff_loadlibrary_error_hint(long code);

// The Device tab text when the library was found but would not come up, carrying the reason
// ff_flashnetwork_init_error() produced. Falls back to ff_flashnetwork_load_failed_text() when
// there is no reason to show.
std::string ff_flashnetwork_init_failed_text(const std::string &path, const std::string &reason);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FFDiagnostics_hpp_
