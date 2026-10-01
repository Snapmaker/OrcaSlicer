#ifndef slic3r_UntrustedInput_hpp_
#define slic3r_UntrustedInput_hpp_

// Rules for input that comes from outside the user's own machine: links a web page or another
// program hands us (edgeslicer://open?file=..., the other slicers' "Open in" schemes), URLs a web
// page asks us to open, files we download for such a link, entry names inside archives, and the
// settings a project or preset file carries.
//
// Everything here is plain C++ (no wx, no curl) so the rules can be unit tested on their own.
// The GUI decides what to do with a verdict (ask, refuse, strip); this file only decides.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

class DynamicPrintConfig;

namespace untrusted {

// ---- URLs ------------------------------------------------------------------------------------

struct Url
{
    std::string scheme;        // lower case, without ':'
    std::string host;          // lower case; IPv6 literals keep their brackets
    int         port = -1;     // -1 when absent
    std::string path;          // from the first '/' up to '?' or '#', "" when absent
    std::string query;         // without '?'
    bool        has_userinfo = false;
};

// Strict parser for absolute "scheme://authority[/path][?query][#fragment]" URLs. Refuses
// whitespace, control characters, backslashes, an empty host, a malformed port and anything
// that is not ASCII in the authority (an IDN must arrive punycoded). Userinfo ("a@b") is
// parsed so that callers can refuse it: "https://printables.com@evil.tld/" is evil.tld.
bool parse_url(const std::string &url, Url &out);

// host == domain, or host ends with "." + domain. Both compared case-insensitively.
// "printables.com.evil.tld" and "evilprintables.com" do not match "printables.com".
bool host_is_or_under(const std::string &host, const std::string &domain);

// IPv4 dotted quad (also the short and hex forms browsers accept), IPv6 literal, or a
// localhost / *.localhost / *.local / *.lan / *.home.arpa / *.internal name.
bool is_local_or_ip_host(const std::string &host);

// Page links on the model sites (the model page, not the file). http or https, no userinfo.
bool is_printables_link(const std::string &url);
bool is_makerworld_link(const std::string &url);
bool is_thingiverse_link(const std::string &url);

// MakerWorld's file hosts: the site itself and its CDN (bblmw.com). A file URL on one of these
// goes through the MakerWorld import path (Plater::import_model_id).
bool is_makerworld_file_url(const std::string &url);

// What a web page may ask us to open in the system browser (common_openurl and friends):
// http(s) with a host, or mailto:. Never file:, javascript:, a custom scheme, a bare or UNC
// path, or anything with whitespace or control characters.
bool is_safe_to_open_externally(const std::string &url);

// ---- pages in our own web views -----------------------------------------------------------------

// http://127.0.0.1:<port>/... or http://localhost:<port>/... - our local page server.
bool is_page_server_url(const std::string &url, int port);

// "file:///C:/dir/a%20b.html" -> "C:/dir/a b.html" (POSIX: "/dir/a b.html"). Returns "" for
// anything else, including "file://host/share" (UNC) forms.
std::string local_path_from_file_url(const std::string &url);

// Files a page of ours may ask the OS to open with their default application (attachments of a
// project: manuals, pictures, models). Anything else (programs, scripts, shortcuts, macro-capable
// documents, unknown types) is shown in its folder instead.
bool is_safe_attachment_to_launch(const std::string &file_name);

// ---- "Open in" links -----------------------------------------------------------------------

struct OpenLink
{
    bool        ok = false;
    std::string scheme;     // lower case: edgeslicer, orcaslicer, prusaslicer, bambustudio, bambustudioopen, cura, ...
    std::string file_url;   // percent-decoded URL of the file to download
    std::string name;       // percent-decoded "&name=" value, "" when absent
    std::string error;      // why ok == false
};

// Parses "<scheme>://open[/]?file=<url-encoded URL>[&name=<name>]" for the schemes this app
// accepts, and "bambustudioopen://<url-encoded URL>". Only splits and decodes; whether the file
// URL may be fetched is check_model_download()'s decision.
OpenLink parse_open_link(const std::string &link);

// Percent-decoding ("+" is left alone). Invalid escapes are kept verbatim.
std::string percent_decode(const std::string &s);

// ---- model downloads -------------------------------------------------------------------------

enum class DownloadVerdict { Allow, Ask, Refuse };

struct DownloadCheck
{
    DownloadVerdict verdict = DownloadVerdict::Refuse;
    std::string     host;     // lower case, "" when the URL did not parse
    std::string     reason;   // for the log and for the message shown to the user
};

// Decides whether the file URL of an "Open in" link may be downloaded.
//   Refuse: not https, userinfo, unparsable, an IP address or a local name (LAN / localhost),
//           or a file name that is not a model type.
//   Allow:  https on a host of a model site we know (see download_allowlist()).
//   Ask:    https on any other public host - the caller asks the user, default "No".
// link_scheme widens the list for the one scheme whose CDN is generic storage:
// bambustudio / bambustudioopen may also fetch from *.amazonaws.com and *.aliyuncs.com
// (MakerWorld's signed download links), as Bambu Studio itself allows.
DownloadCheck check_model_download(const std::string &file_url, const std::string &link_scheme = std::string());

// The domains (each matching itself and its subdomains) check_model_download() allows.
const std::vector<std::string> &download_allowlist();

// Model files a download may be saved as and opened: .3mf .stl .step .stp .obj .zip.
bool has_model_extension(const std::string &file_name);

// Turns a name taken from a link, a URL path or a Content-Disposition header into a safe plain
// file name for the download folder: only the last path component survives, reserved and
// control characters become '_', leading/trailing dots and spaces go, "." / ".." and Windows
// device names (CON, NUL, COM1, ...) are defused, and the length is capped (the extension kept).
// Returns "" when nothing usable is left. Never returns a name with a path separator.
std::string sanitize_download_filename(const std::string &name);

// Upper bound for a model download (same cap as the MakerWorld import path).
constexpr std::size_t MODEL_DOWNLOAD_SIZE_LIMIT = std::size_t(500) * 1024 * 1024;

// Checks that the first bytes of a downloaded file match its extension: 3MF/ZIP start with
// "PK\3\4", a binary STL's size agrees with its triangle count (or it is ASCII "solid"),
// STEP starts with "ISO-10303-21", OBJ is text. *why gets a short reason on failure.
bool content_matches_extension(const std::string &file_name, const std::string &head, unsigned long long file_size, std::string *why = nullptr);

// ---- archive entries ---------------------------------------------------------------------------

// True for a relative path made of plain segments separated by '/': no '\\', no drive or ':' ,
// no leading '/', no "." or ".." segment, no empty segment, no control characters. Used before
// an archive entry name becomes part of a path on disk (zip-slip).
bool is_safe_archive_relative_path(const std::string &path);

enum class ArchiveEntryName {
    Reject, // unsafe: refuse the entry (the confined extractor refuses the whole archive)
    Skip,   // nothing to extract: a bare "./" or "." directory entry
    Ok      // `out` holds the normalised name
};

// The one place an archive entry name is cleaned up and then judged. Harmless spellings that
// common zip tools produce are normalised first: backslash separators (PowerShell 5.1
// Compress-Archive, some .NET zippers), a leading "./" or "./" segments (bsdtar), repeated
// separators ("a//b") and a trailing separator. The result is then held to is_safe_archive_relative_path
// as strictly as ever. Rejected: ".." segments anywhere, an absolute path ("/x", "\x", UNC
// "\\server\share", "\\?\C:\x"), a drive letter or any ':' (C:x, alternate data streams),
// control characters / NUL, look-alikes of '.' '/' '\' ':', segments made only of dots and
// spaces, and names over 1024 bytes. `out` is only written for Ok.
ArchiveEntryName normalize_archive_entry_path(const std::string &raw, std::string &out);

// The last segment of a name returned by normalize_archive_entry_path (the whole name when it
// has no '/'). For extractors that flatten entries to their file name.
std::string archive_entry_leaf(const std::string &normalized);

// True if candidate stays under root after weakly_canonical. Rejects an embedded NUL in either
// path. A trailing separator on root is ignored. Compared component-wise so a sibling that
// shares a prefix (/tmp/root2 vs /tmp/root) is not accepted. A symlink-to-dir root is followed
// (extraction into that directory is allowed). A symlink at the candidate's last component is
// not followed, so a dest-file symlink can be replaced rather than written through.
bool is_path_within_root(const boost::filesystem::path &root, const boost::filesystem::path &candidate);

// expat's XML_GetBuffer / XML_ParseBuffer take an int length. An archive entry larger than
// INT_MAX cannot be handed to those APIs without truncating the size (Orca #15958).
inline bool xml_entry_size_ok(std::uint64_t uncomp_size)
{
    return uncomp_size <= static_cast<std::uint64_t>(std::numeric_limits<int>::max());
}

// ---- settings in project / preset files ----------------------------------------------------------

// Options a file we did not write may not set silently:
//   post_process      runs programs on this computer when G-code is exported (for Bambu Lab
//                     printers already when a plate is sliced);
//   filename_format   the output name; flagged when it would leave the output folder.
enum class SettingRisk { RunsPrograms, WritesOutsideOutputFolder, NetworkEndpoint };

struct UntrustedSetting
{
    std::string key;
    std::string source;   // "project settings" or the embedded preset's name
    std::string value;    // what the file carries (post_process lines joined by '\n')
    SettingRisk risk;
};

// post_process lines joined by '\n', blank lines dropped, each line trimmed.
std::string joined_post_process(const DynamicPrintConfig &cfg);

// True when the text of a filename_format template (outside {...} and [...] placeholders)
// names a directory: a path separator, a drive, or a ".." segment.
bool filename_format_leaves_folder(const std::string &format);

// Keys that point the app at a network endpoint or carry credentials for one.
const std::vector<std::string> &network_endpoint_keys();

struct TrustedValues
{
    std::set<std::string> post_process;     // joined_post_process() of the user's own presets
    std::set<std::string> filename_format;  // filename_format of the user's own presets
};

// Lists what cfg carries that the user has not already got in one of their own presets.
// An empty post_process, a filename_format that stays in the folder and empty network keys are
// never listed. Network keys are listed whenever they are set (callers strip them silently).
std::vector<UntrustedSetting> find_untrusted_settings(const DynamicPrintConfig &cfg, const std::string &source,
                                                      const TrustedValues &trusted);

// Replaces every key listed in `found` (for this source) with baseline's value, or with the
// option's default when baseline is null or lacks the key. Returns how many keys changed.
std::size_t neutralize_settings(DynamicPrintConfig &cfg, const std::vector<UntrustedSetting> &found,
                                const DynamicPrintConfig *baseline);

} // namespace untrusted
} // namespace Slic3r

#endif // slic3r_UntrustedInput_hpp_
