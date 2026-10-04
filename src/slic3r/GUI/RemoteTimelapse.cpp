#include "RemoteTimelapse.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "PrinterTimelapse.hpp"
#include "RemoteAccess.hpp"
#include "RemoteControl.hpp"
#include "SnapmakerLan.hpp"
#include "Printer/PrinterFileSystem.h"
#include "libslic3r/libslic3r.h"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

extern "C" BambuLib* bambulib_get(); // Printer/PrinterFileSystem.cpp: the loaded BambuSource's table

namespace Slic3r { namespace GUI { namespace RemoteTimelapse {

// File-local, but in a named namespace: the GUI library is a unity build (see PrinterTimelapse.cpp).
namespace detail {

namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using tcp      = asio::ip::tcp;
using nlohmann::json;
using namespace Slic3r::GUI::Timelapse;

// How long a list stays good for the routes that only need a file's path / preview name; the list
// route itself always asks the printer again.
constexpr long long LIST_TTL_MS        = 5 * 60 * 1000;
// A Bambu printer allows few storage conversations, so this process opens one at a time per
// printer: a list waits this long for a running one before it answers from its last list.
constexpr int       LIST_WAIT_S        = 4;
constexpr int       THUMB_WAIT_S       = 25;
constexpr int       DOWNLOAD_WAIT_S    = 90;
constexpr size_t    THUMB_BATCH        = 12;
constexpr size_t    THUMB_CACHE_BYTES  = 32u << 20;
constexpr std::uint64_t CACHE_CAP_BYTES = 4ull << 30; // the Bambu copies of one process
constexpr int       STALL_S            = 90;          // no new byte from the printer for this long: give up

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string error_body(const std::string& text, const std::string& code)
{
    json j;
    j["error"] = text;
    j["code"]  = code;
    return j.dump();
}

Answer fail(int status, const std::string& code, const std::string& text)
{
    Answer a;
    a.status = status;
    a.body   = error_body(text, code);
    return a;
}

Answer fail(const Failure& f) { return fail(f.status, f.code, f.text); }

const char* reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 416: return "Range Not Satisfiable";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return status < 400 ? "OK" : "Internal Server Error";
    }
}

std::string head_of(int status, const std::string& type, const std::string& length_header, const std::string& extra)
{
    return "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n" +
           "Content-Type: " + type + "\r\n" + length_header +
           "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n" + extra + "Connection: close\r\n\r\n";
}

void write_answer(tcp::socket& client, const Answer& a)
{
    const std::string text = head_of(a.status, a.type, "Content-Length: " + std::to_string(a.body.size()) + "\r\n", a.headers) + a.body;
    boost::system::error_code ec;
    asio::write(client, asio::buffer(text), ec);
}

bool write_bytes(tcp::socket& client, const char* data, size_t n)
{
    boost::system::error_code ec;
    asio::write(client, asio::buffer(data, n), ec);
    return !ec;
}

void set_recv_timeout(tcp::socket& s, int seconds)
{
#ifdef _WIN32
    DWORD ms = (DWORD) seconds * 1000;
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*) &ms, sizeof(ms));
#else
    struct timeval tv { seconds, 0 };
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

std::string picture_type(const std::string& bytes)
{
    if (bytes.size() >= 4 && (unsigned char) bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') return "image/png";
    return "image/jpeg";
}

// ---- which printer ------------------------------------------------------------------------------

struct Target
{
    enum Kind { Moonraker, Bambu } kind { Moonraker };
    std::string id, name, kind_name;
    std::string base; // Moonraker: http://<address>
    std::string url;  // Bambu: the tunnel URL. Carries the access code: never logged.
};

json printer_json(const Target& t)
{
    return json{ { "id", t.id }, { "name", t.name }, { "kind", t.kind_name } };
}

bool on_main(std::function<void()> fn, int timeout_ms = 10000)
{
    return RemoteAccess::call_on_main(std::move(fn), timeout_ms) == MainCallResult::Done;
}

MachineObject* find_machine(DeviceManager* dm, const std::string& id)
{
    std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
    for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
    for (const auto& kv : all)
        if (kv.second && kv.second->dev_id == id) return kv.second;
    return nullptr;
}

// What resolving a printer id found: the target, or the answer that says why not.
bool resolve(const std::string& printer, Target& t, Answer& why)
{
    t    = Target();
    t.id = printer;
    if (printer.empty() || printer.size() > 200) { why = fail(404, "no_printer", "no such printer"); return false; }

    if (printer.compare(0, 3, "sm:") == 0) {
        SnapmakerLan::Device d;
        if (!SnapmakerLan::find(printer.substr(3), d)) { why = fail(404, "no_printer", "no such printer: " + printer); return false; }
        t.kind      = Target::Moonraker;
        t.kind_name = "snapmaker";
        t.name      = d.name.empty() ? d.ip : d.name;
        t.base      = SnapmakerLan::base_url(d);
        SnapmakerLan::Status s;
        if (SnapmakerLan::cached_status(d, s)) {
            if (!s.online) { why = fail(409, "offline", t.name + " is offline"); return false; }
            if (s.login_required) { why = fail(501, "unsupported", t.name + " requires a login for its LAN API, so its files cannot be read from here"); return false; }
        }
        return true;
    }

    if (printer == "host" || printer == "connect" || printer.compare(0, 3, "ph:") == 0) {
        auto targets = std::make_shared<std::vector<RemoteControl::HostTarget>>();
        if (!on_main([targets]() { try { RemoteControl::list_host_targets(*targets); } catch (...) {} })) {
            why = fail(503, "busy", "the slicer is busy");
            return false;
        }
        for (const RemoteControl::HostTarget& h : *targets) {
            if (h.id != printer) continue;
            if (!h.host_type.empty()) {
                why = fail(501, "unsupported", "this printer keeps no timelapses EdgeSlicer can read (only Moonraker printers and Bambu printers do)");
                return false;
            }
            t.kind      = Target::Moonraker;
            t.kind_name = printer == "connect" ? "connect" : "printhost";
            t.base      = h.base;
            HttpTarget ht;
            t.name      = parse_base_url(h.base, ht) ? ht.host : h.base;
            return true;
        }
        why = fail(404, "no_printer", "no such printer: " + printer);
        return false;
    }

    // A Bambu printer, by its dev_id. Everything about it lives on the GUI thread.
    struct Info
    {
        bool        found { false }, connected { false }, sd { false }, busy { false }, lib { false };
        int         local { 0 }, remote { 0 };
        std::string ip, code, ver, name, uuid;
    };
    auto info = std::make_shared<Info>();
    if (!on_main([info, printer]() {
            info->lib = PrinterFileSystem::HasTunnelLibrary();
            DeviceManager* dm = wxGetApp().getDeviceManager();
            MachineObject* m  = dm ? find_machine(dm, printer) : nullptr;
            if (!m) return;
            info->found     = true;
            info->name      = m->dev_name;
            info->connected = m->is_connected();
            info->ip        = m->dev_ip;
            info->code      = m->get_access_code();
            info->ver       = m->get_ota_version();
            info->local     = (int) m->file_local;
            info->remote    = m->get_file_remote();
            info->sd        = m->sdcard_state == MachineObject::HAS_SDCARD_NORMAL || m->sdcard_state == MachineObject::HAS_SDCARD_READONLY;
            info->busy      = m->is_camera_busy_off();
            if (wxGetApp().app_config) info->uuid = wxGetApp().app_config->get("slicer_uuid");
        })) {
        why = fail(503, "busy", "the slicer is busy");
        return false;
    }
    if (!info->found) { why = fail(404, "no_printer", "no such printer: " + printer); return false; }
    t.kind      = Target::Bambu;
    t.kind_name = "bambu";
    t.name      = info->name.empty() ? printer : info->name;
    if (!info->lib) { why = fail(tunnel_failure(-2)); return false; }
    if (!info->connected) { why = fail(409, "offline", t.name + " is not connected to this PC"); return false; }
    if (info->ip.empty() || info->code.empty()) {
        why = fail(409, "no_lan", "Reading " + t.name + "'s storage works over the local network: add the printer by its IP address and access code on the PC");
        return false;
    }
    if (!info->local && !info->remote) { why = fail(501, "unsupported", "The printer's firmware does not support browsing its storage; update the printer"); return false; }
    if (!info->sd) { why = fail(409, "no_storage", t.name + " reports no usable SD card"); return false; }
    if (info->busy) { why = fail(409, "busy", t.name + " is busy downloading; try again after it finishes"); return false; }
    // MediaFilePanel::fetchUrl's LAN branch, with the FTPS user named.
    t.url = "bambu:///local/" + info->ip + ".?port=6000&user=bblp&passwd=" + info->code + "&device=" + printer +
            "&net_ver=" + NetworkAgent::get_version() + "&dev_ver=" + info->ver + "&cli_id=" + info->uuid +
            "&cli_ver=" + std::string(SLIC3R_VERSION);
    return true;
}

// ---- Moonraker ----------------------------------------------------------------------------------

struct MoonList
{
    std::vector<File> files;
    long long         when { 0 };
};
std::mutex                      s_moon_mutex;
std::map<std::string, MoonList> s_moon; // by printer id

// GET a small thing from the printer. status 0 = no answer at all.
bool moon_get(const std::string& url, std::string& body, unsigned& status, std::string& error, size_t limit, int timeout_s)
{
    bool ok = false;
    status  = 0;
    Http::get(url)
        .tls_policy(Http::TlsPolicy::PrintHost) // printer: keep accepting self-signed certificates
        .timeout_connect(std::min(timeout_s, 5))
        .timeout_max(timeout_s)
        .size_limit(limit)
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; ok = true; })
        .on_error([&](std::string b, std::string e, unsigned s) { body = std::move(b); status = s; error = e; })
        .perform_sync();
    return ok;
}

