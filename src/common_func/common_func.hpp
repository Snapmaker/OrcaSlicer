#ifndef _common_func_hppp_
#define _common_func_hppp_
#include <iostream>


// The product name lives here and nowhere else. version.inc reads these two lines back
// out of this header for CMake, and libslic3r.h derives its own names from them, so the
// three definitions that used to drift apart are now one.
#define SLIC3R_APP_NAME "EdgeSlicer"
#define SLIC3R_APP_KEY "EdgeSlicer"
// The data directories we used to live in, newest first. There have been two renames now
// (Snapmaker_Orca -> UltraOne -> EdgeSlicer), so this is a list rather than a constant and
// the first one that actually exists on disk is the one migrated from: a user coming from
// UltraOne gets their UltraOne data, a user who skipped that release gets their
// Snapmaker_Orca data, and nobody gets the older of two directories they both have.
//
// Read from here by the first-start migration (DataDirMigration.cpp) and by nothing else,
// so a later release drops all of this by emptying the list.
#define SLIC3R_LEGACY_APP_KEYS \
    {                          \
        "UltraOne", "Snapmaker_Orca"  \
    }
// The oldest of them, kept as a single name for the few places that only need to say
// "the directory this fork started out in".
#define SLIC3R_LEGACY_APP_KEY "Snapmaker_Orca"
#define SLIC3R_VERSION "01.10.01.50"
#define Snapmaker_VERSION "2.4.2.0" // the one version number; version.inc reads it back for CMake/CPack
#define MIN_FIRM_VER "1.5.0"
#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "0000000" // 0000000 means uninitialized
#endif
#define SLIC3R_BUILD_ID Snapmaker_VERSION
// #define SLIC3R_RC_VERSION "01.10.01.50"
#define BBL_RELEASE_TO_PUBLIC 1
#define BBL_INTERNAL_TESTING 0
#define ORCA_CHECK_GCODE_PLACEHOLDERS 0

namespace common
{
	// Remembers the --datadir of this process's command line (either "--datadir X" or
	// "--datadir=X"), so the reads below look where AppConfig will. Call it before initSentry().
	void set_datadir_from_command_line(int argc, char** argv);
#ifdef _WIN32
	void set_datadir_from_command_line(int argc, wchar_t** argv);
#endif

	// That --datadir (UTF-8), or "" when the command line had none.
	std::string datadir_override();

	// EdgeSlicer relaunching itself (GUI_App::request_relaunch) starts the new process with
	// "--relaunch-after <pid of the old one>". Call this first thing in main(): it waits until
	// that process has exited (at most two minutes; at once when it is gone already), so the new
	// one can never hand its launch to, or fight for ports and files with, the dying instance.
	// It leaves a one-line note in the environment variable EDGESLICER_RELAUNCH_NOTE for the log.
	// The option itself is not something the command line parser knows: drop it with
	// is_relaunch_after_arg() when copying the arguments on.
	void wait_for_relaunch_parent(int argc, char** argv);
#ifdef _WIN32
	void wait_for_relaunch_parent(int argc, wchar_t** argv);
#endif
	// True for "--relaunch-after" itself; the argument after it is its value (skip it too).
	bool is_relaunch_after_arg(const char* arg);
#ifdef _WIN32
	bool is_relaunch_after_arg(const wchar_t* arg);
#endif

	// The EdgeSlicer.conf AppConfig will use: --datadir, else a portable "data_dir" next to
	// the application, else the per-user default (%APPDATA%\EdgeSlicer on Windows). UTF-8.
	std::string app_config_path();

	// Read one value out of the "app" section of that file. For code that runs before
	// AppConfig exists; a missing file, key or a broken file gives "" / default_value.
	std::string get_app_config_string(const std::string& key);
	bool        get_app_config_bool(const std::string& key, bool default_value);

	// A crash report's minidump holds the command line: on Windows crashpad copies the process
	// parameters block, on macOS/Linux the argv strings sit at the top of the main thread's
	// stack. These overwrite the values of secret options (--hub-token) with '*' in those
	// copies. Call them only once the arguments have been copied elsewhere for parsing.
	void mask_secret_args(int argc, char** argv);
#ifdef _WIN32
	void mask_secret_args_in_process_command_line();
#endif

	std::string get_pc_name();

	std::string get_flutter_version();

	std::string get_profile_version();

	std::string getMachineId();

	std::string getLocalArea();

	std::string getLanguage();

    } // namespace common

#endif