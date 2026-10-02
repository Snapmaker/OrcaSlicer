#include "PrinterTimelapse.hpp"

#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/detail/md5.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

namespace Slic3r { namespace GUI { namespace Timelapse {

using nlohmann::json;

// Everything file-local lives in this namespace rather than at file scope: the GUI library is a
// unity build, and a static helper here must not meet a same-named one from a neighbour.
namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string extension(const std::string& name)
{
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot));
}

std::string stem(const std::string& name)
{
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool is_picture_name(const std::string& name)
{
    const std::string ext = extension(name);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char) s[a])) ++a;
    while (b > a && std::isspace((unsigned char) s[b - 1])) --b;
    return s.substr(a, b - a);
}

// A non-negative decimal that fits 64 bits, the whole string. Digits only: "+1", " 1", "1e3" are not.
bool parse_u64(const std::string& s, std::uint64_t& out)
{
    if (s.empty() || s.size() > 19) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (std::uint64_t) (c - '0');
    }
    out = v;
    return true;
}

std::uint64_t json_u64(const json& j, const char* key)
{
    if (!j.contains(key)) return 0;
    const json& v = j[key];
    if (v.is_number_unsigned()) return v.get<std::uint64_t>();
    if (v.is_number_integer()) return (std::uint64_t) std::max<std::int64_t>(0, v.get<std::int64_t>());
    if (v.is_number_float()) return (std::uint64_t) std::max(0.0, v.get<double>());
    if (v.is_string()) { std::uint64_t u = 0; return parse_u64(v.get<std::string>(), u) ? u : 0; }
    return 0;
}

std::int64_t json_time(const json& j, const char* key)
{
    if (!j.contains(key)) return 0;
    const json& v = j[key];
    if (v.is_number()) return (std::int64_t) v.get<double>();
    if (v.is_string()) { std::uint64_t u = 0; return parse_u64(v.get<std::string>(), u) ? (std::int64_t) u : 0; }
    return 0;
}

std::string json_str(const json& j, const char* key)
{
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void pause_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The library's log lines (it calls this from its own threads). They can carry the printer's
// address and, in a URL, its access code, so they only ever go to the debug log - and are freed
// through the library that allocated them.
struct LoggerContext { const BambuLib* lib; };
void tunnel_log(void* context, int level, tchar const* msg)
{
    auto* c = static_cast<LoggerContext*>(context);
    if (msg) {
#ifdef _WIN32
        std::wstring w(msg);
        std::string  s;
        s.reserve(w.size());
        for (wchar_t ch : w) s += (ch >= 32 && ch < 127) ? (char) ch : '?';
#else
        std::string s(msg);
#endif
        BOOST_LOG_TRIVIAL(debug) << "Timelapse tunnel [" << level << "]: " << s;
    }
    if (c && c->lib && c->lib->Bambu_FreeLogMsg && msg) c->lib->Bambu_FreeLogMsg(msg);
}
LoggerContext& logger_context(const BambuLib& lib)
{
    // One per library table, for the lifetime of the process (the tunnel may log after close).
    static LoggerContext ctx { nullptr };
    ctx.lib = &lib;
    return ctx;
}

} // namespace

// ---- files ------------------------------------------------------------------------------------

bool valid_name(const std::string& name)
{
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == '#' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    // A trailing dot or space is a different name to Windows than to the printer.
    return name.back() != '.' && name.back() != ' ' && name.front() != ' ';
}

bool is_video_name(const std::string& name)
{
    const std::string ext = extension(name);
    return ext == ".mp4" || ext == ".avi" || ext == ".mkv" || ext == ".mov" || ext == ".webm" || ext == ".m4v";
}

std::string mime_for(const std::string& name)
{
    const std::string ext = extension(name);
    if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
    if (ext == ".avi") return "video/x-msvideo";
    if (ext == ".mkv") return "video/x-matroska";
    if (ext == ".mov") return "video/quicktime";
    if (ext == ".webm") return "video/webm";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    return "application/octet-stream";
}