Answer moon_list(const Target& t, std::vector<File>* out = nullptr)
{
    std::string body, error;
    unsigned    status = 0;
    json        j;
    j["printer"] = printer_json(t);
    j["source"]  = "moonraker";
    j["stale"]   = false;
    if (!moon_get(t.base + "/server/files/list?root=timelapse", body, status, error, 8u << 20, 10)) {
        if (status == 0) return fail(409, "offline", t.name + " did not answer" + (error.empty() ? std::string() : " (" + error + ")"));
        if (status == 400 || status == 404) {
            // Moonraker answers 400 "Invalid root path" when no timelapse component registered one.
            j["files"] = json::array();
            j["note"]  = t.name + " has no timelapse folder (Moonraker's \"timelapse\" root is not registered)";
            { std::lock_guard<std::mutex> lock(s_moon_mutex); s_moon[t.id] = MoonList{ {}, now_ms() }; }
            if (out) out->clear();
            Answer a;
            a.body = j.dump();
            return a;
        }
        return fail(502, "printer_error", t.name + " answered HTTP " + std::to_string(status));
    }
    std::vector<File> files;
    try {
        files = parse_moonraker_list(json::parse(body));
    } catch (...) {
        return fail(502, "printer_error", t.name + " sent a file list that could not be read");
    }
    { std::lock_guard<std::mutex> lock(s_moon_mutex); s_moon[t.id] = MoonList{ files, now_ms() }; }
    j["files"] = files_json(files);
    if (out) *out = files;
    Answer a;
    a.body = j.dump();
    return a;
}

