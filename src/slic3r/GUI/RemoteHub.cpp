// The hub process (see RemoteHub.hpp). The server itself is wx-free (Boost.Asio, libcurl via
// Http, nlohmann::json, the OS); only the tray icon at the bottom of this file uses wx, with its
// own minimal wxApp (never GUI_App) created by run_server().
#include "RemoteHub.hpp"

#include "BambuCamRelay.hpp"
#include "RemoteNotify.hpp"
#include "WebPush.hpp"
#include "AppPush.hpp"
#include "HubEventDedupe.hpp"
#include "HubPushOwner.hpp"
#include "GcodeArchive.hpp" // normalize_printer: the Reprint list's printer names
#include "SnapmakerLan.hpp" // the LAN cards a "connect" record may belong to
#include "HMS.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/ServerLifetime.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

// The hub identity (design_hosted_relay.md section 2) is an Ed25519 key pair, minted and kept by
// the same OpenSSL that WebPush's VAPID keys and AppPush's APNs signing already use - no new
// dependency, and EVP_PKEY_ED25519 has been in every OpenSSL we build against since 1.1.1.
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mstcpip.h>
#  include <iphlpapi.h>
#  include <netioapi.h>
#  include <windows.h>
#  pragma comment(lib, "iphlpapi.lib")
#else
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include <wx/app.h>
#include <wx/icon.h>
#include <wx/image.h>
#include <wx/init.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/taskbar.h>
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace RemoteHub {

namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using tcp      = asio::ip::tcp;
using json     = nlohmann::json;

static const int         HUB_PORT           = 13640;
// The stream player is served by the hub itself (resources/web/orca/player.html plus verbatim
// copies of go2rtc's two scripts); only its WebSocket goes on to go2rtc, with the hub's
// credentials attached. go2rtc's own API is never reachable from a browser.
static const char* const PLAYER_PAGE = "/stream.html";
static const char* const PLAYER_JS[] = { "/video-stream.js", "/video-rtc.js" };
static const char* const GO2RTC_WS   = "/api/ws";
static const size_t      MAX_API_BODY       = 64 * 1024;
static const uint64_t    MAX_UPLOAD         = 2ull * 1024 * 1024 * 1024;
static const int         IDLE_EXIT_SECONDS  = 60;
// How long shutdown() waits for connection and helper threads once it has shut their sockets.
// Past it the server is left alive for them rather than destroyed underneath them.
static const int         SHUTDOWN_DRAIN_MS  = 3000;
// Where the remote-access card sends people who have no Tailscale yet, and where the one error
// nobody can fix from this PC (tailnet-wide HTTPS certificates) is actually switched on.
static const char* const TAILSCALE_DOWNLOAD_URL  = "https://tailscale.com/download/windows";
static const char* const TAILSCALE_DNS_ADMIN_URL = "https://login.tailscale.com/admin/dns";
// Request hygiene. The head cap is generous for a browser (cookies + a long referer) and small
// enough that a dribbling client cannot grow the buffer. The connection caps leave room for a
// phone with six live camera WebSockets plus its polling, several times over; the admin listener
// only ever talks to this PC's slicer instances and its own page.
static const size_t      MAX_HEAD           = 16 * 1024;
static const int         HEAD_TIMEOUT_S     = 10;   // to read the request head
static const int         BODY_TIMEOUT_S     = 30;   // between reads of a body we buffer
static const int         UPLOAD_TIMEOUT_S   = 120;  // between reads of an upload (phones stall)
static const int         WRITE_TIMEOUT_S    = 30;   // a peer that stops reading a plain response
static const int         TUNNEL_WRITE_S     = 120;  // ... and one that stops reading a video stream
static const int         MAX_CONNECTIONS    = 128;  // main listener
static const int         MAX_ADMIN_CONNECTIONS = 64;
// Uploads (<datadir>/hub/uploads) are scratch: the hub keeps them under a total quota and drops
// the oldest, but never one younger than an hour (it may still be opening in a slicer).
static const uint64_t    UPLOAD_QUOTA       = 1ull * 1024 * 1024 * 1024;
static const long long   UPLOAD_MIN_AGE_S   = 3600;
// A slicer instance is a full wxWidgets + OpenGL process; a phone must not be able to fork the PC.
static const int         MAX_INSTANCES      = 6;
// Printer events (RemoteEvents.hpp): how many the hub keeps, and how many of those it writes back
// to disk so a hub restart does not lose the last hour of a print.
static const size_t      MAX_EVENTS         = 200;
// The last-known printer status the hub keeps for itself, so /summary and /state can answer with
// no slicer window open at all (the app's follow-up 2). The hub polls each live instance's own
// /api/printers on this cadence, merges the rows by printer id and remembers them in
// <datadir>/hub/printers.json; a row nobody has refreshed since is served with its age, marked
// stale, rather than withheld. STALE_AFTER_MS only decides the `stale` flag - the row is served
// either way, because a status from an hour ago is still what the printer was last seen doing.
static const int         PRINTERS_POLL_MS   = 10000;
static const long long   PRINTERS_STALE_MS  = 30000;
static const size_t      MAX_CACHED_PRINTERS = 64;

// ------------------------------------------------------------------ paths ----

std::string hub_dir()       { return (fs::path(data_dir()) / "hub").string(); }
std::string instances_dir() { return (fs::path(hub_dir()) / "instances").string(); }
std::string uploads_dir()   { return (fs::path(hub_dir()) / "uploads").string(); }
std::string saves_dir()     { return (fs::path(hub_dir()) / "saves").string(); }
static std::string hub_json_path()     { return (fs::path(hub_dir()) / "hub.json").string(); }
// Left by a clean quit next to where hub.json was: why the hub quit ("tray", "request", "idle"), so
// a slicer that finds no hub can say which it was. Removed again when a hub starts.
static std::string last_exit_json_path() { return (fs::path(hub_dir()) / "last_exit.json").string(); }
static std::string streams_json_path() { return (fs::path(hub_dir()) / "streams.json").string(); }
static std::string settings_json_path() { return (fs::path(hub_dir()) / "settings.json").string(); } // survives a hub quit (hub.json does not)
static std::string events_json_path()   { return (fs::path(hub_dir()) / "events.json").string(); }   // the printer-event ring, likewise
static std::string printers_json_path() { return (fs::path(hub_dir()) / "printers.json").string(); } // the last-known printer status, likewise

static void ensure_dirs()
{
    boost::system::error_code ec;
    for (const std::string& d : { hub_dir(), instances_dir(), uploads_dir(), saves_dir() })
        fs::create_directories(d, ec);
}

static std::string read_file(const std::string& path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& data)
{
    const std::string tmp = path + ".tmp";
    {
        boost::nowide::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
    }
    boost::system::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { // rename over an open file can fail on Windows: write in place instead
        boost::nowide::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        fs::remove(tmp, ec);
    }
    return true;
}

// ---------------------------------------------------------------- helpers ----

static bool is_private_v4(const asio::ip::address& a)
{
    if (!a.is_v4()) return false;
    const uint32_t v = a.to_v4().to_uint();
    return (v >> 24) == 10 || (v >> 24) == 127 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE;
}

static std::string percent_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static std::string percent_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char) s[i + 1]) && std::isxdigit((unsigned char) s[i + 2])) {
            out += (char) std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

static std::string query_param(const std::string& query, const std::string& key)
{
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string kv = query.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) return percent_decode(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return "";
}

static std::string cookie_value(const std::string& cookie_header, const std::string& name)
{
    size_t pos = 0;
    while (pos < cookie_header.size()) {
        size_t semi = cookie_header.find(';', pos);
        if (semi == std::string::npos) semi = cookie_header.size();
        std::string kv = cookie_header.substr(pos, semi - pos);
        size_t b = kv.find_first_not_of(' ');
        if (b != std::string::npos) kv = kv.substr(b);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == name) return kv.substr(eq + 1);
        pos = semi + 1;
    }
    return "";
}

static bool valid_token(const std::string& t)
{
    return t.size() >= 10 && t.size() <= 32 && t.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
}

// Compare a secret without an early exit, so a wrong value takes the same time whatever prefix
// it got right. The lengths are compared normally (they are not the secret) and an empty value
// never matches - an unset token or secret must not authenticate anybody.
static bool ct_equal(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty() || a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned char) (a[i] ^ b[i]);
    return diff == 0;
}

// The phone link's token, straight from the OS CSPRNG (std::random_device; a seeded mt19937 has
// only as much entropy as its seed). Rejection sampling keeps the 32 symbols equally likely.
static std::string random_token()
{
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789"; // no 0/o/1/l look-alikes
    const unsigned    n     = (unsigned) sizeof(alphabet) - 1;
    const unsigned    limit = 256u - (256u % n); // 256 % 32 == 0 today; correct if the alphabet changes
    std::random_device rd;
    std::string        t;
    while (t.size() < 14) {
        const unsigned v = rd() & 0xffu;
        if (v >= limit) continue;
        t += alphabet[v % n];
    }
    return t;
}

static long long now_unix() { return (long long) std::time(nullptr); }

static std::string timestamp_compact()
{
    std::time_t t = std::time(nullptr);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string to_hex(const unsigned char* p, size_t n)
{
    static const char* hx = "0123456789abcdef";
    std::string        s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += hx[p[i] >> 4]; s += hx[p[i] & 15]; }
    return s;
}


static std::string json_error(const std::string& msg)
{
    json j;
    j["error"] = msg;
    return j.dump();
}

static const char* status_text(int status)
{
    switch (status) {
    case 200: return "200 OK";                   case 202: return "202 Accepted";
    case 302: return "302 Found";                case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";            case 415: return "415 Unsupported Media Type";
    case 404: return "404 Not Found";            case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";    case 502: return "502 Bad Gateway";
    case 429: return "429 Too Many Requests";
    case 503: return "503 Service Unavailable";  default:  return "500 Internal Server Error";
    }
}

#ifdef _WIN32
// The address of the interface that owns the 0.0.0.0/0 route with the best metric (VPNs
// usually route through 0.0.0.0/1 + 128.0.0.0/1, so this stays the real LAN adapter).
static std::string default_route_ipv4_win()
{
    std::string           out;
    PMIB_IPFORWARD_TABLE2 routes = nullptr;
    if (GetIpForwardTable2(AF_INET, &routes) != NO_ERROR || !routes) return out;
    ULONG best_if = 0, best_metric = ~0UL;
    for (ULONG i = 0; i < routes->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = routes->Table[i];
        if (r.DestinationPrefix.PrefixLength != 0) continue;
        MIB_IPINTERFACE_ROW iface = {};
        iface.Family              = AF_INET;
        iface.InterfaceIndex      = r.InterfaceIndex;
        ULONG metric = r.Metric + (GetIpInterfaceEntry(&iface) == NO_ERROR ? iface.Metric : 0);
        if (metric < best_metric) { best_metric = metric; best_if = r.InterfaceIndex; }
    }
    FreeMibTable(routes);
    if (best_if == 0) return out;
    PMIB_UNICASTIPADDRESS_TABLE addrs = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &addrs) != NO_ERROR || !addrs) return out;
    for (ULONG i = 0; i < addrs->NumEntries; ++i) {
        const MIB_UNICASTIPADDRESS_ROW& a = addrs->Table[i];
        if (a.InterfaceIndex == best_if && a.DadState == IpDadStatePreferred) {
            char buf[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, (void*) &a.Address.Ipv4.sin_addr, buf, sizeof(buf))) out = buf;
            break;
        }
    }
    FreeMibTable(addrs);
    return out;
}
#endif

static std::vector<std::string> lan_ips()
{
    std::vector<std::string> out;
#ifdef _WIN32
    {
        const std::string a = default_route_ipv4_win();
        if (!a.empty()) out.push_back(a);
    }
#endif
    try {
        asio::io_context      ioc;
        asio::ip::udp::socket s(ioc);
        s.open(asio::ip::udp::v4());
        s.connect(asio::ip::udp::endpoint(asio::ip::make_address_v4("8.8.8.8"), 53)); // sends nothing
        const std::string a = s.local_endpoint().address().to_string();
        if (a != "0.0.0.0" && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    } catch (...) {}
    try {
        asio::io_context ioc;
        tcp::resolver    r(ioc);
        for (const auto& e : r.resolve(asio::ip::host_name(), "")) {
            const auto a = e.endpoint().address();
            if (a.is_v4() && !a.is_loopback()) {
                const std::string s = a.to_string();
                if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
            }
        }
    } catch (...) {}
    return out;
}

// ------------------------------------------------------------ processes ----

static bool pid_alive(long pid)
{
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
#else
    return ::kill((pid_t) pid, 0) == 0;
#endif
}

static long current_pid()
{
#ifdef _WIN32
    return (long) ::GetCurrentProcessId();
#else
    return (long) ::getpid();
#endif
}

static std::string current_exe()
{
#ifdef __linux__
    // Inside an AppImage, start the AppImage, not the unpacked binary.
    if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) return appimage;
#endif
    boost::system::error_code ec;
    return boost::dll::program_location(ec).string();
}

#ifdef _WIN32
static std::wstring widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), &w[0], n);
    return w;
}

static std::wstring quote_arg(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t       bs  = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { out.append(bs * 2 + 1, L'\\'); out += c; bs = 0; continue; }
        out.append(bs, L'\\'); bs = 0;
        out += c;
    }
    out.append(bs * 2, L'\\');
    return out + L"\"";
}
#endif

// Start a process that outlives us. `env` entries are added to the child's environment;
// `job` (Windows Job Object handle) ties the child to OUR lifetime instead.
static long spawn_process(const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env,
                          bool hide_console, void* job)
{
#ifdef _WIN32
    std::wstring cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quote_arg(widen(a));
    }
    std::vector<std::pair<std::wstring, std::wstring>> saved;
    for (const auto& kv : env) {
        wchar_t old[4096] = {};
        DWORD   n         = ::GetEnvironmentVariableW(widen(kv.first).c_str(), old, 4096);
        saved.emplace_back(widen(kv.first), n > 0 && n < 4096 ? std::wstring(old) : std::wstring());
        ::SetEnvironmentVariableW(widen(kv.first).c_str(), widen(kv.second).c_str());
    }
    STARTUPINFOW        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb                  = sizeof(si);
    DWORD flags            = CREATE_UNICODE_ENVIRONMENT | (hide_console ? CREATE_NO_WINDOW : 0);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    BOOL ok = FALSE;
    if (!job) {
        // Leave any job we are in, so the child is not killed together with us.
        std::vector<wchar_t> buf2 = buf;
        ok = ::CreateProcessW(nullptr, buf2.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
    }
    if (!ok)
        ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
    for (const auto& kv : saved)
        ::SetEnvironmentVariableW(kv.first.c_str(), kv.second.empty() ? nullptr : kv.second.c_str());
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: CreateProcess failed (" << ::GetLastError() << ") for " << args.front();
        return 0;
    }
    if (job) ::AssignProcessToJobObject((HANDLE) job, pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return (long) pi.dwProcessId;
#else
    (void) hide_console; (void) job;
    // Double fork: the grandchild is re-parented to init, so nobody has to reap it.
    int pipefd[2];
    if (::pipe(pipefd) != 0) return 0;
    pid_t child = ::fork();
    if (child < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return 0; }
    if (child == 0) {
        ::close(pipefd[0]);
        ::setsid();
        pid_t grand = ::fork();
        if (grand == 0) {
            ::close(pipefd[1]);
            for (const auto& kv : env) ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
            std::vector<char*> argv;
            for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execv(args.front().c_str(), argv.data());
            ::_exit(127);
        }
        long gp = (long) grand;
        (void) !::write(pipefd[1], &gp, sizeof(gp));
        ::_exit(0);
    }
    ::close(pipefd[1]);
    long gp = 0;
    (void) !::read(pipefd[0], &gp, sizeof(gp));
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    return gp;
#endif
}

// ------------------------------------------------------------ HTTP bits ----

// Every connection is served on its own detached thread, so a peer that stops talking must not
// be able to hold one for ever. These are blocking sockets: a receive timeout is what makes a
// stalled read fail instead of parking the thread, and 0 puts it back to "wait as long as it
// takes" for the long-lived tunnels (WebSocket, MJPEG, the U1 relay).
static void set_socket_timeout(tcp::socket& s, int option, int seconds)
{
    if (!s.is_open()) return;
#ifdef _WIN32
    DWORD ms = (DWORD) seconds * 1000;
    ::setsockopt(s.native_handle(), SOL_SOCKET, option, (const char*) &ms, sizeof(ms));
#else
    struct timeval tv { seconds, 0 };
    ::setsockopt(s.native_handle(), SOL_SOCKET, option, &tv, sizeof(tv));
#endif
}
static void set_read_timeout(tcp::socket& s, int seconds) { set_socket_timeout(s, SO_RCVTIMEO, seconds); }
// A peer that stops reading fills our send buffer; without this the write parks the thread.
static void set_write_timeout(tcp::socket& s, int seconds) { set_socket_timeout(s, SO_SNDTIMEO, seconds); }

// A tunnel has no timeout (a camera can be quiet for a while), so keep-alive probes are what
// tell us the phone went away - without them a lost Wi-Fi connection holds two threads until
// the OS default of two hours.
static void set_keepalive(tcp::socket& s, int idle_seconds)
{
    if (!s.is_open()) return;
    boost::system::error_code ig;
    s.set_option(asio::socket_base::keep_alive(true), ig);
#ifdef _WIN32
    struct tcp_keepalive ka = { 1, (ULONG) idle_seconds * 1000, 5000 };
    DWORD                out = 0;
    ::WSAIoctl(s.native_handle(), SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &out, nullptr, nullptr);
#else
    (void) idle_seconds;
#endif
}

// How many connections each listener is serving right now; the cap answers 503 beyond it so a
// flood cannot exhaust threads or handles.
struct ConnCount
{
    std::atomic<int> n { 0 };
};

class ConnGuard
{
public:
    ConnGuard(ConnCount& c, int limit) : m_c(c) { m_ok = ++m_c.n <= limit; }
    ~ConnGuard() { --m_c.n; }
    bool ok() const { return m_ok; }
private:
    ConnCount& m_c;
    bool       m_ok { false };
};

struct Request
{
    std::string method, target, path, query, head, pending, cookies, file_name;
    std::string host, secret, sec_fetch_site, content_type; // Host, X-Hub-Secret, Sec-Fetch-Site, Content-Type (lower-cased where compared)
    std::string ts_login, fwd_proto; // Tailscale-User-Login, X-Forwarded-Proto: set by Tailscale Serve, trusted from loopback only
    // Set by phase 1's relay for a stream it re-injected on loopback, so that the trust rule above
    // can tell it apart from a genuine Serve connection. Always false today; the rule is already
    // written to honour it (Testing::trusted_proxy_headers).
    bool        via_relay { false };
    size_t      content_length { 0 };
};

static bool read_request(tcp::socket& client, Request& r)
{
    // Bounded and deadlined: a client that dribbles bytes without ever finishing the head hits
    // either the buffer limit (read_until then fails) or the receive timeout, and the connection
    // is dropped instead of growing without limit.
    set_read_timeout(client, HEAD_TIMEOUT_S);
    set_write_timeout(client, WRITE_TIMEOUT_S);
    asio::streambuf req(MAX_HEAD);
    boost::system::error_code ec;
    asio::read_until(client, req, "\r\n\r\n", ec);
    if (ec) return false;
    std::string head(asio::buffers_begin(req.data()), asio::buffers_end(req.data()));
    const size_t head_end = head.find("\r\n\r\n") + 4;
    r.pending             = head.substr(head_end);
    head.resize(head_end);
    r.head = head;
    std::istringstream first(head.substr(0, head.find("\r\n")));
    std::string        version;
    first >> r.method >> r.target >> version;
    size_t pos = head.find("\r\n") + 2;
    while (pos < head_end - 2) {
        size_t      nl   = head.find("\r\n", pos);
        std::string line = head.substr(pos, nl - pos);
        size_t      colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = lower(line.substr(0, colon));
            std::string val = line.substr(colon + 1);
            size_t b = val.find_first_not_of(' ');
            val = b == std::string::npos ? "" : val.substr(b);
            if (key == "cookie") r.cookies = val;
            else if (key == "content-length") r.content_length = (size_t) std::max(0LL, std::atoll(val.c_str()));
            else if (key == "x-file-name") r.file_name = val;
            else if (key == "host") r.host = lower(val);
            else if (key == "x-hub-secret") r.secret = val;
            else if (key == "sec-fetch-site") r.sec_fetch_site = lower(val);
            else if (key == "content-type") r.content_type = lower(val);
            else if (key == "tailscale-user-login") r.ts_login = val;
            else if (key == "x-forwarded-proto") r.fwd_proto = lower(val);
        }
        pos = nl + 2;
    }
    const size_t q = r.target.find('?');
    r.path         = q == std::string::npos ? r.target : r.target.substr(0, q);
    r.query        = q == std::string::npos ? "" : r.target.substr(q + 1);
    return !r.method.empty();
}

static void write_all(tcp::socket& s, const std::string& data)
{
    boost::system::error_code ec;
    asio::write(s, asio::buffer(data), ec);
}

static void respond(tcp::socket& s, int status, const std::string& type, const std::string& body, const std::string& extra_headers = "")
{
    std::ostringstream o;
    // nosniff so a body the hub relays (an uploaded name, a printer's answer) can never be
    // re-interpreted as script; no-referrer because the phone's token is in the path and the page
    // loads printer pages in iframes.
    o << "HTTP/1.1 " << status_text(status) << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "Referrer-Policy: no-referrer\r\n"
      << extra_headers
      << "Connection: close\r\n\r\n"
      << body;
    write_all(s, o.str());
    // A request rejected before its body was read still has that body in flight. Closing now makes
    // Windows answer the unread bytes with RST, and the client sees a connection reset instead of
    // the 4xx it was sent (the gates hit this as a random status 0). Half-close and drain what is
    // left, briefly and bounded, so the response reaches the client whole.
    boost::system::error_code ig;
    s.shutdown(tcp::socket::shutdown_send, ig);
    s.non_blocking(true, ig);
    char       sink[16384];
    size_t     drained  = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (drained < (4u << 20) && std::chrono::steady_clock::now() < deadline) {
        const size_t n = s.read_some(boost::asio::buffer(sink), ig);
        if (ig == boost::asio::error::would_block) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); ig.clear(); continue; }
        if (ig || n == 0) break; // EOF: the client has closed its side
        drained += n;
    }
    s.non_blocking(false, ig);
}

static void respond_json(tcp::socket& s, int status, const std::string& body) { respond(s, status, "application/json", body); }

// A 200 the phone may keep: an archive record's preview, which never changes under its URL. The
// same headers as respond() except the cache line (private: the URL carries the phone's token).
static void respond_cacheable(tcp::socket& s, const std::string& type, const std::string& body)
{
    std::ostringstream o;
    o << "HTTP/1.1 200 OK\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: private, max-age=604800, immutable\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "Referrer-Policy: no-referrer\r\n"
      << "Connection: close\r\n\r\n"
      << body;
    write_all(s, o.str());
    boost::system::error_code ig;
    s.shutdown(tcp::socket::shutdown_send, ig);
}

// Make the upstream close after this response (unless it is a WebSocket upgrade), so the
// browser cannot reuse the spliced connection for a request that belongs to us.
static std::string force_close(const std::string& head)
{
    std::string out;
    bool        upgrade = false;
    size_t      pos     = 0;
    while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos) nl = head.size();
        std::string line = head.substr(pos, nl - pos);
        std::string key  = lower(line.substr(0, std::min<size_t>(11, line.size())));
        if (key.compare(0, 8, "upgrade:") == 0) upgrade = true;
        if (key.compare(0, 11, "connection:") != 0 && !line.empty()) out += line + "\r\n";
        pos = nl + 2;
    }
    if (upgrade) return head;
    return out + "Connection: close\r\n\r\n";
}

static void pump(tcp::socket& from, tcp::socket& to)
{
    char                      buf[16384];
    boost::system::error_code ec;
    for (;;) {
        size_t n = from.read_some(asio::buffer(buf), ec);
        if (ec) break;
        asio::write(to, asio::buffer(buf, n), ec);
        if (ec) break;
    }
    boost::system::error_code ig;
    to.shutdown(tcp::socket::shutdown_both, ig);
    from.shutdown(tcp::socket::shutdown_both, ig);
}

// Splice the client onto 127.0.0.1:<port>, replaying the (rewritten) request head first.
// Works for plain responses and WebSocket upgrades alike.
// Run a command to completion and capture what it prints (the tailscale CLI). Windows only for now.
static bool run_capture(const std::vector<std::string>& args, std::string& out, int& exit_code, int timeout_ms)
{
    out.clear();
    exit_code = -1;
#ifdef _WIN32
    std::wstring cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quote_arg(widen(a));
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return false;
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = wr;
    si.hStdError   = wr;
    PROCESS_INFORMATION  pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
    ::CloseHandle(wr);
    if (!ok) { ::CloseHandle(rd); return false; }
    std::thread reader([&]() {
        char  b[4096];
        DWORD n = 0;
        while (::ReadFile(rd, b, sizeof(b), &n, nullptr) && n > 0) out.append(b, n);
    });
    const DWORD w = ::WaitForSingleObject(pi.hProcess, (DWORD) timeout_ms);
    if (w != WAIT_OBJECT_0) ::TerminateProcess(pi.hProcess, 1);
    reader.join();
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    exit_code = (int) code;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    return w == WAIT_OBJECT_0;
#else
    (void) args; (void) timeout_ms;
    return false;
#endif
}

// ---- Tailscale (remote access): the user's own Tailscale install publishes the hub inside their
// tailnet with `tailscale serve`; the phone runs the Tailscale app signed in to the same account.
static std::string tailscale_exe()
{
    // SNORCA_TAILSCALE_EXE lets a gate point the hub at a stand-in CLI (test_hub_persist.py) so
    // that the remote-access paths can be exercised without a tailnet - and without this PC's
    // real Serve configuration being touched. Unset or empty falls through to the real locations.
    if (const char* over = std::getenv("SNORCA_TAILSCALE_EXE"); over && *over) return over;
#ifdef _WIN32
    const char*       pf = std::getenv("ProgramFiles");
    const std::string p  = std::string(pf ? pf : "C:\\Program Files") + "\\Tailscale\\tailscale.exe";
    if (fs::exists(p)) return p;
#endif
    return "tailscale"; // PATH
}

struct TailscaleState
{
    bool        installed { false };
    std::string backend;       // Running, NeedsLogin, Stopped, ...
    std::string dns_name;      // <machine>.<tailnet>.ts.net
    std::string login;         // the node owner's login
    bool        https { false }; // the tailnet issues certificates (admin console > DNS > HTTPS)
    bool        serving { false };
    int         serving_port { 0 }; // the loopback port the Serve proxy points at
    std::string error;
    long long   checked_at { 0 };
};

static TailscaleState tailscale_query()
{
    TailscaleState t;
    std::string    out;
    int            code = 0;
    t.checked_at = (long long) std::time(nullptr);
    if (!run_capture({ tailscale_exe(), "status", "--json" }, out, code, 15000)) {
        t.error = "Tailscale is not installed on this PC";
        return t;
    }
    t.installed = true;
    try {
        json j    = json::parse(out);
        t.backend = j.value("BackendState", "");
        json self = j.value("Self", json::object());
        t.dns_name = self.value("DNSName", "");
        while (!t.dns_name.empty() && t.dns_name.back() == '.') t.dns_name.pop_back();
        if (self.contains("UserID")) {
            const std::string uid   = std::to_string(self["UserID"].get<long long>());
            json              users = j.value("User", json::object());
            if (users.contains(uid)) t.login = lower(users[uid].value("LoginName", ""));
        }
        t.https = !j.value("CertDomains", json::array()).empty();
    } catch (...) {
        t.error = code == 0 ? "unexpected output from tailscale status" : out.substr(0, 200);
        return t;
    }
    if (t.backend != "Running") {
        t.error = t.backend == "NeedsLogin" ? "Tailscale is installed but not signed in on this PC" : "Tailscale is not running (" + t.backend + ")";
        return t;
    }
    if (run_capture({ tailscale_exe(), "serve", "status", "--json" }, out, code, 15000) && code == 0) {
        t.serving_port = Testing::serve_status_target_port(out);
        t.serving      = t.serving_port > 0;
    }
    return t;
}

// Pulled out of tailscale_query() so the parsing rule can be driven from a test with captured
// `tailscale serve status --json` text and no real tailnet (RemoteHub::Testing, remote_hub_tests.cpp).
// `tailscale serve status --json` nests the forwarded port under Web -> "<domain>:443" ->
// Handlers -> "/" -> Proxy = "http://127.0.0.1:<port>". Only the root path ("/") is ours - the
// hub never registers anything else with Serve - and only a loopback proxy counts as "serving".
// ---- remote access, as one state the page can draw --------------------------------------------
// Six strings and one link, in one place. Before this the hub page re-derived "installed but not
// signed in" from three separate booleans and carried its own copy of every sentence, so the two
// could (and did) drift. classify_remote_access() is pure, so the whole table is a unit test.
const char* Testing::remote_access_state_name(Testing::RemoteAccessState st)
{
    switch (st) {
    case RemoteAccessState::NotInstalled: return "not_installed";
    case RemoteAccessState::NotSignedIn:  return "not_signed_in";
    case RemoteAccessState::NotRunning:   return "not_running";
    case RemoteAccessState::HttpsOff:     return "https_off";
    case RemoteAccessState::Serving:      return "serving";
    case RemoteAccessState::Ready:        return "ready";
    default:                              return "error";
    }
}