std::vector<File> parse_moonraker_list(const json& answer)
{
    const json* arr = &answer;
    if (answer.is_object() && answer.contains("result")) arr = &answer["result"];
    std::vector<File> out;
    if (!arr->is_array()) return out;
    // Pictures by stem first, so a video finds its preview whatever order they are listed in.
    std::map<std::string, std::string> pictures;
    for (const json& f : *arr) {
        if (!f.is_object()) continue;
        std::string name = json_str(f, "path");
        if (name.empty()) name = json_str(f, "filename"); // older Moonraker
        if (name.find('/') != std::string::npos || !is_picture_name(name) || !valid_name(name)) continue;
        // .jpg wins over .png when a renderer left both.
        auto it = pictures.find(stem(name));
        if (it == pictures.end() || extension(name) == ".jpg") pictures[stem(name)] = name;
    }
    for (const json& f : *arr) {
        if (!f.is_object()) continue;
        File file;
        file.name = json_str(f, "path");
        if (file.name.empty()) file.name = json_str(f, "filename");
        // Top level only: moonraker-timelapse keeps its frames in sub-folders of the same root.
        if (file.name.find('/') != std::string::npos || !is_video_name(file.name) || !valid_name(file.name)) continue;
        file.size = json_u64(f, "size");
        file.time = json_time(f, "modified");
        auto it   = pictures.find(stem(file.name));
        if (it != pictures.end()) {
            file.thumbnail     = it->second;
            file.has_thumbnail = true;
        }
        out.push_back(file);
    }
    sort_newest_first(out);
    return out;
}

std::vector<File> parse_bambu_list(const json& reply)
{
    std::vector<File> out;
    if (!reply.is_object() || !reply.contains("file_lists") || !reply["file_lists"].is_array()) return out;
    for (const json& f : reply["file_lists"]) {
        if (!f.is_object()) continue;
        File file;
        file.name = json_str(f, "name");
        file.path = json_str(f, "path");
        if (!valid_name(file.name) || !is_video_name(file.name)) continue;
        file.size          = json_u64(f, "size");
        file.time          = json_time(f, "time");
        // Every timelapse the printer records gets a preview (the Device tab asks each one for
        // "<path>#thumbnail"); whether this one really has it is only known by asking.
        file.has_thumbnail = true;
        if (f.contains("duration") && f["duration"].is_number()) file.duration_s = f["duration"].get<double>();
        out.push_back(file);
    }
    sort_newest_first(out);
    return out;
}

void sort_newest_first(std::vector<File>& files)
{
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) {
        if (a.time != b.time) return a.time > b.time;
        return a.name > b.name;
    });
}

json files_json(const std::vector<File>& files)
{
    json arr = json::array();
    for (const File& f : files) {
        json j;
        j["name"]          = f.name;
        j["size"]          = f.size;
        j["time"]          = f.time;
        j["duration_s"]    = f.duration_s >= 0 ? json(f.duration_s) : json(nullptr);
        j["mime"]          = mime_for(f.name);
        j["has_thumbnail"] = f.has_thumbnail;
        arr.push_back(j);
    }
    return arr;
}

const File* find_file(const std::vector<File>& files, const std::string& name)
{
    for (const File& f : files)
        if (f.name == name) return &f;
    return nullptr;
}

bool match_route(const std::string& path, std::string& printer, std::string& what)
{
    static const std::string head = "/printers/", tail = "/timelapses";
    if (path.compare(0, head.size(), head) != 0) return false;
    const std::string rest  = path.substr(head.size());
    const size_t      slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) return false;
    const std::string sub = rest.substr(slash);
    if (sub == tail) what = "list";
    else if (sub == tail + "/thumbnail") what = "thumbnail";
    else if (sub == tail + "/video") what = "video";
    else return false;
    printer = rest.substr(0, slash);
    return true;
}

// ---- HTTP Range -------------------------------------------------------------------------------