// The file as the last list saw it, listing again when that is old or does not have it.
bool moon_file(const Target& t, const std::string& name, File& out, Answer& why)
{
    {
        std::lock_guard<std::mutex> lock(s_moon_mutex);
        auto it = s_moon.find(t.id);
        if (it != s_moon.end() && now_ms() - it->second.when < LIST_TTL_MS)
            if (const File* f = find_file(it->second.files, name)) { out = *f; return true; }
    }
    std::vector<File> files;
    Answer a = moon_list(t, &files);
    if (a.status != 200) { why = a; return false; }
    if (const File* f = find_file(files, name)) { out = *f; return true; }
    why = fail(404, "no_file", "no such timelapse on " + t.name + ": " + name);
    return false;
}

Answer moon_thumbnail(const Target& t, const std::string& name)
{
    File   f;
    Answer why;
    if (!moon_file(t, name, f, why)) return why;
    if (!f.has_thumbnail) return fail(404, "no_thumbnail", "no preview picture for " + name);
    std::string body, error;
    unsigned    status = 0;
    if (!moon_get(t.base + "/server/files/timelapse/" + url_encode(f.thumbnail), body, status, error, 8u << 20, 10)) {
        if (status == 404) return fail(404, "no_thumbnail", "no preview picture for " + name);
        return fail(status == 0 ? 409 : 502, status == 0 ? "offline" : "printer_error", t.name + " did not send the preview picture");
    }
    Answer a;
    a.type = mime_for(f.thumbnail);
    a.body = std::move(body);
    return a;
}