Testing::RemoteAccessInfo Testing::classify_remote_access(bool installed, const std::string& backend, bool https,
                                                          bool serving, bool on, const std::string& error)
{
    RemoteAccessInfo r;
    if (!installed) {
        r.state      = RemoteAccessState::NotInstalled;
        r.message    = "Remote access uses Tailscale, a free private network between your PC and your phone. "
                       "Install it on both and sign in with the same account.";
        r.action     = "Install Tailscale";
        r.action_url = TAILSCALE_DOWNLOAD_URL;
        return r;
    }
    if (backend == "NeedsLogin" || backend == "Starting") {
        r.state   = RemoteAccessState::NotSignedIn;
        r.message = "Tailscale is installed but not signed in on this PC. Open Tailscale from the system tray "
                    "(or run `tailscale login`) and sign in with the same account as your phone, then try again.";
        r.action  = "Try again";
        return r;
    }
    if (backend != "Running") {
        r.state   = RemoteAccessState::NotRunning;
        r.message = backend.empty() ? "Tailscale is not running on this PC." : "Tailscale is not running (" + backend + ").";
        r.action  = "Try again";
        return r;
    }
    if (!https) {
        // The one error that cannot be fixed from this PC at all: it is a tailnet-wide setting in
        // the admin console, so the card links straight at the page that has the switch.
        r.state      = RemoteAccessState::HttpsOff;
        r.message    = "HTTPS certificates are not enabled for your tailnet: Tailscale admin console > DNS > "
                       "HTTPS Certificates > Enable, then try again.";
        r.action     = "Open DNS settings";
        r.action_url = TAILSCALE_DNS_ADMIN_URL;
        return r;
    }
    if (on && serving) {
        r.state   = RemoteAccessState::Serving;
        r.message = "Remote access is on. Your phone can reach this hub from anywhere through your tailnet.";
        return r;
    }
    if (on && !serving) {
        // remote_on is set but Serve is not pointed here: the reconcile in bind() is the usual cure
        // and it runs by itself, so the button repeats it rather than reporting a dead end.
        r.state   = RemoteAccessState::Error;
        r.message = error.empty() ? "Tailscale is no longer serving this hub; turn remote access off and on again." : error;
        r.action  = "Try again";
        return r;
    }
    if (!error.empty()) {
        r.state   = RemoteAccessState::Error;
        r.message = error;
        r.action  = "Try again";
        return r;
    }
    r.state   = RemoteAccessState::Ready;
    r.message = "Tailscale is ready. Turn remote access on to publish this hub inside your tailnet.";
    return r;
}

// ---- hub identity (design_hosted_relay.md section 2) ------------------------------------------
// hubid = the first 16 hex characters of SHA-256 over the 32 raw bytes of the Ed25519 public key.
// A hash rather than the key itself so that the durable name a relay knows this hub by is short
// enough for a URL label and reveals nothing but the key it was derived from.
std::string Testing::hubid_from_public_key(const std::vector<unsigned char>& public_key)
{
    if (public_key.size() != 32) return std::string();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(public_key.data(), public_key.size(), digest);
    static const char* HEX = "0123456789abcdef";
    std::string        out;
    for (int i = 0; i < 8; ++i) { // 8 bytes = 16 hex characters
        out.push_back(HEX[digest[i] >> 4]);
        out.push_back(HEX[digest[i] & 0x0f]);
    }
    return out;
}

std::string Testing::hubid_from_public_key_hex(const std::string& public_key_hex)
{
    if (public_key_hex.size() != 64) return std::string();
    std::vector<unsigned char> raw;
    raw.reserve(32);
    for (size_t i = 0; i < 64; i += 2) {
        const int hi = hex_nibble(public_key_hex[i]), lo = hex_nibble(public_key_hex[i + 1]);
        if (hi < 0 || lo < 0) return std::string();
        raw.push_back((unsigned char) ((hi << 4) | lo));
    }
    return hubid_from_public_key(raw);
}

// What settings.json carries under "identity". The private half is in here and nowhere else - not
// hub.json (which the phone-facing code reads), not /pair, not /hub/info, never a log line.
std::string Testing::identity_settings_dump(const Testing::HubIdentity& id)
{
    json j;
    j["hubid"]   = id.hubid;
    j["public"]  = id.public_hex;
    j["private"] = id.private_hex;
    return j.dump();
}

Testing::HubIdentity Testing::identity_from_settings(const std::string& settings_json_text)
{
    HubIdentity id;
    try {
        json j = json::parse(settings_json_text);
        // Accept either the whole settings file or just the identity object, so a caller that
        // already dug the member out does not have to re-wrap it.
        const json o = j.contains("identity") ? j["identity"] : j;
        if (!o.is_object()) return HubIdentity();
        id.public_hex  = lower(o.value("public", std::string()));
        id.private_hex = lower(o.value("private", std::string()));
        id.hubid       = lower(o.value("hubid", std::string()));
    } catch (...) { return HubIdentity(); }
    // The stored hubid is only ever a cache of the derivation; the key wins if the two disagree,
    // so a hand-edited or truncated settings file cannot make this hub answer to the wrong name.
    const std::string derived = hubid_from_public_key_hex(id.public_hex);
    if (derived.empty()) return HubIdentity();
    id.hubid = derived;
    return id;
}

// ---- signing with the hub identity (the push forwarder's X-Hub-Sig) ---------------------------
// Pure Ed25519 through the same OpenSSL EVP_PKEY_ED25519 the identity is minted with: no new
// dependency, and EVP_DigestSign with a null digest is exactly RFC 8032's one-shot signature.

static bool hex_to_bytes(const std::string& hex, std::vector<unsigned char>& out)
{
    out.clear();
    if (hex.size() % 2) return false;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return false; }
        out.push_back((unsigned char) ((hi << 4) | lo));
    }
    return true;
}

static const char* const SIG_B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string sig_b64url_encode(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned(data[i]) << 16) | (i + 1 < len ? unsigned(data[i + 1]) << 8 : 0u) |
                           (i + 2 < len ? unsigned(data[i + 2]) : 0u);
        out += SIG_B64URL[(v >> 18) & 63];
        out += SIG_B64URL[(v >> 12) & 63];
        if (i + 1 < len) out += SIG_B64URL[(v >> 6) & 63];
        if (i + 2 < len) out += SIG_B64URL[v & 63];
    }
    return out;
}

// base64url with the padding tolerated (and '+' '/' as well, so a standard-alphabet signature
// still decodes). Anything else is refused rather than skipped.
static bool sig_b64url_decode(const std::string& in, std::vector<unsigned char>& out)
{
    out.clear();
    unsigned acc  = 0;
    int      bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = ((acc << 6) | (unsigned) v) & 0xffffff;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((unsigned char) ((acc >> bits) & 0xff)); }
    }
    return true;
}

Testing::HubIdentity Testing::identity_from_seed_hex(const std::string& private_hex)
{
    HubIdentity                id;
    std::vector<unsigned char> seed;
    if (private_hex.size() != 64 || !hex_to_bytes(private_hex, seed)) return id;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    OPENSSL_cleanse(seed.data(), seed.size());
    if (!pkey) return id;
    unsigned char pub[32] = {};
    size_t        publen  = sizeof(pub);
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &publen) == 1 && publen == sizeof(pub)) {
        id.public_hex  = to_hex(pub, publen);
        id.private_hex = lower(private_hex);
        id.hubid       = hubid_from_public_key(std::vector<unsigned char>(pub, pub + publen));
    }
    EVP_PKEY_free(pkey);
    return id;
}

bool identity_sign(const Testing::HubIdentity& id, const std::string& message, std::string& out_sig_b64url)
{
    out_sig_b64url.clear();
    std::vector<unsigned char> seed;
    if (id.private_hex.size() != 64 || !hex_to_bytes(id.private_hex, seed)) return false;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    OPENSSL_cleanse(seed.data(), seed.size());
    if (!pkey) return false;
    unsigned char sig[64] = {};
    size_t        siglen  = sizeof(sig);
    EVP_MD_CTX*   md      = EVP_MD_CTX_new();
    const bool    ok      = md && EVP_DigestSignInit(md, nullptr, nullptr, nullptr, pkey) == 1 &&
                   EVP_DigestSign(md, sig, &siglen, (const unsigned char*) message.data(), message.size()) == 1 &&
                   siglen == sizeof(sig);
    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    if (!ok) return false;
    out_sig_b64url = sig_b64url_encode(sig, siglen);
    return true;
}

bool identity_verify(const std::string& public_hex, const std::string& message, const std::string& sig_b64url)
{
    std::vector<unsigned char> pub, sig;
    if (public_hex.size() != 64 || !hex_to_bytes(public_hex, pub)) return false;
    if (!sig_b64url_decode(sig_b64url, sig) || sig.size() != 64) return false;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
    if (!pkey) return false;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    const bool  ok = md && EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, pkey) == 1 &&
                    EVP_DigestVerify(md, sig.data(), sig.size(), (const unsigned char*) message.data(), message.size()) == 1;
    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ok;
}

// ---- the loopback trust of Tailscale Serve's headers (design section 6.6) ---------------------
// Tailscale Serve terminates on loopback and sets Tailscale-User-Login / X-Forwarded-Proto,
// stripping whatever a client tried to send. That is the whole basis for trusting them - so they
// are trusted from a loopback peer and from nowhere else. A request that walked in on the LAN
// listener carrying a Tailscale-User-Login header is a client lying about who it is, and clearing
// the field here is what stops it ever reaching login_allowed().
//
// The `via_relay` argument is the hook phase 1 needs: a relayed stream is re-injected as a loopback
// peer (so is_private_v4 is satisfied without being loosened) and would therefore *look* exactly
// like Serve. It is not - nothing upstream of it strips these headers - so it never gets the trust.
bool Testing::trusted_proxy_headers(bool peer_is_loopback, bool via_relay)
{
    return peer_is_loopback && !via_relay;
}

// The pairing document's origins and identity, pure so the shape can be tested without a hub.
// Three named origins, one of them always empty today, and the public half of the hub identity.
std::string Testing::pair_identity_json(const std::string& lan_url, const std::string& remote_url,
                                        const std::string& relay_url, const std::string& hubid,
                                        const std::string& public_key_hex)
{
    json j;
    j["urls"]       = json{ { "lan", lan_url }, { "remote", remote_url }, { "relay", relay_url } };
    j["hubid"]      = hubid;
    j["public_key"] = public_key_hex; // the public half only, always
    return j.dump();
}

int Testing::serve_status_target_port(const std::string& serve_status_json_text)
{
    try {
        const json s        = json::parse(serve_status_json_text);
        const json web_all  = s.value("Web", json::object());
        for (const auto& web : web_all) {
            const json handlers = web.value("Handlers", json::object());
            for (const auto& [path, h] : handlers.items()) {
                const std::string proxy = h.value("Proxy", ""), want = "http://127.0.0.1:";
                if (path == "/" && proxy.compare(0, want.size(), want) == 0)
                    return std::atoi(proxy.c_str() + want.size());
            }
        }
    } catch (...) {}
    return 0;
}

// The hub's durable identity: one Ed25519 key pair per data dir, minted the first time
// settings.json is written and never again (design_hosted_relay.md section 2 - a relay will know
// this hub by the hubid derived from the public half, and a hub that re-minted would lose its
// registration on every start). OpenSSL's EVP_PKEY_ED25519, the same library WebPush's VAPID keys
// and AppPush's APNs signing already use, so nothing new is linked.
//
// `private_hex` is the 32-byte Ed25519 seed - the whole secret. It is written to settings.json and
// to nothing else: not hub.json, not /pair, not /hub/info, and never a log line.
static Testing::HubIdentity mint_hub_identity()
{
    Testing::HubIdentity id;
    EVP_PKEY*            pkey = nullptr;
    EVP_PKEY_CTX*        ctx  = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) return id;
    if (EVP_PKEY_keygen_init(ctx) == 1) EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);
    if (!pkey) return id;
    unsigned char pub[32] = {}, priv[32] = {};
    size_t        publen = sizeof(pub), privlen = sizeof(priv);
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &publen) == 1 && publen == sizeof(pub) &&
        EVP_PKEY_get_raw_private_key(pkey, priv, &privlen) == 1 && privlen == sizeof(priv)) {
        id.public_hex  = to_hex(pub, publen);
        id.private_hex = to_hex(priv, privlen);
        id.hubid       = Testing::hubid_from_public_key(std::vector<unsigned char>(pub, pub + publen));
    }
    OPENSSL_cleanse(priv, sizeof(priv));
    EVP_PKEY_free(pkey);
    return id;
}

// Per-run secrets (std::random_device is the OS CSPRNG on every platform we build).
static std::string random_hex(int bytes)
{
    static const char* hx = "0123456789abcdef";
    std::random_device rd;
    std::string        s;
    for (int i = 0; i < bytes; ++i) { const unsigned v = rd() & 0xffu; s += hx[v >> 4]; s += hx[v & 15]; }
    return s;
}

static std::string basic_auth(const std::string& user, const std::string& pass)
{
    const std::string raw = user + ":" + pass;
    std::string       out(boost::beast::detail::base64::encoded_size(raw.size()), '\0');
    out.resize(boost::beast::detail::base64::encode(out.data(), raw.data(), raw.size()));
    return "Basic " + out;
}

// A free loopback port for go2rtc (bound and released; the race with another process is
// theoretical on a PC and go2rtc simply fails to start, which the log shows).
static int free_loopback_port()
{
    try {
        asio::io_context ioc;
        tcp::acceptor    a(ioc);
        a.open(tcp::v4());
        a.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        return (int) a.local_endpoint().port();
    } catch (...) { return 0; }
}

// Native separators: this path is shown to the user to paste into the firewall dialog.
static std::string go2rtc_exe_path()
{
    return fs::path(resources_dir() + "/tools/go2rtc/go2rtc.exe").make_preferred().string();
}

// ---- Stream quality variants -------------------------------------------------------------
// The phone's Quality setting picks a *stream name*: "<name>" as the camera sends it, or a
// "<name>_med" / "<name>_low" variant registered beside it. Registering those variants means
// re-encoding, and go2rtc re-encodes by shelling out to an ffmpeg binary (its `ffmpeg:` source
// scheme) - it does not carry a codec of its own. We now **bundle** one: an LGPL-3.0 ffmpeg build
// installs beside go2rtc.exe (CMake's FFMPEG_BIN_DIR, see docs/superpowers/specs/2026-09-12-bundled-ffmpeg.md),
// so the transcoded variants are available in a stock install rather than only on a PC where the
// user happened to put an ffmpeg on PATH.
//
// The bundled build is LGPL, which means it carries **no libx264** (that is GPL): its software
// H.264 encoder is libopenh264. That matters here because go2rtc's built-in `h264` template is
// `-codec:v libx264 ... -preset:v superfast -tune:v zerolatency`, which this ffmpeg would reject
// outright - so start_go2rtc() writes its own `ffmpeg: h264:` template in the config. See
// ffmpeg_h264_template() below for the encoder settings and why each one is what it is.
//
// Quality still degrades gracefully: a PC whose install lost the bundled exe (or a platform we do
// not ship one for) falls back to PATH, and with neither the variants are simply not registered -
// a stream go2rtc cannot start is worse than an absent one, because the tile goes black instead of
// falling back to the source. Independently of any of this, the Bambu MJPEG relay's frame-rate
// knob (BambuCamRelay, ?fps=) needs no decoder at all and honours Medium/Low on its own.
static std::string ffmpeg_path()
{
    // The bundled build first (installed by CMake beside go2rtc.exe), then PATH as the fallback
    // for a tree or platform that has none.
    const std::string beside = fs::path(resources_dir() + "/tools/go2rtc/ffmpeg.exe").make_preferred().string();
    boost::system::error_code ec;
    if (fs::exists(beside, ec)) return beside;
#ifdef _WIN32
    std::string out; int code = 0;
    if (run_capture({ "where", "ffmpeg" }, out, code, 8000) && code == 0) {
        std::istringstream is(out); std::string line;
        if (std::getline(is, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) return line;
        }
    }
#endif
    return "";
}
// The variant suffixes the hub can actually register, in descending quality. Empty without an
// ffmpeg: a stream go2rtc cannot start is worse than an absent one, because the tile would go
// black instead of falling back. Computed once - an ffmpeg appearing mid-run is not worth a
// PATH lookup per request - and the hub logs which case it is at startup.
static const std::vector<std::string>& quality_variants()
{
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        if (!ffmpeg_path().empty()) out = { "med", "low" };
        return out;
    }();
    return v;
}

// The `h264` encoder template go2rtc uses for our variants, replacing its built-in one.
//
// go2rtc 1.9.14 ships `-codec:v libx264 -g:v 30 -preset:v superfast -tune:v zerolatency
// -profile:v main -level:v 4.1`. Every one of those three x264-only flags is fatal with the
// bundled LGPL build, which has no libx264 at all, so the template is ours:
//
//   libopenh264            the LGPL build's software H.264 encoder (Cisco, BSD-2-Clause). It has
//                          no -preset/-tune knobs; the equivalents are spelled out below.
//   constrained_baseline   no B-frames by construction, which is what "zerolatency" buys with
//                          x264: a frame is emitted as soon as it is encoded, nothing is held to
//                          reorder. Also the profile every phone decoder handles, and what
//                          WebRTC/MSE want.
//   -bf 0                  belt and braces on the same point.
//   -rc_mode bitrate       track -b:v. The default ("quality") lets the bitrate wander, which is
//                          the opposite of what a Quality step is for.
//   -g:v                   keyframe interval. Set per variant to ~2 s of that variant's frame
//                          rate (30 @15 fps, 20 @10 fps) so a joining viewer waits at most 2 s
//                          for its first picture without spending the bitrate on more IDRs.
//
// -b:v and -g:v are appended per variant by variant_src()'s #raw, so one template serves both.
static std::string ffmpeg_h264_template()
{
    return "-codec:v libopenh264 -profile:v constrained_baseline -rc_mode bitrate -bf 0";
}

// go2rtc's `ffmpeg:` source, pointed back at the stream the hub already registered, so a variant
// is a re-encode of our own stream rather than a second connection to the printer - the camera
// still sees exactly one consumer. go2rtc starts the ffmpeg process lazily - only when a viewer
// actually opens the variant - and kills it when the last consumer goes, so an unwatched
// _med/_low costs nothing at all. The gate asserts both halves of that.
//
//   Medium  1280x720 @ 15 fps, 1.5 Mbps, keyframe every 2 s
//   Low      854x480 @ 10 fps, 0.6 Mbps, keyframe every 2 s
//
// 854 rather than 640 so Low is a real 16:9 480p: the phone's Low step is meant to be watchable,
// and at 0.6 Mbps the extra width costs little. #width alone keeps the aspect ratio.
//
// Hardware encoding is **off by default**. go2rtc's #hardware would pick a GPU encoder, and on a
// PC with a working one that is cheaper - but it fails in ways software encoding does not (a
// headless/RDP session with no GPU, a driver that refuses a second session, an encoder that is
// already busy with a game), and it fails as a black tile rather than an error the user sees.
// Software libopenh264 costs ~5% of one core for a 720p Medium on this PC (see the spec), which is
// not worth that risk by default. To turn it on, add `#hardware` to the strings below and
// rebuild; go2rtc then tries dxva2/cuda/qsv and falls back to software by itself.
// Each extra ffmpeg argument is its own #raw= segment, one token per segment and never a space
// inside one. That is not a style choice: go2rtc rejects a registration whose source contains a
// space outright, with `400 streams: source with spaces may be insecure`, so the natural
// `#raw=-r 10 -b:v 600k` form registers as nothing at all and the variant silently does not
// exist. (Found by gating it - see the spec. The hub's PUT is fire-and-forget on a detached
// thread, so the 400 would never have surfaced anywhere a user or a log would show it.)
static std::string variant_raw(std::initializer_list<const char*> toks)
{
    std::string s;
    for (const char* t : toks) s += "#raw=" + std::string(t);
    return s;
}

static std::string variant_src(const std::string& base_name, const std::string& q)
{
    const bool low = (q == "low");
    // -r caps the frame rate, -b:v/-maxrate the bitrate, -g:v the keyframe interval (~2 s at
    // that rate). #width alone scales and keeps the aspect ratio.
    if (low)
        return "ffmpeg:" + base_name + "#video=h264#width=854" +
               variant_raw({ "-r", "10", "-b:v", "600k", "-maxrate", "600k", "-g:v", "20" });
    return "ffmpeg:" + base_name + "#video=h264#width=1280" +
           variant_raw({ "-r", "15", "-b:v", "1500k", "-maxrate", "1500k", "-g:v", "30" });
}
// ---- WebRTC (Phase 2): go2rtc's media port ------------------------------------------------
// Everything else the hub runs is loopback-only, but WebRTC media goes straight from go2rtc to
// the phone, so this one port has to be reachable on the LAN and on the tailnet. A predictable
// port keeps a Windows Firewall rule the user allows once valid across restarts; 8555 is
// go2rtc's own documented default, and if something else on the PC already holds it (another
// go2rtc, say) we take the next free one and say which on the hub page.
static const int WEBRTC_PORT_FIRST = 8555;
static const int WEBRTC_PORT_LAST  = 8574;

// Free for both protocols on every interface. A dual-stack listener elsewhere on the PC makes
// the v4 bind fail too, which is what we want: go2rtc would not get the port either.
static bool port_free_any(int port)
{
    try {
        asio::io_context ioc;
        tcp::acceptor    a(ioc);
        a.open(tcp::v4());
        a.bind(tcp::endpoint(tcp::v4(), (unsigned short) port));
        asio::ip::udp::socket u(ioc);
        u.open(asio::ip::udp::v4());
        u.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), (unsigned short) port));
        return true;
    } catch (...) { return false; }
}

static int free_webrtc_port()
{
    for (int p = WEBRTC_PORT_FIRST; p <= WEBRTC_PORT_LAST; ++p)
        if (port_free_any(p)) return p;
    return 0;
}

// ---- who holds HUB_PORT when bind() had to step past it ---------------------------------
// bind() below tries 13640..13659 in order and takes the first free one; when that is not 13640
// the phone link the hub prints carries whatever port it landed on, and unless the firewall rule
// covers the whole range (cmake/nsis/SnapmakerURLProtocols_install.nsh) or the user is told what
// happened, "No hub answered on that link" is all they see. This answers "what is on 13640" by
// shelling out to the same two tools a person would reach for themselves: `netstat -ano` for the
// pid listening on the port, then `tasklist` for that pid's image name. Both run through
// run_capture(), same as the tailscale and firewall queries above.

// Pulled out for testing (RemoteHub::Testing, remote_hub_tests.cpp): given `netstat -ano` text,
// the pid of whatever is LISTENING on `port`, or 0. netstat's columns are whitespace-separated
// and its local-address column is "<addr>:<port>", so this looks for a line whose Proto is TCP,
// whose State is LISTENING, and whose local address ends in ":<port>" (a raw suffix match would
// also hit port 1364 0 style false positives, so the match requires ':' immediately before it and
// nothing but the line's own whitespace after).
long Testing::netstat_holder_pid(const std::string& netstat_text, int port)
{
    const std::string suffix = ":" + std::to_string(port);
    std::istringstream is(netstat_text);
    std::string        line;
    while (std::getline(is, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::istringstream ls(line);
        std::string        proto, local, remote, state, pid_s;
        if (!(ls >> proto >> local >> remote >> state >> pid_s)) continue;
        if (proto != "TCP" || state != "LISTENING") continue;
        if (local.size() < suffix.size() || local.compare(local.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        try {
            return std::stol(pid_s);
        } catch (...) { continue; }
    }
    return 0;
}

// Pulled out for testing, same reasons: `tasklist /fi "PID eq <n>" /fo csv /nh` prints one CSV
// row, `"image.exe","1234","Console","1","12,345 K"`, or the literal
// "INFO: No tasks are running which match the specified criteria." when the pid is gone. The
// image name is the first quoted field.
std::string Testing::tasklist_image_name(const std::string& tasklist_csv_text)
{
    const size_t open = tasklist_csv_text.find('"');
    if (open == std::string::npos) return "";
    const size_t close = tasklist_csv_text.find('"', open + 1);
    if (close == std::string::npos || close <= open + 1) return "";
    return tasklist_csv_text.substr(open + 1, close - open - 1);
}

// What (if anything) is listening on `port` right now, as a short "<exe> (pid N)" string for the
// log and the status JSON, or "" if nothing answers (the port freed up between the failed bind
// and this call - reported as such rather than guessed at).
static std::string port_holder_description(int port)
{
#ifdef _WIN32
    std::string out;
    int         code = 0;
    if (!run_capture({ "netstat", "-ano", "-p", "TCP" }, out, code, 8000)) return "";
    const long pid = Testing::netstat_holder_pid(out, port);
    if (pid <= 0) return "";
    std::string tout;
    if (run_capture({ "tasklist", "/fi", "PID eq " + std::to_string(pid), "/fo", "csv", "/nh" }, tout, code, 8000)) {
        const std::string image = Testing::tasklist_image_name(tout);
        if (!image.empty()) return image + " (pid " + std::to_string(pid) + ")";
    }
    return "pid " + std::to_string(pid);
#else
    (void) port;
    return "";
#endif
}

// What Windows Firewall thinks of go2rtc.exe. WebRTC media arrives inbound on the port above, so
// without an allow rule for the profile the phone's network is on, the peer connection never
// completes and the page silently stays on MSE through the hub. We only *look*: adding a rule
// needs administrator rights and doing it silently would be wrong, so the answer is shown to the
// user as a note with the two things they can allow.
struct FirewallState
{
    std::string state { "unknown" }; // allowed | partial | missing | blocked | unknown
    std::string note;                // one short sentence, shown on the hub page (and on the phone)
    std::string command;             // the exact netsh line, shown only behind "Show command"
    std::string networks;            // profiles the PC's live networks are in ("Public, Private")
    long long   checked_at { 0 };
};

// Joined for a sentence, each value once (two rules for the same profile, two networks in the
// same profile - the user only wants to read "Private" once).
static std::string join_words(const std::vector<std::string>& v, const char* sep)
{
    std::string              out;
    std::vector<std::string> seen;
    for (const std::string& s : v) {
        if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
        seen.push_back(s);
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

// Windows Firewall through PowerShell rather than netsh: Get-NetFirewallRule answers with
// property values (Allow/Inbound/Private) that are the same in every Windows display language,
// while netsh's verbose output is localised and would have to be parsed by label.
//
// `label` is the program name used in the sentences shown to the user ("go2rtc.exe", "EdgeSlicer.exe");
// `netsh_hint` is the exact command they can paste into an elevated prompt to fix a "missing" or
// "partial" state themselves - we only ever *look*, never run netsh add ourselves.
static FirewallState firewall_query(const std::string& exe, int port, const std::string& label, const std::string& netsh_hint)
{
    FirewallState fw;
    fw.checked_at = (long long) std::time(nullptr);
#ifdef _WIN32
    std::string quoted = exe; // '' escapes a quote inside a PowerShell single-quoted string
    for (size_t i = 0; i < quoted.size(); ++i)
        if (quoted[i] == '\'') quoted.insert(i++, 1, '\'');
    const std::string script =
        "$p='" + quoted + "';$f=[IO.Path]::GetFullPath($p);"
        "$r=@(Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue |"
        " Where-Object { try { [IO.Path]::GetFullPath($_.Program) -ieq $f } catch { $false } } |"
        " Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.Enabled -eq 'True' -and"
        " $_.Direction -eq 'Inbound' });"
        "foreach ($x in $r) { $(if ($x.Action -eq 'Block') { 'BLOCK=' } else { 'RULE=' }) + $x.Profile };"
        "foreach ($n in @(Get-NetConnectionProfile -ErrorAction SilentlyContinue)) { 'NET=' + $n.NetworkCategory };"
        "'DONE'";
    std::string out;
    int         code = 0;
    if (!run_capture({ "powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script }, out, code, 30000) ||
        out.find("DONE") == std::string::npos) {
        fw.note    = "Windows Firewall could not be checked for " + label + ".";
        fw.command = netsh_hint;
        return fw;
    }
    std::vector<std::string> rules, blocks, nets;
    std::istringstream       is(out);
    std::string              line;
    while (std::getline(is, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, 5, "RULE=") == 0) rules.push_back(line.substr(5));
        else if (line.compare(0, 6, "BLOCK=") == 0) blocks.push_back(line.substr(6));
        else if (line.compare(0, 4, "NET=") == 0) nets.push_back(line.substr(4) == "DomainAuthenticated" ? "Domain" : line.substr(4));
    }
    // Only the profiles the PC's live networks are in matter: a rule that covers Public does
    // nothing for a phone on a network Windows filed as Private.
    auto covers = [](const std::vector<std::string>& rs, const std::string& n) {
        for (const std::string& r : rs)
            if (r == "Any" || lower(r).find(lower(n)) != std::string::npos) return true;
        return false;
    };
    std::vector<std::string> uncovered, denied;
    for (const std::string& n : nets) {
        if (covers(blocks, n) && std::find(denied.begin(), denied.end(), n) == denied.end()) denied.push_back(n);
        else if (!covers(rules, n) && std::find(uncovered.begin(), uncovered.end(), n) == uncovered.end()) uncovered.push_back(n);
    }
    fw.networks = join_words(nets, ", ");
    // One short sentence for the page; the exact netsh line goes in `command`, shown only behind
    // "Show command" (hub.html). `note` used to carry both - callers that still want the full
    // wording (older readers, logging) can concatenate note + " " + command themselves.
    if (!denied.empty()) {
        // Windows writes one of these when the user dismisses its "allow access?" prompt, and a
        // block rule wins over any allow rule, so this has to be reported ahead of them.
        fw.state   = "blocked";
        fw.note    = "Windows Firewall has a rule that blocks " + label + " on this " +
                     join_words(denied, " and ") + " network.";
        fw.command = netsh_hint;
    } else if (rules.empty()) {
        fw.state   = "missing";
        fw.note    = "Windows Firewall has no rule for " + label + " on this network.";
        fw.command = netsh_hint;
    } else if (!uncovered.empty()) {
        fw.state   = "partial";
        fw.note    = "Windows Firewall allows " + label + " on " + join_words(rules, " / ") +
                     " networks, but this PC is on " + join_words(uncovered, " and ") + ".";
        fw.command = netsh_hint;
    } else {
        fw.state = "allowed";
        fw.note  = "";
    }
#else
    (void) exe; (void) port; (void) netsh_hint;
    fw.note    = "Direct connections need an inbound port open for " + label + ".";
    fw.command = netsh_hint;
#endif
    return fw;
}

// go2rtc's call site: the hub page appends its own "video still works without it" sentence
// (resources/web/orca/hub.html, videoLine()), so the note here stays generic.
static FirewallState firewall_query_go2rtc(const std::string& exe, int port)
{
    return firewall_query(exe, port, "go2rtc.exe",
        "netsh advfirewall firewall add rule name=\"go2rtc\" dir=in action=allow program=\"" + exe +
        "\" protocol=TCP localport=" + std::to_string(port) + " profile=private,domain");
}

// The Host header must name this PC's loopback (a DNS-rebound name is not accepted).
static bool loopback_host(const std::string& host)
{
    if (host.empty()) return false;
    std::string h = host;
    if (h[0] == '[') { const size_t e = h.find(']'); if (e != std::string::npos) h = h.substr(0, e + 1); }
    else { const size_t c = h.find(':'); if (c != std::string::npos) h = h.substr(0, c); }
    return h == "127.0.0.1" || h == "localhost" || h == "[::1]";
}

// Rewrite a request head for go2rtc: drop the client's Cookie and Authorization, drop the hub's
// own `lt` query parameter, attach the hub's go2rtc credentials.
static std::string go2rtc_head(const std::string& head, const std::string& auth)
{
    std::string out;
    size_t      pos   = 0;
    bool        first = true;
    while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos) nl = head.size();
        const std::string line = head.substr(pos, nl - pos);
        pos = nl + 2;
        if (first) {
            first = false;
            std::istringstream is(line);
            std::string        m, t, v;
            is >> m >> t >> v;
            const size_t q = t.find('?');
            if (q != std::string::npos) {
                const std::string  path = t.substr(0, q);
                std::istringstream qs(t.substr(q + 1));
                std::string        kv, kept;
                while (std::getline(qs, kv, '&')) {
                    if (kv.compare(0, 3, "lt=") == 0) continue;
                    if (!kept.empty()) kept += '&';
                    kept += kv;
                }
                t = kept.empty() ? path : path + "?" + kept;
            }
            out += m + " " + t + " " + v + "\r\n";
            continue;
        }
        if (line.empty()) break;
        const std::string key = lower(line.substr(0, line.find(':')));
        if (key == "cookie" || key == "authorization") continue;
        out += line + "\r\n";
    }
    out += "Authorization: " + auth + "\r\n\r\n";
    return out;
}

// Snapmaker U1 (camera-streamer behind nginx): the Stream tab embeds http://<ip>/webcam/webrtc,
// which only a browser on the LAN can reach. The same server offers raw H.264 at
// /webcam/stream.h264, which go2rtc can carry to a phone anywhere - via the relay below.
static std::string u1_h264_url(const json& host)
{
    const std::string rurl = host.value("rurl", "");
    const std::string tail = "/webcam/webrtc";
    if (rurl.compare(0, 7, "http://") != 0) return "";
    std::string u = rurl;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.size() <= tail.size() || u.compare(u.size() - tail.size(), tail.size(), tail) != 0) return "";
    const std::string hostport = u.substr(7, u.size() - 7 - tail.size());
    if (hostport.empty() || hostport.find('/') != std::string::npos) return "";
    return "http://" + hostport + "/webcam/stream.h264";
}

static std::string u1_stream_name(const std::string& id)
{
    std::string n = "u1_";
    for (unsigned char c : id) n += std::isalnum(c) ? (char) c : '_';
    return n;
}

static std::string encode_component(const std::string& s)
{
    static const char* hx = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
        else { out += '%'; out += hx[c >> 4]; out += hx[c & 15]; }
    }
    return out;
}

// GET a raw H.264 stream from a camera server and re-serve it from the first SPS onward (a
// stream that starts mid-GOP begins with a slice byte that also reads as an HEVC header, and
// go2rtc then decodes nothing). Chunked transfer is undone; the client gets a plain body.
static void relay_h264(tcp::socket& client, const std::string& url)
{
    std::string rest  = url.substr(7);
    const size_t sl   = rest.find('/');
    std::string hostport = rest.substr(0, sl), path = sl == std::string::npos ? "/" : rest.substr(sl);
    std::string host = hostport, port = "80";
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }
    asio::io_context          ioc;
    tcp::resolver             resolver(ioc);
    boost::system::error_code ec;
    auto endpoints = resolver.resolve(host, port, ec);
    if (ec) { respond(client, 502, "text/plain", "camera address did not resolve"); return; }
    tcp::socket up(ioc);
    asio::connect(up, endpoints, ec);
    if (ec) { respond(client, 502, "text/plain", "camera did not answer"); return; }
    up.set_option(tcp::no_delay(true));
    // Long-lived from here: no read deadline on the consumer (go2rtc), but a camera that stops
    // sending must not hold this thread, and keep-alive probes end it if go2rtc disappears.
    set_read_timeout(up, 30);
    set_read_timeout(client, 0);
    set_write_timeout(client, TUNNEL_WRITE_S);
    set_keepalive(client, 30);
    write_all(up, "GET " + path + " HTTP/1.1\r\nHost: " + hostport + "\r\nConnection: close\r\n\r\n");
    std::string buf;
    char        tmp[16384];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        const size_t n = up.read_some(asio::buffer(tmp), ec);
        if (ec || buf.size() > 65536) { respond(client, 502, "text/plain", "bad answer from the camera"); return; }
        buf.append(tmp, n);
    }
    const size_t      he   = buf.find("\r\n\r\n") + 4;
    const std::string head = lower(buf.substr(0, he));
    if (head.compare(0, 12, "http/1.1 200") != 0 && head.compare(0, 12, "http/1.0 200") != 0) {
        respond(client, 502, "text/plain", "camera answered " + buf.substr(0, buf.find("\r\n")));
        return;
    }
    const bool chunked = head.find("transfer-encoding: chunked") != std::string::npos;
    write_all(client, "HTTP/1.1 200 OK\r\nContent-Type: video/h264\r\nCache-Control: no-store\r\n"
                      "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n");

    bool        started = false;
    std::string pend; // unforwarded bytes while waiting for the first SPS
    auto emit = [&](const char* p, size_t n) -> bool {
        boost::system::error_code e;
        if (started) { asio::write(client, asio::buffer(p, n), e); return !e; }
        pend.append(p, n);
        size_t at = std::string::npos;
        for (size_t k = 0; k + 3 < pend.size(); ++k)
            if (pend[k] == 0 && pend[k + 1] == 0 && pend[k + 2] == 1 && ((unsigned char) pend[k + 3] & 0x1f) == 7) {
                at = (k > 0 && pend[k - 1] == 0) ? k - 1 : k;
                break;
            }
        if (at == std::string::npos) { if (pend.size() > 4) pend.erase(0, pend.size() - 4); return true; }
        started = true;
        asio::write(client, asio::buffer(pend.data() + at, pend.size() - at), e);
        pend.clear();
        return !e;
    };
    std::string acc = buf.substr(he);
    auto more = [&]() -> bool {
        const size_t n = up.read_some(asio::buffer(tmp), ec);
        if (ec) return false;
        acc.append(tmp, n);
        return true;
    };
    if (!chunked) {
        for (;;) {
            if (!acc.empty() && !emit(acc.data(), acc.size())) break;
            acc.clear();
            if (!more()) break;
        }
    } else {
        size_t need = 0;
        bool   alive = true;
        while (alive) {
            if (need == 0) {
                const size_t nl = acc.find("\r\n");
                if (nl == std::string::npos) { if (!more()) break; continue; }
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 2);
                const size_t semi = line.find(';');
                if (semi != std::string::npos) line.resize(semi);
                need = (size_t) std::strtoul(line.c_str(), nullptr, 16);
                if (need == 0) break; // last chunk
                continue;
            }
            if (acc.empty()) { if (!more()) break; continue; }
            const size_t take = std::min(need, acc.size());
            if (!emit(acc.data(), take)) { alive = false; break; }
            acc.erase(0, take);
            need -= take;
            if (need == 0) {
                while (acc.size() < 2 && alive) alive = more();
                if (alive) acc.erase(0, 2); // CRLF after the chunk data
            }
        }
    }
    boost::system::error_code ig;
    up.shutdown(tcp::socket::shutdown_both, ig);
    client.shutdown(tcp::socket::shutdown_both, ig);
}