namespace {
// The one range in a header, before it meets a size: first < 0 = suffix form ("-n"), last < 0 =
// open end ("a-"). False when the header is not exactly one byte range.
bool split_range(const std::string& header, long long& first, long long& last, std::uint64_t& a, std::uint64_t& b)
{
    std::string h = trim(header);
    if (h.size() < 7 || lower(h.substr(0, 6)) != "bytes=") return false;
    h = trim(h.substr(6));
    if (h.find(',') != std::string::npos) return false; // several ranges: serve the whole file
    const size_t dash = h.find('-');
    if (dash == std::string::npos) return false;
    const std::string sa = trim(h.substr(0, dash)), sb = trim(h.substr(dash + 1));
    if (sa.empty() && sb.empty()) return false;
    a = b = 0;
    if (!sa.empty() && !parse_u64(sa, a)) return false;
    if (!sb.empty() && !parse_u64(sb, b)) return false;
    first = sa.empty() ? -1 : 0;
    last  = sb.empty() ? -1 : 0;
    if (first == 0 && last == 0 && b < a) return false; // "bytes=5-2" is invalid: ignore it
    return true;
}
} // namespace

ByteRange parse_range(const std::string& header, std::uint64_t total)
{
    ByteRange r;
    long long     first, last;
    std::uint64_t a, b;
    if (header.empty() || !split_range(header, first, last, a, b)) return r;
    if (first < 0) { // the last b bytes
        if (b == 0 || total == 0) { r.kind = ByteRange::Unsatisfiable; return r; }
        r.kind  = ByteRange::Satisfiable;
        r.start = b >= total ? 0 : total - b;
        r.end   = total - 1;
        return r;
    }
    if (a >= total) { r.kind = ByteRange::Unsatisfiable; return r; }
    r.kind  = ByteRange::Satisfiable;
    r.start = a;
    r.end   = (last < 0 || b >= total) ? total - 1 : b;
    return r;
}

std::string normalize_range(const std::string& header)
{
    long long     first, last;
    std::uint64_t a, b;
    if (header.empty() || !split_range(header, first, last, a, b)) return std::string();
    if (first < 0) return b == 0 ? std::string() : "bytes=-" + std::to_string(b);
    return "bytes=" + std::to_string(a) + "-" + (last < 0 ? std::string() : std::to_string(b));
}

std::string content_range(const ByteRange& r, std::uint64_t total)
{
    if (r.kind != ByteRange::Satisfiable) return "bytes */" + std::to_string(total);
    return "bytes " + std::to_string(r.start) + "-" + std::to_string(r.end) + "/" + std::to_string(total);
}

bool parse_content_range(const std::string& value, std::uint64_t& start, std::uint64_t& end, std::uint64_t& total)
{
    std::string v = trim(value);
    if (v.size() < 7 || lower(v.substr(0, 6)) != "bytes ") return false;
    v = trim(v.substr(6));
    const size_t dash = v.find('-'), slash = v.find('/');
    if (dash == std::string::npos || slash == std::string::npos || dash > slash) return false;
    return parse_u64(trim(v.substr(0, dash)), start) && parse_u64(trim(v.substr(dash + 1, slash - dash - 1)), end) &&
           parse_u64(trim(v.substr(slash + 1)), total) && start <= end && end < total;
}

std::string url_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string content_disposition(const std::string& name)
{
    std::string ascii;
    for (unsigned char c : name) ascii += (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char) c : '_';
    return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + url_encode(name);
}

// ---- an upstream HTTP answer --------------------------------------------------------------------

bool parse_base_url(const std::string& base, HttpTarget& out)
{
    std::string rest;
    if (boost::istarts_with(base, "http://")) { out.tls = false; out.port = 80; rest = base.substr(7); }
    else if (boost::istarts_with(base, "https://")) { out.tls = true; out.port = 443; rest = base.substr(8); }
    else return false;
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest.resize(slash);
    if (rest.empty()) return false;
    if (rest.front() == '[') { // [v6]:port
        const size_t close = rest.find(']');
        if (close == std::string::npos) return false;
        out.host = rest.substr(1, close - 1);
        rest     = rest.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') return false;
        if (!rest.empty()) rest = rest.substr(1); else rest.clear();
        if (!rest.empty()) { std::uint64_t p = 0; if (!parse_u64(rest, p) || p == 0 || p > 65535) return false; out.port = (int) p; }
        return !out.host.empty();
    }
    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        std::uint64_t p = 0;
        if (!parse_u64(rest.substr(colon + 1), p) || p == 0 || p > 65535) return false;
        out.port = (int) p;
        rest.resize(colon);
    }
    out.host = rest;
    return !out.host.empty();
}