// The video, proxied: the phone's Range goes to the printer, and the printer's bytes come back
// one buffer at a time. A printer that ignores Range is answered for it (206 from the full body).
void moon_video(tcp::socket& client, const Target& t, const std::string& name, const std::string& range, bool download)
{
    HttpTarget ht;
    if (!parse_base_url(t.base, ht)) { write_answer(client, fail(502, "printer_error", "this printer has no usable address")); return; }
    if (ht.tls) { write_answer(client, fail(501, "unsupported", "streaming a video from an https print host is not supported")); return; }

    asio::io_context ioc;
    tcp::socket      up(ioc);
    {
        boost::system::error_code ec;
        tcp::resolver             res(ioc);
        auto                      eps = res.resolve(ht.host, std::to_string(ht.port), ec);
        if (ec) { write_answer(client, fail(409, "offline", t.name + " could not be found on the network")); return; }
        boost::system::error_code cec = asio::error::would_block;
        asio::async_connect(up, eps, [&cec](const boost::system::error_code& e, const tcp::endpoint&) { cec = e; });
        ioc.run_for(std::chrono::seconds(6));
        if (cec) {
            boost::system::error_code ig;
            up.close(ig);
            write_answer(client, fail(409, "offline", t.name + " did not answer"));
            return;
        }
    }
    set_recv_timeout(up, 60);
    const std::string fwd  = normalize_range(range);
    const std::string host = (ht.host.find(':') != std::string::npos ? "[" + ht.host + "]" : ht.host) +
                             (ht.port != 80 ? ":" + std::to_string(ht.port) : std::string());
    const std::string req  = "GET /server/files/timelapse/" + url_encode(name) + " HTTP/1.1\r\nHost: " + host +
                            "\r\nUser-Agent: EdgeSlicer\r\nAccept: */*\r\nConnection: close\r\n" +
                            (fwd.empty() ? std::string() : "Range: " + fwd + "\r\n") + "\r\n";
    boost::system::error_code ec;
    asio::write(up, asio::buffer(req), ec);
    asio::streambuf headbuf(64 * 1024);
    if (!ec) asio::read_until(up, headbuf, "\r\n\r\n", ec);
    if (ec) { write_answer(client, fail(502, "printer_error", t.name + " did not answer the video request")); return; }
    std::string raw(asio::buffers_begin(headbuf.data()), asio::buffers_end(headbuf.data()));
    const size_t he = raw.find("\r\n\r\n") + 4;
    std::string  pending = raw.substr(he); // body bytes already read
    raw.resize(he);
    UpstreamHead uh;
    if (!parse_upstream_head(raw, uh)) { write_answer(client, fail(502, "printer_error", t.name + " sent an unreadable answer")); return; }

    if (uh.status == 404) { write_answer(client, fail(404, "no_file", "no such timelapse on " + t.name + ": " + name)); return; }
    const std::string disposition = download ? "Content-Disposition: " + content_disposition(name) + "\r\n" : std::string();
    const std::string type        = mime_for(name);
    if (uh.status == 416) {
        Answer a  = fail(416, "bad_range", "that range is not in the file");
        a.headers = uh.content_range.empty() ? std::string() : "Content-Range: " + uh.content_range + "\r\n";
        write_answer(client, a);
        return;
    }
    if (uh.status != 200 && uh.status != 206) {
        write_answer(client, fail(502, "printer_error", t.name + " answered HTTP " + std::to_string(uh.status)));
        return;
    }

    // What to send: `skip` body bytes dropped first, then `length` bytes (unknown = to the end).
    std::uint64_t skip = 0, length = 0;
    bool          known = uh.has_length && !uh.chunked;
    std::string   head;
    if (uh.status == 206) {
        std::uint64_t a = 0, b = 0, total = 0;
        if (!parse_content_range(uh.content_range, a, b, total)) {
            write_answer(client, fail(502, "printer_error", t.name + " sent a partial answer without a usable Content-Range"));
            return;
        }
        length = b - a + 1;
        known  = true;
        head   = head_of(206, type, "Content-Length: " + std::to_string(length) + "\r\n",
                         "Accept-Ranges: bytes\r\nContent-Range: " + uh.content_range + "\r\n" + disposition);
    } else if (known) {
        const ByteRange r = parse_range(range, uh.content_length);
        if (r.kind == ByteRange::Unsatisfiable) {
            Answer a  = fail(416, "bad_range", "that range is not in the file");
            a.headers = "Content-Range: " + content_range(r, uh.content_length) + "\r\n";
            write_answer(client, a);
            return;
        }
        if (r.kind == ByteRange::Satisfiable) {
            skip   = r.start;
            length = r.length();
            head   = head_of(206, type, "Content-Length: " + std::to_string(length) + "\r\n",
                             "Accept-Ranges: bytes\r\nContent-Range: " + content_range(r, uh.content_length) + "\r\n" + disposition);
        } else {
            length = uh.content_length;
            head   = head_of(200, type, "Content-Length: " + std::to_string(length) + "\r\n", "Accept-Ranges: bytes\r\n" + disposition);
        }
    } else {
        // No length, or chunked: pass the body through exactly as it comes (a chunked body keeps
        // its framing, so the phone's client decodes it). No Range can be answered for it.
        head = head_of(200, type, uh.chunked ? "Transfer-Encoding: chunked\r\n" : std::string(), disposition);
    }
    if (!write_bytes(client, head.data(), head.size())) return;

    std::uint64_t sent = 0;
    auto emit = [&](const char* p, size_t n) -> bool {
        if (skip) {
            const size_t drop = (size_t) std::min<std::uint64_t>(skip, n);
            skip -= drop;
            p += drop;
            n -= drop;
        }
        if (known) n = (size_t) std::min<std::uint64_t>(n, length - sent);
        if (n == 0) return true;
        sent += n;
        return write_bytes(client, p, n);
    };
    if (!pending.empty() && !emit(pending.data(), pending.size())) return;
    std::vector<char> buf(256 * 1024);
    while (!known || sent < length) {
        const size_t n = up.read_some(asio::buffer(buf.data(), buf.size()), ec);
        if (ec || n == 0) break;
        if (!emit(buf.data(), n)) break; // the phone went away
    }
    boost::system::error_code ig;
    up.shutdown(tcp::socket::shutdown_both, ig);
}

