#include "FFDiagnostics.hpp"

#include <algorithm>
#include <sstream>

#include "FlashNetwork.h"

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------------------------
// FNET error codes
// ---------------------------------------------------------------------------------------------

namespace {

// Only the plain file name, so the zip does not leak the user's home directory - and so two logs
// from different folders cannot silently overwrite each other in the archive.
std::string base_name(const std::string &path)
{
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string trimmed_or(const std::string &value, const char *fallback)
{
    return value.empty() ? std::string(fallback) : value;
}

} // namespace

std::string fnet_error_name(int code)
{
    switch (code) {
    case FNET_OK:                                  return "FNET_OK";
    case FNET_ERROR:                               return "FNET_ERROR";
    case FNET_ABORTED_BY_CALLBACK:                 return "FNET_ABORTED_BY_CALLBACK";
    case FNET_DIVICE_IS_BUSY:                      return "FNET_DIVICE_IS_BUSY";
    case FNET_GCODE_NOT_FOUND:                     return "FNET_GCODE_NOT_FOUND";
    case FNET_VERIFY_LAN_DEV_FAILED:               return "FNET_VERIFY_LAN_DEV_FAILED";
    case FNET_UNAUTHORIZED:                        return "FNET_UNAUTHORIZED";
    case FNET_INVALID_VALIDATION:                  return "FNET_INVALID_VALIDATION";
    case FNET_DEVICE_HAS_BEEN_BOUND:               return "FNET_DEVICE_HAS_BEEN_BOUND";
    case FNET_ABORT_AI_JOB_FAILED:                 return "FNET_ABORT_AI_JOB_FAILED";
    case FENT_AI_JOB_NOT_ENOUGH_POINTS:            return "FENT_AI_JOB_NOT_ENOUGH_POINTS";
    case FNET_NO_EXISTING_AI_MODEL_JOB:            return "FNET_NO_EXISTING_AI_MODEL_JOB";
    case FNET_INPUT_FAILED_THE_REVIEW:             return "FNET_INPUT_FAILED_THE_REVIEW";
    case FNET_PRINT_LIST_MODEL_COUNT_EXCEEDED:     return "FNET_PRINT_LIST_MODEL_COUNT_EXCEEDED";
    case FNET_CONN_SEND_ERROR:                     return "FNET_CONN_SEND_ERROR";
    default:                                       return "FNET_UNKNOWN";
    }
}

std::string fnet_error_text(int code)
{
    switch (code) {
    case FNET_OK:
        return "ok";
    // The library's catch-all. On a LAN call it is almost always the network rather than the
    // printer: nothing listening on the address, a firewall in the way, or the request timed out.
    case FNET_ERROR:
        return "no reply from the printer - check the IP address, that the printer is powered on "
               "and on the same network, and that LAN mode is enabled on it";
    case FNET_ABORTED_BY_CALLBACK:
        return "cancelled";
    case FNET_DIVICE_IS_BUSY:
        return "the printer is busy - it is printing or running a calibration and refuses new work";
    case FNET_GCODE_NOT_FOUND:
        return "the printer does not have that G-code file";
    // The one a C5 owner hits most: a mistyped check code, or a serial number from a different
    // machine. The printer answered, so the network is fine.
    case FNET_VERIFY_LAN_DEV_FAILED:
        return "serial number or check code rejected - the printer answered but did not accept "
               "these credentials (Settings > Network > Network Mode on the printer shows both)";
    case FNET_UNAUTHORIZED:
        return "the FlashForge account session has expired - sign in again";
    case FNET_INVALID_VALIDATION:
        return "the user name, password or SMS code was not accepted";
    case FNET_DEVICE_HAS_BEEN_BOUND:
        return "the printer is already bound to another FlashForge account";
    case FNET_ABORT_AI_JOB_FAILED:
        return "the AI job could not be aborted";
    case FENT_AI_JOB_NOT_ENOUGH_POINTS:
        return "the FlashForge account does not have enough AI points";
    case FNET_NO_EXISTING_AI_MODEL_JOB:
        return "there is no such AI job";
    case FNET_INPUT_FAILED_THE_REVIEW:
        return "the input was rejected by content review";
    case FNET_PRINT_LIST_MODEL_COUNT_EXCEEDED:
        return "the print list is full";
    case FNET_CONN_SEND_ERROR:
        return "the cloud connection dropped while sending";
    default:
        // A newer FlashNetwork.dll can return codes this build predates. Say so plainly instead
        // of pretending the number means nothing.
        return "unrecognised error from FlashNetwork - report this code";
    }
}

std::string fnet_error_description(int code)
{
    std::ostringstream out;
    out << fnet_error_name(code) << " (" << code << "): " << fnet_error_text(code);
    return out.str();
}

bool fnet_succeeded(int code)
{
    return code == FNET_OK;
}

// ---------------------------------------------------------------------------------------------
// Connection test
// ---------------------------------------------------------------------------------------------

bool ff_test_reachable(const FFConnectionTestResult &result)
{
    if (!result.unavailable_reason.empty())
        return false;
    // A detail call that got a real answer - including a credential rejection - also proves the
    // printer is on the network, which is what "reachable" means here.
    if (result.product_attempted && fnet_succeeded(result.product_code))
        return true;
    return result.detail_attempted && fnet_succeeded(result.detail_code);
}

bool ff_test_fully_ok(const FFConnectionTestResult &result)
{
    return result.unavailable_reason.empty() &&
           result.product_attempted && fnet_succeeded(result.product_code) &&
           result.detail_attempted && fnet_succeeded(result.detail_code);
}

std::string ff_test_summary(const FFConnectionTestResult &result)
{
    if (!result.unavailable_reason.empty())
        return "Test could not run: " + result.unavailable_reason;

    if (ff_test_fully_ok(result)) {
        std::ostringstream out;
        out << "Reachable: yes. " << trimmed_or(result.product_name, "(unnamed printer)");
        if (!result.firmware_version.empty())
            out << ", firmware " << result.firmware_version;
        return out.str();
    }

    // Report the first call that failed - the later one's code is meaningless once an earlier
    // step failed.
    if (result.product_attempted && !fnet_succeeded(result.product_code))
        return "Reachable: no. " + fnet_error_description(result.product_code);
    if (result.detail_attempted && !fnet_succeeded(result.detail_code))
        return "Reachable: yes, but the printer would not describe itself. " +
               fnet_error_description(result.detail_code);

    return "Test did not run.";
}

std::string ff_test_report(const FFConnectionTestResult &result)
{
    std::ostringstream out;
    out << "FlashForge LAN connection test\n";
    out << "  serial number   : " << trimmed_or(result.serial_number, "(none entered)") << "\n";
    out << "  address         : " << trimmed_or(result.ip, "(none entered)") << ":" << result.port << "\n";
    // Deliberately no check code line. See the header.
    out << "  check code      : not recorded\n";

    if (!result.unavailable_reason.empty()) {
        out << "  result          : not run (" << result.unavailable_reason << ")\n";
        return out.str();
    }

    out << "  reachable       : " << (ff_test_reachable(result) ? "yes" : "no") << "\n";
    out << "  fnet_getLanDevProduct : "
        << (result.product_attempted ? fnet_error_description(result.product_code)
                                     : std::string("not attempted"))
        << "\n";
    out << "  fnet_getLanDevDetail  : "
        << (result.detail_attempted ? fnet_error_description(result.detail_code)
                                    : std::string("not attempted"))
        << "\n";
    out << "  product name    : " << trimmed_or(result.product_name, "(unknown)") << "\n";
    out << "  machine type    : " << trimmed_or(result.machine_type, "(unknown)") << "\n";
    out << "  firmware        : " << trimmed_or(result.firmware_version, "(unknown)") << "\n";
    out << "  mac address     : " << trimmed_or(result.mac_address, "(unknown)") << "\n";
    out << "  printer status  : " << trimmed_or(result.status, "(unknown)") << "\n";
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Diagnostics zip
// ---------------------------------------------------------------------------------------------

std::string ff_diagnostics_file_name(const std::string &date_yyyy_mm_dd)
{
    return "EdgeSlicer_FlashForge_diagnostics_" + date_yyyy_mm_dd + ".zip";
}

std::string ff_diagnostics_summary(const FFDiagnosticsInput &input)
{
    std::ostringstream out;
    out << "EdgeSlicer FlashForge diagnostics\n";
    out << "=================================\n\n";
    out << "application version : " << trimmed_or(input.app_version, "(unknown)") << "\n";
    if (!input.app_build.empty())
        out << "build               : " << input.app_build << "\n";
    out << "operating system    : " << trimmed_or(input.os_description, "(unknown)") << "\n";
    out << "FlashNetwork.dll    : " << trimmed_or(input.flashnetwork_dll_path, "(not found)") << "\n";
    out << "FlashNetwork loaded : " << (input.flashnetwork_loaded ? "yes" : "no") << "\n";
    out << "FlashNetwork version: " << trimmed_or(input.flashnetwork_version, "(unknown)") << "\n";
    out << "\n";
    out << ff_test_report(input.test);
    out << "\n";
    out << "summary: " << ff_test_summary(input.test) << "\n";
    out << "\n";
    out << "This archive contains no printer check code.\n";
    return out.str();
}

std::vector<FFDiagnosticsEntry> ff_diagnostics_manifest(const FFDiagnosticsInput &input)
{
    std::vector<FFDiagnosticsEntry> entries;

    FFDiagnosticsEntry summary;
    summary.zip_path    = "summary.txt";
    summary.inline_text = ff_diagnostics_summary(input);
    summary.is_inline   = true;
    entries.push_back(summary);

    FFDiagnosticsEntry test;
    test.zip_path    = "connection_test.txt";
    test.inline_text = ff_test_report(input.test);
    test.is_inline   = true;
    entries.push_back(test);

    // Two names could collide once flattened to base names (FlashNetwork rotates by date, our own
    // logs by date too, but a user can have copies). Keep the first and suffix the rest rather
    // than silently dropping one.
    auto add_files = [&entries](const std::vector<std::string> &paths, const char *dir) {
        int n = 0;
        for (const std::string &path : paths) {
            if (path.empty())
                continue;
            std::string name = base_name(path);
            std::string zip  = std::string(dir) + "/" + name;
            const bool taken = std::any_of(entries.begin(), entries.end(),
                                           [&zip](const FFDiagnosticsEntry &e) { return e.zip_path == zip; });
            if (taken)
                zip = std::string(dir) + "/" + std::to_string(++n) + "_" + name;
            FFDiagnosticsEntry entry;
            entry.zip_path    = zip;
            entry.source_path = path;
            entry.is_inline   = false;
            entries.push_back(entry);
        }
    };

    add_files(input.flashnetwork_logs, "flashnetwork");
    add_files(input.app_logs, "log");

    return entries;
}

// ---------------------------------------------------------------------------------------------
// Where FlashNetwork.dll is looked for
// ---------------------------------------------------------------------------------------------

namespace {

std::string with_sep(const std::string &dir)
{
    if (dir.empty())
        return std::string();
    const char last = dir[dir.size() - 1];
    return (last == '/' || last == '\\') ? dir : dir + "/";
}

} // namespace

std::vector<std::string> ff_flashnetwork_search_paths(const std::string &exe_dir,
                                                      const std::string &data_dir)
{
    std::vector<std::string> paths;
    // Beside the executable first: that is where the installer puts it, and where a per-machine
    // install keeps it read-only for ordinary users.
    if (!exe_dir.empty())
        paths.push_back(with_sep(exe_dir) + "FlashNetwork.dll");
    // Then the writable per-user location, which is the only one a user without administrator
    // rights can fill in by hand.
    if (!data_dir.empty())
        paths.push_back(with_sep(data_dir) + "plugins/FlashNetwork.dll");
    return paths;
}

std::string ff_flashnetwork_missing_text(const std::vector<std::string> &searched)
{
    std::ostringstream out;
    out << "FlashForge printers need FlashForge's FlashNetwork library, which is not installed.\n\n"
           "Looked for it in:\n";
    if (searched.empty()) {
        out << "  (no location could be determined)\n";
    } else {
        for (const std::string &path : searched)
            out << "  " << path << "\n";
    }
    out << "\nUse Download to fetch it, or Locate to point at a copy you already have.";
    return out.str();
}

std::string ff_flashnetwork_load_failed_text(const std::string &path)
{
    std::ostringstream out;
    out << "FlashForge's FlashNetwork library was found but could not be loaded:\n\n  "
        << trimmed_or(path, "(unknown path)") << "\n\n"
           "This is usually a 32-bit copy of the library next to a 64-bit EdgeSlicer, or a "
           "download that did not finish. Use Download to replace it, or Locate to point at "
           "another copy.";
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Why FlashNetwork did not come up
// ---------------------------------------------------------------------------------------------

std::vector<std::string> ff_flashnetwork_dat_search_paths(const std::string &resources_dir,
                                                          const std::string &dll_path,
                                                          const std::string &data_dir,
                                                          const std::string &dat_name)
{
    std::vector<std::string> paths;
    auto add = [&paths](const std::string &path) {
        if (std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(path);
    };
    if (dat_name.empty())
        return paths;
    if (!resources_dir.empty())
        add(with_sep(resources_dir) + "data/" + dat_name);
    const std::size_t slash = dll_path.find_last_of("/\\");
    if (slash != std::string::npos) {
        const std::string dll_dir = dll_path.substr(0, slash + 1);
        add(dll_dir + dat_name);
        add(dll_dir + "resources/data/" + dat_name);
    }
    if (!data_dir.empty())
        add(with_sep(data_dir) + "plugins/" + dat_name);
    return paths;
}

std::string ff_loadlibrary_error_hint(long code)
{
    switch (code) {
    case 2:   // ERROR_FILE_NOT_FOUND
    case 3:   // ERROR_PATH_NOT_FOUND
        return "the file is not there";
    case 5:   // ERROR_ACCESS_DENIED
        return "access denied - security software may be blocking the library";
    case 126: // ERROR_MOD_NOT_FOUND
        return "a library it depends on is missing - usually the Microsoft Visual C++ runtime";
    case 127: // ERROR_PROC_NOT_FOUND
        return "a library it depends on is too old - usually the Microsoft Visual C++ runtime";
    case 193: // ERROR_BAD_EXE_FORMAT
        return "not a valid 64-bit library - a 32-bit copy, or a truncated download";
    case 216: // ERROR_EXE_MACHINE_TYPE_MISMATCH
        return "built for a different processor architecture";
    case 1114: // ERROR_DLL_INIT_FAILED
        return "the library's own start-up code failed";
    default:
        return std::string();
    }
}

std::string ff_flashnetwork_init_error(FFNetInitStage stage, const std::string &library_path,
                                       const std::string &detail, long code, bool dat_found)
{
    std::ostringstream out;
    const std::string lib = trimmed_or(library_path, "(unknown path)");
    switch (stage) {
    case FFNetInitStage::LoadFailed: {
        out << "could not load " << lib;
        if (code != 0) {
            out << ": system error " << code;
            const std::string hint = ff_loadlibrary_error_hint(code);
            if (!hint.empty())
                out << " (" << hint << ")";
        }
        if (!detail.empty())
            out << ": " << detail;
        break;
    }
    case FFNetInitStage::MissingSymbol:
        out << lib << " does not export " << trimmed_or(detail, "(unnamed symbol)")
            << " - this FlashNetwork build is not one EdgeSlicer can use";
        break;
    case FFNetInitStage::InitFailed:
        out << "fnet_initlize returned " << code << " (" << fnet_error_name(int(code)) << ")"
            << " with server settings " << trimmed_or(detail, "(none)");
        if (!dat_found)
            out << " - that file does not exist, and the library cannot start without it";
        break;
    case FFNetInitStage::BadVersion:
        out << lib << " reports version " << trimmed_or(detail, "(none)")
            << "; EdgeSlicer needs a 3.x FlashNetwork";
        break;
    }
    return out.str();
}

std::string ff_flashnetwork_init_failed_text(const std::string &path, const std::string &reason)
{
    if (reason.empty())
        return ff_flashnetwork_load_failed_text(path);
    std::ostringstream out;
    out << "FlashForge's FlashNetwork library was found but could not be started:\n\n  "
        << trimmed_or(path, "(unknown path)") << "\n\n"
        << "Reason: " << reason << "\n\n"
           "If the reason names FLASHNETWORK9.DAT, the library's server-settings file is missing "
           "from this install: reinstall EdgeSlicer, or place the file beside FlashNetwork.dll. "
           "FlashForge's own Orca-Flashforge keeps it in its resources\\data folder, so Locate "
           "can also point at the FlashNetwork.dll of such an installation.";
    return out.str();
}

bool ff_diagnostics_manifest_is_free_of(const std::vector<FFDiagnosticsEntry> &entries,
                                        const std::string                    &secret)
{
    if (secret.empty())
        return true;
    for (const FFDiagnosticsEntry &entry : entries) {
        if (entry.zip_path.find(secret) != std::string::npos)
            return false;
        if (entry.inline_text.find(secret) != std::string::npos)
            return false;
        if (entry.source_path.find(secret) != std::string::npos)
            return false;
    }
    return true;
}

}} // namespace Slic3r::GUI