bool parse_upstream_head(const std::string& head, UpstreamHead& out)
{
    out = UpstreamHead();
    std::istringstream in(head);
    std::string        line;
    if (!std::getline(in, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, 5, "HTTP/") != 0) return false;
    const size_t sp = line.find(' ');
    if (sp == std::string::npos) return false;
    out.status = std::atoi(line.c_str() + sp + 1);
    if (out.status < 100 || out.status > 599) return false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
        if (key == "content-length") out.has_length = parse_u64(value, out.content_length);
        else if (key == "content-range") out.content_range = value;
        else if (key == "content-type") out.content_type = value;
        else if (key == "transfer-encoding") out.chunked = lower(value).find("chunked") != std::string::npos;
    }
    return true;
}

// ---- the Bambu storage tunnel -------------------------------------------------------------------

Message split_sample(const unsigned char* buffer, std::size_t size)
{
    Message m;
    if (!buffer || size == 0) return m;
    // PrinterFileSystem::HandleResponse: the JSON ends at the first "\n\n"; with none, the whole
    // sample is JSON.
    const unsigned char* end      = buffer + size;
    const unsigned char* json_end = (const unsigned char*) std::memchr(buffer, '\n', size);
    while (json_end && json_end + 3 < end && json_end[1] != '\n')
        json_end = (const unsigned char*) std::memchr(json_end + 2, '\n', end - json_end - 2);
    if (json_end) json_end = std::min(json_end + 2, end);
    else json_end = end;
    try {
        const json root = json::parse(std::string((const char*) buffer, json_end - buffer));
        if (!root.is_object()) return m;
        if (root.contains("result") && !root["result"].is_null()) {
            m.is_reply = true;
            m.result   = root["result"].get<int>();
            m.sequence = root.value("sequence", -1);
            m.body     = root.contains("reply") ? root["reply"] : json::object();
        } else {
            m.cmdtype  = root.value("cmdtype", 0);
            m.sequence = root.value("sequence", -1);
            m.body     = root.contains("notify") ? root["notify"] : json::object();
        }
        if (m.body.is_null()) m.body = json::object();
        m.ok = true;
    } catch (...) {
        return Message();
    }
    m.data = json_end;
    m.size = (std::size_t) (end - json_end);
    return m;
}

std::string request_text(int cmdtype, std::uint32_t sequence, const json& req)
{
    json root;
    root["cmdtype"]  = cmdtype;
    root["sequence"] = sequence;
    root["req"]      = req;
    return root.dump();
}

struct Md5::Impl { boost::uuids::detail::md5 md5; };
Md5::Md5() : m(new Impl) {}
Md5::~Md5() = default;
void Md5::update(const void* data, std::size_t size) { if (size) m->md5.process_bytes(data, size); }
std::string Md5::hex()
{
    // Exactly PrinterFileSystem::DownloadNextFile's digest, which is what the printer's
    // "file_md5" was checked against there.
    boost::uuids::detail::md5::digest_type digest;
    m->md5.get_digest(digest);
    for (int i = 0; i < 4; ++i) digest[i] = boost::endian::endian_reverse(digest[i]);
    std::string out;
    const auto  bytes = reinterpret_cast<const char*>(&digest[0]);
    boost::algorithm::hex(bytes, bytes + sizeof(digest), std::back_inserter(out));
    return out;
}

TunnelSession::TunnelSession(const BambuLib& lib) : m_lib(lib) {}
TunnelSession::~TunnelSession() { close(); }

void TunnelSession::close()
{
    if (!m_tunnel) return;
    Bambu_Tunnel t = m_tunnel;
    m_tunnel       = nullptr;
    if (m_lib.Bambu_Close) m_lib.Bambu_Close(t);
    if (m_lib.Bambu_Destroy) m_lib.Bambu_Destroy(t);
}