// ---- Bambu --------------------------------------------------------------------------------------

const BambuLib& lib() { return *bambulib_get(); }

struct BambuPrinter
{
    std::timed_mutex                   session; // one storage conversation at a time
    std::mutex                         m;       // the rest
    std::vector<File>                  files;
    long long                          listed { 0 };
    std::map<std::string, std::string> thumbs;
    size_t                             thumb_bytes { 0 };
};
std::mutex                                           s_bambu_mutex;
std::map<std::string, std::shared_ptr<BambuPrinter>> s_bambu;

std::shared_ptr<BambuPrinter> bambu_printer(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_bambu_mutex);
    auto& p = s_bambu[id];
    if (!p) p = std::make_shared<BambuPrinter>();
    return p;
}

// List over an open-able session; the caller holds p->session.
int bambu_list_locked(const Target& t, BambuPrinter& p, TunnelSession& s)
{
    if (!s.is_open()) {
        const int r = s.open(t.url);
        if (r != 0) return r;
    }
    std::vector<File> files;
    const int r = list_timelapses(s, files);
    if (r != SUCCESS) return r;
    std::lock_guard<std::mutex> lock(p.m);
    p.files  = std::move(files);
    p.listed = now_ms();
    // Pictures of files that went away are dropped with them.
    for (auto it = p.thumbs.begin(); it != p.thumbs.end();) {
        if (find_file(p.files, it->first)) ++it;
        else { p.thumb_bytes -= it->second.size(); it = p.thumbs.erase(it); }
    }
    return SUCCESS;
}

Answer bambu_list_answer(const Target& t, BambuPrinter& p, bool stale)
{
    json j;
    j["printer"] = printer_json(t);
    j["source"]  = "bambu_storage";
    j["stale"]   = stale;
    {
        std::lock_guard<std::mutex> lock(p.m);
        j["files"] = files_json(p.files);
    }
    Answer a;
    a.body = j.dump();
    return a;
}

Answer bambu_list(const Target& t)
{
    auto                              p = bambu_printer(t.id);
    std::unique_lock<std::timed_mutex> lock(p->session, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(LIST_WAIT_S))) {
        // A download (or another list) holds the printer's one conversation: say what we last saw.
        bool listed;
        { std::lock_guard<std::mutex> l(p->m); listed = p->listed != 0; }
        if (listed) return bambu_list_answer(t, *p, true);
        return fail(409, "busy", t.name + "'s storage is busy (a video is being copied from it); try again shortly");
    }
    TunnelSession s(lib());
    const int     r = bambu_list_locked(t, *p, s);
    if (r != SUCCESS) return fail(tunnel_failure(r));
    return bambu_list_answer(t, *p, false);
}

// The file as the last list saw it; lists (holding the session) when there is none or it is old.
bool bambu_file(const Target& t, BambuPrinter& p, const std::string& name, File& out, Answer& why, int wait_s)
{
    {
        std::lock_guard<std::mutex> l(p.m);
        if (p.listed && now_ms() - p.listed < LIST_TTL_MS)
            if (const File* f = find_file(p.files, name)) { out = *f; return true; }
    }
    std::unique_lock<std::timed_mutex> lock(p.session, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(wait_s))) {
        why = fail(409, "busy", t.name + "'s storage is busy; try again shortly");
        return false;
    }
    TunnelSession s(lib());
    const int     r = bambu_list_locked(t, p, s);
    if (r != SUCCESS) { why = fail(tunnel_failure(r)); return false; }
    std::lock_guard<std::mutex> l(p.m);
    if (const File* f = find_file(p.files, name)) { out = *f; return true; }
    why = fail(404, "no_file", "no such timelapse on " + t.name + ": " + name);
    return false;
}