static void tunnel(tcp::socket& client, int port, const std::string& head, const std::string& pending)
{
    asio::io_context ioc;
    tcp::socket      up(ioc);
    up.connect(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), (unsigned short) port));
    up.set_option(tcp::no_delay(true));
    // A tunnel is long-lived (a camera WebSocket, an MJPEG stream): drop the head deadline, and
    // let TCP keep-alive - not a timeout - be what notices a phone that walked out of range.
    set_read_timeout(client, 0);
    set_write_timeout(client, TUNNEL_WRITE_S);
    set_keepalive(client, 30);
    set_keepalive(up, 30);
    write_all(up, head);
    if (!pending.empty()) write_all(up, pending);
    std::thread t([&]() { pump(client, up); });
    pump(up, client);
    t.join();
}

// Read a small request body (form parameters, JSON).
static bool read_small_body(tcp::socket& client, Request& r, std::string& body, size_t limit)
{
    if (r.content_length > limit) return false;
    body = r.pending;
    set_read_timeout(client, BODY_TIMEOUT_S); // a body that stops arriving must not hold the thread
    boost::system::error_code ec;
    while (body.size() < r.content_length) {
        char   buf[8192];
        size_t n = client.read_some(asio::buffer(buf, std::min(sizeof(buf), r.content_length - body.size())), ec);
        if (ec) return false;
        body.append(buf, n);
    }
    return true;
}

static std::string ff_camera_url_from_detail(const std::string& body)
{
    try {
        json j = json::parse(body);
        if (j.contains("detail") && j["detail"].is_object() && j["detail"].contains("cameraStreamUrl"))
            return j["detail"]["cameraStreamUrl"].get<std::string>();
        if (j.contains("cameraStreamUrl")) return j["cameraStreamUrl"].get<std::string>();
    } catch (...) {}
    return "";
}

// A phone upload: stream the body to <uploads>/<stamp>_<name>. Only model files, only
// a bare file name (X-File-Name, percent-encoded UTF-8), Content-Length required.
static bool spool_upload(tcp::socket& client, Request& r, std::string& out_path, std::string& error)
{
    std::string name = percent_decode(r.file_name);
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::string clean;
    for (unsigned char c : name)
        if (c >= 32 && std::string("<>:\"|?*").find((char) c) == std::string::npos) clean += (char) c;
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.size() > 100) clean = clean.substr(clean.size() - 100);
    if (clean.empty()) { error = "X-File-Name header with the file name is required"; return false; }
    const std::string ext = lower(fs::path(clean).extension().string());
    if (ext != ".3mf" && ext != ".stl" && ext != ".obj" && ext != ".step" && ext != ".stp" &&
        ext != ".glb" && ext != ".gltf") {
        error = "only .3mf, .stl, .obj, .step and .glb files can be opened";
        return false;
    }
    if (r.content_length == 0) { error = "empty upload (Content-Length required)"; return false; }
    if (r.content_length > MAX_UPLOAD) { error = "file too large"; return false; }
    ensure_dirs();
    // One folder per upload so the file keeps its own name (it becomes the object / project name).
    // The stamp is only one-second precise, so a random suffix keeps two uploads in the same
    // second from sharing a folder (the second used to overwrite the first).
    const fs::path folder = fs::path(uploads_dir()) / (timestamp_compact() + "_" + random_hex(4));
    boost::system::error_code ec;
    fs::create_directories(folder, ec);
    out_path = (folder / clean).string();
    boost::nowide::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot write to " + uploads_dir(); return false; }
    set_read_timeout(client, UPLOAD_TIMEOUT_S); // a stalled upload frees its thread and its file
    size_t received = std::min(r.pending.size(), r.content_length);
    f.write(r.pending.data(), (std::streamsize) received);
    char buf[65536];
    while (received < r.content_length) {
        size_t n = client.read_some(asio::buffer(buf, std::min(sizeof(buf), r.content_length - received)), ec);
        if (ec) {
            error = "upload interrupted";
            f.close();
            boost::system::error_code ig;
            fs::remove_all(folder, ig); // no half-file and no empty folder left behind
            out_path.clear();
            return false;
        }
        f.write(buf, (std::streamsize) n);
        received += n;
    }
    f.close();
    return true;
}

// Uploads are scratch space: keep <datadir>/hub/uploads under UPLOAD_QUOTA by deleting whole
// upload folders, oldest first, but never one younger than an hour - a slicer may still be
// opening it. Runs at hub start and after every upload. <datadir>/hub/saves holds the user's own
// projects and is never touched here.
static void gc_uploads()
{
    struct Entry { fs::path dir; long long age_key; uint64_t bytes; };
    std::vector<Entry>        entries;
    uint64_t                  total = 0;
    boost::system::error_code ec;
    if (!fs::is_directory(uploads_dir(), ec)) return;
    for (fs::directory_iterator it(uploads_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code ig;
        if (!fs::is_directory(it->path(), ig)) continue;
        Entry e;
        e.dir      = it->path();
        e.bytes    = 0;
        e.age_key  = (long long) fs::last_write_time(it->path(), ig);
        for (fs::recursive_directory_iterator f(it->path(), ig), fend; !ig && f != fend; f.increment(ig)) {
            boost::system::error_code i2;
            if (fs::is_regular_file(f->path(), i2)) {
                e.bytes += (uint64_t) fs::file_size(f->path(), i2);
                const long long t = (long long) fs::last_write_time(f->path(), i2);
                if (t > e.age_key) e.age_key = t; // a folder is as young as its newest file
            }
        }
        total += e.bytes;
        entries.push_back(e);
    }
    if (total <= UPLOAD_QUOTA) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.age_key < b.age_key; });
    const long long now = now_unix();
    uint64_t        freed = 0;
    for (const Entry& e : entries) {
        if (total - freed <= UPLOAD_QUOTA) break;
        if (now - e.age_key < UPLOAD_MIN_AGE_S) continue; // still in use, or about to be
        boost::system::error_code ig;
        fs::remove_all(e.dir, ig);
        if (!ig) freed += e.bytes;
    }
    if (freed > 0)
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: uploads over quota (" << total / (1024 * 1024) << " MiB), freed "
                                << freed / (1024 * 1024) << " MiB";
    else
        BOOST_LOG_TRIVIAL(warning) << "RemoteHub: uploads over quota (" << total / (1024 * 1024)
                                   << " MiB) but every upload is younger than an hour";
}

// --------------------------------------------------------------- server ----

struct Instance
{
    long        pid { 0 };
    int         port { 0 };
    long long   started { 0 };
    std::string title, path;
    bool        slicing { false };
    bool        hidden { false };
    bool        needs_attention { false };
    std::string attention_reason;
    bool        alive { false };
};

class HubServer
{
public:
    HubServer(std::string token, bool phone) : m_token(std::move(token)), m_phone(phone) {}

    bool start();                 // state, go2rtc, relay, listener, hub.json
    void loop(bool idle_exit);    // until request_quit(); with idle_exit also once nobody needs us
    // Closes the listeners, ends every connection and waits (bounded) for every thread this
    // server started. False when some are still running: the caller must then not destroy the
    // server, because those threads still use it.
    bool shutdown();
    // A detached helper thread that shutdown() knows about and waits for. False once shutting down.
    bool spawn(std::function<void()> fn) { return m_life.spawn(std::move(fn)); }
    // `reason` ends up in last_exit.json ("tray", "request", "idle"); the first one given wins,
    // so the OnExit() that follows a tray quit does not overwrite it.
    void request_quit(const char* reason = nullptr)
    {
        const char* none = nullptr;
        if (reason != nullptr) m_quit_reason.compare_exchange_strong(none, reason);
        m_quit = true;
    }

    struct Snapshot
    {
        bool        phone { false };
        int         port { 0 };
        int         admin_port { 0 };
        std::string token, url;
        int         instances { 0 };
        int         hidden { 0 };
        int         attention { 0 };
    };
    Snapshot snapshot();
    // The two ways a phone can reach this hub. They are different origins - scheme, host and port
    // all differ - so nothing a browser keeps on one (cookie, localStorage, service worker, push
    // subscription) is visible on the other; the page has to be told both and switch deliberately.
    struct PhoneLinks
    {
        std::string lan;                 // http://<lan ip>:<port>/r/<token>/  ("" while phone access is off)
        std::string remote;              // https://<machine>.<tailnet>.ts.net/r/<token>/  ("" while remote is off)
        // The hosted relay from design_hosted_relay.md: a third origin, on the same footing as the
        // other two, that a phone can be handed when neither the LAN nor the tailnet reaches this
        // PC. Nothing hosts it yet - phase 0 only wires the slot so the page, /pair, the app and
        // update_notify_link() below already carry it and phase 1 is purely additive. Always "".
        std::string relay;
        std::vector<std::string> ips;    // the LAN addresses the lan link was picked from
    };
    PhoneLinks phone_links();
    // Off and on again keeps the same link. The token is the hub's, remembered in settings.json,
    // and `token` is only taken up by a hub that has none of its own yet (a fresh data dir):
    // a phone's saved link, QR code or home-screen icon must not die because somebody used the
    // toggle. new_link() is the one thing that replaces it.
    bool        set_phone(bool on, const std::string& token); // rebinds the listener, persists
    std::string new_link(); // rotate the phone token (explicit only); returns the new one
    long     spawn_slicer(const std::string& file, bool hidden = false); // "" = an empty window
    // One request to an instance's loopback API (HTTP: never on the tray's GUI thread).
    std::pair<int, std::string> instance_post(long pid, const std::string& sub);
    bool     instance_window(long pid, bool show);
    bool     instance_quit(long pid, bool discard);
    std::vector<Instance> instances(bool probe);
    int      instances_in_flight(); // live instances plus spawns that have not registered yet

    // The single point where a printer event enters the hub, whoever sent it: it assigns the id
    // and the time, stores it in the ring, mirrors the ring to <datadir>/hub/events.json and shows
    // the tray balloon. Everything that delivers an event onwards hangs off this one function.
    void accept_event(nlohmann::json& event);
    json events_json(int since); // {events: [...], last_id: n} for /hub/events and /r/<token>/events

    // ---- what the native app asks for (EdgeSlicer app phase 3 follow-ups) ----
    // One answer with everything the app's Devices screen draws, built from what THIS process
    // knows: the printer rows it polled off the instances and remembered, the camera wall, the
    // event high-water mark and this hub's own identity. It never needs an open slicer window -
    // that was the whole complaint: with the slicer closed the app could say nothing at all.
    json summary_json();
    json pair_json();                       // the pairing document: names, both URLs, capabilities
    // The last-known status of every printer any instance has reported, newest value per id, each
    // row carrying `age_s` and `stale`. `instance` is the pid that last reported it, or 0 when no
    // window is open any more.
    json printers_json();
    // The running job's picture per printer, from the G-code archive's sidecars. The map form is
    // what /summary uses: the archive is one folder of small JSON files, so it is read ONCE for
    // every printer rather than once per printer. `jobs` says which job each printer is running,
    // so the record whose name matches wins over the merely newest one.
    std::map<std::string, std::string> printer_thumbnail_paths(const std::map<std::string, std::string>& jobs);
    std::string printer_thumbnail_path(const std::string& printer_id); // one printer, same rules
    std::string printer_thumbnail(const std::string& printer_id);      // ... and its bytes
    // This hub's identity: a uuid minted once, when this data dir's hub settings are created, and
    // never again. A client that sees it change knows the event ids restarted - which it otherwise
    // has to infer from an id going backwards (the app's follow-up 6).
    std::string hub_instance();
    void poll_printers();                   // one round: ask every live instance, merge, remember

private:
    void load_events();          // start(): the ring from the last run
    void save_events_locked();   // m_mutex held
    void load_printers();        // start(): the last-known printer status from the last run
    void save_printers_locked(); // m_mutex held
    std::string archive_dir();   // where the G-code archive keeps its sidecars (see m_archive_dir)
public:
    // The G-code archive as the Reprint tab lists it, read by the hub itself: one page of records,
    // newest first, from a cached pass over the sidecars. No slicer window is involved, so the
    // list answers with every window closed and does not wait on a window that is busy.
    json        archive_page_json(const std::string& printer, int offset, int limit, const std::string& model = "");
    // One record's preview, by the record's own id; "" = none.
    std::string archive_thumbnail(const std::string& id);
private:
    std::vector<json> archive_records(); // every record, newest first; cached (see m_archive_cache)
    json  info_json();
    json  instances_json();
    void  write_hub_json();
    void  update_notify_link(); // tell RemoteNotify which link a notification should open
    bool  bind(bool lan);
    bool  bind_admin();
    void  accept_loop(std::shared_ptr<tcp::acceptor> acceptor, bool admin);
    void  serve(std::unique_ptr<tcp::socket> sock, bool admin);
    void  handle_hub(tcp::socket& client, Request& r);
    void  handle_phone(tcp::socket& client, Request& r, const std::string& rest);
    void  start_go2rtc();
    void  register_streams();
    std::string go2rtc_base_locked() const; // http://user:pass@127.0.0.1:port ("" while go2rtc is down); m_mutex held
    std::pair<int, std::string> onvif_discover(); // GET /api/onvif on go2rtc: {status, body}
    std::string state_for_phone();
    bool  lookup_host(const std::string& id, std::string& ip, std::string& code);
    std::string relay_h264_url(const std::string& id); // the U1 raw stream behind /relay/h264?id=, or ""
    FirewallState  firewall_state(bool refresh);       // cached; the query runs on a detached thread
    FirewallState  lan_firewall_state(bool refresh);   // same, but for the phone/LAN listener port
    TailscaleState remote_state(bool refresh);         // cached ~15 s; runs the tailscale CLI off the lock
    TailscaleState remote_state_nowait();              // /hub/info's: never waits on the CLI once there is an answer
    bool  set_remote(bool on, std::string& error);     // tailscale serve on/off for this hub
    void  remote_logins(const std::string& add, const std::string& remove);
    bool  login_allowed(const std::string& login);
    json  remote_json_locked() const;                  // m_mutex held
    void  remember_old_token_locked(const std::string& token); // m_mutex held
    static Instance probe_instance(Instance inst);

    std::mutex                     m_mutex;
    std::string                    m_token;
    // The last few links this hub replaced. They authenticate nothing - they exist so a phone
    // that still has one is told what happened instead of getting a bare 404.
    std::vector<std::string>       m_old_tokens;
    bool                           m_phone { false };
    bool                           m_lan { false };
    int                            m_port { 0 };
    // The io_context every listener and connection socket belongs to, shared with the threads
    // that use them, and the count of those threads - so shutdown() can end them and wait, and a
    // socket can never outlive its io_context (crash a2375b06). Declared before the acceptors so
    // they are destroyed first.
    ServerLifetime                 m_life;
    std::shared_ptr<tcp::acceptor> m_acceptor;
    // The control plane has its own acceptor on an ephemeral loopback port, recorded in hub.json
    // as admin_port. Nothing else binds it, so no tunnel (Tailscale Serve, zrok, anything a user
    // starts by hand) can ever front /hub/* or the U1 relay, whatever the peer address says.
    std::shared_ptr<tcp::acceptor> m_admin_acceptor;
    int                            m_admin_port { 0 };
    // Spawns that have not dropped their <pid>.json yet, so a burst of phone uploads cannot get
    // past the instance cap while the first process is still starting.
    std::vector<std::pair<long, long long>> m_recent_spawns;
    std::string                    m_state; // full Stream-tab state JSON (with credentials)
    std::string                    m_secret;      // per run; in hub.json and the hub page, required as X-Hub-Secret on /hub/*
    // This data dir's durable relay identity (design_hosted_relay.md section 2): an Ed25519 key
    // pair minted once and kept in settings.json, plus the hubid derived from the public half.
    // m_identity.private_hex is the only secret in this process that must never be serialised
    // anywhere a phone, a page or a log can see - write_hub_json() is the single writer.
    Testing::HubIdentity           m_identity;
    std::string                    m_go2rtc_user, m_go2rtc_pass, m_go2rtc_auth; // go2rtc credentials, this process only
    bool                           m_remote_on { false };        // publish through Tailscale Serve (persisted)
    std::vector<std::string>       m_allowed_logins;             // tailnet logins that may connect (lower-case, persisted)
    TailscaleState                 m_ts;                         // last tailscale_query()
    std::string                    m_last_login;                 // most recent remote visitor
    long long                      m_last_login_at { 0 };
    int                            m_go2rtc_port { 0 };
    int                            m_webrtc_port { 0 };          // go2rtc's WebRTC media port (0 = WebRTC off)
    FirewallState                  m_fw;                         // last firewall_query() (go2rtc/WebRTC)
    std::atomic<bool>              m_fw_busy { false };
    FirewallState                  m_lan_fw;                     // last lan_firewall_state() (the phone/LAN listener port)
    std::atomic<bool>              m_lan_fw_busy { false };
    std::atomic<bool>              m_ts_busy { false };          // a remote_state_nowait() refresh is running
    // Set by bind() when it had to step past HUB_PORT: what (if anything) was found holding it,
    // for the status JSON's "port_note" and the hub page's warning. Empty once the hub is on
    // HUB_PORT itself.
    std::string                    m_port_note_holder;    // "<exe> (pid N)" or "" (nothing found / not applicable)
    long                           m_go2rtc_pid { 0 };
    void*                          m_job { nullptr };
    std::atomic<bool>              m_quit { false };
    std::atomic<const char*>       m_quit_reason { nullptr }; // a string literal, see request_quit()
    // Printer events, newest last. The hub owns the sequence, so ids keep increasing across every
    // instance that reports and across a restart of the hub itself.
    std::deque<json>               m_events;
    int                            m_next_event_id { 0 };
    // The printers, as the instances last reported them: printer id -> the row plus when it
    // arrived and which window sent it. This is what makes the phone app's Devices screen work
    // with the slicer closed; it is refreshed by poll_printers() and persisted to printers.json.
    struct CachedPrinter
    {
        json      row;             // the instance's own /api/printers row, as it sent it
        long long at { 0 };        // unix ms when this value was read
        long      instance { 0 };  // the pid that reported it; 0 once that window is gone
    };
    std::map<std::string, CachedPrinter> m_printers;
    // Where the archive's sidecars are, as an instance reported it (GET /api/info). Remembered so
    // a thumbnail still resolves after the window that told us closed; empty falls back to
    // <datadir>/gcode_archive, which is the archive's own default.
    std::string                    m_archive_dir;
    // What the instances last said about the archive (/api/info): on (1) or off (0), -1 before any
    // window has said; and how many records it keeps. Read again once a minute.
    int                            m_archive_enabled { -1 };
    int                            m_archive_max { 0 };
    long long                      m_archive_info_at { 0 };
    // The parsed sidecars, and what the folder looked like when they were read (its entry count
    // and its newest write time). A list request reuses them until the folder changes or they
    // are ARCHIVE_CACHE_MS old, so opening the Reprint tab is one small read, not a hundred.
    std::vector<json>              m_archive_cache;
    std::string                    m_archive_cache_sig;
    long long                      m_archive_cache_at { 0 };
    std::mutex                     m_archive_mutex; // the cache only; never held with m_mutex
    std::string                    m_hub_instance; // this data dir's hub uuid (settings.json)
    // How many phone links this data dir has ever had, 1 for the first. It has to be its own
    // counter rather than the number of remembered old tokens, because that list is capped at
    // three: a client comparing versions must see a number that only ever goes up.
    int                            m_token_version { 1 };
};

// The tray balloon, set by HubApp once the icon exists (the server itself is wx-free and runs on
// its own thread; the hook marshals to the GUI thread itself). Left empty in a headless hub, and
// then an event simply is not shown on the PC. Its own lock: the listener is already accepting by
// the time the icon is made, so setting and calling it really can race.
using BalloonFn = std::function<void(const std::string& title, const std::string& text, const std::string& severity)>;
static std::mutex s_balloon_mutex;
static BalloonFn  s_balloon;

static void set_balloon(BalloonFn fn)
{
    std::lock_guard<std::mutex> lock(s_balloon_mutex);
    s_balloon = std::move(fn);
}

static BalloonFn balloon_fn()
{
    std::lock_guard<std::mutex> lock(s_balloon_mutex);
    return s_balloon;
}

json HubServer::info_json()
{
    const FirewallState fw     = firewall_state(false);     // takes m_mutex itself: before the lock below
    const FirewallState lan_fw = lan_firewall_state(false); // ditto
    json j;
    std::lock_guard<std::mutex> lock(m_mutex);
    json v;
    v["webrtc_port"]  = m_webrtc_port;
    v["firewall"]     = m_webrtc_port ? fw.state : std::string("off");
    v["note"]         = m_webrtc_port ? fw.note : std::string("No free port for WebRTC video; the phone uses relayed video.");
    v["command"]      = m_webrtc_port ? fw.command : std::string();
    v["networks"]     = fw.networks;
    v["go2rtc_exe"]   = go2rtc_exe_path();
    j["video"]       = v;
    j["alive"]       = true;
    j["pid"]         = current_pid();
    j["port"]        = m_port;
    j["admin_port"]  = m_admin_port;
    j["phone"]       = m_phone;
    j["token"]       = m_token;
    j["go2rtc_port"] = m_go2rtc_port;
    j["relay_port"]  = BambuCamRelay::get().port();
    j["version"]     = std::string(SLIC3R_VERSION);
    j["remote"]      = remote_json_locked();
    j["ips"]         = json::array();
    j["url"]         = "";
    if (m_phone) {
        const std::vector<std::string> ips = lan_ips();
        j["ips"] = ips;
        if (!ips.empty()) j["url"] = "http://" + ips.front() + ":" + std::to_string(m_port) + "/r/" + m_token + "/";
    }
    // Named for what the phone page does with them. `url` is kept as it was (the hub page and the
    // tray read it); lan_url is the same string, remote_url is the Tailscale one or empty.
    j["lan_url"]    = j["url"];
    j["remote_url"] = j["remote"].is_object() ? j["remote"].value("url", std::string()) : std::string();
    // The third origin and this hub's durable name for it. Empty and inert in phase 0; here now so
    // the hub page, the tray and the app all learn the shape before anything fills it in.
    j["relay_url"]  = std::string();
    j["hubid"]      = m_identity.hubid;
    j["public_key"] = m_identity.public_hex; // the public half only - see write_hub_json()
    // Set only when bind() had to step past HUB_PORT, so a hub sitting on 13640 as usual sends no
    // port_note at all. `held_by` is "" when the probe could not tell what has it (gone by the
    // time we looked, or a non-Windows build).
    if (m_port != HUB_PORT) {
        json note;
        note["default_port"] = HUB_PORT;
        note["port"]         = m_port;
        note["held_by"]      = m_port_note_holder;
        // The netsh line hub.html shows behind "Show command" if the user wants to allow this
        // build through the firewall rather than stop the other program - same port-range
        // convention as lan_firewall_state()'s hint below.
        note["command"] = "netsh advfirewall firewall add rule name=\"EdgeSlicer\" dir=in action=allow program=\"" +
                           current_exe() + "\" protocol=TCP localport=" + std::to_string(HUB_PORT) + "-" +
                           std::to_string(HUB_PORT + 19) + " profile=private,domain";
        j["port_note"] = note;
    }
    // The phone/LAN listener's own firewall reachability, same shape as video.firewall/note above
    // (state values: allowed | partial | missing | blocked | unknown), plus the exact netsh command
    // an administrator can run - the hub only ever looks, never runs `netsh ... add` itself.
    json lv;
    lv["port"]     = m_port;
    lv["firewall"] = (m_phone && m_lan) ? lan_fw.state : std::string("off");
    lv["note"]     = (m_phone && m_lan) ? lan_fw.note : std::string();
    lv["command"]  = (m_phone && m_lan) ? lan_fw.command : std::string();
    lv["networks"] = lan_fw.networks;
    j["lan_firewall"] = lv;
    return j;
}