int TunnelSession::open(const std::string& url, int timeout_ms)
{
    close();
    if (!m_lib.Bambu_Create || !m_lib.Bambu_Open) return -2;
    Bambu_Tunnel t   = nullptr;
    int          ret = m_lib.Bambu_Create(&t, url.c_str());
    if (ret == 0 && t && m_lib.Bambu_SetLogger) m_lib.Bambu_SetLogger(t, tunnel_log, &logger_context(m_lib));
    if (ret == 0) ret = m_lib.Bambu_Open(t);
    if (ret == 0) {
        const long long deadline = now_ms() + timeout_ms;
        for (;;) {
            ret = m_lib.Bambu_StartStreamEx ? m_lib.Bambu_StartStreamEx(t, CTRL_TYPE) :
                  m_lib.Bambu_StartStream   ? m_lib.Bambu_StartStream(t, false) : -2;
            if (ret != Bambu_would_block) break;
            if (now_ms() > deadline) { ret = TUNNEL_TIMEOUT; break; }
            pause_ms(10);
        }
    }
    if (ret == 0) {
        m_tunnel   = t;
        m_sequence = 0;
        return 0;
    }
    if (t) {
        if (m_lib.Bambu_Close) m_lib.Bambu_Close(t);
        if (m_lib.Bambu_Destroy) m_lib.Bambu_Destroy(t);
    }
    // PrinterFileSystem::Reconnect: 1 from StartStream = the printer has no conversation to spare.
    return ret == 1 ? ERROR_RES_BUSY : ret;
}

int TunnelSession::send(const std::string& text, int timeout_ms)
{
    const long long deadline = now_ms() + timeout_ms;
    for (;;) {
        const int n = m_lib.Bambu_SendMessage(m_tunnel, CTRL_TYPE, text.c_str(), (int) text.size());
        if (n == 0) return 0;
        if (n != Bambu_would_block) return ERROR_PIPE;
        if (now_ms() > deadline) return TUNNEL_TIMEOUT;
        pause_ms(10);
    }
}

int TunnelSession::request(int cmdtype, const json& req, const ReplyFn& on_reply, int idle_timeout_ms)
{
    if (!m_tunnel || !m_lib.Bambu_SendMessage || !m_lib.Bambu_ReadSample) return ERROR_PIPE;
    const std::uint32_t seq = m_sequence++;
    int                 res = send(request_text(cmdtype, seq, req), idle_timeout_ms);
    if (res != 0) { close(); return res; }
    long long deadline = now_ms() + idle_timeout_ms;
    for (;;) {
        Bambu_Sample sample;
        std::memset(&sample, 0, sizeof(sample));
        const int n = m_lib.Bambu_ReadSample(m_tunnel, &sample);
        if (n == Bambu_would_block) {
            if (now_ms() > deadline) { close(); return TUNNEL_TIMEOUT; }
            pause_ms(5);
            continue;
        }
        if (n != 0) { close(); return ERROR_PIPE; } // stream end or a broken tunnel
        deadline = now_ms() + idle_timeout_ms;
        const Message m = split_sample(sample.buffer, (std::size_t) std::max(0, sample.size));
        if (!m.ok || !m.is_reply || m.sequence != (int) seq) continue; // a notification, or not ours
        if (on_reply && !on_reply(m)) { close(); return TUNNEL_STOPPED; }
        if (m.result != CONTINUE) return m.result;
    }
}

int list_timelapses(TunnelSession& s, std::vector<File>& out, const std::string& storage)
{
    json req;
    req["type"]        = "timelapse";
    req["api_version"] = 2;
    if (!storage.empty()) req["storage"] = storage;
    out.clear();
    return s.request(LIST_INFO, req, [&out](const Message& m) {
        if (m.result == SUCCESS || m.result == CONTINUE) {
            std::vector<File> part = parse_bambu_list(m.body);
            out.insert(out.end(), part.begin(), part.end());
        }
        return true;
    });
}

