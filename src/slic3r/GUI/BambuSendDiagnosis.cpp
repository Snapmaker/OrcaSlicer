#include "BambuSendDiagnosis.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>

#include <wx/utils.h>

#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

namespace Slic3r { namespace GUI {

namespace {

// Every sentence below is marked with L() for the catalogue and translated here, so the phone hub
// can ask for the English text its other messages are in.
std::string tr(bool translate, const char* s) { return translate ? I18N::translate_utf8(s) : std::string(s); }

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool has_any(const std::string& haystack_lower, std::initializer_list<const char*> needles)
{
    for (const char* n : needles)
        if (haystack_lower.find(n) != std::string::npos)
            return true;
    return false;
}

bool is_one_of(int code, std::initializer_list<int> codes) { return std::find(codes.begin(), codes.end(), code) != codes.end(); }

// The printer's own words for "unsigned command": its MQTT reply carries reason "mqtt message verify
// failed" (seen with Bambu's Authorization Control firmware). Matched loosely, the wording is theirs.
bool mentions_verify_refusal(const std::string& text_lower)
{
    return has_any(text_lower, { "verify fail", "verification fail", "verify_fail" });
}

std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + " " + b;
}

} // namespace

BambuLanSkip bambu_lan_skip_reason(bool has_ip, bool cloud_print_only, bool has_sdcard, bool has_access_code)
{
    if (!has_ip)           return BambuLanSkip::NoIp;
    if (cloud_print_only)  return BambuLanSkip::CloudOnlyFirmware;
    if (!has_sdcard)       return BambuLanSkip::NoSdcard;
    if (!has_access_code)  return BambuLanSkip::NoAccessCode;
    return BambuLanSkip::None;
}

const char* bambu_lan_skip_tag(BambuLanSkip skip)
{
    switch (skip) {
    case BambuLanSkip::NoIp:              return "no_ip";
    case BambuLanSkip::CloudOnlyFirmware: return "low_version";
    case BambuLanSkip::NoSdcard:          return "no_sdcard";
    case BambuLanSkip::NoAccessCode:      return "no_password";
    case BambuLanSkip::None:              break;
    }
    return "";
}

bool bambu_cloud_print_supported(bool ultranet_plugin) { return !ultranet_plugin; }

bool bambu_reply_is_auth_refusal(const std::string& result, const std::string& reason)
{
    const std::string r = lower(result);
    if (r.compare(0, 4, "fail") != 0) return false; // "fail", "failed", "FAIL"
    return mentions_verify_refusal(lower(reason));
}

BambuLanFailure bambu_classify_lan_failure(int code, const std::string& detail, bool printer_refused)
{
    if (printer_refused) return BambuLanFailure::Refused;
    if (is_one_of(code, { BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE, BAMBU_NETWORK_ERR_PRINT_WR_FILE_OVER_SIZE,
                          BAMBU_NETWORK_ERR_FILE_OVER_SIZE }))
        return BambuLanFailure::FileTooLarge;
    if (is_one_of(code, { BAMBU_NETWORK_ERR_PRINT_WR_FILE_NOT_EXIST, BAMBU_NETWORK_ERR_FILE_NOT_EXIST }))
        return BambuLanFailure::FileMissing;

    // The plug-in's text says more than the code: an FTP "530 Login incorrect" and an MQTT CONNACK
    // "not authorized" both come back as a plain upload or connect failure.
    const std::string d = lower(detail);
    if (has_any(d, { "530", "login incorrect", "access code", "password", "not authori", "authenticat" }))
        return BambuLanFailure::AccessCode;
    if (mentions_verify_refusal(d))
        return BambuLanFailure::Refused;
    if (is_one_of(code, { BAMBU_NETWORK_ERR_CONNECT_FAILED, BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED }) ||
        has_any(d, { "connect", "timed out", "timeout", "unreachable", "no route", "refused" }))
        return BambuLanFailure::Connect;
    if (is_one_of(code, { BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED, BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED,
                          BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED }))
        return BambuLanFailure::Upload;
    if (is_one_of(code, { BAMBU_NETWORK_ERR_SEND_MSG_FAILED, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED }))
        return BambuLanFailure::Publish;
    return BambuLanFailure::Other;
}

std::string BambuSendFailureText::full() const { return join(join(headline, detail), where); }

BambuSendFailureText bambu_send_failure_text(const BambuSendFailure& f, bool t)
{
    BambuSendFailureText out;
    const bool           lan_ran = f.lan_skip == BambuLanSkip::None;

    const std::string dev_mode_fix = tr(t, L("Newer Bambu firmware only accepts commands from software other than Bambu's own "
                                             "when both LAN Only Mode and Developer Mode are turned on in the printer's network "
                                             "settings. Turn both on, re-enter the access code the printer then shows on the "
                                             "Device tab, and send again."));

    if (!lan_ran) {
        // The LAN route never ran. Without a cloud to fall back to, "-3120" is still the number support
        // knows this failure by, so it stays, next to the reason tag.
        out.code = f.cloud_tried ? f.cloud_code : f.skipped_code;
        const std::string tag = bambu_lan_skip_tag(f.lan_skip);
        switch (f.lan_skip) {
        case BambuLanSkip::NoSdcard:
            out.headline = Slic3r::format(tr(t, L("Not sent: the printer reports no microSD card, and printing over LAN needs one (error %1%, %2%).")), out.code, tag);
            out.detail   = tr(t, L("P1 and A1 series printers need a microSD card inserted to print over the local network. Insert "
                                   "one (or reseat it), wait until the Device tab shows the card, then send again."));
            break;
        case BambuLanSkip::NoIp:
            out.headline = Slic3r::format(tr(t, L("Not sent: the printer's IP address is unknown - it was not found on this network (error %1%, %2%).")), out.code, tag);
            out.detail   = tr(t, L("Put this computer on the same network as the printer (not a guest Wi-Fi, another VLAN or a VPN), "
                                   "and allow EdgeSlicer through the firewall so it can hear the printer's discovery broadcasts "
                                   "on UDP ports 2021 and 1990. Then open the Device tab, wait for the printer to appear, and send again."));
            break;
        case BambuLanSkip::NoAccessCode:
            out.headline = Slic3r::format(tr(t, L("Not sent: no access code has been entered for this printer (error %1%, %2%).")), out.code, tag);
            out.detail   = tr(t, L("Read the access code from the printer's screen (in its network / LAN settings), enter it on "
                                   "the Device tab, then send again."));
            break;
        case BambuLanSkip::CloudOnlyFirmware:
        default:
            out.headline = Slic3r::format(tr(t, L("Not sent: the printer's firmware reports that it only takes prints through the Bambu cloud (error %1%, %2%).")), out.code, tag);
            out.detail   = tr(t, L("Update the printer's firmware, or turn on LAN Only Mode on the printer, then send again."));
            break;
        }
        if (f.cloud_bound && !f.cloud_supported)
            out.detail = join(tr(t, L("EdgeSlicer sends to Bambu printers over the local network only - its network plug-in has no "
                                      "Bambu cloud printing - and the local-network send was not attempted.")), out.detail);
        else if (f.cloud_tried)
            out.detail = join(out.detail, Slic3r::format(tr(t, L("Sending through the Bambu cloud instead failed as well (error %1%).")), f.cloud_code));
    } else {
        out.code = f.lan_code;
        switch (bambu_classify_lan_failure(f.lan_code, f.lan_detail, f.printer_refused)) {
        case BambuLanFailure::Connect:
            out.headline = Slic3r::format(tr(t, L("LAN send failed: could not connect to the printer (error %1%).")), out.code);
            out.detail   = tr(t, L("Check that the printer is on and that this computer is on the same network (not a guest Wi-Fi, "
                                   "another VLAN or a VPN), and that no firewall blocks the printer's ports: MQTT 8883 and FTPS 990."));
            break;
        case BambuLanFailure::AccessCode:
            out.headline = Slic3r::format(tr(t, L("LAN send failed: the printer rejected the access code (error %1%).")), out.code);
            out.detail   = tr(t, L("The access code changes when LAN Only Mode is switched or the printer is reset. Read the current "
                                   "one from the printer's screen, re-enter it on the Device tab, then send again."));
            break;
        case BambuLanFailure::Upload:
            out.headline = Slic3r::format(tr(t, L("LAN send failed: uploading the file to the printer failed (error %1%).")), out.code);
            out.detail   = tr(t, L("Check that no firewall blocks FTPS (port 990) to the printer, that the access code is current, "
                                   "and that the microSD card has free space and is not damaged or write-protected."));
            break;
        case BambuLanFailure::Publish:
            out.headline = Slic3r::format(tr(t, L("LAN send failed: the file was uploaded, but the printer did not take the print command (error %1%).")), out.code);
            out.detail   = join(tr(t, L("The command goes over MQTT (port 8883). If the printer is connected on the Device tab, "
                                        "the most likely cause is the printer refusing it.")), dev_mode_fix);
            break;
        case BambuLanFailure::Refused:
            out.headline = Slic3r::format(tr(t, L("The printer refused the print command from EdgeSlicer (error %1%).")), out.code);
            out.detail   = dev_mode_fix;
            break;
        case BambuLanFailure::FileTooLarge:
            out.headline = Slic3r::format(tr(t, L("The print file exceeds the maximum allowable size (1GB) (error %1%).")), out.code);
            out.detail   = tr(t, L("Simplify the model and slice again."));
            break;
        case BambuLanFailure::FileMissing:
            out.headline = Slic3r::format(tr(t, L("Print file not found (error %1%).")), out.code);
            out.detail   = tr(t, L("Slice again, then send."));
            break;
        case BambuLanFailure::Other:
            out.headline = Slic3r::format(tr(t, L("LAN send failed (error %1%).")), out.code);
            out.detail   = f.lan_detail.empty() ? std::string() :
                                                  Slic3r::format(tr(t, L("The network plug-in reported: %1%")), f.lan_detail);
            break;
        }
        if (f.cloud_bound && !f.upload_only && !f.cloud_supported)
            out.detail = join(out.detail, tr(t, L("(There is no Bambu cloud fallback: EdgeSlicer's network plug-in prints over the local network only.)")));
        else if (f.cloud_tried)
            out.detail = join(out.detail, Slic3r::format(tr(t, L("Sending through the Bambu cloud instead failed as well (error %1%).")), f.cloud_code));
    }

    if (lan_ran && f.ultranet_log)
        out.where = Slic3r::format(tr(t, L("Details: ultranet_print.log and the lines starting with \"%1%\" in the EdgeSlicer log, in %2%")),
                                   f.log_prefix, f.log_dir);
    else
        out.where = Slic3r::format(tr(t, L("Details: the lines starting with \"%1%\" in the EdgeSlicer log, in %2%")), f.log_prefix, f.log_dir);
    return out;
}

std::string bambu_log_dir_for_display(const std::string& data_dir, const std::string& appdata)
{
    // Windows paths: neither case nor slash direction tells two of them apart.
    auto same_path = [](std::string a, std::string b) {
        std::replace(a.begin(), a.end(), '/', '\\');
        std::replace(b.begin(), b.end(), '/', '\\');
        return lower(a) == lower(b);
    };
    if (!appdata.empty() && data_dir.size() >= appdata.size() &&
        same_path(data_dir.substr(0, appdata.size()), appdata) &&
        (data_dir.size() == appdata.size() || data_dir[appdata.size()] == '\\' || data_dir[appdata.size()] == '/')) {
        std::string rest = data_dir.substr(appdata.size());
        std::replace(rest.begin(), rest.end(), '/', '\\');
        return "%APPDATA%" + rest + "\\log";
    }
    const char sep = data_dir.find('\\') != std::string::npos ? '\\' : '/';
    if (!data_dir.empty() && (data_dir.back() == '\\' || data_dir.back() == '/'))
        return data_dir + "log";
    return data_dir + sep + "log";
}

std::string bambu_log_dir_for_display()
{
    std::string appdata;
#ifdef _WIN32
    wxString env;
    if (wxGetEnv("APPDATA", &env))
        appdata = env.ToUTF8().data();
#endif
    return bambu_log_dir_for_display(data_dir(), appdata);
}

}} // namespace Slic3r::GUI