void HubServer::write_hub_json()
{
    json j;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        j["pid"]         = current_pid();
        j["port"]        = m_port;
        j["admin_port"]  = m_admin_port; // loopback-only control plane; this is what /hub/* answers on
        j["phone"]       = m_phone;
        j["token"]       = m_token;
        j["secret"]      = m_secret;
        j["go2rtc_port"] = m_go2rtc_port;
        j["webrtc_port"] = m_webrtc_port;
        j["version"]     = std::string(SLIC3R_VERSION);
        j["remote_on"]      = m_remote_on;
        j["allowed_logins"] = m_allowed_logins;
    }
    write_file(hub_json_path(), j.dump(2));
    json st;
    st["remote_on"]      = j["remote_on"];
    st["allowed_logins"] = j["allowed_logins"];
    // The phone switch too. It used to live only in hub.json, which a clean quit deletes, so
    // the next slicer start re-spawned the hub from ITS remembered copy of the switch - stale
    // whenever the last change was made from the hub page - and "home mode off" never stuck.
    st["phone"]          = j["phone"];
    {
        // hub.json is deleted on a clean quit; the phones' link has to outlive it, or every hub
        // restart would hand out a new one and kill every saved link and home-screen icon.
        std::lock_guard<std::mutex> lock(m_mutex);
        st["token"]      = m_token;
        st["old_tokens"] = m_old_tokens;
        st["hub_instance"] = m_hub_instance; // written once, then carried forward unchanged
        st["token_version"] = m_token_version;
        // The relay identity, private half included. settings.json is this hub's own file on its
        // own data dir, and it already holds the phone token and the VAPID private key for exactly
        // the same reason: losing it would silently break every client that ever trusted this hub.
        // hub.json deliberately does NOT get a copy - that file is the one instances and the page
        // read, and the private key has no business being anywhere they can see it.
        if (m_identity.valid()) st["identity"] = json::parse(Testing::identity_settings_dump(m_identity));
    }
    // The notification destinations live here too, and for the same reason: a hub restart must
    // not lose the relay somebody set up. RemoteNotify owns them; this is the only writer of the
    // file, so it asks for them rather than keeping a second copy.
    st["notify"] = RemoteNotify::settings_json();
    // Web Push lives here for the same reason and one more: the VAPID key pair *is* this hub's
    // identity to every phone that ever subscribed, so losing it would silently break all of them.
    st["webpush"] = WebPush::settings_json();
    // The native app's device rows and the two providers' settings, for the same reason again: a
    // hub restart must not make every paired phone re-register, and the .p8 path and the service
    // account path are the person's configuration, not ours to forget.
    st["apppush"] = AppPush::settings_json();
    write_file(settings_json_path(), st.dump(2));
    update_notify_link();
}

// Both links, whenever anything that decides them changes. lan_ips() opens a socket and resolves
// this PC's own name, so it is never called under the lock.
HubServer::PhoneLinks HubServer::phone_links()
{
    bool phone;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        phone = m_phone;
    }
    PhoneLinks l;
    if (phone) l.ips = lan_ips();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (phone && !l.ips.empty()) l.lan = "http://" + l.ips.front() + ":" + std::to_string(m_port) + "/r/" + m_token + "/";
    if (m_remote_on && m_ts.serving && !m_ts.dns_name.empty()) l.remote = "https://" + m_ts.dns_name + "/r/" + m_token + "/";
    // l.relay stays empty in phase 0: there is no relay to name one against yet. Everything
    // downstream already treats an empty link as "this path does not exist", so nothing shows.
    return l;
}

// The link a notification should open on the phone. The Tailscale one first, because it works
// from anywhere; the Wi-Fi one otherwise; nothing at all while phone access is off, and then the
// notifier simply gets no link rather than one that cannot resolve. Both are handed over now: a
// notification carries the pair, and the phone opens whichever one its owner prefers.
//
// The order design_hosted_relay.md section 2 settles on is tailnet -> relay -> LAN: the tailnet
// wins on merit (free, and WireGuard-encrypted end to end), the relay is the paid fallback, the
// LAN is only reachable from the house. l.relay is empty in phase 0, so the middle rung is a
// no-op today and slotting it in later changes nothing else.
void HubServer::update_notify_link()
{
    const PhoneLinks l = phone_links();
    RemoteNotify::set_phone_links(l.remote, l.lan);
    WebPush::set_phone_links(l.remote, l.lan);
}

// ------------------------------------------------------- printer events ----

// A short string a phone or a balloon can show, and nothing longer: an event comes from a printer
// (an HMS message, a Klipper error, a file name) and none of that may become a way to push a wall
// of text into the hub's memory, its state file or a notification.
static std::string clip(const json& j, const char* key, size_t limit)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return std::string();
    std::string s = j[key].get<std::string>();
    if (s.size() > limit) s.resize(limit);
    // Control characters would break the balloon and are never meaningful here.
    for (char& c : s)
        if ((unsigned char) c < 0x20) c = ' ';
    return s;
}

static bool one_of(const std::string& v, std::initializer_list<const char*> allowed)
{
    for (const char* a : allowed)
        if (v == a) return true;
    return false;
}

// This process's claims on the printers whose notifications it sends (HubPushOwner). One per
// process: there is one hub per process, and the claims live exactly as long as it does.
static HubPushOwner& push_owner()
{
    static HubPushOwner owner;
    return owner;
}

void HubServer::accept_event(json& event)
{
    // Normalise before anything else: the kinds and severities are a closed set (the P4/P5
    // contract), and everything else is text with a length. An event that names a kind nobody
    // knows becomes "error"/"warning" rather than being passed through as written.
    json e;
    const std::string kind     = clip(event, "kind", 24);
    const std::string severity = clip(event, "severity", 12);
    e["kind"]     = one_of(kind, { "started", "finished", "failed", "cancelled", "paused", "resumed", "runout", "error" }) ? kind : "error";
    e["severity"] = one_of(severity, { "info", "warning", "error" }) ? severity : "warning";
    e["title"]    = clip(event, "title", 200);
    e["text"]     = clip(event, "text", 500);
    const std::string code = clip(event, "code", 64), job = clip(event, "job", 200);
    if (!code.empty()) e["code"] = code;
    if (!job.empty()) e["job"] = job;
    // The printer's own job id, when the watcher had one: it is what tells "the same print, seen
    // again" from "the same file, printed again" (HubEventDedupe), and the app keeps it.
    {
        const std::string job_id = clip(event, "job_id", 64);
        if (!job_id.empty() && job_id != "0") e["job_id"] = job_id;
    }
    e["instance"] = event.is_object() && event.contains("instance") && event["instance"].is_number_integer()
                        ? event["instance"].get<long long>() : 0LL;
    json p = json::object();
    if (event.is_object() && event.contains("printer") && event["printer"].is_object()) {
        const json& in = event["printer"];
        p["id"]   = clip(in, "id", 128);
        p["name"] = clip(in, "name", 128);
        const std::string k = clip(in, "kind", 24);
        p["kind"] = one_of(k, { "bambu", "snapmaker", "printhost", "connect" }) ? k : "printhost";
    } else {
        p = json { { "id", "" }, { "name", "" }, { "kind", "printhost" } };
    }
    e["printer"] = p;

    std::string title, text, sev;
    int         id = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const long long now_ms = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count();
        // Every slicer window watches the same printers and each one reports what it sees, so a
        // print start arrived once per open window ("started" twice, 10 s apart, 2026-09-06), and
        // an HMS code once per window that had a LAN session at the time. HubEventDedupe says
        // whether this is one the ring already holds - one start per job until the job ends, one
        // error per code for half an hour, the rest inside two minutes, and only across windows:
        // a window's own watcher reports edges, so a repeat from the same window is real. The
        // answer is the stored event, and nothing is sent again.
        const std::string pid = p.value("id", std::string());
        const int         dup = HubEventDedupe::find_duplicate(m_events, e, now_ms);
        if (dup >= 0) {
            event = m_events[(size_t) dup];
            if (!event.contains("uid") && event.contains("id") && event["id"].is_number_integer())
                event["uid"] = HubEventDedupe::uid(m_hub_instance, event["id"].get<long long>());
            BOOST_LOG_TRIVIAL(info) << "RemoteHub: event " << e["kind"].get<std::string>() << " on " << pid
                                    << " repeats stored event " << event.value("id", 0) << " from another window - not stored or sent again";
            return;
        }
        e["id"]   = ++m_next_event_id;
        e["time"] = now_ms;
        e["uid"]  = HubEventDedupe::uid(m_hub_instance, e["id"].get<long long>());
        m_events.push_back(e);
        while (m_events.size() > MAX_EVENTS) m_events.pop_front();
        save_events_locked();
        id    = e["id"].get<int>();
        title = e["title"].get<std::string>();
        text  = e["text"].get<std::string>();
        sev   = e["severity"].get<std::string>();
    }
    event = e; // the caller answers with what was really stored

    // P5 (relay notifications) sends from here: one call with `e`, off this thread - unless another
    // hub on this PC already sends this printer's notifications (HubPushOwner). The event stays in
    // this hub's ring either way; only the delivery is skipped, so one printer is one notification
    // however many EdgeSlicer data dirs are running.
    update_notify_link();
    bool              deliver_it = true;
    const std::string printer_id = p.value("id", std::string());
    if (HubPushOwner::claimable_kind(p.value("kind", std::string())) && !printer_id.empty() && RemoteNotify::has_destinations()) {
        deliver_it = push_owner().claim(printer_id);
        if (!deliver_it)
            BOOST_LOG_TRIVIAL(info) << "RemoteHub: event " << id << " on " << printer_id
                                    << " stored, not sent: another EdgeSlicer hub on this PC sends that printer's notifications";
    }
    if (deliver_it) RemoteNotify::deliver(e); // queued; this thread never waits on a relay

    // On the PC: anything that went wrong, and the one piece of good news worth interrupting for.
    if (deliver_it && (sev == "warning" || sev == "error" || e["kind"] == "finished"))
        if (BalloonFn show = balloon_fn()) show(title, text, sev);
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: event " << id << " " << e["kind"].get<std::string>() << " on "
                            << p.value("id", std::string()) << ": " << title;
}

json HubServer::events_json(int since)
{
    json out;
    out["events"] = json::array();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const json& e : m_events)
        if (e.value("id", 0) > since) {
            json row = e;
            // Stored before events carried one: the same value the hub would have given it.
            if (!row.contains("uid") && row.contains("id") && row["id"].is_number_integer())
                row["uid"] = HubEventDedupe::uid(m_hub_instance, row["id"].get<long long>());
            out["events"].push_back(row);
        }
    out["last_id"] = m_next_event_id;
    // Which hub these ids belong to. A data dir's hub keeps this for ever; a fresh one mints a new
    // value, and that - not an id that went backwards - is how a client detects the reset.
    out["hub_instance"] = m_hub_instance;
    return out;
}

void HubServer::save_events_locked()
{
    json j;
    j["last_id"] = m_next_event_id;
    j["events"]  = json::array();
    for (const json& e : m_events) j["events"].push_back(e);
    write_file(events_json_path(), j.dump());
}

// ------------------------------------------------ the printers the hub remembers ----
//
// Everything below exists for one reason: with no slicer window open the phone app could say
// nothing about the printers at all, because /i/<pid>/api/printers needs a live instance. The hub
// already hears about the printers (it is where the instances' events land), so it now also keeps
// what they last reported: one row per printer id, the time it arrived and the window that sent
// it. /summary and /state serve those rows whether or not a window is open, and a row nobody has
// refreshed lately is handed over with its age and a `stale` flag rather than withheld - "this is
// what it was last doing, 40 minutes ago" is exactly the answer a person wants.

// The PC's own name, as a person recognises it in a list of hubs. Cached: the resolver call is
// not free and this never changes while we run.
static std::string pc_host_name()
{
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, []() {
        try { cached = asio::ip::host_name(); } catch (...) {}
#ifdef _WIN32
        if (cached.empty()) { const char* c = std::getenv("COMPUTERNAME"); if (c) cached = c; }
#endif
        if (cached.empty()) cached = "this PC";
    });
    return cached;
}

// What this hub goes by on the phone. The product name plus the PC, which is what distinguishes
// two hubs in the app's list; the hub has no name a person can set (yet), so it is derived.
static std::string hub_name()
{
    return std::string(SLIC3R_APP_NAME) + " on " + pc_host_name();
}

// The job a printer row says it is running. Which field that is depends on the printer: a Bambu
// row has `task` (MachineObject::subtask_name), while a print host - Klipper, Moonraker, PrusaLink -
// has it in `stage`, where fill_from_print_stats puts print_stats.filename. A summary that only
// looked at `task` would show no job for every print host, which is most of them.
static std::string row_job(const nlohmann::json& row)
{
    if (!row.is_object()) return "";
    std::string job = row.value("task", std::string());
    if (job.empty()) job = row.value("subtask_name", std::string());
    if (job.empty()) job = row.value("stage", std::string());
    return job;
}

static long long now_millis()
{
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// One round of the poll: ask each live instance for its printers, merge what comes back into the
// cache and write it down. Runs on the hub's own loop thread, never on a request; an instance that
// is slicing may take a moment to answer and nothing here may hold m_mutex across that.
void HubServer::poll_printers()
{
    const std::vector<Instance> live = instances(false);
    for (const Instance& inst : live) {
        if (inst.port <= 0) continue;
        std::string body;
        int         status = 0;
        Http::get("http://127.0.0.1:" + std::to_string(inst.port) + "/api/printers")
            .timeout_connect(1).timeout_max(10)
            .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
            .on_error([&](std::string, std::string, unsigned) {})
            .perform_sync();
        if (status != 200) continue;
        json rows;
        try { rows = json::parse(body); } catch (...) { continue; }
        if (!rows.is_object() || !rows.contains("printers") || !rows["printers"].is_array()) continue;
        const long long at = now_millis();
        std::lock_guard<std::mutex> lock(m_mutex);
        std::set<std::string> reported;
        for (const json& row : rows["printers"]) {
            if (!row.is_object()) continue;
            const std::string id = row.value("id", std::string());
            if (id.empty() || id.size() > 200) continue;
            CachedPrinter& c = m_printers[id];
            c.row      = row;
            c.at       = at;
            c.instance = inst.pid;
            reported.insert(id);
        }
        // A Snapmaker row this window (or the hub's previous run) reported and no longer does is
        // gone, not stale: the Device tab's connect card after a disconnect, or one that now gives
        // way to the printer's LAN card, or a LAN entry that was a duplicate. Kept, it sat in the
        // app for hours as a stale "Snapmaker a1pr8y...amazonaws.com" card. Bambu and print-host
        // rows keep the old rule (shown as stale) - a Bambu printer can drop out of the device list
        // for a moment.
        for (auto it = m_printers.begin(); it != m_printers.end();) {
            const std::string kind = it->second.row.value("kind", std::string());
            if ((kind == "connect" || kind == "snapmaker") && !reported.count(it->first) &&
                (it->second.instance == inst.pid || it->second.instance == 0))
                it = m_printers.erase(it);
            else
                ++it;
        }
        // A hub that ran for weeks with a changing fleet must not grow without bound: keep the
        // most recently seen rows and drop the oldest.
        while (m_printers.size() > MAX_CACHED_PRINTERS) {
            auto oldest = m_printers.begin();
            for (auto it = m_printers.begin(); it != m_printers.end(); ++it)
                if (it->second.at < oldest->second.at) oldest = it;
            m_printers.erase(oldest);
        }
        save_printers_locked();
    }
    // Where the archive keeps its sidecars, so a thumbnail still resolves once every window is
    // closed. Asked once per round off the same instances, and only while we do not have it.
    bool need_dir;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        need_dir = m_archive_dir.empty() || now_millis() - m_archive_info_at > 60000;
    }
    if (need_dir) {
        for (const Instance& inst : live) {
            if (inst.port <= 0) continue;
            std::string body;
            Http::get("http://127.0.0.1:" + std::to_string(inst.port) + "/api/info")
                .timeout_connect(1).timeout_max(5)
                .on_complete([&](std::string b, unsigned s) { if (s == 200) body = b; })
                .on_error([&](std::string, std::string, unsigned) {})
                .perform_sync();
            std::string dir;
            int         enabled = -1, max = 0;
            try {
                const json info = json::parse(body);
                dir             = info.value("archive_dir", std::string());
                if (info.contains("archive_enabled") && info["archive_enabled"].is_boolean())
                    enabled = info["archive_enabled"].get<bool>() ? 1 : 0;
                max = info.value("archive_max", 0);
            } catch (...) {}
            if (dir.empty()) continue;
            std::lock_guard<std::mutex> lock(m_mutex);
            const bool moved  = m_archive_dir != dir;
            m_archive_dir     = dir;
            m_archive_enabled = enabled;
            m_archive_max     = max;
            m_archive_info_at = now_millis();
            if (moved) save_printers_locked();
            break;
        }
    }
}

// The rows, as the phone and the app get them. `age_s` is how long ago the value was read and
// `stale` says whether that is longer than the poll can explain; `instance` is the window that can
// still be asked to act on this printer (0 = none open, and then the app knows not to offer the
// control buttons rather than offering ones that would 404).
json HubServer::printers_json()
{
    const std::vector<Instance> live = instances(false);
    const long long             now  = now_millis();
    std::lock_guard<std::mutex> lock(m_mutex);
    json out = json::array();
    for (const auto& kv : m_printers) {
        json row = kv.second.row;
        row["id"] = kv.first;
        const long long age_ms = now > kv.second.at ? now - kv.second.at : 0;
        row["age_s"]  = (long long) (age_ms / 1000);
        row["stale"]  = age_ms > PRINTERS_STALE_MS;
        row["seen_at"] = kv.second.at;
        // The pid only stays in the row while that window is still there: the app addresses the
        // control route as /i/<pid>/..., so a pid that has gone would be worse than none.
        long pid = 0;
        for (const Instance& i : live)
            if (i.pid == kv.second.instance) pid = i.pid;
        row["instance"] = pid;
        out.push_back(row);
    }
    return out;
}

void HubServer::save_printers_locked()
{
    json j;
    j["printers"] = json::array();
    for (const auto& kv : m_printers) {
        json e;
        e["id"]       = kv.first;
        e["row"]      = kv.second.row;
        e["at"]       = kv.second.at;
        e["instance"] = (long long) kv.second.instance;
        j["printers"].push_back(e);
    }
    if (!m_archive_dir.empty()) j["archive_dir"] = m_archive_dir;
    write_file(printers_json_path(), j.dump());
}

// What the printers were doing when this hub last ran. Served straight away (with an age that says
// how old it is), so the app's first refresh after a PC reboot is not blank.
void HubServer::load_printers()
{
    try {
        json j = json::parse(read_file(printers_json_path()));
        for (const auto& e : j.value("printers", json::array())) {
            if (!e.is_object()) continue;
            const std::string id = e.value("id", std::string());
            if (id.empty() || !e.contains("row") || !e["row"].is_object()) continue;
            CachedPrinter c;
            c.row      = e["row"];
            c.at       = e.value("at", (long long) 0);
            c.instance = 0; // whatever window reported it last time is certainly gone
            m_printers[id] = c;
        }
        m_archive_dir = j.value("archive_dir", std::string());
    } catch (...) {}
}

std::string HubServer::archive_dir()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_archive_dir.empty()) return m_archive_dir;
    }
    return (fs::path(data_dir()) / "gcode_archive").string();
}

// ---------------------------------------------- the G-code archive, for the Reprint tab ----
//
// The Reprint tab used to list the archive through a slicer window: the page asked the hub for
// the windows (/api/instances, which probes every one of them), then asked the first one for
// /api/archive, then fetched every preview through the same window. With no window open it said
// so and listed nothing; with a busy one it waited on it; and it drew nothing at all until every
// step had answered. The sidecars are plain JSON files in a folder the hub already knows
// (archive_dir(), the same one the printer thumbnails read), so the hub lists them itself.

static const long long ARCHIVE_CACHE_MS = 30000;

// An archive record id: what GcodeArchive::sanitize() produces. Checked before it goes near a
// file name, the same alphabet the instance's own routes accept.
static bool archive_id_ok(const std::string& id)
{
    if (id.empty() || id.size() > 200 || id[0] == '.') return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return false;
    return true;
}

std::vector<json> HubServer::archive_records()
{
    const fs::path            root(archive_dir());
    boost::system::error_code ec;
    // What the folder looks like right now, cheaply: how many entries and the newest write time.
    // A new record, a deleted one or a rewritten sidecar all move one of the two.
    std::string sig = root.string();
    {
        size_t      n      = 0;
        std::time_t newest = fs::is_directory(root, ec) ? fs::last_write_time(root, ec) : 0;
        if (fs::is_directory(root, ec))
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                ++n;
                boost::system::error_code ig;
                const std::time_t t = fs::last_write_time(it->path(), ig);
                if (!ig && t > newest) newest = t;
            }
        sig += "|" + std::to_string(n) + "|" + std::to_string((long long) newest);
    }
    {
        std::lock_guard<std::mutex> lock(m_archive_mutex);
        if (sig == m_archive_cache_sig && now_millis() - m_archive_cache_at < ARCHIVE_CACHE_MS) return m_archive_cache;
    }
    std::vector<json> out;
    if (fs::is_directory(root, ec)) {
        // A record's printer as the phone should see it (GcodeArchive::normalize_printer): a U1 send
        // filed under the Snapmaker cloud's broker is listed under its LAN card when its serial names
        // one, and no printer is ever called by an address. The chips below are built from the same.
        const std::vector<SnapmakerLan::Device> lan = SnapmakerLan::devices();
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            boost::system::error_code ig;
            if (it->path().extension() != ".json" || !fs::is_regular_file(it->path(), ig)) continue;
            json j;
            try { j = json::parse(read_file(it->path().string())); } catch (...) { continue; }
            if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) continue;
            const std::string id = j["id"].get<std::string>();
            if (!archive_id_ok(id)) continue;
            GcodeArchive::normalize_printer(j, lan);
            // Never a path on the PC, exactly as the instance's /api/archive hands them out.
            j.erase("path");
            j.erase("project_path");
            const std::string file = j.value("file", std::string());
            j["has_thumbnail"] = fs::is_regular_file(root / (id + ".png"), ig);
            j["exists"]        = !file.empty() && file.find_first_of("/\\") == std::string::npos &&
                          fs::is_regular_file(root / file, ig);
            out.push_back(std::move(j));
        }
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        const long long ta = a.value("time", (long long) 0), tb = b.value("time", (long long) 0);
        return ta != tb ? ta > tb : a.value("id", std::string()) > b.value("id", std::string());
    });
    // A record that lacks its printer's model (or names the printer by its serial) takes them from
    // the printer's other records, then every record says which model it was sliced for: the phone
    // groups and filters by that, and offers every printer of the model as a target.
    GcodeArchive::annotate_models(out, GcodeArchive::model_names());
    std::lock_guard<std::mutex> lock(m_archive_mutex);
    m_archive_cache     = out;
    m_archive_cache_sig = sig;
    m_archive_cache_at  = now_millis();
    return out;
}

json HubServer::archive_page_json(const std::string& printer, int offset, int limit, const std::string& model)
{
    const std::vector<json> all = archive_records();
    json                    j;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_archive_enabled >= 0) j["enabled"] = m_archive_enabled == 1;
        if (m_archive_max > 0) j["max"] = m_archive_max;
    }
    // Every printer the records went to, in list order: the page's filter chips, which have to
    // know about printers whose records are not on the page it has loaded yet.
    json                  printers = json::array();
    std::set<std::string> seen;
    std::vector<const json*> rows;
    for (const json& r : all) {
        const json        p  = r.contains("printer") && r["printer"].is_object() ? r["printer"] : json::object();
        const std::string id = p.value("id", std::string());
        if (!id.empty() && seen.insert(id).second)
            printers.push_back({ { "id", id }, { "name", p.value("name", std::string()) }, { "kind", p.value("kind", std::string()) } });
        const std::string mk = r.value("model_key", std::string("other"));
        if ((printer.empty() || id == printer) && (model.empty() || mk == model)) rows.push_back(&r);
    }
    // And every model the records were sliced for (the app's chips), "Other" last, each with the
    // printers this hub knows of that model: where a record of it may be reprinted.
    j["models"] = GcodeArchive::archive_models(all, printers_json(), GcodeArchive::model_names());
    offset = std::max(0, offset);
    limit  = std::max(1, std::min(200, limit));
    j["records"] = json::array();
    for (size_t i = (size_t) offset; i < rows.size() && i < (size_t) offset + (size_t) limit; ++i) j["records"].push_back(*rows[i]);
    j["total"]       = (long long) rows.size();
    j["offset"]      = offset;
    j["limit"]       = limit;
    j["next_offset"] = (size_t) offset + (size_t) limit < rows.size() ? json((long long) offset + limit) : json(nullptr);
    j["printers"]    = printers;
    j["source"]      = "hub";
    return j;
}

std::string HubServer::archive_thumbnail(const std::string& id)
{
    if (!archive_id_ok(id)) return "";
    const fs::path            png = fs::path(archive_dir()) / (id + ".png");
    boost::system::error_code ec;
    if (!fs::is_regular_file(png, ec)) return "";
    if (fs::file_size(png, ec) > 8u * 1024 * 1024) return ""; // a preview, not an arbitrary file
    return read_file(png.string());
}

// The running job's picture, from the G-code archive: the hub reads the sidecars itself (they are
// plain JSON next to a <id>.png), so this works with every slicer window closed - which is the
// case the app asked about. Preference goes to the record whose sent name is the job the printer
// says it is running; failing that, the newest record for that printer, which is what a person
// looking at a card would expect. "" when there is nothing to show, and the caller answers 404.
std::map<std::string, std::string> HubServer::printer_thumbnail_paths(const std::map<std::string, std::string>& jobs)
{
    struct Pick { std::string best, named; long long best_t { -1 }, named_t { -1 }; };
    std::map<std::string, Pick>        picks;
    std::map<std::string, std::string> out;

    const fs::path            root(archive_dir());
    boost::system::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code ig;
        if (!fs::is_regular_file(it->path(), ig) || it->path().extension() != ".json") continue;
        json j;
        try { j = json::parse(read_file(it->path().string())); } catch (...) { continue; }
        if (!j.is_object() || !j.contains("printer") || !j["printer"].is_object()) continue;
        const std::string printer = j["printer"].value("id", std::string());
        const std::string id      = j.value("id", std::string());
        if (printer.empty() || id.empty()) continue;
        // The picture is a sibling of the sidecar, named after the record. Nothing from a request
        // is involved in building this path.
        const fs::path png = root / (id + ".png");
        if (!fs::is_regular_file(png, ig)) continue;
        const long long t = j.value("time", (long long) 0);
        Pick& p = picks[printer];
        if (t > p.best_t) { p.best_t = t; p.best = png.string(); }
        // The job the printer says it is running, matched against the name the printer was really
        // given and against the archived file name: either can be what it reports.
        const auto job = jobs.find(printer);
        if (job != jobs.end() && !job->second.empty() &&
            (j.value("sent_name", std::string()) == job->second || j.value("file", std::string()) == job->second))
            if (t > p.named_t) { p.named_t = t; p.named = png.string(); }
    }
    for (const auto& kv : picks)
        out[kv.first] = kv.second.named.empty() ? kv.second.best : kv.second.named;
    return out;
}

std::string HubServer::printer_thumbnail_path(const std::string& printer_id)
{
    if (printer_id.empty()) return "";
    std::map<std::string, std::string> jobs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_printers.find(printer_id);
        if (it != m_printers.end()) jobs[printer_id] = row_job(it->second.row);
    }
    const std::map<std::string, std::string> all = printer_thumbnail_paths(jobs);
    const auto hit = all.find(printer_id);
    return hit == all.end() ? std::string() : hit->second;
}

// The bytes, for the route. "" both when there is no such picture and when the file went away
// between the two calls, and the route answers 404 either way.
std::string HubServer::printer_thumbnail(const std::string& printer_id)
{
    const std::string path = printer_thumbnail_path(printer_id);
    return path.empty() ? std::string() : read_file(path);
}

// This hub's identity, minted when this data dir's hub settings are created and never again.
// Everything a client can use to spot an event-id reset in one step hangs off this value, so it
// must not change for any other reason - not a hub restart, not a new phone link, not an upgrade.
std::string HubServer::hub_instance()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hub_instance;
}