int fetch_thumbnails(TunnelSession& s, const std::vector<File>& files, std::map<std::string, std::string>& out)
{
    if (files.empty()) return SUCCESS;
    // PrinterFileSystem::UpdateFocusThumbnail2: by path ("<path>#thumbnail") when the printer
    // gave paths, by name ("files") on the old firmware that did not.
    const bool by_path = std::all_of(files.begin(), files.end(), [](const File& f) { return !f.path.empty(); });
    json       arr     = json::array();
    for (const File& f : files) arr.push_back(by_path ? f.path + "#thumbnail" : f.name);
    json req;
    req[by_path ? "paths" : "files"] = arr;
    std::map<std::string, std::string> partial; // a picture split over several replies ("continue")
    auto owner = [&](const Message& m) -> const File* {
        const std::string path = json_str(m.body, "path");
        if (!path.empty()) {
            const std::string base = path.substr(0, path.find_last_of('#'));
            for (const File& f : files)
                if (!f.path.empty() && f.path == base) return &f;
        }
        // Old firmware names the picture after the video ("<stem>.jpg" for "<stem>.mp4").
        const std::string pic = json_str(m.body, "thumbnail");
        for (const File& f : files)
            if (!pic.empty() && (stem(f.name) == stem(pic) || f.name == pic)) return &f;
        return files.size() == 1 ? &files.front() : nullptr;
    };
    return s.request(SUB_FILE, req, [&](const Message& m) {
        if (m.result != SUCCESS && m.result != CONTINUE) return true; // the request's own end
        const std::size_t size = (std::size_t) json_u64(m.body, "size");
        const File*       f    = owner(m);
        if (!f || size == 0 || m.size < size) return true; // no picture for this one: skip it
        std::string& buf = partial[f->name];
        buf.append((const char*) m.data, size);
        if (!m.body.value("continue", false)) {
            out[f->name] = std::move(buf);
            partial.erase(f->name);
        }
        return true;
    });
}

int download(TunnelSession& s, const File& file, const ChunkFn& on_chunk)
{
    json req;
    if (file.path.empty()) req["file"] = file.name;
    else req["path"] = file.path;
    Md5           md5;
    std::uint64_t got = 0, total = 0;
    std::string   want_md5;
    int           verdict = SUCCESS;
    const int     res     = s.request(FILE_DOWNLOAD, req, [&](const Message& m) {
        if (m.result != SUCCESS && m.result != CONTINUE) return true;
        const std::uint64_t offset = json_u64(m.body, "offset");
        const std::size_t   size   = (std::size_t) std::min<std::uint64_t>(json_u64(m.body, "size"), m.size);
        total                      = json_u64(m.body, "total");
        if (offset != got) { verdict = FILE_SIZE_ERR; return false; } // a gap or a repeat: not a file we can trust
        md5.update(m.data, size);
        got += size;
        if (m.result == SUCCESS) want_md5 = json_str(m.body, "file_md5");
        return on_chunk ? on_chunk(offset, total, m.data, size) : true;
    }, 30000);
    if (verdict != SUCCESS) return verdict;
    if (res != SUCCESS) return res;
    if (got != total) return FILE_SIZE_ERR;
    if (!want_md5.empty() && !boost::iequals(md5.hex(), want_md5)) return FILE_CHECK_ERR;
    return SUCCESS;
}

Failure tunnel_failure(int result)
{
    switch (result) {
    case -2:
        return { 501, "no_tunnel", "Browsing this printer's storage needs the storage component of the PC's network plug-in, which is not installed" };
    case ERROR_RES_BUSY:
        return { 409, "busy", "The printer's storage is busy with another connection (the PC's Device tab storage page, or another download); try again shortly" };
    case FILE_NO_EXIST:
        return { 404, "no_file", "That file is no longer on the printer" };
    case STORAGE_UNAVAILABLE:
        return { 409, "no_storage", "The printer's storage is unavailable; check that its SD card is inserted" };
    case FILE_TYPE_ERR:
        return { 501, "unsupported", "The printer's firmware does not support browsing timelapses" };
    case FILE_CHECK_ERR:
    case FILE_SIZE_ERR:
        return { 502, "printer_error", "The file arrived damaged from the printer; try again" };
    case TUNNEL_TIMEOUT:
        return { 504, "printer_error", "The printer did not answer in time" };
    default:
        return { 502, "printer_error", "Could not read the printer's storage (error " + std::to_string(result) + ")" };
    }
}

}}} // namespace Slic3r::GUI::Timelapse