Answer bambu_thumbnail(const Target& t, const std::string& name)
{
    auto p = bambu_printer(t.id);
    auto cached = [&](Answer& a) {
        std::lock_guard<std::mutex> l(p->m);
        auto it = p->thumbs.find(name);
        if (it == p->thumbs.end()) return false;
        a.type = picture_type(it->second);
        a.body = it->second;
        return true;
    };
    Answer a;
    if (cached(a)) return a;
    std::unique_lock<std::timed_mutex> lock(p->session, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(THUMB_WAIT_S))) return fail(409, "busy", t.name + "'s storage is busy; try again shortly");
    if (cached(a)) return a; // another request fetched it while this one waited
    TunnelSession s(lib());
    bool          have_list;
    { std::lock_guard<std::mutex> l(p->m); have_list = p->listed && find_file(p->files, name); }
    if (!have_list) {
        const int r = bambu_list_locked(t, *p, s);
        if (r != SUCCESS) return fail(tunnel_failure(r));
    }
    // This one and the next few without a picture yet, in list order: a phone asks for every
    // visible row at once, and one conversation answers them all.
    std::vector<File> batch;
    {
        std::lock_guard<std::mutex> l(p->m);
        const File* want = find_file(p->files, name);
        if (!want) return fail(404, "no_file", "no such timelapse on " + t.name + ": " + name);
        batch.push_back(*want);
        bool after = false;
        for (const File& f : p->files) {
            if (f.name == name) { after = true; continue; }
            if (after && batch.size() < THUMB_BATCH && !p->thumbs.count(f.name)) batch.push_back(f);
        }
    }
    if (!s.is_open()) {
        const int r = s.open(t.url);
        if (r != 0) return fail(tunnel_failure(r));
    }
    std::map<std::string, std::string> got;
    const int r = fetch_thumbnails(s, batch, got);
    {
        std::lock_guard<std::mutex> l(p->m);
        for (auto& kv : got) {
            if (p->thumb_bytes + kv.second.size() > THUMB_CACHE_BYTES) { p->thumbs.clear(); p->thumb_bytes = 0; }
            p->thumb_bytes += kv.second.size();
            p->thumbs[kv.first] = std::move(kv.second);
        }
    }
    if (cached(a)) return a;
    if (r != SUCCESS && r != FILE_NO_EXIST) return fail(tunnel_failure(r));
    return fail(404, "no_thumbnail", "no preview picture for " + name);
}

// ---- the Bambu disk copy ------------------------------------------------------------------------

struct CacheFile
{
    std::mutex              m;
    std::condition_variable cv;
    fs::path                path;
    std::uint64_t           total { 0 }, have { 0 };
    bool                    started { false }, done { false }, failed { false };
    Failure                 failure;
    int                     readers { 0 };
};
std::mutex                                        s_cache_mutex;
std::map<std::string, std::shared_ptr<CacheFile>> s_cache; // "<printer>\n<name>"

fs::path cache_root()
{
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = (int) getpid();
#endif
    return fs::temp_directory_path() / "EdgeSlicer" / "timelapses" / std::to_string(pid);
}

std::string safe_segment(const std::string& id)
{
    std::string out;
    for (unsigned char c : id) out += (std::isalnum(c) || c == '-' || c == '_') ? (char) c : '_';
    return out.empty() ? std::string("printer") : out;
}

// Room for one more copy: oldest finished copies nobody is reading go first.
void evict(std::uint64_t incoming)
{
    std::vector<std::pair<std::time_t, fs::path>> done;
    std::uint64_t                                 used = 0;
    boost::system::error_code                     ec;
    const fs::path                                root = cache_root();
    if (!fs::exists(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!fs::is_regular_file(it->path(), ec) || it->path().extension() == ".ok") continue;
        used += fs::file_size(it->path(), ec);
        if (fs::exists(it->path().string() + ".ok", ec)) done.emplace_back(fs::last_write_time(it->path(), ec), it->path());
    }
    std::sort(done.begin(), done.end());
    for (const auto& d : done) {
        if (used + incoming <= CACHE_CAP_BYTES) break;
        bool busy = false;
        for (const auto& kv : s_cache) // the caller holds s_cache_mutex
            if (kv.second->path == d.second) { std::lock_guard<std::mutex> l(kv.second->m); busy = kv.second->readers > 0 || !kv.second->done; }
        if (busy) continue;
        const std::uint64_t size = fs::file_size(d.second, ec);
        fs::remove(d.second.string() + ".ok", ec);
        if (fs::remove(d.second, ec)) used -= std::min(used, size);
        for (auto it = s_cache.begin(); it != s_cache.end();)
            if (it->second->path == d.second) it = s_cache.erase(it); else ++it;
    }
}