// One JSON with everything the app's Devices screen draws. Deliberately built here rather than
// proxied: the point is that it answers with no slicer window open.
json HubServer::summary_json()
{
    const PhoneLinks links = phone_links();   // opens sockets: never under m_mutex
    const json       rows  = printers_json(); // takes m_mutex itself
    json cams = json::array();
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    // The camera wall, so a printer row can name the camera that watches it. Joined by id and, for
    // a LAN printer, by the address the camera sits on - which is the join the app was doing by
    // hand out of /state, and the one that is easy to get wrong.
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array()))
            cams.push_back(json{ { "id", h.value("id", "") }, { "alias", h.value("alias", "") }, { "ip", h.value("ip", "") } });
    } catch (...) {}

    json out;
    // `name` is the PC, because that is what a person picks a hub by in the app's list (and what
    // the app labels its card with); `hub` is this hub's own longer label. Both are sent so a
    // client can show either without inventing one out of the URL.
    out["name"]         = pc_host_name();
    out["hub"]          = hub_name();
    out["host"]         = pc_host_name();
    out["version"]      = std::string(SLIC3R_VERSION);
    out["hub_instance"] = hub_instance();
    out["lan_url"]      = links.lan;
    out["remote_url"]   = links.remote;
    out["urls"]         = json{ { "lan", links.lan }, { "remote", links.remote }, { "relay", links.relay } };
    out["printers"]     = json::array();
    std::string token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out["last_id"] = m_next_event_id; // the same high-water mark /events reports
        token          = m_token;         // for the thumbnail URLs built below
    }
    // One pass over the archive for every row, rather than one per printer: which job each is
    // running first, then the pictures.
    std::map<std::string, std::string> jobs;
    for (const json& row : rows)
        jobs[row.value("id", std::string())] = row_job(row);
    const std::map<std::string, std::string> thumbs = printer_thumbnail_paths(jobs);

    for (const json& row : rows) {
        const std::string id = row.value("id", std::string());
        json p;
        p["id"]       = id;
        p["name"]     = row.value("name", std::string());
        p["model"]    = row.value("model", std::string());
        p["vendor"]   = row.value("kind", std::string()); // bambu | snapmaker | printhost | connect
        p["kind"]     = row.value("kind", std::string());
        // A row here is deliberately ALSO a valid /api/printers row: the app's existing mapper
        // reads print_status first and falls back to status, so both are carried under their own
        // names, and `state` is the plain-language name /summary documents.
        p["state"]        = row.contains("print_status") ? row.value("print_status", std::string()) : row.value("status", std::string());
        p["status"]       = p["state"];
        p["print_status"] = p["state"];
        p["online"]   = row.value("online", false);
        p["printing"] = row.value("printing", false);
        p["percent"]     = row.value("percent", 0);
        p["left_time_s"] = row.value("left_time_s", 0);
        // `job` is the plain-language name; `task` is kept as well, because that is the name the
        // app's existing mapper reads off an /api/printers row.
        p["job"]      = row_job(row);
        p["task"]     = p["job"];
        p["layer"]        = row.value("layer", 0);
        p["total_layers"] = row.value("total_layers", 0);
        // Temperatures only when the printer really reported them: a card that shows 0 C where
        // nothing is known reads as "cold", which is a different statement from "not known".
        if (row.contains("bed_temp"))   p["bed_temp"]   = row["bed_temp"];
        if (row.contains("bed_target")) p["bed_target"] = row["bed_target"];
        if (row.contains("nozzles"))    p["nozzles"]    = row["nozzles"];
        // What the app's buttons need, carried through unchanged.
        for (const char* k : { "can_pause", "can_resume", "can_stop", "can_print", "can_upload" })
            if (row.contains(k)) p[k] = row[k];
        // The address, for a client that joins a camera to a printer by it (and for a card that
        // shows where a LAN printer is), and why the printer stopped, in its own words.
        if (row.contains("ip"))          p["ip"]          = row["ip"];
        if (row.contains("print_error")) p["print_error"] = row["print_error"];
        if (row.contains("stage"))       p["stage"]       = row["stage"];
        // The phone's native printer screen: what may be set (heaters with limits, speed, light,
        // fans), the U1's toolhead filaments and a Bambu printer's AMS units - as the instance's
        // row carried them. A stale row keeps them for display; the app offers no control on one.
        for (const char* k : { "controls", "toolheads", "ams", "ext_spools", "hms" })
            if (row.contains(k)) p[k] = row[k];
        p["stale"]    = row.value("stale", false);
        p["age_s"]    = row.value("age_s", 0);
        p["instance"] = row.value("instance", 0);
        // The camera that watches this printer, if any: its own id first, then a LAN camera on the
        // same address.
        std::string cam;
        const std::string ip = row.value("ip", std::string());
        for (const json& c : cams) {
            if (c.value("id", std::string()) == id) { cam = id; break; }
            if (!ip.empty() && (c.value("id", std::string()) == ip || c.value("ip", std::string()) == ip)) cam = c.value("id", std::string());
        }
        if (!cam.empty()) p["camera"] = cam;
        // The thumbnail URL is offered whenever there is a picture to serve, so the app can draw
        // it without a probe that would 404 most of the time.
        if (thumbs.find(id) != thumbs.end())
            p["thumbnail"] = "/r/" + token + "/printers/" + percent_encode(id) + "/thumbnail.png";
        out["printers"].push_back(p);
    }
    return out;
}

// The pairing document, in the shape the app already reads (tools/mock_hub.py --with-pair).
json HubServer::pair_json()
{
    const PhoneLinks links = phone_links();
    json j;
    j["name"]    = pc_host_name();  // the PC, which is what the app labels the card with
    j["hub"]     = hub_name();      // the name this hub goes by
    j["host"]    = pc_host_name();
    j["version"] = std::string(SLIC3R_VERSION);
    j["hub_instance"] = hub_instance();
    // Three named origins, one of which is always empty in phase 0. The apps read this object
    // generically (HubUrl accepts any https origin with a /r/<token>/ path), so the relay needs no
    // app change to become usable the day something fills it in.
    {
        std::string hubid, pubkey;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            hubid  = m_identity.hubid;
            pubkey = m_identity.public_hex;
        }
        // One helper builds `urls`, `hubid` and `public_key` so the document the apps read and the
        // shape the tests pin can never drift apart.
        j.update(json::parse(Testing::pair_identity_json(links.lan, links.remote, links.relay, hubid, pubkey)));
    }
    j["ips"]     = links.ips;
    // What this build and this configuration can actually push with, so the app does not register
    // for a provider that will never deliver (APNs needs the .p8 AND HTTP/2 in our libcurl).
    const json prov = AppPush::providers_json();
    j["push"]    = json{ { "webpush", true },
                         { "apns", prov.value("apns", false) },
                         { "fcm", prov.value("fcm", false) },
                         { "unified", false } };
    // The token's generation, so a client can tell "the link was replaced" from "the same link
    // again" without comparing the tokens themselves. 1 for a data dir's first link, +1 for every
    // "New link" since; persisted, because it must never go backwards.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        j["token_version"] = m_token_version;
    }
    j["features"] = json::array({ "events", "control", "send", "summary", "thumbnail", "webrtc", "quality", "apppush", "hubid" });
    j["capabilities"] = j["features"];
    return j;
}

void HubServer::load_events()
{
    try {
        json j = json::parse(read_file(events_json_path()));
        for (const auto& e : j.value("events", json::array()))
            if (e.is_object()) m_events.push_back(e);
        while (m_events.size() > MAX_EVENTS) m_events.pop_front();
        // Never go backwards, whatever the file says: the phone asks for everything after the last
        // id it saw, and a reused id would hide an event from it for good.
        m_next_event_id = std::max(j.value("last_id", 0), m_events.empty() ? 0 : m_events.back().value("id", 0));
    } catch (...) {}
}

// Opens, sets SO_EXCLUSIVEADDRUSE (Windows) / SO_REUSEADDR (elsewhere), and tries HUB_PORT..
// HUB_PORT+19 on `addr`, taking the first that binds. Pulled out of bind() so the fallback path
// below can retry with the previous mode's address after a failed rebind, without duplicating the
// loop. Returns the bound acceptor (listening) or nullptr, and always sets *out_port to whichever
// port the last attempt used (for the caller's log line on total failure).
static std::shared_ptr<tcp::acceptor> try_bind_range(asio::io_context& ioc, const asio::ip::address_v4& addr, int* out_port)
{
    auto acceptor = std::make_shared<tcp::acceptor>(ioc);
    boost::system::error_code ec;
    acceptor->open(tcp::v4(), ec);
    if (ec) return nullptr;
#ifndef _WIN32
    acceptor->set_option(tcp::acceptor::reuse_address(true), ec); // on Windows this would let two hubs share the port
#else
    // Otherwise a second hub (another data dir) could bind 127.0.0.1:<port> underneath our
    // 0.0.0.0:<port> and silently take the loopback traffic.
    acceptor->set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true), ec);
#endif
    int port = HUB_PORT;
    for (; port < HUB_PORT + 20; ++port) {
        acceptor->bind(tcp::endpoint(addr, (unsigned short) port), ec);
        if (!ec) break;
    }
    if (out_port) *out_port = port;
    if (ec) return nullptr;
    acceptor->listen(64, ec);
    if (ec) return nullptr;
    return acceptor;
}

bool HubServer::bind(bool lan)
{
    // Close the OLD acceptor before trying to bind the new one. This used to run the other way
    // round (bind first, close on success) and that was the actual bug behind "the phone link
    // came back on 13641 for no reason": on a fresh start the hub binds 127.0.0.1:13640
    // (phone off); switching phone access on calls bind(true), which - with the old ordering -
    // tried a NEW SO_EXCLUSIVEADDRUSE 0.0.0.0:13640 while the OLD 127.0.0.1:13640 acceptor was
    // still open in this same process. Windows treats that as a conflict (exclusive access can't
    // be granted over an existing bind, even a narrower one, even in the same process), so the
    // loop below stepped to 13641 every single time home mode was turned on - nothing else on the
    // PC needed to be running at all. Freeing the port first removes that self-conflict; the
    // holder diagnostics further down remain for when something genuinely external holds
    // HUB_PORT.
    //
    // A few hundred ms with no listener while the rebind happens is acceptable (nothing else in
    // the hub depends on this socket being continuously open); if the rebind then fails, the old
    // acceptor's mode and port are restored below rather than leaving the hub deaf.
    std::shared_ptr<tcp::acceptor> old;
    bool                           had_old = false;
    bool                           old_lan = false;
    int                            old_port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        old      = m_acceptor;
        had_old  = (bool) old;
        old_lan  = m_lan;
        old_port = m_port;
    }
    if (old) { boost::system::error_code ig; old->close(ig); } // releases the port; its accept loop exits

    int  port = HUB_PORT;
    auto acceptor = try_bind_range(m_life.io(), lan ? asio::ip::address_v4::any() : asio::ip::address_v4::loopback(), &port);
    if (!acceptor) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: no free port in " << HUB_PORT << "-" << (HUB_PORT + 19)
                                 << " for " << (lan ? "0.0.0.0" : "127.0.0.1") << " (another hub is probably already running)";
        // Never leave the hub with no listener at all: put back what was working before, on its
        // own address and port, if there was one.
        if (had_old) {
            int fallback_port = old_port;
            auto fallback = try_bind_range(m_life.io(), old_lan ? asio::ip::address_v4::any() : asio::ip::address_v4::loopback(), &fallback_port);
            if (fallback) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_acceptor = fallback;
                m_lan      = old_lan;
                m_port     = fallback_port;
                m_life.spawn([this, fallback]() { accept_loop(fallback, false); });
                BOOST_LOG_TRIVIAL(warning) << "RemoteHub: could not switch listener mode; restored "
                                           << (old_lan ? "0.0.0.0" : "127.0.0.1") << ":" << fallback_port;
            } else {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_acceptor = nullptr;
                BOOST_LOG_TRIVIAL(error) << "RemoteHub: lost the listener entirely trying to rebind";
            }
        }
        return false;
    }

    // Something else on the PC already holds HUB_PORT - find out what, so the log and the status
    // JSON can say so instead of just handing out a link on a port nothing is allowed to reach yet
    // (a tester's report, 2026-09-17: their phone link came back on 13641 with no explanation).
    // With the old acceptor now closed above, this only fires for a genuine external holder -
    // this hub's own previous-mode acceptor can no longer be mistaken for one.
    // The holder lookup (netstat + tasklist) takes several seconds on a busy PC. It used to run
    // right here, before the listener was published and hub.json written, and that delay was
    // enough for a slicer that had just spawned this hub to give up waiting for it
    // (ensure_running polls for ~6 s), so its phone-access request never arrived and the
    // hub stayed loopback-only. The lookup is only a diagnostic, so it now runs on its own
    // thread after the listener is up and fills in the note when it is done.
    const bool fell_back = port != HUB_PORT;
    if (fell_back)
        BOOST_LOG_TRIVIAL(warning) << "RemoteHub: port " << HUB_PORT << " is in use; the phone link uses " << port << " instead";
    bool was_remote_on;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_acceptor         = acceptor;
        m_lan              = lan;
        m_port             = port;
        m_port_note_holder.clear();
        was_remote_on      = m_remote_on;
    }
    m_life.spawn([this, acceptor]() { accept_loop(acceptor, false); });
    if (fell_back)
        m_life.spawn([this]() {
            const std::string holder = port_holder_description(HUB_PORT);
            if (!holder.empty()) BOOST_LOG_TRIVIAL(warning) << "RemoteHub: port " << HUB_PORT << " is held by " << holder;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_port != HUB_PORT) m_port_note_holder = holder;
        });
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: listening on " << (lan ? "0.0.0.0" : "127.0.0.1") << ":" << port;
    // Tailscale Serve's forwarding target is Tailscale's own persisted config, not ours: if remote
    // access was already on and the listener just moved port (this bind, or the very first one),
    // Serve is still pointed at wherever it was told last and has to be re-pointed here too, or the
    // tailnet URL keeps forwarding to a dead port. set_remote(true, ...) below re-runs the same
    // `tailscale serve` command with the port current right now, which is exactly what re-pointing
    // it means; it is a no-op for the tailnet config itself when the target already matches.
    if (was_remote_on) {
        m_life.spawn([this]() {
            TailscaleState t = remote_state(true);
            int            p;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                p = m_port;
            }
            if (t.installed && t.backend == "Running" && (!t.serving || t.serving_port != p)) {
                std::string err;
                set_remote(true, err);
                if (!err.empty()) BOOST_LOG_TRIVIAL(warning) << "RemoteHub: could not re-point Tailscale Serve at the new port: " << err;
                else BOOST_LOG_TRIVIAL(info) << "RemoteHub: re-pointed Tailscale Serve at 127.0.0.1:" << p;
            }
        });
    }
    return true;
}

// The control plane: 127.0.0.1 on whatever ephemeral port the OS hands out, written to hub.json
// so the slicer instances, the tray and the hub page can find it. It is never rebound, so the
// /relay/h264 URLs registered in go2rtc stay valid when phone access flips the main listener.
bool HubServer::bind_admin()
{
    auto acceptor = std::make_shared<tcp::acceptor>(m_life.io());
    boost::system::error_code ec;
    acceptor->open(tcp::v4(), ec);
    if (ec) return false;
#ifdef _WIN32
    // Same reason as the main listener: an ephemeral port is still a port another process could
    // bind underneath us, and this one carries the control plane.
    acceptor->set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true), ec);
#endif
    acceptor->bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
    if (ec) { BOOST_LOG_TRIVIAL(error) << "RemoteHub: no loopback port for the control plane: " << ec.message(); return false; }
    acceptor->listen(32, ec);
    if (ec) return false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_admin_acceptor = acceptor;
        m_admin_port     = (int) acceptor->local_endpoint().port();
    }
    m_life.spawn([this, acceptor]() { accept_loop(acceptor, true); });
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: control plane on 127.0.0.1:" << m_admin_port;
    return true;
}

void HubServer::accept_loop(std::shared_ptr<tcp::acceptor> acceptor, bool admin)
{
    for (;;) {
        auto sock = std::make_unique<tcp::socket>(m_life.io());
        boost::system::error_code ec;
        acceptor->accept(*sock, ec);
        if (ec) break; // closed by a rebind or at shutdown
        tcp::socket* raw = sock.release();
        if (!m_life.spawn([this, raw, admin]() { serve(std::unique_ptr<tcp::socket>(raw), admin); })) {
            delete raw; // shutting down: never started, so still ours (this thread keeps the io_context alive)
            break;
        }
    }
}

void HubServer::start_go2rtc()
{
#ifdef _WIN32
    const std::string exe = go2rtc_exe_path();
    if (!fs::exists(exe)) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: missing " << exe;
        return;
    }
    // A random loopback port and per-run credentials: nothing on this PC reaches go2rtc's API
    // except through the hub. local_auth makes go2rtc check them for loopback peers too, and
    // allow_paths leaves only the three routes the hub uses registered (go2rtc 1.9.14 then also
    // drops its built-in player pages, which the hub serves itself), so even a leaked credential
    // cannot reach /api/config, /api/restart or /api/exit. An orphan go2rtc cannot exist: it is
    // in a kill-on-close job object.
    const int port = free_loopback_port();
    if (port == 0) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: no free loopback port for go2rtc";
        return;
    }
    m_go2rtc_user = random_hex(8);
    m_go2rtc_pass = random_hex(16);
    m_go2rtc_auth = basic_auth(m_go2rtc_user, m_go2rtc_pass);
    // WebRTC media goes straight from go2rtc to the phone (Phase 2). The signalling still rides
    // the hub's /api/ws tunnel - go2rtc 1.9.14 answers webrtc/offer on the WebSocket, so
    // allow_paths does not need go2rtc's /api/webrtc (WHEP) route and stays as it is. The public
    // STUN server only matters off the tailnet: it lets go2rtc learn its own public address so a
    // phone on mobile data can try a direct path. On the tailnet and on the LAN the host
    // candidates (100.x, 192.168.x/10.x) are what actually connect.
    const int webrtc_port = free_webrtc_port();
    if (webrtc_port == 0) BOOST_LOG_TRIVIAL(warning) << "RemoteHub: no free WebRTC port; video stays on MSE";
    const std::string cfg_path = (fs::path(hub_dir()) / "go2rtc.yaml").string();
    {
        boost::nowide::ofstream cfg(cfg_path);
        cfg << "api:\n  listen: \"127.0.0.1:" << port << "\"\n"
            << "  username: \"" << m_go2rtc_user << "\"\n  password: \"" << m_go2rtc_pass << "\"\n"
            << "  local_auth: true\n"
            << "  allow_paths: [\"/api/ws\", \"/api/streams\", \"/api/onvif\"]\n";
        // RTSP. This listener used to be off unconditionally, and it has to stay off when we have
        // no ffmpeg - nothing the hub does needs it and an open RTSP port is surface we do not
        // want. But go2rtc's `ffmpeg:` sources are **piped back through go2rtc's own RTSP
        // listener**: with `rtsp: listen: ""` every variant fails at the moment a viewer opens it
        // with `streams: exec: rtsp module disabled`, which reaches the phone as a black tile.
        // (Found exactly that way while gating this branch - the config looked right and the
        // variants registered fine; only requesting one showed it.)
        //
        // So: a loopback-only port, and only when there is an ffmpeg to need it. 127.0.0.1 means
        // it is not reachable from the LAN or the tailnet, and it is a random free port rather
        // than 8554 so two hubs on one PC cannot collide.
        const bool want_ffmpeg = !ffmpeg_path().empty();
        int rtsp_port = want_ffmpeg ? free_loopback_port() : 0;
        if (want_ffmpeg && rtsp_port == 0)
            BOOST_LOG_TRIVIAL(warning) << "RemoteHub: no free loopback port for go2rtc's RTSP; quality variants will not start";
        if (rtsp_port > 0) cfg << "rtsp:\n  listen: \"127.0.0.1:" << rtsp_port << "\"\n";
        else               cfg << "rtsp:\n  listen: \"\"\n";
        // WebRTC media (Phase 2). Everything else the hub runs is loopback-only; this is the one
        // port that has to be reachable from the phone, because the media goes straight from
        // go2rtc to the phone rather than through the hub. The earlier note here said "no WebRTC
        // listener, deliberately" - that was true while nothing opened the port or asked the
        // firewall about it. Both now exist (free_webrtc_port() above, firewall_query()), so the
        // listener goes on and the page decides per tile whether to use it. What has not changed:
        // Tailscale Serve is an HTTPS reverse proxy and cannot forward UDP, so a phone arriving
        // through a Serve origin still cannot use WebRTC - stream_center.html detects that origin
        // and stays on MSE (see docs/superpowers/specs/2026-09-12-webrtc-video.md). The public
        // STUN server only matters off the tailnet; on the tailnet and the LAN the host
        // candidates (100.x, 192.168.x/10.x) are what actually connect.
        if (webrtc_port > 0)
            cfg << "webrtc:\n  listen: \":" << webrtc_port << "\"\n"
                << "  ice_servers:\n    - urls: [\"stun:stun.cloudflare.com:3478\"]\n";
        else
            cfg << "webrtc:\n  listen: \"\"\n";
        cfg << "srtp:\n  listen: \"\"\n";
        // ffmpeg for the Quality variants. Two things have to be said explicitly.
        //
        // `bin:` - go2rtc otherwise looks for "ffmpeg" on PATH, and the whole point of bundling
        // one is not to depend on what happens to be installed on the user's PC. An ffmpeg on
        // PATH could also be any build at all, including one whose flags differ; naming our own
        // exe makes the template below a statement about a known binary.
        //
        // `h264:` - go2rtc's built-in template is libx264, which the bundled LGPL build does not
        // have (see ffmpeg_h264_template()). Without this override every variant would die at
        // startup with "Unknown encoder 'libx264'" and the tile would go black.
        const std::string ff = ffmpeg_path();
        if (!ff.empty()) {
            std::string ffy = ff;
            for (auto& c : ffy) if (c == '\\') c = '/'; // YAML-safe, and ffmpeg accepts forward slashes
            cfg << "ffmpeg:\n  bin: \"" << ffy << "\"\n"
                << "  h264: \"" << ffmpeg_h264_template() << "\"\n";
        }
    }
    if (!m_job) {
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // go2rtc dies with the hub
            ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
            m_job = job;
        }
    }
    m_go2rtc_pid = spawn_process({ exe, "-config", cfg_path }, {}, true, m_job);
    if (m_go2rtc_pid <= 0) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: failed to start go2rtc";
        return;
    }
    m_go2rtc_port = port;
    m_webrtc_port = webrtc_port;
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: go2rtc pid " << m_go2rtc_pid << " on 127.0.0.1:" << port
                            << " (credential-only), WebRTC media on " << (webrtc_port ? std::to_string(webrtc_port) : std::string("off"));
    {
        const std::string ff = ffmpeg_path();
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: quality variants "
                                << (ff.empty() ? "off (no ffmpeg found; MJPEG fps knob only)"
                                               : "on via " + ff);
    }
    if (webrtc_port > 0) firewall_state(true); // one PowerShell run on a detached thread; result cached
#endif
}

// Cached; a refresh runs the (slow) PowerShell query on a detached thread and never blocks a
// request, so the hub page's 3 s poll always gets the last answer straight away.
FirewallState HubServer::firewall_state(bool refresh)
{
    int port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        port = m_webrtc_port;
        // Re-checked every few minutes so the hub page notices by itself once the user has
        // allowed go2rtc in the firewall (or removed the rule again).
        if (!refresh && m_fw.checked_at && (long long) std::time(nullptr) - m_fw.checked_at < 300) return m_fw;
    }
    if (port > 0 && !m_fw_busy.exchange(true)) {
        const bool started = m_life.spawn([this, port]() {
            FirewallState fw = firewall_query_go2rtc(go2rtc_exe_path(), port);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_fw = fw;
            }
            if (fw.state != "allowed")
                BOOST_LOG_TRIVIAL(info) << "RemoteHub: Windows Firewall for go2rtc.exe: " << fw.state << " (" << fw.note
                                         << (fw.command.empty() ? "" : " " + fw.command) << ")";
            m_fw_busy = false;
        });
        if (!started) m_fw_busy = false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fw;
}

// Same idea as firewall_state() above, but for the LAN listener itself: does Windows Firewall let
// a phone reach this PC's own port, not just go2rtc's WebRTC one? Checked only while phone access
// is actually on and bound to a LAN address - point is moot on loopback-only or before the first
// bind. current_exe() rather than go2rtc_exe_path(): the phone talks to this process directly.
FirewallState HubServer::lan_firewall_state(bool refresh)
{
    bool on = false;
    int  port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        on   = m_phone && m_lan && m_port > 0;
        port = m_port;
        if (!refresh && m_lan_fw.checked_at && (long long) std::time(nullptr) - m_lan_fw.checked_at < 300) return m_lan_fw;
    }
    if (on && !m_lan_fw_busy.exchange(true)) {
        const bool started = m_life.spawn([this, port]() {
            const std::string exe = current_exe();
            FirewallState      fw = firewall_query(exe, port, "EdgeSlicer.exe",
                "netsh advfirewall firewall add rule name=\"EdgeSlicer\" dir=in action=allow program=\"" + exe +
                "\" protocol=TCP localport=" + std::to_string(HUB_PORT) + "-" + std::to_string(HUB_PORT + 19) +
                " profile=private,domain");
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lan_fw = fw;
            }
            if (fw.state != "allowed")
                BOOST_LOG_TRIVIAL(info) << "RemoteHub: Windows Firewall for the phone/LAN port " << port << ": " << fw.state << " (" << fw.note
                                         << (fw.command.empty() ? "" : " " + fw.command) << ")";
            m_lan_fw_busy = false;
        });
        if (!started) m_lan_fw_busy = false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lan_fw;
}

void HubServer::register_streams()
{
    std::string state, base, secret;
    int         port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state  = m_state;
        base   = go2rtc_base_locked();
        secret = m_secret;
        port   = m_admin_port; // /relay/h264 lives on the control plane, never on the tunnelled listener
    }
    if (base.empty() || state.empty()) return;
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array())) {
            std::string name = h.value("rname", ""), src = h.value("rsrc", "");
            if (!u1_h264_url(h).empty() && port > 0) {
                // The U1 raw stream, fetched by go2rtc from this hub (SPS-first), never from the printer directly.
                name = u1_stream_name(h.value("id", ""));
                src  = "http://127.0.0.1:" + std::to_string(port) + "/relay/h264?id=" + encode_component(h.value("id", "")) + "&lt=" + secret;
            }
            if (name.empty() || src.empty()) continue;
            // Http::put() is a file-upload PUT (its read callback dereferences the missing
            // file and crashes); put2() is a plain PUT with no body, which is what go2rtc wants.
            const std::string url = base + "/api/streams?name=" + name + "&src=" + percent_encode(src);
            std::thread([url]() {
                for (int attempt = 0; attempt < 3; ++attempt) {
                    bool ok = false;
                    Http::put2(url).timeout_connect(2).timeout_max(5).on_complete([&ok](std::string, unsigned) { ok = true; }).perform_sync();
                    if (ok) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // go2rtc may still be starting
                }
            }).detach();
            // Quality variants beside the source stream, when an ffmpeg exists to make them (see
            // quality_variants()). Each is a re-encode of the stream just registered, not a second
            // connection to the printer, so the camera still sees one consumer. Registered lazily
            // by go2rtc - the ffmpeg process only starts when a viewer actually opens the variant,
            // so an unused _med/_low costs nothing.
            for (const std::string& q : quality_variants()) {
                const std::string vurl = base + "/api/streams?name=" + name + "_" + q +
                                         "&src=" + percent_encode(variant_src(name, q));
                std::thread([vurl]() {
                    // A freshly started go2rtc answers its API before it will accept an *exec*
                    // source: for the first ~5 s a PUT of an `ffmpeg:` (or `echo:`) stream comes
                    // back `400 streams: source not supported`, while `rtsp:` is taken at once.
                    // The source stream above therefore registers immediately and the variants
                    // did not, which is exactly the window the hub registers in - so on a normal
                    // start the variants were silently absent and every _med/_low tile went black.
                    //
                    // Twelve attempts at 1.5 s covers ~18 s, comfortably past that window (the
                    // old three attempts covered 4.5 s and always fell inside it). A 400 lands in
                    // on_error, not on_complete, so `ok` stays false and the loop does retry -
                    // it simply ran out of attempts. Logged on final failure rather than failing
                    // silently, because a missing variant is otherwise invisible until a viewer
                    // opens the tile.
                    for (int attempt = 0; attempt < 12; ++attempt) {
                        bool ok = false;
                        Http::put2(vurl).timeout_connect(2).timeout_max(5).on_complete([&ok](std::string, unsigned) { ok = true; }).perform_sync();
                        if (ok) return;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                    }
                    BOOST_LOG_TRIVIAL(warning) << "RemoteHub: go2rtc kept refusing a quality variant; it will fall back to the source stream";
                }).detach();
            }
        }
    } catch (...) {}
}

std::string HubServer::go2rtc_base_locked() const
{
    if (m_go2rtc_port == 0) return "";
    return "http://" + m_go2rtc_user + ":" + m_go2rtc_pass + "@127.0.0.1:" + std::to_string(m_go2rtc_port);
}

// ONVIF WS-Discovery, run by go2rtc for the PC's Stream tab (through /hub/onvif). go2rtc answers
// 404 "no sources" when nothing is found; that is passed through as it is.
std::pair<int, std::string> HubServer::onvif_discover()
{
    std::string base;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        base = go2rtc_base_locked();
    }
    if (base.empty()) return { 503, json_error("stream relay is not running") };
    int         status = 0;
    std::string body;
    Http::get(base + "/api/onvif")
        .timeout_connect(2).timeout_max(15)
        .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
        .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? err : b; })
        .perform_sync();
    return { status == 0 ? 502 : status, body };
}

