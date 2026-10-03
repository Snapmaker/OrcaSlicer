#ifndef slic3r_GUI_PrinterTimelapse_hpp_
#define slic3r_GUI_PrinterTimelapse_hpp_

// A printer's timelapse videos, for the phone: what is on the printer, a preview picture per video,
// and the video itself with HTTP Range, so a phone can stream, seek and resume.
//
// This file is the part that needs neither wx, DeviceManager nor a socket, so it is unit tested
// on its own (tests/slic3rutils/printer_timelapse_tests.cpp): the listing parsers, the Range
// arithmetic, the file-name rules, and the client of the Bambu storage tunnel. RemoteTimelapse
// is the glue that finds the printer and writes the answers to the phone.
//
// Two sources:
//
//  * A Snapmaker U1 (or any Moonraker printer): the files in Moonraker's `timelapse` root,
//    GET /server/files/list?root=timelapse, and each file at /server/files/timelapse/<name>.
//    moonraker-timelapse renders a preview picture beside each video under the same stem
//    (<stem>.jpg); a video with such a picture "has a thumbnail".
//
//  * A Bambu printer: the same storage tunnel the Device tab's storage page uses (the
//    BambuSource library's Bambu_* API, BambuTunnel.h), driven here without wx. It is a stream of
//    JSON requests, each {"cmdtype", "sequence", "req"}, answered by one or more samples of
//    "<json>\n\n<binary payload>" carrying {"result", "sequence", "reply"} (PrinterFileSystem.cpp
//    is the reference). LIST_INFO lists, SUB_FILE fetches "<path>#thumbnail", FILE_DOWNLOAD
//    streams a file from offset 0 in CONTINUE chunks and ends with its MD5. There is no ranged
//    download in that protocol, which is why RemoteTimelapse keeps a disk copy of a Bambu video
//    and serves Range requests from it.

#define BAMBU_DYNAMIC
#include "Printer/BambuTunnel.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Timelapse {

// ---- files ----------------------------------------------------------------------------------

struct File
{
    std::string name;          // bare file name: the key every route takes
    std::string path;          // Bambu: the printer's own path ("" on old firmware, which is asked by name)
    std::string thumbnail;     // Moonraker: the preview picture's name in the same root ("" = none)
    std::uint64_t size { 0 };
    std::int64_t  time { 0 };  // unix seconds; 0 = unknown
    double        duration_s { -1 }; // < 0 = unknown
    bool          has_thumbnail { false };
};

// A name the routes accept: a bare file name of 1..255 bytes - no directory separator, no '#'
// (the Bambu tunnel's sub-path separator), no control character, not "." or "..". Everything a
// phone sends is checked against this before it reaches a printer or the disk cache.
bool        valid_name(const std::string& name);
// .mp4 / .avi / .mkv / .mov / .webm / .m4v, case-insensitive.
bool        is_video_name(const std::string& name);
// The Content-Type a phone's player expects for that name; application/octet-stream otherwise.
std::string mime_for(const std::string& name);

// Moonraker's GET /server/files/list?root=timelapse answer ({"result": [...]}, or the bare array).
// Top-level videos only; each one's thumbnail is the picture with the same stem, when listed.
std::vector<File> parse_moonraker_list(const nlohmann::json& answer);
// A Bambu LIST_INFO reply ({"file_lists": [{"name", "path", "time", "size"}]}); videos only.
std::vector<File> parse_bambu_list(const nlohmann::json& reply);
// Newest first, then by name, so the order is stable for files with the same time.
void              sort_newest_first(std::vector<File>& files);
// The `files` array the list route answers with (name, size, time, duration_s, mime, has_thumbnail).
nlohmann::json    files_json(const std::vector<File>& files);
// The file with that name, or nullptr.
const File*       find_file(const std::vector<File>& files, const std::string& name);

// The instance API path after "/api": "/printers/{id}/timelapses" (what = "list"),
// ".../timelapses/thumbnail" ("thumbnail") or ".../timelapses/video" ("video"). `printer` is the id
// segment as it arrived, still percent-encoded. False for every other path.
bool match_route(const std::string& path, std::string& printer, std::string& what);

// ---- HTTP Range -----------------------------------------------------------------------------

struct ByteRange
{
    enum Kind { None, Satisfiable, Unsatisfiable };
    Kind          kind { None };
    std::uint64_t start { 0 };
    std::uint64_t end { 0 }; // inclusive
    std::uint64_t length() const { return kind == Satisfiable ? end - start + 1 : 0; }
};

// One byte range out of a Range header, against a file of `total` bytes: "bytes=a-b", "bytes=a-"
// or "bytes=-n" (the last n). Anything else - several ranges, another unit, garbage - is None:
// RFC 9110 lets a server ignore such a header and send the whole file, which every player handles.
// A start at or past the end, or a suffix of zero, is Unsatisfiable (416).
ByteRange   parse_range(const std::string& header, std::uint64_t total);
// The same header in the canonical form, for forwarding to a printer that is asked before the
// size is known: "" when it is not one syntactically valid byte range.
std::string normalize_range(const std::string& header);
// "bytes a-b/total", or "bytes */total" for an unsatisfiable one.
std::string content_range(const ByteRange& r, std::uint64_t total);
// "bytes a-b/total" out of an upstream answer. False when it is not one.
bool        parse_content_range(const std::string& value, std::uint64_t& start, std::uint64_t& end, std::uint64_t& total);
// attachment; filename="<ascii fallback>"; filename*=UTF-8''<percent-encoded name>
std::string content_disposition(const std::string& name);
// RFC 3986 unreserved characters kept, everything else %XX (a path segment, a query value).
std::string url_encode(const std::string& s);