void copy_from_printer(Target t, File f, std::shared_ptr<CacheFile> e)
{
    auto finish = [&](int result) {
        {
            std::lock_guard<std::mutex> l(e->m);
            if (result == SUCCESS) e->done = true;
            else { e->failed = true; e->failure = tunnel_failure(result); }
        }
        e->cv.notify_all();
        if (result == SUCCESS) {
            boost::nowide::ofstream ok(e->path.string() + ".ok", std::ios::binary | std::ios::trunc);
            ok << f.size;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "RemoteTimelapse: copying a timelapse from " << t.id << " failed: " << result;
            std::lock_guard<std::mutex> l(s_cache_mutex);
            auto it = s_cache.find(t.id + "\n" + f.name);
            if (it != s_cache.end() && it->second == e) s_cache.erase(it); // the next request starts over
        }
    };
    auto p = bambu_printer(t.id);
    std::unique_lock<std::timed_mutex> lock(p->session, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(DOWNLOAD_WAIT_S))) { finish(ERROR_RES_BUSY); return; }
    boost::system::error_code ec;
    fs::create_directories(e->path.parent_path(), ec);
    boost::nowide::ofstream out(e->path.string(), std::ios::binary | std::ios::trunc);
    if (!out) { finish(FILE_OPEN_ERR); return; }
    TunnelSession s(lib());
    int r = s.open(t.url);
    if (r == 0)
        r = download(s, f, [&](std::uint64_t offset, std::uint64_t total, const unsigned char* data, std::size_t n) {
            out.write((const char*) data, (std::streamsize) n);
            out.flush(); // a reader may follow right behind
            if (!out) return false;
            {
                std::lock_guard<std::mutex> l(e->m);
                e->total = total;
                e->have  = offset + n;
            }
            e->cv.notify_all();
            return true;
        });
    out.close();
    if (r != SUCCESS) fs::remove(e->path, ec); // may stay while a reader has it open; overwritten next time
    finish(r);
}

// The folders an earlier run left behind when it did not get to shutdown() (a crash, a kill):
// anything under timelapses/ that is not this process's and was not touched for two days.
void sweep_old_runs()
{
    static std::once_flag once;
    std::call_once(once, [] {
        boost::system::error_code ec;
        const fs::path            mine = cache_root(), parent = mine.parent_path();
        const std::time_t         cutoff = std::time(nullptr) - 2 * 24 * 3600;
        for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
            boost::system::error_code e2;
            if (it->path() == mine || !fs::is_directory(it->path(), e2)) continue;
            if (fs::last_write_time(it->path(), e2) < cutoff && !e2) fs::remove_all(it->path(), e2);
        }
    });
}

std::shared_ptr<CacheFile> cache_entry(const Target& t, const File& f)
{
    sweep_old_runs();
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    const std::string key = t.id + "\n" + f.name;
    auto              it  = s_cache.find(key);
    if (it != s_cache.end()) return it->second;
    auto e  = std::make_shared<CacheFile>();
    e->path = cache_root() / safe_segment(t.id) / f.name;
    boost::system::error_code ec;
    // A finished copy from earlier in this process run (its entry was evicted from the map only).
    if (fs::exists(e->path.string() + ".ok", ec) && fs::file_size(e->path, ec) == f.size && !ec) {
        e->started = e->done = true;
        e->total = e->have = f.size;
    } else {
        evict(f.size);
        e->started = true;
        std::thread(copy_from_printer, t, f, e).detach();
    }
    s_cache[key] = e;
    return e;
}