// The phone only sees ids, aliases, the source kind, go2rtc stream names and direct
// printer-page URLs. Addresses, access codes and camera credentials stay here.
std::string HubServer::state_for_phone()
{
    const FirewallState fw = firewall_state(false); // takes m_mutex itself
    std::string state;
    int         webrtc_port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state       = m_state;
        webrtc_port = m_webrtc_port;
    }
    json out;
    out["hosts"]  = json::array();
    out["active"] = json::array();
    // Whether the phone should try WebRTC at all, and - when the PC's firewall would very likely
    // block it - one sentence the viewer can act on. The player falls back to MSE either way.
    out["webrtc"]     = webrtc_port > 0;
    out["video_note"] = (webrtc_port > 0 && fw.state != "allowed") ? fw.note : "";
    // Quality: which downscaled variants the hub was able to register. Empty means "source only",
    // and the page then shows Quality as High-only rather than offering settings that do nothing.
    // See ffmpeg_path(): transcoded variants need an ffmpeg we do not bundle. The Bambu MJPEG
    // relay's frame-drop knob needs no encoder, so it is reported separately and is always on.
    out["quality"]      = json::array();
    for (const std::string& q : quality_variants()) out["quality"].push_back(q);
    out["quality_mjpeg"] = true; // ?fps= on the Bambu relay, decoder-free
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array())) {
            json p;
            p["id"]    = h.value("id", "");
            p["alias"] = h.value("alias", "");
            p["rkind"] = h.value("rkind", "");
            p["rname"] = h.value("rname", "");
            p["rurl"]  = h.value("rurl", "");
            if (!u1_h264_url(h).empty()) {
                p["rname"] = u1_stream_name(h.value("id", "")); // the go2rtc stream fed by /relay/h264
                p["relay"] = true;                               // rurl still works on the LAN
            }
            // The variant stream names this host actually has, so the page asks for a name that
            // exists rather than guessing "<name>_low" and getting a 404 from go2rtc.
            if (!p["rname"].get<std::string>().empty()) {
                json qn = json::object();
                for (const std::string& q : quality_variants())
                    qn[q] = p["rname"].get<std::string>() + "_" + q;
                p["qnames"] = qn;
            }
            out["hosts"].push_back(p);
        }
        out["active"] = j.value("active", json::array());
    } catch (...) {}
    // The phone's own "where am I, and where else could I be" - the Connection control in the page
    // header reads these. A visitor here already holds the token (and, through Tailscale Serve, an
    // allow-listed tailnet login), so the PC's LAN address tells them nothing they could not have
    // learned by looking at the link they used.
    const PhoneLinks links = phone_links();
    out["lan_url"]    = links.lan;
    out["remote_url"] = links.remote;
    out["relay_url"]  = links.relay; // always "" in phase 0; the control ignores an empty origin
    out["ips"]        = links.ips;
    // Added for the native app (phase 3 follow-ups 2 and 6), and additive on purpose: every field
    // the page already reads is untouched.
    //   printers      the last-known status of every printer, each row with age_s / stale /
    //                 instance - so the Devices screen works with no slicer window open;
    //   hub_instance  this hub's identity, the same value /events and /summary carry.
    out["printers"]     = printers_json();
    out["hub_instance"] = hub_instance();
    return out.dump();
}

bool HubServer::lookup_host(const std::string& id, std::string& ip, std::string& code)
{
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array()))
            if (h.value("id", "") == id) {
                ip   = h.value("ip", "");
                code = h.value("code", "");
                return !ip.empty();
            }
    } catch (...) {}
    return false;
}

std::string HubServer::relay_h264_url(const std::string& id)
{
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array()))
            if (h.value("id", "") == id) return u1_h264_url(h);
    } catch (...) {}
    return "";
}

Instance HubServer::probe_instance(Instance inst)
{
    Http::get("http://127.0.0.1:" + std::to_string(inst.port) + "/api/info")
        .timeout_connect(1).timeout_max(3)
        .on_complete([&inst](std::string body, unsigned status) {
            if (status != 200) return;
            try {
                json j        = json::parse(body);
                inst.alive    = j.value("pid", 0L) == inst.pid;
                inst.title    = j.value("title", "");
                inst.path     = j.value("path", "");
                inst.slicing  = j.value("slicing", false);
                inst.hidden   = j.value("hidden", inst.hidden);
                inst.needs_attention  = j.value("needs_attention", false);
                inst.attention_reason = j.value("attention_reason", "");
            } catch (...) {}
        })
        .perform_sync();
    return inst;
}

// Every slicer instance drops <instances>/<pid>.json when its loopback API starts; a dead
// pid means a crash and the file is dropped here.
std::vector<Instance> HubServer::instances(bool probe)
{
    std::vector<Instance> out;
    boost::system::error_code ec;
    if (!fs::is_directory(instances_dir(), ec)) return out;
    for (fs::directory_iterator it(instances_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        Instance inst;
        try {
            json j       = json::parse(read_file(it->path().string()));
            inst.pid     = j.value("pid", 0L);
            inst.port    = j.value("port", 0);
            inst.started = j.value("started", 0LL);
            inst.hidden  = j.value("hidden", false);
            inst.needs_attention  = j.value("needs_attention", false);
            inst.attention_reason = j.value("attention_reason", "");
            inst.title   = j.value("title", "");
            inst.path    = j.value("path", "");
        } catch (...) {}
        if (inst.pid <= 0 || inst.port <= 0 || !pid_alive(inst.pid)) {
            boost::system::error_code ig;
            fs::remove(it->path(), ig);
            continue;
        }
        inst.alive = true;
        out.push_back(inst);
    }
    std::sort(out.begin(), out.end(), [](const Instance& a, const Instance& b) { return a.started != b.started ? a.started < b.started : a.pid < b.pid; });
    if (probe) {
        std::vector<std::thread> threads;
        for (Instance& inst : out) threads.emplace_back([&inst]() { inst = probe_instance(inst); });
        for (std::thread& t : threads) t.join();
    }
    return out;
}

// How many slicer processes we are on the hook for: the ones that registered plus the ones that
// were just spawned and have not written their <pid>.json yet (that takes a few seconds, which is
// long enough for a phone to ask for a dozen more).
int HubServer::instances_in_flight()
{
    const std::vector<Instance> live = instances(false);
    const long long             now  = now_unix();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recent_spawns.erase(std::remove_if(m_recent_spawns.begin(), m_recent_spawns.end(),
                                         [&](const std::pair<long, long long>& s) {
                                             if (now - s.second > 120) return true;  // gave up waiting
                                             if (!pid_alive(s.first)) return true;   // died or never started
                                             for (const Instance& i : live)
                                                 if (i.pid == s.first) return true;  // registered: counted below
                                             return false;
                                         }),
                          m_recent_spawns.end());
    return (int) live.size() + (int) m_recent_spawns.size();
}

json HubServer::instances_json()
{
    json j;
    j["instances"] = json::array();
    int index      = 0;
    for (const Instance& inst : instances(true)) {
        if (!inst.alive) continue;
        json ji;
        ji["id"]      = inst.pid;
        ji["index"]   = ++index;
        ji["pid"]     = inst.pid;
        ji["title"]   = inst.title;
        ji["path"]    = inst.path;
        ji["slicing"] = inst.slicing;
        ji["hidden"]  = inst.hidden;
        ji["needs_attention"]  = inst.needs_attention;
        ji["attention_reason"] = inst.attention_reason;
        j["instances"].push_back(ji);
    }
    return j;
}

HubServer::Snapshot HubServer::snapshot()
{
    Snapshot s;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        s.phone      = m_phone;
        s.port       = m_port;
        s.admin_port = m_admin_port;
        s.token      = m_token;
    }
    if (s.phone) {
        const std::vector<std::string> ips = lan_ips();
        if (!ips.empty()) s.url = "http://" + ips.front() + ":" + std::to_string(s.port) + "/r/" + s.token + "/";
    }
    for (const Instance& inst : instances(false)) {
        ++s.instances;
        if (inst.hidden) ++s.hidden;
        if (inst.needs_attention) ++s.attention;
    }
    return s;
}

// Turning phone access off and on keeps the same link: a QR code somebody scanned, a saved
// bookmark and a home-screen icon all carry the token, and minting a new one on every toggle
// broke all three for no reason anybody asked for. new_link() is the only thing that replaces it.
bool HubServer::set_phone(bool on, const std::string& token)
{
    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        changed = on != m_phone;
        m_phone = on;
        // Only a hub with no link of its own takes up the caller's remembered one (a fresh data
        // dir). A hub that has one keeps it: it is what the phones out there are using.
        if (!valid_token(m_token)) m_token = valid_token(token) ? token : random_token();
    }
    if (!changed) return true;
    const bool ok = bind(on);
    write_hub_json();
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: phone access " << (on ? "on" : "off");
    return ok;
}

// Remember a link we are replacing, so the phones still holding it get told rather than a 404.
// Three is enough for "I pressed it twice"; an old token is not a credential (nothing compares
// against this list to let anybody in), but there is no reason to keep them forever either.
void HubServer::remember_old_token_locked(const std::string& token)
{
    if (!valid_token(token)) return;
    for (const std::string& t : m_old_tokens)
        if (t == token) return;
    m_old_tokens.push_back(token);
    if (m_old_tokens.size() > 3) m_old_tokens.erase(m_old_tokens.begin(), m_old_tokens.end() - 3);
}

// An explicit "new link", and nothing else, replaces the token: every link, QR code and
// home-screen icon made from the old one stops working, so both callers (the hub page and the
// tray) ask the person first and say that in the question.
std::string HubServer::new_link()
{
    std::string token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        remember_old_token_locked(m_token);
        m_token = random_token();
        ++m_token_version; // this data dir is on its next link; /pair says which
        token   = m_token;
    }
    write_hub_json();
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: a new phone link was made; anything saved from the old one stops working";
    return token;
}

TailscaleState HubServer::remote_state(bool refresh)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!refresh && m_ts.checked_at && (long long) std::time(nullptr) - m_ts.checked_at < 15) return m_ts;
    }
    TailscaleState t = tailscale_query(); // slow-ish (two CLI runs): never under the lock
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ts = t;
    return m_ts;
}

// Stale-while-revalidate: with an answer already cached, a stale one is returned at once and one
// refresh runs on a detached thread (like firewall_state()); only the very first call, with
// nothing cached yet, waits for the CLI.
TailscaleState HubServer::remote_state_nowait()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ts.checked_at) {
            if ((long long) std::time(nullptr) - m_ts.checked_at >= 15 && !m_ts_busy.exchange(true)) {
                const bool started = m_life.spawn([this]() {
                    TailscaleState t = tailscale_query();
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_ts = t;
                    }
                    m_ts_busy = false;
                });
                if (!started) m_ts_busy = false;
            }
            return m_ts;
        }
    }
    return remote_state(false);
}

// The remote-access card's whole content, in the shape the page draws it. The six states and
// their wording come from Testing::classify_remote_access() so that nothing here and nothing in
// hub.html carries a second copy of a sentence; the older flat booleans stay alongside because the
// tray and the phone page read them.
json HubServer::remote_json_locked() const
{
    json r;
    r["installed"]      = m_ts.installed;
    r["state"]          = m_ts.backend;
    r["dns_name"]       = m_ts.dns_name;
    r["login"]          = m_ts.login;
    r["https"]          = m_ts.https;
    r["serving"]        = m_ts.serving;
    r["on"]             = m_remote_on;
    r["url"]            = (m_remote_on && m_ts.serving && !m_ts.dns_name.empty()) ? "https://" + m_ts.dns_name + "/r/" + m_token + "/" : "";
    r["allowed_logins"] = m_allowed_logins;
    r["last_login"]     = m_last_login;
    r["last_login_at"]  = m_last_login_at;
    std::string err = m_ts.error;
    if (err.empty() && m_remote_on && m_ts.installed && !m_ts.serving) err = "Tailscale is no longer serving this hub; turn remote access off and on again";
    r["error"] = err;
    const Testing::RemoteAccessInfo st = Testing::classify_remote_access(m_ts.installed, m_ts.backend, m_ts.https,
                                                                        m_ts.serving, m_remote_on, m_ts.error);
    r["access"] = json{ { "state", Testing::remote_access_state_name(st.state) },
                        { "message", st.message },
                        { "action", st.action },
                        { "action_url", st.action_url } };
    // The `tailscale funnel` line the card's advanced note shows verbatim. Documented, never run:
    // Funnel publishes the hub to the whole internet with only the path token in front of it, which
    // is a deliberate choice nobody should make by clicking a button (design section 3).
    r["funnel_command"] = "tailscale funnel --bg --https=443 http://127.0.0.1:" + std::to_string(m_port);
    return r;
}

bool HubServer::set_remote(bool on, std::string& error)
{
    int port;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        port = m_port;
    }
    std::string out;
    int         code = 0;
    if (on) {
        TailscaleState t = remote_state(true);
        // The refusal the caller shows is the classifier's sentence, so the message a failed
        // toggle produces and the message the card was already showing are one and the same
        // string - "not installed", "not signed in" and "HTTPS certificates are off" included.
        if (!t.installed || t.backend != "Running" || !t.https) {
            error = Testing::classify_remote_access(t.installed, t.backend, t.https, t.serving, false, t.error).message;
            return false;
        }
        // The first run also fetches the certificate, which can take half a minute.
        if (!run_capture({ tailscale_exe(), "serve", "--bg", "--https=443", "http://127.0.0.1:" + std::to_string(port) }, out, code, 90000) || code != 0) {
            error = out.empty() ? "tailscale serve failed" : out.substr(0, 300);
            return false;
        }
        t = remote_state(true);
        if (!t.serving) { error = "tailscale serve did not take: " + out.substr(0, 200); return false; }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remote_on = true;
        if (m_allowed_logins.empty() && !t.login.empty()) m_allowed_logins.push_back(t.login);
    } else {
        run_capture({ tailscale_exe(), "serve", "--https=443", "off" }, out, code, 30000);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remote_on = false;
    }
    write_hub_json();
    if (!on) remote_state(true);
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: remote access (Tailscale) " << (on ? "on" : "off");
    return true;
}

void HubServer::remote_logins(const std::string& add, const std::string& remove)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string a = lower(add), r = lower(remove);
    if (!a.empty() && a.size() < 200 && a.find_first_of(" \t\r\n\"<>") == std::string::npos &&
        std::find(m_allowed_logins.begin(), m_allowed_logins.end(), a) == m_allowed_logins.end())
        m_allowed_logins.push_back(a);
    if (!r.empty()) m_allowed_logins.erase(std::remove(m_allowed_logins.begin(), m_allowed_logins.end(), r), m_allowed_logins.end());
}

bool HubServer::login_allowed(const std::string& login)
{
    const std::string l = lower(login);
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool ok = std::find(m_allowed_logins.begin(), m_allowed_logins.end(), l) != m_allowed_logins.end();
    if (ok && (m_last_login != l || (long long) std::time(nullptr) - m_last_login_at > 600)) {
        m_last_login    = l;
        m_last_login_at = (long long) std::time(nullptr);
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: remote visit by " << l;
    }
    return ok;
}

long HubServer::spawn_slicer(const std::string& file, bool hidden)
{
    // The child must live on this hub's data dir: without --datadir it starts on the default one,
    // registers with (or starts) the hub over there, and this hub never lists it. Only a hub on a
    // custom --datadir (tests, a second profile) ever noticed, since the two coincide otherwise.
    std::vector<std::string> args = { current_exe(), "--datadir", data_dir() };
    if (!file.empty()) args.push_back(file);
    std::vector<std::pair<std::string, std::string>> env { { "SNORCA_NEW_INSTANCE", "1" } };
    env.emplace_back("SNORCA_HIDDEN", hidden ? "1" : "0"); // explicit either way
    const long pid = spawn_process(args, env, false, nullptr);
    if (pid > 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_recent_spawns.emplace_back(pid, now_unix()); // counts against the cap until it registers
    }
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: new " << (hidden ? "hidden" : "visible") << " instance pid " << pid
                            << (file.empty() ? std::string() : " for " + file);
    return pid;
}

std::pair<int, std::string> HubServer::instance_post(long pid, const std::string& sub)
{
    Instance target;
    for (const Instance& inst : instances(false))
        if (inst.pid == pid) target = inst;
    if (!target.alive) return { 404, json_error("no such slicer instance") };
#ifdef _WIN32
    ::AllowSetForegroundWindow((DWORD) pid); // let the instance raise its own window
#endif
    int         status = 502;
    std::string body   = json_error("the slicer did not answer");
    // Http::post only sets the POST fields when a body is present; without one libcurl falls
    // back to its default read callback (stdin) and the hub faults. Always send a body.
    Http::post("http://127.0.0.1:" + std::to_string(target.port) + sub)
        .timeout_connect(2).timeout_max(120)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .set_post_body(std::string("via=hub"))
        .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
        .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? json_error(err) : b; })
        .perform_sync();
    return { status, body };
}

bool HubServer::instance_window(long pid, bool show) { return instance_post(pid, std::string("/api/window?show=") + (show ? "1" : "0")).first == 200; }
bool HubServer::instance_quit(long pid, bool discard) { return instance_post(pid, std::string("/api/quit") + (discard ? "?discard=1" : "")).first == 200; }

// Loopback-only: the slicer instances and the hub's own page (GET /hub/) talk to these.
void HubServer::handle_hub(tcp::socket& client, Request& r)
{
    if (r.path == "/hub/info" && r.method == "GET") {
        // Never the blocking remote_state(false) here: every slicer's "is the hub there?" probe
        // lands on this route with a short timeout, and a stale cache used to make it wait for two
        // tailscale CLI runs (up to 15 s each) - long enough for the probe to give up and report
        // a running hub as gone (the Home tab's false "not running", 2026-09-23).
        remote_state_nowait();
        respond_json(client, 200, info_json().dump());
    } else if (r.path == "/hub/remote" && r.method == "POST") {
        // ?on=1|0 turns Tailscale Serve for this hub on or off; ?add= / ?remove= edit the allow-list.
        const std::string on = query_param(r.query, "on");
        remote_logins(percent_decode(query_param(r.query, "add")), percent_decode(query_param(r.query, "remove")));
        std::string error;
        if (on == "1" || on == "0") set_remote(on == "1", error);
        else write_hub_json();
        remote_state(true);
        json j = info_json();
        // A refused toggle reports the classifier's own sentence (set_remote uses it too), so the
        // card shows the same wording whether it got there by failing a toggle or by simply
        // looking. `access.message` is what hub.html draws; `error` stays for older readers.
        if (!error.empty()) {
            j["remote"]["error"]             = error;
            j["remote"]["access"]["message"] = error;
            if (j["remote"]["access"].value("state", std::string()) == "ready") j["remote"]["access"]["state"] = "error";
            if (j["remote"]["access"].value("action", std::string()).empty()) j["remote"]["access"]["action"] = "Try again";
        }
        respond_json(client, error.empty() ? 200 : 409, j.dump());
    } else if ((r.path == "/hub/" || r.path == "/hub/index.html") && r.method == "GET") {
        // The page gets the per-run secret and sends it back as X-Hub-Secret on every call.
        std::string page = read_file(resources_dir() + "/web/orca/hub.html"), secret;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            secret = m_secret;
        }
        const std::string ph = "__HUB_SECRET__";
        for (size_t at = page.find(ph); at != std::string::npos; at = page.find(ph, at + secret.size())) page.replace(at, ph.size(), secret);
        respond(client, 200, "text/html; charset=utf-8", page, "X-Frame-Options: DENY\r\n");
    } else if (r.path == "/hub/onvif" && r.method == "GET") {
        const auto res = onvif_discover();
        respond(client, res.first, res.first == 200 ? "application/json" : "text/plain; charset=utf-8", res.second);
    } else if (r.path == "/hub/qrcode.js" && r.method == "GET") {
        respond(client, 200, "application/javascript", read_file(resources_dir() + "/web/orca/qrcode.js"));
    } else if (r.path == "/hub/instances" && r.method == "GET") {
        respond_json(client, 200, instances_json().dump());
    } else if (r.path == "/hub/new" && r.method == "POST") {
        const int open = instances_in_flight();
        if (open >= MAX_INSTANCES) {
            respond_json(client, 429, json_error("too many slicer windows are open (" + std::to_string(open) + " of " +
                                                 std::to_string(MAX_INSTANCES) + "); close one first"));
            return;
        }
        const long pid = spawn_slicer("", query_param(r.query, "hidden") == "1");
        json j;
        j["ok"]  = pid > 0;
        j["pid"] = pid;
        respond_json(client, pid > 0 ? 200 : 500, pid > 0 ? j.dump() : json_error("could not start a slicer"));
    } else if (r.path == "/hub/state" && r.method == "POST") {
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        if (!read_small_body(client, r, body, 4 * 1024 * 1024)) { respond_json(client, 413, json_error("state too large")); return; }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = body;
        }
        write_file(streams_json_path(), body);
        register_streams();
        respond_json(client, 200, "{\"ok\":true}");
    } else if (r.path == "/hub/event" && r.method == "POST") {
        // A slicer instance's watcher (RemoteEvents) handing over one printer event. Same shape as
        // /hub/state: loopback, the per-run secret, JSON. The hub assigns the id and the time.
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        if (!read_small_body(client, r, body, MAX_API_BODY)) { respond_json(client, 413, json_error("event too large")); return; }
        json e;
        try {
            e = json::parse(body);
        } catch (...) {
            respond_json(client, 400, json_error("the event is not JSON"));
            return;
        }
        if (!e.is_object()) { respond_json(client, 400, json_error("the event must be an object")); return; }
        accept_event(e);
        respond_json(client, 200, e.dump());
    } else if (r.path == "/hub/events" && r.method == "GET") {
        respond_json(client, 200, events_json(std::atoi(query_param(r.query, "since").c_str())).dump());
    } else if (r.path == "/hub/phone" && r.method == "POST") {
        set_phone(query_param(r.query, "on") == "1", query_param(r.query, "token") /* only used by a hub with no link yet */);
        respond_json(client, 200, info_json().dump());
    } else if (r.path == "/hub/newlink" && r.method == "POST") {
        // The one route that replaces the phone link. Everything saved from the old one stops
        // working, so the hub page and the tray both ask the person before calling it.
        new_link();
        respond_json(client, 200, info_json().dump());
    } else if (r.path == "/hub/notify" && r.method == "GET") {
        // Every credential here (an ntfy topic, a Pushover key, a webhook's secret path) comes
        // back masked to its last four characters. This route lives on the admin listener, so
        // the phone origin cannot reach it at all.
        respond_json(client, 200, RemoteNotify::masked_json().dump());
    } else if (r.path == "/hub/notify" && r.method == "POST") {
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        if (!read_small_body(client, r, body, 64 * 1024)) { respond_json(client, 413, json_error("that is too large for a destination")); return; }
        const auto res = RemoteNotify::configure(body);
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/notify" && r.method == "DELETE") {
        const auto res = RemoteNotify::remove(query_param(r.query, "id"));
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/notify/test" && r.method == "POST") {
        update_notify_link();
        const auto res = RemoteNotify::test(query_param(r.query, "id"), "");
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/hms/override" && r.method == "POST") {
        // Record our own description for an error code. This is the capture path behind the
        // eventual "describe this error" button: the phone sees a code with no sentence, the
        // owner types what the printer's screen said, and every surface shows it from then on.
        // Same shape as --hms-add, and it writes the same user overlay.
        // {"code":"0C00010000020015","text":"...","lang":"en","model":"31B","note":"...","force":false}
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        if (!read_small_body(client, r, body, 64 * 1024)) { respond_json(client, 413, json_error("that is too large for a description")); return; }
        json in;
        try {
            in = json::parse(body.empty() ? "{}" : body);
        } catch (const std::exception&) {
            respond_json(client, 400, json_error("that is not json"));
            return;
        }
        if (!in.is_object()) { respond_json(client, 400, json_error("expected a json object")); return; }
        HMSQuery* q = wxGetApp().get_hms_query();
        if (!q) { respond_json(client, 503, json_error("the error-code lookup is not ready yet")); return; }
        std::string err;
        const bool  ok = q->add_override(in.value("code", std::string()), in.value("text", std::string()),
                                         in.value("lang", std::string("en")), in.value("model", std::string("*")),
                                         in.value("source", std::string("recorded from the hub")),
                                         in.value("note", std::string()), in.value("force", false), err);
        if (!ok) { respond_json(client, 400, json_error(err)); return; }
        std::string normalized;
        HMSQuery::is_valid_code(in.value("code", std::string()), normalized);
        json out;
        out["ok"]   = true;
        out["code"] = normalized;
        // What the surfaces will show for it now, so the caller can confirm the capture landed.
        out["description"] = std::string(q->describe_error(in.value("model", std::string()), normalized).ToUTF8().data());
        out["file"]        = HMSQuery::user_override_path();
        respond_json(client, 200, out);
    } else if (r.path == "/hub/hms/reload" && r.method == "POST") {
        // Re-read both overlay files now, for an edit made outside the app. The lookup also
        // notices an mtime change by itself, so this is a convenience rather than a requirement.
        if (HMSQuery* q = wxGetApp().get_hms_query()) q->reload_overrides();
        json out;
        out["ok"] = true;
        respond_json(client, 200, out);
    } else if (r.path == "/hub/push" && r.method == "GET") {
        // The phones that subscribed to Web Push, each one masked to the push service it uses and
        // the tail of its endpoint. The VAPID public key is here because it is public by design;
        // the private half of that pair never leaves settings.json.
        respond_json(client, 200, WebPush::masked_json().dump());
    } else if (r.path == "/hub/push" && r.method == "DELETE") {
        const auto res = WebPush::remove(query_param(r.query, "id"));
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/push/options" && r.method == "POST") {
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        if (!read_small_body(client, r, body, 16 * 1024)) { respond_json(client, 413, json_error("that is too large")); return; }
        const auto res = WebPush::set_options(body);
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/push/test" && r.method == "POST") {
        // Sent on this thread to every subscribed phone, with no retries: somebody is watching
        // the page for the answer, and a dead endpoint should show as itself.
        update_notify_link();
        const auto res = WebPush::test("");
        write_hub_json(); // a test can prune a subscription the push service says is gone
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/push/debug" && r.method == "POST") {
        // Only answers at all with SNORCA_DEBUG_ROUTES=1 (WebPush::debug_op checks): the gate
        // cross-checks this hub's RFC 8291 and VAPID output against Python and the RFC's own
        // test vectors, and there is no reason for a shipped hub to expose its primitives.
        std::string body;
        if (!read_small_body(client, r, body, 64 * 1024)) { respond_json(client, 413, json_error("that is too large")); return; }
        const auto res = WebPush::debug_op(body);
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/apppush" && r.method == "GET") {
        // The devices the native app registered, each masked to its platform, label and the last
        // four of its token - plus whether each provider could send right now and, when it could
        // not, why in a sentence. The .p8 and the service account appear here only as "has_key".
        respond_json(client, 200, AppPush::masked_json().dump());
    } else if (r.path == "/hub/apppush" && r.method == "DELETE") {
        const auto res = AppPush::remove(query_param(r.query, "id"));
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/apppush/options" && r.method == "POST") {
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        // Larger than the Web Push options cap because a pasted .p8 or service-account JSON can
        // legitimately be a couple of kilobytes.
        if (!read_small_body(client, r, body, 64 * 1024)) { respond_json(client, 413, json_error("that is too large")); return; }
        const auto res = AppPush::set_options(body);
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/apppush/test" && r.method == "POST") {
        // Sent on this thread to every registered device, with no retries: somebody is watching
        // the page for the answer, and each result names the host it reached so an APNs
        // environment mismatch is diagnosable from here rather than from a debugger.
        const auto res = AppPush::test();
        write_hub_json(); // a test can prune a device the platform says is gone
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/apppush/hosted/check" && r.method == "POST") {
        // The hub page's "Check": /healthz on the push service and, when asked, /v1/quota (which
        // also registers this hub on first use) - synchronously, somebody is watching for it.
        std::string body;
        if (!read_small_body(client, r, body, 4 * 1024)) { respond_json(client, 413, json_error("that is too large")); return; }
        const auto res = AppPush::hosted_check(body);
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/apppush/hosted/unregister" && r.method == "POST") {
        // The push service forgets this hub's id, key and counters. It is registered again by the
        // next push, so this is "forget me now", not an off switch (that is the mode).
        const auto res = AppPush::hosted_unregister();
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/apppush/debug" && r.method == "POST") {
        // Only answers at all with SNORCA_DEBUG_ROUTES=1 (AppPush::debug_op checks).
        std::string body;
        if (!read_small_body(client, r, body, 64 * 1024)) { respond_json(client, 413, json_error("that is too large")); return; }
        const auto res = AppPush::debug_op(body);
        respond_json(client, res.first, res.second);
    } else if (r.path == "/hub/quit" && r.method == "POST") {
        respond_json(client, 200, "{\"ok\":true}");
        request_quit("request");
    } else if (r.path.compare(0, 15, "/hub/instances/") == 0 && r.method == "POST") {
        // /hub/instances/<pid>/window?show=1|0, /hub/instances/<pid>/quit[?discard=1] and
        // /hub/instances/<pid>/attention/clear (the hub page's Dismiss)
        const std::string rest  = r.path.substr(15);
        const size_t      slash = rest.find('/');
        const long        pid   = std::atol(rest.substr(0, slash).c_str());
        const std::string sub   = slash == std::string::npos ? "" : rest.substr(slash);
        if (sub == "/window") {
            auto res = instance_post(pid, std::string("/api/window?show=") + (query_param(r.query, "show") == "0" ? "0" : "1"));
            respond_json(client, res.first, res.second);
        } else if (sub == "/attention/clear") {
            auto res = instance_post(pid, "/api/attention/clear");
            respond_json(client, res.first, res.second);
        } else if (sub == "/quit") {
            auto res = instance_post(pid, std::string("/api/quit") + (query_param(r.query, "discard") == "1" ? "?discard=1" : ""));
            respond_json(client, res.first, res.second);
        } else {
            respond_json(client, 404, json_error("no such hub route"));
        }
    } else {
        respond_json(client, 404, json_error("no such hub route"));
    }
}