// ---- an upstream HTTP answer (the Moonraker proxy) ---------------------------------------------

struct HttpTarget
{
    std::string host;
    int         port { 80 };
    bool        tls { false };
};
// "http://host[:port][/...]" (or https://). False for anything else.
bool parse_base_url(const std::string& base, HttpTarget& out);

struct UpstreamHead
{
    int           status { 0 };
    bool          has_length { false };
    std::uint64_t content_length { 0 };
    std::string   content_range;
    std::string   content_type;
    bool          chunked { false };
};
// The status line and the headers this proxy cares about, out of a response head (up to and
// excluding the blank line). False when the status line is not HTTP.
bool parse_upstream_head(const std::string& head, UpstreamHead& out);

// ---- the Bambu storage tunnel -----------------------------------------------------------------

// PrinterFileSystem's command and result numbers (PrinterFileSystem.h), plus two of ours.
enum Command { LIST_INFO = 0x0001, SUB_FILE = 0x0002, FILE_DEL = 0x0003, FILE_DOWNLOAD = 0x0004 };
enum Result {
    SUCCESS = 0, CONTINUE = 1, ERROR_JSON = 2, ERROR_PIPE = 3, ERROR_CANCEL = 4, ERROR_RES_BUSY = 5,
    FILE_NO_EXIST = 10, FILE_NAME_INVALID = 11, FILE_SIZE_ERR = 12, FILE_OPEN_ERR = 13,
    FILE_READ_WRITE_ERR = 14, FILE_CHECK_ERR = 15, FILE_TYPE_ERR = 16, STORAGE_UNAVAILABLE = 17,
    TUNNEL_TIMEOUT = 1001,  // ours: nothing arrived in time
    TUNNEL_STOPPED = 1002,  // ours: the reader asked to stop
};
constexpr int CTRL_TYPE = 0x3001;

// One sample off the tunnel: the JSON head, and the binary payload that follows "\n\n".
struct Message
{
    bool                 ok { false };       // the head parsed as JSON
    bool                 is_reply { false }; // has "result" (else a notification, cmdtype >= 0x100)
    int                  result { 0 };
    int                  sequence { -1 };
    int                  cmdtype { 0 };
    nlohmann::json       body;               // "reply" (or "notify")
    const unsigned char* data { nullptr };
    std::size_t          size { 0 };
};
Message split_sample(const unsigned char* buffer, std::size_t size);
// The bytes PrinterFileSystem sends for one request.
std::string request_text(int cmdtype, std::uint32_t sequence, const nlohmann::json& req);

// The MD5 PrinterFileSystem compares against a download's "file_md5", in the same upper-case hex.
class Md5
{
public:
    Md5();
    ~Md5();
    void        update(const void* data, std::size_t size);
    std::string hex();
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// One conversation with a printer's storage, blocking, on the calling thread. Not thread-safe:
// one thread drives one session (the printer allows few at a time anyway, see RemoteTimelapse).
class TunnelSession
{
public:
    explicit TunnelSession(const BambuLib& lib);
    ~TunnelSession();
    TunnelSession(const TunnelSession&) = delete;
    TunnelSession& operator=(const TunnelSession&) = delete;

    // Bambu_Create + Bambu_Open + Bambu_StartStreamEx(CTRL_TYPE). 0, or the library's error
    // (-2 for the placeholder library, 1 = the printer has no conversation free -> ERROR_RES_BUSY).
    int  open(const std::string& url, int timeout_ms = 20000);
    bool is_open() const { return m_tunnel != nullptr; }
    void close();

    // Send one request and hand every reply to it to `on_reply` until the printer says it is the
    // last (result != CONTINUE). Notifications and replies to other sequences are skipped. Returns
    // the last result, TUNNEL_STOPPED when on_reply returned false (the session is then closed:
    // the printer is still sending), TUNNEL_TIMEOUT after `idle_timeout_ms` without a sample, or
    // ERROR_PIPE when the tunnel broke.
    using ReplyFn = std::function<bool(const Message&)>;
    int request(int cmdtype, const nlohmann::json& req, const ReplyFn& on_reply, int idle_timeout_ms = 20000);

private:
    int send(const std::string& text, int timeout_ms);

    const BambuLib& m_lib;
    Bambu_Tunnel    m_tunnel { nullptr };
    std::uint32_t   m_sequence { 0 };
};

// LIST_INFO type=timelapse. `storage` "" = the SD card (what the Device tab's storage page asks).
int list_timelapses(TunnelSession& s, std::vector<File>& out, const std::string& storage = std::string());
// SUB_FILE "<path>#thumbnail" (or by name on old firmware) for each of `files`, in one request.
// Pictures land in `out` keyed by the video's name; a file the printer has no picture for is
// simply missing. Returns the request's result.
int fetch_thumbnails(TunnelSession& s, const std::vector<File>& files, std::map<std::string, std::string>& out);
// FILE_DOWNLOAD, chunk by chunk. `on_chunk(offset, total, data, size)` returns false to stop.
// Checks size and MD5 at the end (FILE_SIZE_ERR / FILE_CHECK_ERR), as the Device tab does.
using ChunkFn = std::function<bool(std::uint64_t offset, std::uint64_t total, const unsigned char* data, std::size_t size)>;
int download(TunnelSession& s, const File& file, const ChunkFn& on_chunk);

// What the phone is told for a tunnel result: HTTP status, machine code, words.
struct Failure
{
    int         status { 502 };
    std::string code;
    std::string text;
};
Failure tunnel_failure(int result);

}}} // namespace Slic3r::GUI::Timelapse

#endif // slic3r_GUI_PrinterTimelapse_hpp_