void serve_copy(tcp::socket& client, std::shared_ptr<CacheFile> e, const std::string& name, const std::string& range, bool download)
{
    struct Reader { CacheFile& e; Reader(CacheFile& c) : e(c) { std::lock_guard<std::mutex> l(e.m); ++e.readers; }
                    ~Reader() { std::lock_guard<std::mutex> l(e.m); --e.readers; } } reader(*e);
    std::uint64_t total = 0;
    {
        std::unique_lock<std::mutex> l(e->m);
        if (!e->cv.wait_for(l, std::chrono::seconds(STALL_S), [&] { return e->total > 0 || e->done || e->failed; })) {
            l.unlock();
            write_answer(client, fail(504, "printer_error", "the printer did not start sending the video"));
            return;
        }
        if (e->failed) {
            const Failure f = e->failure;
            l.unlock();
            write_answer(client, fail(f));
            return;
        }
        total = e->total;
    }
    const ByteRange r = parse_range(range, total);
    if (r.kind == ByteRange::Unsatisfiable) {
        Answer a  = fail(416, "bad_range", "that range is not in the file");
        a.headers = "Content-Range: " + content_range(r, total) + "\r\n";
        write_answer(client, a);
        return;
    }
    const std::uint64_t start  = r.kind == ByteRange::Satisfiable ? r.start : 0;
    const std::uint64_t length = r.kind == ByteRange::Satisfiable ? r.length() : total;
    const std::string   extra  = "Accept-Ranges: bytes\r\n" +
                              (r.kind == ByteRange::Satisfiable ? "Content-Range: " + content_range(r, total) + "\r\n" : std::string()) +
                              (download ? "Content-Disposition: " + content_disposition(name) + "\r\n" : std::string());
    const std::string head = head_of(r.kind == ByteRange::Satisfiable ? 206 : 200, mime_for(name),
                                     "Content-Length: " + std::to_string(length) + "\r\n", extra);
    if (!write_bytes(client, head.data(), head.size())) return;

    boost::nowide::ifstream in;
    std::vector<char>       buf(256 * 1024);
    std::uint64_t           pos = start, left = length;
    while (left > 0) {
        std::uint64_t avail = 0;
        {
            std::unique_lock<std::mutex> l(e->m);
            if (!e->cv.wait_for(l, std::chrono::seconds(STALL_S), [&] { return e->have > pos || e->failed || e->done; })) return;
            if (e->have <= pos) return; // failed (or a short file): the phone sees a short body and retries
            avail = e->have - pos;
        }
        if (!in.is_open()) {
            in.open(e->path.string(), std::ios::binary);
            if (!in) return;
        }
        const std::size_t want = (std::size_t) std::min<std::uint64_t>({ avail, left, (std::uint64_t) buf.size() });
        in.clear();
        in.seekg((std::streamoff) pos);
        in.read(buf.data(), (std::streamsize) want);
        const std::size_t got = (std::size_t) in.gcount();
        if (got == 0) return;
        if (!write_bytes(client, buf.data(), got)) return; // the phone went away (a seek closes the old request)
        pos += got;
        left -= got;
    }
}

void bambu_video(tcp::socket& client, const Target& t, const std::string& name, const std::string& range, bool download)
{
    // A copy already being made or made: no need to look the file up on the printer again.
    std::shared_ptr<CacheFile> e;
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto it = s_cache.find(t.id + "\n" + name);
        if (it != s_cache.end()) e = it->second;
    }
    if (!e) {
        auto   p = bambu_printer(t.id);
        File   f;
        Answer why;
        if (!bambu_file(t, *p, name, f, why, LIST_WAIT_S)) { write_answer(client, why); return; }
        e = cache_entry(t, f);
    }
    serve_copy(client, e, name, range, download);
}

} // namespace detail

using namespace detail;

Answer list(const std::string& printer)
{
    Target t;
    Answer why;
    if (!resolve(printer, t, why)) return why;
    return t.kind == Target::Bambu ? bambu_list(t) : moon_list(t);
}

Answer thumbnail(const std::string& printer, const std::string& name)
{
    if (!valid_name(name)) return fail(400, "bad_name", "name must be a file name the list gave");
    Target t;
    Answer why;
    if (!resolve(printer, t, why)) return why;
    return t.kind == Target::Bambu ? bambu_thumbnail(t, name) : moon_thumbnail(t, name);
}

void video(tcp::socket& client, const std::string& printer, const std::string& name, const std::string& range, bool download)
{
    try {
        if (!valid_name(name) || !is_video_name(name)) { write_answer(client, fail(400, "bad_name", "name must be a video file name the list gave")); return; }
        Target t;
        Answer why;
        if (!resolve(printer, t, why)) { write_answer(client, why); return; }
        if (t.kind == Target::Bambu) bambu_video(client, t, name, range, download);
        else moon_video(client, t, name, range, download);
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(warning) << "RemoteTimelapse: video request ended: " << ex.what();
    }
}

void shutdown()
{
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        s_cache.clear();
    }
    boost::system::error_code ec;
    fs::remove_all(cache_root(), ec); // best effort: a file still open stays until the next start
}

}}} // namespace Slic3r::GUI::RemoteTimelapse