// The phone reaches a slicer instance's API through /r/<token>/i/<pid>/api/..., and used to reach
// *all* of it: whatever route the instance grew next was exposed the day it was written. This is
// the explicit list instead, one entry per route of the manifest in RemoteAccess::handle_api, so a
// new instance route is opt-in. /api/debug/* is deliberately not here - it is the PC's own back
// door and is not in the manifest either. test_hardening.py compares the two and fails on drift.
static bool instance_api_allowed(const std::string& method, const std::string& sub)
{
    const bool get = method == "GET", post = method == "POST", del = method == "DELETE";
    // DELETE is only ever the G-code archive's; every other route below answers get/post alone, so
    // a DELETE at one of them still falls through to false.
    if (!get && !post && !del) return false;
    auto is_index = [](const std::string& s) {
        return !s.empty() && s.size() <= 9 && s.find_first_not_of("0123456789") == std::string::npos;
    };
    // /api/plates/<index>/<what> and /api/jobs/<id>
    if (sub.compare(0, 12, "/api/plates/") == 0) {
        const std::string  rest  = sub.substr(12);
        const size_t       slash = rest.find('/');
        if (slash == std::string::npos || !is_index(rest.substr(0, slash))) return false;
        const std::string what = rest.substr(slash);
        if (what == "/send") return post; // send the sliced plate to a printer (RemoteSend)
        return get && (what == "/thumbnail.png" || what == "/layout" || what == "/preview" ||
                       what == "/preview.png" || what == "/preview/status");
    }
    if (sub.compare(0, 10, "/api/jobs/") == 0) return get && is_index(sub.substr(10));
    // /api/printers/<id>/control - <id> is a Bambu dev_id, the literal "host" / "connect" or a LAN
    // Snapmaker's sm:<id> (hence the colon and the percent escape the page makes of it), so this
    // segment is a name, not an index; keep it to what a printer id can hold and nothing else. The
    // page sends it through encodeURIComponent, so a percent escape is part of that (the instance
    // decodes it); a slash is not, encoded or otherwise, so this stays one segment.
    if (sub.compare(0, 14, "/api/printers/") == 0) {
        const std::string rest  = sub.substr(14);
        const size_t      slash = rest.find('/');
        if (slash == std::string::npos || rest.substr(slash) != "/control") return false;
        const std::string id = rest.substr(0, slash);
        if (!post || id.empty() || id.size() > 64) return false;
        if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:%") != std::string::npos) return false;
        return id.find("%2f") == std::string::npos && id.find("%2F") == std::string::npos;
    }

    // /api/archive/<id> (GET the record, DELETE it), /api/archive/<id>/thumbnail.png, the stage 2
    // pair /api/archive/<id>/send and /api/archive/<id>/delete, and a Bambu reprint's mapping
    // sheet /api/archive/<id>/preview. The id is a name the
    // archive itself made: letters, digits, dot, dash and underscore, one segment, nothing to
    // decode - checked here before it reaches a file system, and again by GcodeArchive::find.
    if (sub.compare(0, 13, "/api/archive/") == 0) {
        const std::string rest  = sub.substr(13);
        const size_t      slash = rest.find('/');
        const std::string id    = slash == std::string::npos ? rest : rest.substr(0, slash);
        if (id.empty() || id.size() > 200) return false;
        if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos) return false;
        if (id == "." || id == "..") return false;
        if (slash == std::string::npos) return get || del;
        const std::string what = rest.substr(slash);
        if (what == "/thumbnail.png") return get;
        if (what == "/send" || what == "/delete" || what == "/preview") return post;
        return false;
    }

    if (sub == "/api" || sub == "/api/")             return get;
    if (sub == "/api/info")                          return get;
    if (sub == "/api/window")                        return get || post;
    if (sub == "/api/quit")                          return post;
    if (sub == "/api/attention/clear")               return post;
    if (sub == "/api/project/open")                  return post;
    if (sub == "/api/plates")                        return get;
    if (sub == "/api/objects/transform")             return post;
    if (sub == "/api/printers")                      return get;
    if (sub == "/api/snapmaker/devices")             return get;
    if (sub == "/api/snapmaker/add")                 return post;
    if (sub == "/api/snapmaker/remove")              return post;
    if (sub == "/api/snapmaker/connect")             return post;
    if (sub == "/api/snapmaker/disconnect")          return post;
    if (sub == "/api/slice")                         return post;
    if (sub == "/api/events")                        return get;
    if (sub == "/api/jobs")                          return get;
    if (sub == "/api/presets")                       return get;
    if (sub == "/api/presets/select")                return post;
    if (sub == "/api/presets/filament_color")        return post;
    if (sub == "/api/presets/filament_add")          return post;
    if (sub == "/api/settings/process")              return get || post;
    if (sub == "/api/settings/process/revert")       return post;
    if (sub == "/api/settings/process/save")         return post;
    if (sub == "/api/archive")                       return get;
    return false;
}

void HubServer::handle_phone(tcp::socket& client, Request& r, const std::string& rest)
{
    // ---- what the native app asks for (EdgeSlicer app phase 3 follow-ups 1-4 and 6) ----
    // All three answer from what this process knows, with no live slicer instance involved. That is
    // the requirement: the app's Devices screen used to be able to say nothing at all with the
    // slicer closed, because the printers were only reachable through /i/<pid>/api/printers.
    if (r.method == "GET" && (rest == "/summary" || rest == "/summary/")) {
        respond_json(client, 200, summary_json().dump());
        return;
    }
    if (r.method == "GET" && (rest == "/pair" || rest == "/pair/")) {
        respond_json(client, 200, pair_json().dump());
        return;
    }
    // GET /printers/<id>/thumbnail.png - the running job's picture for one printer, out of the
    // G-code archive's sidecars. The id is whatever /api/printers calls the printer, so it is
    // percent-decoded and then only ever compared against what a sidecar says; nothing from the
    // request reaches a file name (printer_thumbnail() builds the path from the sidecar's own id).
    if (r.method == "GET" && rest.compare(0, 10, "/printers/") == 0 && rest.size() > 10) {
        const std::string tail = rest.substr(10);
        const size_t      slash = tail.rfind('/');
        if (slash == std::string::npos || tail.substr(slash) != "/thumbnail.png") {
            respond_json(client, 404, json_error("no such route; see /api"));
            return;
        }
        const std::string id = percent_decode(tail.substr(0, slash));
        if (id.empty() || id.size() > 200) { respond_json(client, 404, json_error("no such printer")); return; }
        const std::string png = printer_thumbnail(id);
        if (png.empty()) { respond_json(client, 404, json_error("no thumbnail for this printer")); return; }
        // Never cached: the next job on the same printer has a different picture at the same URL.
        respond(client, 200, "image/png", png, "Pragma: no-cache\r\nExpires: 0\r\n");
        return;
    }
    // ---- the installable web app: manifest, icons, service worker (P2/P7) ----
    // Everything here is under /r/<token>/, which means three things at once: the token gate the
    // caller already passed is the only gate these need, the manifest's relative start_url and
    // scope resolve to this phone link by construction, and the service worker's scope is exactly
    // this link and nothing wider. A manifest fetch is credentials-omitted by default in every
    // browser - which is fine precisely because the gate is the path and not the cookie. Do not
    // "improve" that into a cookie gate.
    if (r.method == "GET" && rest == "/manifest.webmanifest") {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            token = m_token;
        }
        json m;
        m["id"]               = "/r/" + token + "/";
        m["name"]             = "Edge Hub";
        m["short_name"]       = "Edge Hub";
        m["description"]      = "The cameras, printers and prints on the PC in your workshop.";
        m["start_url"]        = "./"; // resolved against this manifest's own URL, so it keeps the token
        m["scope"]            = "./";
        m["display"]          = "standalone";
        m["orientation"]      = "any";
        m["theme_color"]      = "#1f1f1f";
        m["background_color"] = "#1f1f1f";
        m["icons"]            = json::array({
            json{ { "src", "icon-192.png" }, { "sizes", "192x192" }, { "type", "image/png" }, { "purpose", "any" } },
            json{ { "src", "icon-512.png" }, { "sizes", "512x512" }, { "type", "image/png" }, { "purpose", "any" } },
            json{ { "src", "icon-512-maskable.png" }, { "sizes", "512x512" }, { "type", "image/png" }, { "purpose", "maskable" } }
        });
        respond(client, 200, "application/manifest+json", m.dump(2));
        return;
    }
    if (r.method == "GET" && (rest == "/icon-192.png" || rest == "/icon-512.png" || rest == "/icon-512-maskable.png" ||
                              rest == "/apple-touch-icon.png")) {
        // A closed set of names mapped to a closed set of files: no part of the request reaches
        // the filesystem, so there is nothing here to traverse out of.
        const char* file = rest == "/icon-192.png"          ? "Snapmaker_Orca_192px.png" :
                           rest == "/icon-512.png"          ? "Snapmaker_Orca_512px.png" :
                           rest == "/icon-512-maskable.png" ? "Snapmaker_Orca_512px_maskable.png" :
                                                              "Snapmaker_Orca_180px.png";
        const std::string png = read_file(resources_dir() + "/images/" + file);
        if (png.empty()) { respond(client, 404, "text/plain", "not found"); return; }
        respond(client, 200, "image/png", png);
        return;
    }
    // ---- the LAN probe target ----
    // 43 bytes of transparent GIF, and the only thing on the hub a page on the *other* origin can
    // ask for. The phone page uses it as an <img> beacon to find out whether the LAN hub is
    // reachable before it navigates there, because on the https (Tailscale) origin a fetch() to
    // http://<lan ip>:13640 is blocked outright as mixed content and never even reports failure.
    // An <img> is passive mixed content and gets further in some browsers - see the note in
    // stream_center.html; the beacon is best-effort and the manual switch is the reliable path.
    if (r.method == "GET" && rest == "/ping.gif") {
        static const unsigned char GIF[] = {
            0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xFF, 0xFF, 0xFF, 0x21, 0xF9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3B
        };
        // Never cached: a cached hit would say "the LAN is reachable" from a train.
        respond(client, 200, "image/gif", std::string((const char*) GIF, sizeof(GIF)),
                "Pragma: no-cache\r\nExpires: 0\r\n");
        return;
    }
    if (r.method == "GET" && rest == "/sw.js") {
        // Served from under the token so its scope is /r/<token>/ - a worker served from the root
        // would claim every phone link this hub ever hands out.
        const std::string js = read_file(resources_dir() + "/web/orca/sw.js");
        if (js.empty()) { respond(client, 404, "text/plain", "not found"); return; }
        respond(client, 200, "application/javascript", js);
        return;
    }
    // ---- Web Push (P7) ----
    if (rest == "/push/key" && r.method == "GET") {
        // The application server key the page hands to PushManager.subscribe(). Public by
        // definition: it is what the push service checks our signature against.
        json j;
        j["key"] = WebPush::public_key_b64u();
        respond_json(client, 200, j.dump());
        return;
    }
    if (rest == "/push/subscription" && r.method == "POST") {
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        if (!read_small_body(client, r, body, 16 * 1024)) { respond_json(client, 413, json_error("that is too large for a subscription")); return; }
        const auto res = WebPush::add_subscription(body);
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
        return;
    }
    if (rest == "/push/subscription" && r.method == "DELETE") {
        // The page saying it unsubscribed. It has to name the endpoint in full, which only the
        // phone that owns it knows, and the answer is the same either way so this cannot be used
        // to find out whether some endpoint is subscribed here.
        std::string body;
        if (!read_small_body(client, r, body, 16 * 1024)) { respond_json(client, 413, json_error("that is too large")); return; }
        std::string endpoint;
        try {
            const json in = json::parse(body);
            if (in.is_object() && in.contains("endpoint") && in["endpoint"].is_string()) endpoint = in["endpoint"].get<std::string>();
        } catch (...) {}
        const auto res = WebPush::remove_subscription(endpoint);
        write_hub_json();
        respond_json(client, res.first, res.second);
        return;
    }
    // ---- native app push (EdgeSlicer app phase 1) ----
    if (rest == "/push/device" && r.method == "POST") {
        // The app registering its APNs or FCM token and the public half of the key pair it made.
        // Same shape and same caps as /push/subscription: this is a subscription, with a device
        // token where a browser would have an endpoint.
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
        std::string body;
        if (!read_small_body(client, r, body, 16 * 1024)) { respond_json(client, 413, json_error("that is too large for a device")); return; }
        const auto res = AppPush::register_device(body);
        if (res.first == 200) write_hub_json();
        respond_json(client, res.first, res.second);
        return;
    }
    if (rest == "/push/device" && r.method == "DELETE") {
        // The app unpairing. It has to name the token in full, which only that device knows, and
        // the answer is the same either way so this cannot be used to find out whether some
        // device token is registered here.
        std::string body;
        if (!read_small_body(client, r, body, 16 * 1024)) { respond_json(client, 413, json_error("that is too large")); return; }
        const auto res = AppPush::forget_device(body);
        write_hub_json();
        respond_json(client, res.first, res.second);
        return;
    }
    // ---- hub-level API (instances, uploads) ----
    if (rest == "/api" || rest == "/api/") {
        json j;
        j["name"]    = "EdgeSlicer hub API";
        j["version"] = 2;
        j["routes"]  = json::array({
            { {"method", "GET"},  {"path", "/api/instances"},       {"description", "running slicer instances: id (pid), index, title, project path, slicing"} },
            { {"method", "GET"},  {"path", "/api/archive[?offset=&limit=&printer={id}&model={key}]"}, {"description", "the G-code archive, read by the hub (no slicer window needed): {enabled?, max?, total, offset, limit, next_offset, printers [{id, name, kind}], models [{key, name, count, printers [{id, name, kind, online}]}], records}, newest first; records are the same rows /i/{id}/api/archive lists plus model_key / model_name (the model the file was sliced for; \"other\" when unknown), and a printer is never named by an address. models[].printers are where a record of that model may be reprinted (send with printer={id}). Sending or deleting one still goes through a slicer window (/i/{id}/api/archive/{id}/send; a Bambu one takes /i/{id}/api/archive/{id}/preview first for its AMS mapping)"} },
            { {"method", "GET"},  {"path", "/api/archive/{id}/thumbnail.png"}, {"description", "one record's preview; cacheable, since a record's preview never changes"} },
            { {"method", "POST"}, {"path", "/api/instances/open"},  {"description", "body = a .3mf/.stl/.obj/.step/.glb file, header X-File-Name = its name; starts a new (hidden) slicer instance with it; ?visible=1 opens a window"} },
            { {"method", "POST"}, {"path", "/i/{id}/open?mode=load|import"}, {"description", "same upload, opened in instance {id}: load = save the current project, then open this project (default for .3mf); import = add the model to the current plate (default otherwise)"} },
            { {"method", "*"},    {"path", "/i/{id}/api/..."},      {"description", "the instance's own API (see GET /i/{id}/api)"} },
            { {"method", "GET"},  {"path", "/state"},               {"description", "camera list for the stream wall, plus lan_url / remote_url / ips (the two origins this hub answers on), the last-known printer rows and hub_instance"} },
            { {"method", "GET"},  {"path", "/summary"},             {"description", "one answer for a phone's device list: hub / host / version / hub_instance, both URLs, last_id, and the printers (id, name, model, vendor, state, percent, left_time_s, job, temps when known, camera, thumbnail, instance pid, plus age_s and stale). Answers with no slicer window open: these are the rows the hub last read off the open windows"} },
            { {"method", "GET"},  {"path", "/pair"},                {"description", "the pairing document: name (this PC), hub, version, hub_instance, urls {lan, remote}, ips, push {webpush, apns, fcm, unified}, token_version and features"} },
            { {"method", "GET"},  {"path", "/printers/{id}/thumbnail.png"}, {"description", "the running job's picture for that printer, from the G-code archive; 404 when there is none"} },
            { {"method", "GET"},  {"path", "/ping.gif"},            {"description", "a 43-byte never-cached GIF; the phone page's beacon for \"is the home network reachable from here\""} },
            { {"method", "GET"},  {"path", "/events?since={id}"},   {"description", "printer events the slicer instances reported (started / finished / failed / cancelled / paused / resumed / runout / error), newest last: {events, last_id, hub_instance}. hub_instance changes only when a data dir's hub settings are created, so a client can tell an id reset from an id it has already seen"} },
            { {"method", "GET"},  {"path", "/push/key"},            {"description", "the hub's VAPID public key, for PushManager.subscribe()"} },
            { {"method", "POST"}, {"path", "/push/subscription"},   {"description", "this browser's PushSubscription JSON; re-post it on every launch"} },
            { {"method", "DELETE"}, {"path", "/push/subscription"}, {"description", "body {endpoint} - this browser unsubscribed"} },
            { {"method", "POST"}, {"path", "/push/device"},        {"description", "the native app's APNs/FCM device token plus its own p256dh/auth; re-post it on every cold launch"} },
            { {"method", "DELETE"}, {"path", "/push/device"},      {"description", "body {platform, token} - this device unpaired"} }
        });
        respond_json(client, 200, j.dump());
        return;
    }
    if (rest == "/api/instances" && r.method == "GET") {
        respond_json(client, 200, instances_json().dump());
        return;
    }
    // The Reprint tab's list, from the hub itself (archive_page_json): no slicer window needed.
    if (rest == "/api/archive" && r.method == "GET") {
        const std::string off = query_param(r.query, "offset"), lim = query_param(r.query, "limit");
        respond_json(client, 200,
                     archive_page_json(percent_decode(query_param(r.query, "printer")), off.empty() ? 0 : std::atoi(off.c_str()),
                                       lim.empty() ? 200 : std::atoi(lim.c_str()), percent_decode(query_param(r.query, "model")))
                         .dump());
        return;
    }
    // GET /api/archive/<id>/thumbnail.png - one record's preview. A record's preview never
    // changes (a new send is a new record with a new id), so this one may be cached, which is
    // what makes scrolling back up the list free.
    if (r.method == "GET" && rest.compare(0, 13, "/api/archive/") == 0) {
        const std::string tail  = rest.substr(13);
        const size_t      slash = tail.find('/');
        if (slash != std::string::npos && tail.substr(slash) == "/thumbnail.png") {
            const std::string png = archive_thumbnail(tail.substr(0, slash));
            if (png.empty()) { respond_json(client, 404, json_error("no thumbnail for this record")); return; }
            respond_cacheable(client, "image/png", png);
            return;
        }
    }
    if (rest == "/api/instances/open" && r.method == "POST") {
        // Refuse before reading the upload: there is no point spooling a gigabyte we will not open.
        const int open = instances_in_flight();
        if (open >= MAX_INSTANCES) {
            respond_json(client, 429, json_error("too many slicer windows are open (" + std::to_string(open) + " of " +
                                                 std::to_string(MAX_INSTANCES) + "); close one on the PC, or open this file in one of them"));
            return;
        }
        std::string path, error;
        if (!spool_upload(client, r, path, error)) { respond_json(client, 400, json_error(error)); return; }
        gc_uploads();
        const long pid = spawn_slicer(path, query_param(r.query, "visible") != "1");
        json j;
        j["ok"]      = pid > 0;
        j["file"]    = path;
        j["spawned"] = pid;
        respond_json(client, pid > 0 ? 200 : 500, pid > 0 ? j.dump() : json_error("could not start a new slicer"));
        return;
    }
    // ---- per-instance: /i/<pid>/open, /i/<pid>/api/... ----
    if (rest.compare(0, 3, "/i/") == 0) {
        const size_t slash = rest.find('/', 3);
        const long   pid   = std::atol(rest.substr(3, slash == std::string::npos ? std::string::npos : slash - 3).c_str());
        const std::string sub = slash == std::string::npos ? "" : rest.substr(slash);
        Instance target;
        for (const Instance& inst : instances(false))
            if (inst.pid == pid) target = inst;
        if (!target.alive) { respond_json(client, 404, json_error("no such slicer instance")); return; }
        const std::string base = "http://127.0.0.1:" + std::to_string(target.port);
        if (sub == "/open" && r.method == "POST") {
            std::string path, error;
            if (!spool_upload(client, r, path, error)) { respond_json(client, 400, json_error(error)); return; }
            gc_uploads();
            std::string mode = query_param(r.query, "mode");
            if (mode.empty()) mode = lower(fs::path(path).extension().string()) == ".3mf" ? "load" : "import";
            int         status = 502;
            std::string body   = json_error("the slicer did not answer");
            Http::post(base + "/api/project/open")
                .timeout_connect(2).timeout_max(900)
                .header("Content-Type", "application/x-www-form-urlencoded")
                .set_post_body("path=" + percent_encode(path) + "&mode=" + mode)
                .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
                .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? json_error(err) : b; })
                .perform_sync();
            respond_json(client, status, body);
            return;
        }
        if (sub.compare(0, 4, "/api") == 0 && (sub.size() == 4 || sub[4] == '/')) {
            if (!instance_api_allowed(r.method, sub)) { respond_json(client, 404, json_error("route not proxied; see /api")); return; }
            if (r.content_length > MAX_API_BODY) { respond_json(client, 413, json_error("body too large")); return; }
            // Replay the request against the instance with the prefix stripped; the instance
            // answers with Connection: close, so the splice ends by itself.
            const std::string line = r.method + " " + sub + (r.query.empty() ? "" : "?" + r.query) + " HTTP/1.1\r\n";
            const std::string head = line + r.head.substr(r.head.find("\r\n") + 2);
            try {
                tunnel(client, target.port, force_close(head), r.pending);
            } catch (const std::exception& e) {
                respond_json(client, 502, json_error(std::string("slicer unreachable: ") + e.what()));
            }
            return;
        }
        respond_json(client, 404, json_error("no such route; see /api"));
        return;
    }
    respond_json(client, 404, json_error("no such route; see /api"));
}

