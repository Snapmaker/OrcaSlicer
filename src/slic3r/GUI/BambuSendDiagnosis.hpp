#ifndef slic3r_GUI_BambuSendDiagnosis_hpp_
#define slic3r_GUI_BambuSendDiagnosis_hpp_

#include <string>

namespace Slic3r { namespace GUI {

// Why a send to a Bambu printer failed, in words a user can act on.
//
// EdgeSlicer's network plug-in (UltraNet) prints over the local network only. Its cloud entry point,
// bambu_network_start_print, always answers BAMBU_NETWORK_ERR_PRINT_SP_POST_TASK_FAILED (-3120).
// The send paths were written for Bambu's own plug-in, where the cloud is the fallback, so a
// cloud-bound printer that could not take the LAN route - no microSD card, no IP address, no access
// code, or a LAN attempt that failed - ended with a bare "-3120" and nothing about the real cause.
// Everything here is decided from plain values so it can be tested without wx, a plug-in or a printer.

// Why the LAN route was not attempted for a cloud-bound printer. Checked in the order PrintJob has
// always filled params.comments in, so the tag matches what the logs have said until now.
enum class BambuLanSkip {
    None,              // every precondition held: the LAN route was (or will be) attempted
    NoIp,              // "no_ip": the printer was never discovered on this network
    CloudOnlyFirmware, // "low_version": the firmware reported cloud-only printing
    NoSdcard,          // "no_sdcard": P1/A1 need a microSD card to print over LAN
    NoAccessCode,      // "no_password": no access code entered for the printer
};

BambuLanSkip bambu_lan_skip_reason(bool has_ip, bool cloud_print_only, bool has_sdcard, bool has_access_code);

// "no_ip", "low_version", "no_sdcard", "no_password", or "" for None - the params.comments values.
const char* bambu_lan_skip_tag(BambuLanSkip skip);

// Whether the loaded plug-in can print through the Bambu cloud at all. UltraNet cannot, so with it
// the cloud "fallback" can only fail, and is not attempted.
bool bambu_cloud_print_supported(bool ultranet_plugin);

// The printer's answer to a command ("result"/"reason" of its MQTT reply) says it refused the
// command as unverified - what Bambu firmware with Authorization Control answers third-party
// software unless LAN Only Mode and Developer Mode are both on.
bool bambu_reply_is_auth_refusal(const std::string& result, const std::string& reason);

// What went wrong on a LAN attempt, from its result code, the last error text the plug-in reported
// through the status callback, and whether the printer's own reply refused the command.
enum class BambuLanFailure {
    Connect,      // could not reach the printer (MQTT 8883 / FTPS 990)
    AccessCode,   // the printer rejected the access code
    Upload,       // the FTPS upload failed
    Publish,      // the file arrived but the print command did not take
    Refused,      // the printer refused the command (LAN Only Mode + Developer Mode)
    FileTooLarge,
    FileMissing,
    Other,
};

BambuLanFailure bambu_classify_lan_failure(int code, const std::string& detail, bool printer_refused);

struct BambuSendFailure
{
    bool         upload_only { false };            // SendJob: to the printer's storage, not a print
    BambuLanSkip lan_skip { BambuLanSkip::None };  // the LAN route was not attempted, and why
    int          skipped_code { -3120 };           // the code shown then: the cloud's -3120 by default
    int          lan_code { 0 };                   // the LAN attempt's result, when it ran
    std::string  lan_detail;                       // the plug-in's last error text for that attempt
    bool         printer_refused { false };        // the printer's reply refused the command
    bool         cloud_bound { true };             // a cloud-bound printer, where the cloud was the fallback
    bool         cloud_supported { true };         // false: the plug-in has no cloud printing
    bool         cloud_tried { false };            // start_print ran after the LAN route
    int          cloud_code { 0 };                 // ... and returned this
    bool         ultranet_log { false };           // the LAN attempt wrote ultranet_print.log
    std::string  log_dir;                          // the log folder, as the user should read it
    std::string  log_prefix { "print_job:" };      // what this path's lines start with in the main log
};

struct BambuSendFailureText
{
    int         code { 0 };  // the number the error panel shows
    std::string headline;    // the status line: the cause in a few words, with the code
    std::string detail;      // what to do about it
    std::string where;       // where the details are logged
    std::string full() const;
};

// translate=false keeps the English text, for the phone hub whose other messages are English.
BambuSendFailureText bambu_send_failure_text(const BambuSendFailure& f, bool translate = true);

// The log folder as a user should type it: "%APPDATA%\EdgeSlicer\log" when the data folder is under
// %APPDATA% (so a screenshot does not carry the Windows user name), the full path otherwise.
std::string bambu_log_dir_for_display(const std::string& data_dir, const std::string& appdata);

// The same, for this process: data_dir() and the APPDATA environment variable.
std::string bambu_log_dir_for_display();

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_BambuSendDiagnosis_hpp_