// What a phone gets when it opens a link this hub has replaced. It says what happened and where
// to get the new one, and deliberately nothing else: no token, no address, no name of this PC.
static const char* const REPLACED_LINK_PAGE = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, viewport-fit=cover">
<meta name="robots" content="noindex">
<title>This link was replaced</title>
<style>
body { margin: 0; background: #2b2b2b; color: #ddd; font-family: 'Segoe UI', Roboto, -apple-system, sans-serif; font-size: 15px; line-height: 1.5; }
.wrap { max-width: 26em; margin: 0 auto; padding: 12vh 22px 22px; }
h1 { font-size: 20px; color: #fff; margin: 0 0 14px; }
p { margin: 0 0 12px; }
.note { color: #999; font-size: 13px; }
</style></head>
<body><div class="wrap">
<h1>This link was replaced</h1>
<p>A new phone link was made on the PC, so this one no longer works.</p>
<p>On the PC, open the hub page from the EdgeSlicer icon next to the clock and scan the new code.</p>
<p class="note">Anything saved from the old link - a bookmark, a QR code, a home-screen icon - has to be made again from the new one.</p>
</div></body></html>
)HTML";

void HubServer::serve(std::unique_ptr<tcp::socket> owner, bool admin)
{
    // Each listener has its own budget: a flood on the phone listener must not stop the PC's own
    // page and slicer instances from reaching the control plane.
    static ConnCount s_main, s_admin;
    ConnGuard        guard(admin ? s_admin : s_main, admin ? MAX_ADMIN_CONNECTIONS : MAX_CONNECTIONS);
    tcp::socket&     client = *owner;
    // shutdown() ends this connection through here: a long-lived tunnel (a camera WebSocket on
    // /api/ws) or a stream would otherwise keep this thread - and its socket - past the server.
    ServerLifetime::Tracked tracked(m_life, client);
    try {
        boost::system::error_code ec;
        const auto peer = client.remote_endpoint(ec).address();
        if (ec || !is_private_v4(peer)) return;
        client.set_option(tcp::no_delay(true));
        Request r;
        if (!read_request(client, r)) return;
        // Over the cap: refuse only after the request head has been read, so closing the socket does
        // not reset the connection with the request still unread (the 503 would then be lost).
        if (!guard.ok()) { respond(client, 503, "text/plain; charset=utf-8", "the hub has too many connections open; try again"); return; }

        std::string token, secret, auth;
        std::vector<std::string> old_tokens;
        int         go2rtc_port;
        bool        phone;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            token       = m_token;
            old_tokens  = m_old_tokens;
            secret      = m_secret;
            auth        = m_go2rtc_auth;
            go2rtc_port = m_go2rtc_port;
            phone       = m_phone;
        }

        // The control plane answers only on the admin listener, and the admin listener answers
        // only the control plane. Anything a tunnel can front is on the other socket.
        const bool control_route = r.path.compare(0, 5, "/hub/") == 0 || r.path == "/relay/h264";
        if (admin != control_route) { respond(client, 404, "text/plain", "not found"); return; }

        // Through Tailscale Serve (a loopback peer carrying Tailscale-User-Login, which Serve sets
        // and strips from clients) only allow-listed tailnet logins get anything at all.
        //
        // Whether those two headers may be believed at all is decided in exactly one place, and
        // this is the call site: Testing::trusted_proxy_headers(). Serve terminates on loopback and
        // overwrites them, which is the entire basis for trusting them, so a request that reached
        // the LAN listener from a real network carrying its own Tailscale-User-Login has them
        // cleared here and can never reach login_allowed() with it. Phase 1's relayed streams are
        // re-injected as loopback peers and will pass `via_relay = true` for the same reason: they
        // look like Serve and are not. `r` is this session's own Request, so clearing is local.
        if (!Testing::trusted_proxy_headers(peer.is_loopback(), r.via_relay)) {
            if (!r.ts_login.empty())
                BOOST_LOG_TRIVIAL(warning) << "RemoteHub: ignoring a Tailscale-User-Login header from a non-loopback peer";
            r.ts_login.clear();
            r.fwd_proto.clear();
        }
        const bool via_serve = peer.is_loopback() && !r.ts_login.empty();
        if (via_serve && !login_allowed(r.ts_login)) {
            BOOST_LOG_TRIVIAL(warning) << "RemoteHub: tailnet login not allowed: " << r.ts_login;
            respond(client, 403, "text/plain; charset=utf-8", "This printer hub is not shared with " + r.ts_login + ". Ask its owner to add you on the hub page.");
            return;
        }
        const std::string cookie_flags = (via_serve && r.fwd_proto == "https") ? "; Secure" : "";

        // The stream player lives at the root so its relative "api/ws" resolves here. Gate: the
        // phone's rt cookie, or - loopback only - the hub secret as `lt`, which is how the PC's own
        // Stream tab embeds it. The two scripts are public (verbatim go2rtc files).
        const bool player_ok = ct_equal(cookie_value(r.cookies, "rt"), token) ||
                               (peer.is_loopback() && ct_equal(query_param(r.query, "lt"), secret));
        if (r.path == PLAYER_PAGE) {
            if (!player_ok) { respond(client, 404, "text/plain", "not found"); return; }
            respond(client, 200, "text/html; charset=utf-8", read_file(resources_dir() + "/web/orca/player.html"));
            return;
        }
        for (const char* p : PLAYER_JS) {
            if (r.path == p) { respond(client, 200, "application/javascript", read_file(resources_dir() + "/web/orca" + std::string(p))); return; }
        }
        if (r.path == GO2RTC_WS) {
            if (!player_ok) { respond(client, 404, "text/plain", "not found"); return; }
            if (go2rtc_port == 0) { respond(client, 503, "text/plain", "stream relay is not running"); return; }
            tunnel(client, go2rtc_port, force_close(go2rtc_head(r.head, auth)), r.pending);
            return;
        }
        // go2rtc pulls a printer's raw H.264 through here (loopback + hub secret; see relay_h264).
        if (r.path == "/relay/h264") {
            if (!(peer.is_loopback() && ct_equal(query_param(r.query, "lt"), secret))) { respond(client, 404, "text/plain", "not found"); return; }
            const std::string url = relay_h264_url(query_param(r.query, "id"));
            if (url.empty()) { respond(client, 404, "text/plain", "unknown camera"); return; }
            relay_h264(client, url);
            return;
        }
        // Control routes for the slicer instances and the hub page on this PC. They only exist on
        // the loopback-only admin listener (checked above), and on top of that: loopback peer,
        // loopback Host (no DNS rebinding), never cross-site, and the per-run secret as a custom
        // header - a cross-origin page cannot send one without a preflight we never answer. The
        // page and its script are the only unauthenticated GETs; the page carries the secret.
        if (r.path.compare(0, 5, "/hub/") == 0) {
            if (!peer.is_loopback() || !loopback_host(r.host) || r.sec_fetch_site == "cross-site") { respond(client, 404, "text/plain", "not found"); return; }
            const bool page = r.method == "GET" && (r.path == "/hub/" || r.path == "/hub/index.html" || r.path == "/hub/qrcode.js");
            if (!page && !ct_equal(r.secret, secret)) { respond_json(client, 403, json_error("missing or wrong X-Hub-Secret")); return; }
            handle_hub(client, r);
            return;
        }
        // /r/<token>/... - the token pulled out of the path first, so it can be compared in
        // constant time instead of as a path prefix.
        const std::string prefix = "/r/" + token;
        std::string       rest, given;
        bool              token_ok = false;
        if (r.path.compare(0, 3, "/r/") == 0) {
            const size_t slash = r.path.find('/', 3);
            given    = r.path.substr(3, slash == std::string::npos ? std::string::npos : slash - 3);
            token_ok = ct_equal(given, token);
            if (slash != std::string::npos) rest = r.path.substr(slash);
        }
        if (!token_ok || (!phone && !peer.is_loopback())) {
            // A link we replaced ourselves gets a page saying so - an installed icon or a saved QR
            // would otherwise open a blank 404 and nobody could tell it from a typo. Same reach as
            // a live link (the LAN only while phone access is on), same constant-time compare, and
            // the page names neither the new token nor anything else.
            if (!token_ok && (phone || peer.is_loopback()))
                for (const std::string& old : old_tokens)
                    if (ct_equal(given, old)) {
                        respond(client, 404, "text/html; charset=utf-8", REPLACED_LINK_PAGE, "X-Frame-Options: DENY\r\n");
                        return;
                    }
            respond(client, 404, "text/plain", "not found");
            return;
        }
        if (rest.empty()) { respond(client, 302, "text/plain", "", "Location: " + prefix + "/\r\n"); return; }
        if (rest == "/" || rest == "/index.html") {
            // A top-level page: never in anybody's frame. (The player at /stream.html deliberately
            // carries no X-Frame-Options - this page frames it, and so does the PC's Stream tab
            // from a different origin, which even SAMEORIGIN would block.)
            respond(client, 200, "text/html; charset=utf-8", read_file(resources_dir() + "/web/orca/stream_center.html"),
                    "X-Frame-Options: DENY\r\nSet-Cookie: rt=" + token + "; Path=/; SameSite=Lax" + cookie_flags + "\r\n");
        } else if (rest == "/state") {
            // Through Tailscale Serve the phone learns who it is signed in as (shown in its top bar).
            std::string st = state_for_phone();
            if (via_serve) {
                try {
                    json j            = json::parse(st);
                    j["remote_login"] = r.ts_login;
                    st                = j.dump();
                } catch (...) {}
            }
            respond_json(client, 200, st);
        } else if (rest == "/events") {
            // What the printers have been doing, for the Devices tab's badge and its list. Only the
            // event fields: nothing here names this PC, its data dir, its instances' ports or the
            // secret, and the token gate above is the same one the page itself passed.
            respond_json(client, 200, events_json(std::atoi(query_param(r.query, "since").c_str())).dump());
        } else if (rest == "/bambu") {
            std::string ip, code;
            if (!lookup_host(query_param(r.query, "id"), ip, code) || code.empty()) { respond(client, 404, "text/plain", "unknown camera"); return; }
            const int relay = BambuCamRelay::get().port();
            if (relay == 0) { respond(client, 503, "text/plain", "camera relay is not running"); return; }
            // Quality for the MJPEG path: the phone passes ?fps= and the relay drops frames to
            // match (BambuCamRelay). Clamped here as well as there - this is the tunnelled
            // listener, so the value arrives from the phone and must reach the relay as nothing
            // but a small integer.
            int fps = std::atoi(query_param(r.query, "fps").c_str());
            if (fps < 0 || fps > 60) fps = 0;
            const std::string new_head = "GET /bambu?ip=" + percent_encode(ip) + "&code=" + percent_encode(code) +
                                         (fps > 0 ? "&fps=" + std::to_string(fps) : "") + " HTTP/1.1\r\n" +
                                         r.head.substr(r.head.find("\r\n") + 2);
            tunnel(client, relay, force_close(new_head), "");
        } else if (rest == "/ff") {
            std::string ip, code;
            if (!lookup_host(query_param(r.query, "id"), ip, code) || ip.find_first_of("\"'\\<>") != std::string::npos) {
                respond(client, 404, "text/plain", "unknown printer");
                return;
            }
            std::string url;
            Http::post("http://" + ip + ":8898/detail")
                .timeout_connect(4).timeout_max(8)
                .header("Content-Type", "application/json")
                .set_post_body(std::string("{\"serialNumber\":\"\",\"checkCode\":\"\"}"))
                .on_complete([&url](std::string body, unsigned) { url = ff_camera_url_from_detail(body); })
                .perform_sync();
            json j;
            j["url"] = url;
            respond_json(client, 200, j.dump());
        } else {
            handle_phone(client, r, rest);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "RemoteHub: session ended: " << e.what();
    }
}

bool HubServer::start()
{
    ensure_dirs();
    {
        boost::system::error_code ig;
        fs::remove(last_exit_json_path(), ig); // describes a hub that is no longer the latest
    }
    // Settings from the last run, unless the caller decided them.
    try {
        json j = json::parse(read_file(hub_json_path()));
        if (!valid_token(m_token)) m_token = j.value("token", "");
        m_phone = m_phone || j.value("phone", false);
    } catch (...) {}
    // Remote access settings live in their own file: hub.json is removed on a clean quit (it
    // is how instances find a live hub), which used to lose remote_on and the allow-list and
    // left Tailscale Serve answering 403 until remote access was switched on again.
    json notify_saved = json::object(), webpush_saved = json::object(), apppush_saved = json::object();
    // Migration: before settings.json existed these two lived in hub.json. A data dir whose last
    // hub was killed rather than quit still has that file, so import them once - settings.json,
    // written below by write_hub_json(), wins from then on.
    if (!fs::exists(settings_json_path())) {
        try {
            json j = json::parse(read_file(hub_json_path()));
            if (j.contains("remote_on") || j.contains("allowed_logins")) {
                m_remote_on = j.value("remote_on", false);
                for (const auto& l : j.value("allowed_logins", json::array())) m_allowed_logins.push_back(lower(l.get<std::string>()));
                BOOST_LOG_TRIVIAL(info) << "RemoteHub: imported remote access settings from an old hub.json";
            }
        } catch (...) {}
    }
    try {
        json j      = json::parse(read_file(settings_json_path()));
        m_remote_on = j.value("remote_on", false);
        // The phone switch as it was last set, from the tray, the hub page or the slicer. This
        // beats the --hub-phone hint the slicer passes (its app config only learns of a change
        // made from the hub page while its Stream panel happens to be polling), exactly as the
        // remembered token beats --hub-token below; the hint only seeds a data dir with no
        // saved switch yet.
        if (j.contains("phone")) m_phone = j.value("phone", false);
        // ... and so do the notification destinations: an ntfy topic or a Pushover key set up
        // once must survive every hub restart.
        notify_saved = j.value("notify", json::object());
        webpush_saved = j.value("webpush", json::object());
        apppush_saved = j.value("apppush", json::object());
        for (const auto& l : j.value("allowed_logins", json::array())) m_allowed_logins.push_back(lower(l.get<std::string>()));
        // ... and so does the phone link, for the same reason: hub.json is gone after a clean
        // quit, and a hub that came back with a new token would break every saved link. The
        // remembered one beats a --hub-token from the slicer, whose copy can be older than the
        // last "New link" somebody made from the tray; a hint only seeds a data dir with none.
        // This hub's identity, for the phone app: minted when these settings are first written and
        // never again, so a client can tell a fresh data dir (whose event ids restart at 1) from
        // this one carrying on. See hub_instance().
        m_hub_instance = j.value("hub_instance", std::string());
        // ... and this data dir's relay identity, if it already has one. A settings file written
        // before this existed, or one whose identity object is malformed, simply yields an invalid
        // one and a fresh pair is minted below.
        m_identity = Testing::identity_from_settings(j.dump());
        // A data dir written before this existed gets its version from what it can still see: the
        // links it remembers replacing, plus this one. Only ever a floor, and it only ever grows.
        m_token_version = std::max(1, j.value("token_version", 0));
        const std::string saved = j.value("token", "");
        for (const auto& t : j.value("old_tokens", json::array())) remember_old_token_locked(t.get<std::string>());
        if (valid_token(saved)) {
            if (valid_token(m_token) && m_token != saved)
                BOOST_LOG_TRIVIAL(info) << "RemoteHub: keeping this data dir's phone link, not the one passed on the command line";
            m_token = saved;
        }
    } catch (...) {} // nothing else runs yet: start() is single-threaded, the _locked helper is safe here
    if (!valid_token(m_token)) m_token = random_token();
    // A data dir with no settings.json yet (or one written before this existed) gets its uuid here,
    // once. write_hub_json() below persists it, and nothing ever replaces it.
    if ((int) m_old_tokens.size() + 1 > m_token_version) m_token_version = (int) m_old_tokens.size() + 1;
    if (m_hub_instance.empty()) {
        m_hub_instance = random_hex(16);
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: this data dir's hub instance is " << m_hub_instance;
    }
    // The durable relay identity, minted once per data dir. Only the hubid is ever logged: the
    // public key is long and uninteresting in a log, and the private key must never appear in one.
    if (!m_identity.valid()) {
        m_identity = mint_hub_identity();
        if (m_identity.valid()) BOOST_LOG_TRIVIAL(info) << "RemoteHub: this data dir's hub id is " << m_identity.hubid;
        else BOOST_LOG_TRIVIAL(warning) << "RemoteHub: could not generate the hub identity (Ed25519 unavailable); remote registration will not be possible";
    }
    m_secret = random_hex(16);
    m_state  = read_file(streams_json_path());
    load_events(); // what the printers did before this hub was restarted, and where the ids got to
    load_printers(); // ... and what they were doing, so the app's first refresh is not blank
    // Web Push before the relay worker: RemoteNotify::deliver() asks it whether any phone is
    // subscribed before deciding there is nothing to queue.
    WebPush::start(webpush_saved); // mints the VAPID key pair the first time this data dir runs
    // The identity signs every request to the hosted push service; handed over before start() so
    // the very first push can be signed. Only AppPush's memory holds the private half.
    AppPush::set_identity(m_identity.hubid, m_identity.public_hex, m_identity.private_hex);
    AppPush::start(apppush_saved);  // reads the .p8 and the service account, if either is set
    RemoteNotify::start(notify_saved); // the relay worker; deliver() is a no-op until it has one

    gc_uploads(); // whatever last time left behind, before anything new lands
    start_go2rtc();
    BambuCamRelay::get().port();
    // The control plane first: register_streams() points go2rtc at /relay/h264 on the admin port.
    if (!bind_admin()) return false;
    // bind() itself reconciles Tailscale Serve's forwarding target against m_port whenever
    // m_remote_on is already true (Serve's config outlives us - it is Tailscale's own - so a hub
    // that starts on a different port than last time, or moved because HUB_PORT was taken, must
    // re-point it or the tailnet URL keeps forwarding to a dead port). That covers both this
    // start-up bind and every later rebind from set_phone(), so there is nothing more to do here.
    if (!bind(m_phone)) return false;
    write_hub_json();
    register_streams();
    return true;
}

void HubServer::loop(bool idle_exit)
{
    auto idle_since = std::chrono::steady_clock::now();
    auto printers_at = std::chrono::steady_clock::now() - std::chrono::milliseconds(PRINTERS_POLL_MS);
    while (!m_quit) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        flush_logs(); // the file sink buffers; keep hub.log readable while we run
        // The printers every open window can see, remembered here so /summary and /state can
        // answer once every window is closed. One round costs one loopback GET per instance and
        // runs on this thread, never on a request.
        if (std::chrono::steady_clock::now() - printers_at >= std::chrono::milliseconds(PRINTERS_POLL_MS)) {
            printers_at = std::chrono::steady_clock::now();
            try { poll_printers(); } catch (...) {}
        }
        // A phone subscribed, or the sender pruned one the push service said was gone. Saving
        // from here means the sender thread never has to reach back into HubServer.
        if (WebPush::consume_dirty() || AppPush::consume_dirty()) write_hub_json();
        if (!idle_exit) continue;
        bool phone;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            phone = m_phone;
        }
        const bool busy = phone || !instances(false).empty();
        const auto now  = std::chrono::steady_clock::now();
        if (busy) idle_since = now;
        else if (now - idle_since > std::chrono::seconds(IDLE_EXIT_SECONDS)) {
            BOOST_LOG_TRIVIAL(info) << "RemoteHub: idle (phone access off, no slicer running), exiting";
            request_quit("idle");
            break;
        }
    }
}

bool HubServer::shutdown()
{
    {
        // The exit note first, then hub.json: a slicer that finds hub.json gone must find the note.
        const char* reason = m_quit_reason.load();
        json        note;
        note["pid"]    = current_pid();
        note["reason"] = reason ? reason : "exit";
        note["at"]     = (long long) std::time(nullptr);
        write_file(last_exit_json_path(), note.dump());
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: shutting down (" << (reason ? reason : "exit") << ")";
        boost::system::error_code ig;
        fs::remove(hub_json_path(), ig);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_acceptor) m_acceptor->close(ig);
        if (m_admin_acceptor) m_admin_acceptor->close(ig);
    }
    // The accept loops end on the closed acceptors; every open connection is shut down so its
    // blocking read returns, and then we wait for all of those threads - and the helper threads
    // (firewall, Tailscale) - to be gone before anything they use is destroyed. Not under m_mutex:
    // the connections take it. Crash a2375b06 was a tunnel thread destroying its socket after
    // HubApp had already destroyed this server, and the io_context with it.
    const bool drained = m_life.stop(std::chrono::milliseconds(SHUTDOWN_DRAIN_MS));
    if (!drained)
        BOOST_LOG_TRIVIAL(warning) << "RemoteHub: " << m_life.running() << " connection/helper thread(s) still running after "
                                   << SHUTDOWN_DRAIN_MS << " ms; the server is left in place for them";
#ifndef _WIN32
    if (m_go2rtc_pid > 0) ::kill((pid_t) m_go2rtc_pid, SIGTERM);
#endif
    // WebPush first: it only sets a flag, and it is that flag which lets a push waiting out a
    // retry backoff give up, so the join below does not have to wait for it.
    WebPush::stop();
    AppPush::stop();      // same reason: sets the flag a backoff checks, and drops the credentials
    RemoteNotify::stop(); // let an in-flight relay send finish, then join the worker
    // On Windows the kill-on-close job object takes go2rtc down with us.
    flush_logs();
    return drained;
}

// ------------------------------------------------------------ tray icon ----

// The hub's face on the desktop: phone access toggle, the hub page (QR code, link, open
// slicers), a new slicer window, quit. Double-click opens the page.
class HubTaskBarIcon : public wxTaskBarIcon
{
public:
    enum { ID_STATUS = wxID_HIGHEST + 100, ID_PHONE, ID_NEWLINK, ID_PAGE, ID_NEW, ID_NEW_HIDDEN, ID_QUIT,
           ID_INST_FIRST = wxID_HIGHEST + 200, ID_INST_PER = 3, ID_INST_MAX = 32,
           ID_INST_LAST = ID_INST_FIRST + ID_INST_PER * ID_INST_MAX };

    HubTaskBarIcon(HubServer& server, std::function<void()> on_quit) : m_server(server), m_on_quit(std::move(on_quit))
    {
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_server.set_phone(!m_server.snapshot().phone, ""); refresh(); }, ID_PHONE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            // The only thing in this menu that breaks something a person made; ask in those words.
            if (wxMessageBox("Make a new phone link?\n\nThe link this hub is using now stops working. Saved links, QR codes "
                             "and home-screen icons made from it all have to be made again from the new code.",
                             "New phone link", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION) != wxYES)
                return;
            m_server.new_link();
            refresh();
            open_page(); // where the new code is
        }, ID_NEWLINK);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { open_page(); }, ID_PAGE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_server.spawn_slicer(""); }, ID_NEW);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_server.spawn_slicer("", true); }, ID_NEW_HIDDEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
            const int n = (e.GetId() - ID_INST_FIRST) / ID_INST_PER, what = (e.GetId() - ID_INST_FIRST) % ID_INST_PER;
            if (n < 0 || n >= (int) m_menu_pids.size()) return;
            const long pid = m_menu_pids[n];
            HubServer* s   = &m_server;
            // HTTP: never on the tray's (GUI) thread. Counted by the server, which waits for it.
            s->spawn([s, pid, what]() {
                if (what == 2) s->instance_quit(pid, false);
                else           s->instance_window(pid, what == 0);
            });
        }, ID_INST_FIRST, ID_INST_LAST);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_on_quit(); }, ID_QUIT);
        Bind(wxEVT_TASKBAR_LEFT_DCLICK, [this](wxTaskBarIconEvent&) { open_page(); });
    }

    void set_icon(const wxIcon& icon)
    {
        m_icon = icon;
        refresh();
    }

    // A printer event, as the balloon the tray icon already owns (Shell_NotifyIcon's NIF_INFO on
    // Windows). Called from the server thread, so the work is handed to the GUI thread; the icon
    // may be gone by then, which is why it is looked up again rather than captured.
    void balloon(const std::string& title, const std::string& text, const std::string& severity)
    {
        wxTaskBarIcon* self = this;
        const wxString t = wxString::FromUTF8(title), b = wxString::FromUTF8(text);
        const int      flags = severity == "error" ? wxICON_ERROR : severity == "warning" ? wxICON_WARNING : wxICON_INFORMATION;
        wxTheApp->CallAfter([self, t, b, flags]() {
#if defined(__WXMSW__) && wxUSE_TASKBARICON_BALLOONS
            self->ShowBalloon(t.IsEmpty() ? wxString("Edge Hub") : t, b, 10000, flags);
#else
            (void) self; (void) t; (void) b; (void) flags;
#endif
        });
    }

    void refresh()
    {
        const HubServer::Snapshot st = m_server.snapshot();
        wxString tip = "Edge Hub";
        tip += st.phone ? "\nPhone access on" : "\nPhone access off";
        tip += wxString::Format("\n%d slicer window%s open", st.instances, st.instances == 1 ? "" : "s");
        if (st.hidden > 0) tip += wxString::Format(" (%d hidden)", st.hidden);
        if (st.attention > 0) tip += wxString::Format("\n%d need%s attention on the PC", st.attention, st.attention == 1 ? "s" : "");
        if (m_icon.IsOk()) SetIcon(m_icon, tip);
    }

    wxMenu* CreatePopupMenu() override
    {
        const HubServer::Snapshot st = m_server.snapshot();
        auto* menu = new wxMenu;
        menu->Append(ID_STATUS, wxString::Format("Edge Hub: %d slicer window%s open", st.instances, st.instances == 1 ? "" : "s"))->Enable(false);
        menu->AppendSeparator();
        menu->AppendCheckItem(ID_PHONE, "Phone access")->Check(st.phone);
        menu->Append(ID_NEWLINK, "New phone link (breaks saved links and icons)...");
        menu->Append(ID_PAGE, st.phone ? "Open hub page (QR code, link, slicers)" : "Open hub page");
        menu->Append(ID_NEW, "Open a new slicer window");
        menu->Append(ID_NEW_HIDDEN, "Open a new hidden slicer");
        // Cheap: pid / title / hidden all come from <datadir>/hub/instances/<pid>.json.
        m_menu_pids.clear();
        auto* subs = new wxMenu;
        int   n    = 0;
        for (const Instance& inst : m_server.instances(false)) {
            if (n >= ID_INST_MAX) break;
            const int base = ID_INST_FIRST + n * ID_INST_PER;
            auto*     one  = new wxMenu;
            one->Append(base + 0, "Show window")->Enable(inst.hidden);
            one->Append(base + 1, "Hide window")->Enable(!inst.hidden);
            one->AppendSeparator();
            one->Append(base + 2, "Quit this window");
            subs->AppendSubMenu(one, wxString::Format(wxT("%d \u00B7 %s%s%s"), n + 1, // wide literal: a narrow UTF-8 dot came out as "Â·" (2026-09-06)
                inst.title.empty() ? wxString("Untitled") : wxString::FromUTF8(inst.title), inst.hidden ? wxString("  (hidden)") : wxString(),
                inst.needs_attention ? wxString("  (needs attention)") : wxString()));
            m_menu_pids.push_back(inst.pid);
            ++n;
        }
        if (n == 0) subs->Append(wxID_ANY, "No slicer is running")->Enable(false);
        menu->AppendSubMenu(subs, "Slicer windows");
        menu->AppendSeparator();
        menu->Append(ID_QUIT, "Quit hub (stops phone access and camera relays)");
        return menu;
    }

private:
    void open_page()
    {
        // The hub page lives on the control plane, not on the listener a tunnel can front.
        wxLaunchDefaultBrowser(wxString::Format("http://127.0.0.1:%d/hub/", m_server.snapshot().admin_port));
    }

    HubServer&            m_server;
    std::function<void()> m_on_quit;
    wxIcon                m_icon;
    std::vector<long>     m_menu_pids; // menu order -> pid, rebuilt with every popup
};

// A wxApp with no windows: the tray icon plus the server running on its own thread.
class HubApp : public wxApp
{
public:
    HubApp(std::string token, bool phone) : m_owned(new HubServer(std::move(token), phone)), m_server(*m_owned) {}

    bool OnInit() override
    {
        SetAppName("Edge Hub");
        SetExitOnFrameDelete(false); // no frames: the loop runs until the server thread ends
        wxInitAllImageHandlers();
        if (!m_server.start()) {
            BOOST_LOG_TRIVIAL(error) << "RemoteHub: could not start the listener";
            return false;
        }
        m_icon = new HubTaskBarIcon(m_server, [this]() { m_server.request_quit("tray"); });
        wxIcon icon(wxString::FromUTF8(Slic3r::var("EdgeSlicer.ico")), wxBITMAP_TYPE_ICO);
        if (!icon.IsOk()) icon = wxIcon(wxString::FromUTF8(Slic3r::var("Snapmaker_Orca_128px.png")), wxBITMAP_TYPE_PNG);
        m_icon->set_icon(icon);
        // From here on a printer event shows on the PC as well (accept_event decides which ones).
        set_balloon([this](const std::string& title, const std::string& text, const std::string& severity) {
            if (m_icon) m_icon->balloon(title, text, severity);
        });
        m_thread = std::thread([this]() {
            m_server.loop(false);
            CallAfter([this]() {
                if (m_icon) m_icon->RemoveIcon();
                ExitMainLoop();
            });
        });
        // Keep the tooltip's instance count fresh.
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (m_icon) m_icon->refresh(); });
        m_timer.Start(5000);
        return true;
    }

    int OnExit() override
    {
        m_timer.Stop();
        set_balloon(nullptr); // the icon is about to go; nothing may reach it after this
        m_server.request_quit();
        if (m_thread.joinable()) m_thread.join();
        if (!m_server.shutdown()) {
            // A connection or helper thread is still running and still uses the server. Leave it
            // alive for them; the process is about to exit and takes everything with it. Destroying
            // it here is what crashed (a2375b06).
            (void) m_owned.release();
        }
        delete m_icon;
        m_icon = nullptr;
        return 0;
    }

private:
    std::unique_ptr<HubServer> m_owned; // released, not destroyed, if shutdown() could not drain
    HubServer&      m_server;
    HubTaskBarIcon* m_icon { nullptr };
    std::thread     m_thread;
    wxTimer         m_timer;
};

int run_server(const std::string& token_hint, bool phone_on)
{
    ensure_dirs();
    try {
        set_log_path_and_level("hub.log", 3); // <datadir>/log/hub.log.<n>
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: starting, version " << SLIC3R_VERSION << ", data dir " << data_dir();
#ifndef _WIN32
        ::signal(SIGPIPE, SIG_IGN);
#endif
        // Only one hub per data dir.
        Info existing = query();
        if (existing.alive) {
            BOOST_LOG_TRIVIAL(info) << "RemoteHub: another hub (pid " << existing.pid << ") is already running, exiting";
            return 0;
        }
        // The tray app owns the server from here; wxEntry returns when it quits.
        wxApp::SetInstance(new HubApp(valid_token(token_hint) ? token_hint : std::string(), phone_on));
#ifdef _WIN32
        // wxEntry wants argv in the ANSI code page; hand it just the module path (GUI_Init does the same).
        wchar_t module_path[MAX_PATH + 1] = {};
        ::GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        char module_ansi[MAX_PATH * 2 + 1] = {};
        ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, module_path, -1, module_ansi, sizeof(module_ansi), nullptr, nullptr);
        int   argc   = 1;
        char* argv[] = { module_ansi, nullptr };
#else
        int   argc   = 1;
        char  name[] = "snapmaker-orca";
        char* argv[] = { name, nullptr };
#endif
        return wxEntry(argc, argv);
    } catch (const std::exception& e) {
        // No console to see it on: leave a note next to the state files.
        boost::nowide::ofstream f((fs::path(hub_dir()) / "hub_error.txt").string(), std::ios::trunc);
        f << "RemoteHub died: " << e.what() << "\n";
        BOOST_LOG_TRIVIAL(error) << "RemoteHub died: " << e.what();
        return 1;
    }
}

// --------------------------------------------------------------- client ----

static std::mutex  s_state_mutex;
static std::string s_last_state;
// ensure_running() one caller at a time within this process: the startup call
// (GUI_App::start_remote_access), the Stream tab and the Home tab can all ask at once, and two
// callers that both find no hub would both spawn one. The second caller now waits and finds the
// first one's hub.
static std::mutex  s_ensure_mutex;

std::string Info::url() const
{
    return (alive && phone && !ips.empty()) ? "http://" + ips.front() + ":" + std::to_string(port) + "/r/" + token + "/" : "";
}

std::string Info::json() const
{
    nlohmann::json j;
    j["on"]    = alive && phone;
    j["port"]  = port;
    j["token"] = token;
    j["ips"]   = ips;
    j["url"]   = url();
    j["remote_url"] = remote_url;
    j["relay_url"]  = relay_url;
    j["hubid"]      = hubid;
    return j.dump();
}

static Info parse_info(const std::string& body)
{
    Info i;
    try {
        json j        = json::parse(body);
        i.alive       = j.value("alive", false);
        i.pid         = j.value("pid", 0L);
        i.port        = j.value("port", 0);
        i.admin_port  = j.value("admin_port", 0);
        i.phone       = j.value("phone", false);
        i.token       = j.value("token", "");
        i.go2rtc_port = j.value("go2rtc_port", 0);
        i.relay_port  = j.value("relay_port", 0);
        if (j.contains("remote") && j["remote"].is_object()) i.remote_url = j["remote"].value("url", "");
        i.relay_url   = j.value("relay_url", "");   // "" until a relay exists (phase 1)
        i.hubid       = j.value("hubid", "");       // this data dir's durable relay identity
        i.version     = j.value("version", "");
        for (const auto& ip : j.value("ips", json::array())) i.ips.push_back(ip.get<std::string>());
    } catch (...) {}
    return i;
}

// admin_port is where /hub/* answers (the loopback-only control plane); port is the listener the
// phone and any tunnel use. Everything below talks to the control plane.
struct HubFile { int port { 0 }; int admin_port { 0 }; std::string secret; bool exists { false }; bool parsed { false }; bool pid_alive { false }; };
static HubFile hub_file()
{
    HubFile h;
    // hub.json is rewritten in place when the rename over it fails (a reader has it open), so a
    // read can catch it empty or half written: that is a moment to wait out, not a missing hub.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(40));
        boost::system::error_code ec;
        h.exists = fs::exists(hub_json_path(), ec);
        if (!h.exists) return h;
        try {
            json j   = json::parse(read_file(hub_json_path()));
            h.parsed = true;
            if (!pid_alive(j.value("pid", 0L))) return h;
            h.pid_alive  = true;
            h.port       = j.value("port", 0);
            h.admin_port = j.value("admin_port", 0);
            h.secret     = j.value("secret", "");
            // A hub from before the split served /hub/* on its one listener; talking to it there is
            // what lets ensure_running() shut it down and put this build's hub in its place.
            if (h.admin_port == 0) h.admin_port = h.port;
            return h;
        } catch (...) {}
    }
    return h;
}

static Info hub_call(const std::string& method, const std::string& path, const std::string& body, long timeout)
{
    const HubFile hf = hub_file();
    if (hf.admin_port == 0) return Info();
    Info        out;
    std::string url = "http://127.0.0.1:" + std::to_string(hf.admin_port) + path;
    Http        http = method == "POST" ? Http::post(url) : Http::get(url);
    if (method == "POST") http.set_post_body(body).header("Content-Type", body.empty() ? "text/plain" : "application/json");
    http.header("X-Hub-Secret", hf.secret);
    http.timeout_connect(1).timeout_max(timeout)
        .on_complete([&out, &hf](std::string b, unsigned status) { if (status == 200) { out = parse_info(b); out.secret = hf.secret; } })
        .perform_sync();
    return out;
}

Info query(long timeout_s) { return hub_call("GET", "/hub/info", "", timeout_s); }

Record record()
{
    Record r;
    const HubFile hf = hub_file();
    r.presence       = HubHome::classify_record(hf.exists, hf.parsed, hf.pid_alive);
    r.pid_alive      = hf.pid_alive;
    if (r.presence == HubHome::Presence::NoRecord) {
        try {
            json j        = json::parse(read_file(last_exit_json_path()));
            r.exit_reason = HubHome::exit_reason_from(j.value("reason", ""));
        } catch (...) {}
    }
    return r;
}

std::pair<int, std::string> onvif_discover()
{
    const HubFile hf = hub_file();
    if (hf.admin_port == 0) return { 503, "the hub is not running" };
    int         status = 0;
    std::string body;
    Http::get("http://127.0.0.1:" + std::to_string(hf.admin_port) + "/hub/onvif")
        .header("X-Hub-Secret", hf.secret)
        .timeout_connect(1).timeout_max(20)
        .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
        .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? err : b; })
        .perform_sync();
    return { status == 0 ? 502 : status, body };
}

Info set_phone(bool on, const std::string& token)
{
    return hub_call("POST", std::string("/hub/phone?on=") + (on ? "1" : "0") + (valid_token(token) ? "&token=" + token : ""), "", 5);
}

Info new_link() { return hub_call("POST", "/hub/newlink", "", 5); }

void quit() { hub_call("POST", "/hub/quit", "", 3); }

bool post_state(const std::string& state_json)
{
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        s_last_state = state_json;
    }
    const HubFile hf = hub_file();
    if (hf.admin_port == 0) return false;
    bool ok = false;
    Http::post("http://127.0.0.1:" + std::to_string(hf.admin_port) + "/hub/state")
        .timeout_connect(1).timeout_max(5)
        .header("Content-Type", "application/json")
        .header("X-Hub-Secret", hf.secret)
        .set_post_body(state_json)
        .on_complete([&ok](std::string, unsigned status) { ok = status == 200; })
        .perform_sync();
    return ok;
}

bool post_event(const std::string& event_json)
{
    const HubFile hf = hub_file();
    if (hf.admin_port == 0) return false; // no hub: the instance keeps its own ring and that is all
    bool ok = false;
    Http::post("http://127.0.0.1:" + std::to_string(hf.admin_port) + "/hub/event")
        .timeout_connect(1).timeout_max(5)
        .header("Content-Type", "application/json")
        .header("X-Hub-Secret", hf.secret)
        .set_post_body(event_json)
        .on_complete([&ok](std::string, unsigned status) { ok = status == 200; })
        .perform_sync();
    return ok;
}

Info ensure_running(const std::string& token_hint, bool phone_on)
{
    std::lock_guard<std::mutex> ensure_lock(s_ensure_mutex);
    Info i = query();
    if (i.alive && i.version != SLIC3R_VERSION) {
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: hub version " << i.version << " != " << SLIC3R_VERSION << ", restarting it";
        const long old_pid = i.pid;
        quit();
        for (int n = 0; n < 30 && pid_alive(old_pid); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        i = Info();
    }
    if (!i.alive) {
        std::vector<std::string> args = { current_exe(), "--hub", "--datadir", data_dir() };
        if (valid_token(token_hint)) { args.push_back("--hub-token"); args.push_back(token_hint); }
        if (phone_on) args.push_back("--hub-phone");
        const long pid = spawn_process(args, {}, true, nullptr);
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: spawned hub pid " << pid;
        if (pid > 0)
            for (int n = 0; n < 60 && !i.alive; ++n) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                i = query();
            }
        if (!i.alive) {
            BOOST_LOG_TRIVIAL(error) << "RemoteHub: the hub did not come up";
            return i;
        }
    }
    if (phone_on && !i.phone) i = set_phone(true, token_hint);
    std::string state;
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        state = s_last_state;
    }
    if (!state.empty()) post_state(state);
    return i;
}

} // namespace RemoteHub
} // namespace GUI
} // namespace Slic3r
