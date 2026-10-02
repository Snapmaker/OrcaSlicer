// The local page server (HttpServer with page security on) serves the flutter pages and a few
// app-produced files to the app's own web views on 127.0.0.1. Any website in the user's browser can
// reach 127.0.0.1 too, so:
//   - every request needs this process's secret (query token, header, or the cookie the first
//     tokened load sets), a Host of 127.0.0.1/localhost:<port>, and no foreign Origin;
//   - /localfile/ and /wcp_download/ return only files the app handed out (grants) or installed
//     resources, resolved to the real file first, and never the data dir's config;
//   - the port is bound exclusively, so a port another process holds is skipped, not shared.
// The first half drives the rules directly; the second starts a real server on a free port and
// talks raw HTTP to it.

#include <catch2/catch.hpp>

#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/PageServerSecurity.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <wx/uri.h>

using namespace Slic3r::GUI;
namespace ps = Slic3r::GUI::page_server;
namespace fs = boost::filesystem;

namespace {

const std::string SECRET(64, 'a');
const uint16_t    PORT = 13619;

ps::RequestInfo good_request(const std::string& target)
{
    ps::RequestInfo r;
    r.method = "GET";
    r.target = target;
    r.host   = "127.0.0.1:13619";
    return r;
}

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    return s;
}

std::string slashes(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

void write_file(const fs::path& p, const std::string& body)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p.string(), std::ios::binary);
    f << body;
}

// A scratch tree: resources/, data/ (with a config and presets), a granted file and a secret one.
struct Tree
{
    fs::path root, res, data, outside, granted_dir;
    Tree()
    {
        root = fs::temp_directory_path() / fs::unique_path("pagesrv_test_%%%%%%%%");
        res         = root / "resources";
        data        = root / "data";
        outside     = root / "outside";
        granted_dir = root / "granted";
        write_file(res / "web" / "flutter_web" / "index.html", "<html>index</html>");
        write_file(res / "profiles" / "Snapmaker" / "U1_cover.png", "png");
        write_file(data / "EdgeSlicer.conf", "{\"access_code\":\"12345678\"}");
        write_file(data / "EdgeSlicer.conf.bak", "{\"access_code\":\"12345678\"}");
        write_file(data / "user" / "default" / "machine" / "p.json", "{\"printhost_apikey\":\"k\"}");
        write_file(data / "tmp" / "plate_1.gcode", "G28");
        write_file(outside / "secret.txt", "top secret");
        write_file(granted_dir / "model.gcode", "G1 X1");
    }
    ~Tree()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string s(const fs::path& p) const { return slashes(p.string()); }
    ps::FileRoots roots() const { return ps::FileRoots{s(res), s(data)}; }
};

} // namespace

// ---- request authorisation -------------------------------------------------------------------

TEST_CASE("page server refuses requests without the secret", "[PageServer]")
{
    auto d = ps::authorize(good_request("/web/flutter_web/index.html"), PORT, SECRET);
    CHECK_FALSE(d.allowed);
    CHECK(d.status == 403);
    CHECK(d.reason == "no-token");

    auto r = good_request("/web/flutter_web/index.html?edge_page_token=" + std::string(64, 'b'));
    d      = ps::authorize(r, PORT, SECRET);
    CHECK_FALSE(d.allowed);
    CHECK(d.reason == "bad-token");

    r        = good_request("/x");
    r.cookie = ps::cookie_name(PORT) + "=" + SECRET.substr(1);
    CHECK_FALSE(ps::authorize(r, PORT, SECRET).allowed);

    // The cookie of another instance (another port) is not ours.
    r.cookie = ps::cookie_name(13620) + "=" + SECRET;
    CHECK_FALSE(ps::authorize(r, PORT, SECRET).allowed);

    // An empty secret never authorises anything.
    r        = good_request("/x?edge_page_token=");
    CHECK_FALSE(ps::authorize(r, PORT, "").allowed);
}

TEST_CASE("page server accepts our web view's token, cookie or header", "[PageServer]")
{
    auto d = ps::authorize(good_request("/web/flutter_web/index.html?path=2&edge_page_token=" + SECRET + "&locale=en-US"), PORT, SECRET);
    CHECK(d.allowed);
    CHECK(d.set_cookie);
    CHECK(d.target == "/web/flutter_web/index.html?path=2&locale=en-US"); // token never reaches the router

    auto r   = good_request("/localfile/cap/0123");
    r.cookie = "other=1; " + ps::cookie_name(PORT) + "=" + SECRET + "; x=y";
    d        = ps::authorize(r, PORT, SECRET);
    CHECK(d.allowed);
    CHECK_FALSE(d.set_cookie);

    r              = good_request("/profiles/a.png");
    r.header_token = SECRET;
    CHECK(ps::authorize(r, PORT, SECRET).allowed);

    // Same-origin requests may carry our own origin, and localhost is the same server.
    r        = good_request("/x?edge_page_token=" + SECRET);
    r.origin = "http://127.0.0.1:13619";
    CHECK(ps::authorize(r, PORT, SECRET).allowed);
    r.host   = "localhost:13619";
    r.origin = "http://localhost:13619";
    CHECK(ps::authorize(r, PORT, SECRET).allowed);
    r.sec_fetch_site = "same-origin";
    CHECK(ps::authorize(r, PORT, SECRET).allowed);
}

TEST_CASE("page server refuses foreign hosts and origins even with the secret", "[PageServer]")
{
    const std::string target = "/web/flutter_web/index.html?edge_page_token=" + SECRET;
    for (const char* host : {"evil.example:13619", "127.0.0.1:13620", "127.0.0.1", "", "127.0.0.1.evil.example:13619", "0.0.0.0:13619"}) {
        auto r = good_request(target);
        r.host = host;
        auto d = ps::authorize(r, PORT, SECRET);
        INFO(host);
        CHECK_FALSE(d.allowed);
        CHECK(d.reason == "host");
    }
    for (const char* origin : {"https://evil.example", "null", "http://127.0.0.1:13620", "http://127.0.0.1", "file://", "http://evil.example:13619"}) {
        auto r   = good_request(target);
        r.origin = origin;
        auto d   = ps::authorize(r, PORT, SECRET);
        INFO(origin);
        CHECK_FALSE(d.allowed);
        CHECK(d.reason == "origin");
    }
    auto r           = good_request(target);
    r.sec_fetch_site = "cross-site";
    CHECK_FALSE(ps::authorize(r, PORT, SECRET).allowed);

    r        = good_request(target);
    r.method = "POST";
    auto d   = ps::authorize(r, PORT, SECRET);
    CHECK_FALSE(d.allowed);
    CHECK(d.status == 405);
}

TEST_CASE("page server token helpers", "[PageServer]")
{
    CHECK(ps::append_token("http://127.0.0.1:1/a", "S") == "http://127.0.0.1:1/a?edge_page_token=S");
    CHECK(ps::append_token("http://127.0.0.1:1/a?x=1#f", "S") == "http://127.0.0.1:1/a?x=1&edge_page_token=S#f");
    std::string tok;
    CHECK(ps::strip_token_param("/a?edge_page_token=S", &tok) == "/a");
    CHECK(tok == "S");
    CHECK(ps::strip_token_param("/a?x=1&edge_page_token=S&y=2") == "/a?x=1&y=2");
    CHECK(ps::strip_token_param("/a?xedge_page_token=S") == "/a?xedge_page_token=S");
    CHECK(ps::strip_token_param("/a") == "/a");
    CHECK(ps::generate_secret().size() == 64);
    CHECK(ps::generate_secret() != ps::generate_secret());
    CHECK(ps::set_cookie_header_value(13619, "S") == "edge_page_13619=S; Path=/; HttpOnly; SameSite=Strict");

    HttpServer server(13619);
    CHECK(server.page_url("/web/x.html") == "http://127.0.0.1:13619/web/x.html"); // lock-down off: no token
    server.enable_page_security(true);
    const std::string secret = server.page_secret();
    CHECK(server.page_url("/web/x.html?path=2") == "http://127.0.0.1:13619/web/x.html?path=2&edge_page_token=" + secret);
    CHECK(server.add_token_if_ours("http://127.0.0.1:13619/web/y.html") == "http://127.0.0.1:13619/web/y.html?edge_page_token=" + secret);
    CHECK(server.add_token_if_ours("http://127.0.0.1:136190/web/y.html") == "http://127.0.0.1:136190/web/y.html");
    CHECK(server.add_token_if_ours("https://evil.example/?u=http://127.0.0.1:13619/") == "https://evil.example/?u=http://127.0.0.1:13619/");
    CHECK(server.add_token_if_ours(server.page_url("/a")) == server.page_url("/a")); // not doubled
}

// ---- path screening ---------------------------------------------------------------------------

TEST_CASE("page server path screening rejects traversal, UNC, streams and aliases", "[PageServer]")
{
#ifdef _WIN32
    CHECK(ps::is_acceptable_request_path("C:/Users/me/model.gcode"));
    CHECK(ps::is_acceptable_request_path("C:\\Users\\me\\model..v2.gcode"));
    for (const char* bad : {
             "", "relative/file.txt", "/rooted/no/drive.txt", "C:relative.txt", "C:/a/../EdgeSlicer.conf", "C:/a/./b.txt",
             "C:\\a\\..\\b.txt", "\\\\server\\share\\f.txt", "//server/share/f.txt", "\\\\?\\C:\\f.txt", "//?/C:/f.txt",
             "\\\\.\\PhysicalDrive0", "C:/a/f.txt:secret", "C:/a/f.txt::$DATA", "C:/a/EdgeSlicer.conf.", "C:/a/EdgeSlicer.conf ",
             "C:/a/f*.txt", "C:/a/f?.txt", "C:/a/\x01.txt"}) {
        INFO(bad);
        CHECK_FALSE(ps::is_acceptable_request_path(bad));
    }
#else
    CHECK(ps::is_acceptable_request_path("/home/me/model.gcode"));
    for (const char* bad : {"", "relative.txt", "/a/../etc/passwd", "/a/./b", "/a/\x01"}) {
        INFO(bad);
        CHECK_FALSE(ps::is_acceptable_request_path(bad));
    }
#endif
}

TEST_CASE("page server file routes serve granted files and resources only", "[PageServer]")
{
    Tree            t;
    ps::FileGrants  grants;
    const auto      roots = t.roots();

    // Nothing granted: a file outside resources is refused, a resource file is served.
    CHECK(ps::resolve_localfile(t.s(t.outside / "secret.txt"), roots, grants).empty());
    CHECK(ps::resolve_localfile(t.s(t.granted_dir / "model.gcode"), roots, grants).empty());
    CHECK_FALSE(ps::resolve_localfile(t.s(t.res / "web" / "flutter_web" / "index.html"), roots, grants).empty());

    // Granting one file opens exactly that file, by path and by capability id.
    const std::string id = grants.grant(t.s(t.granted_dir / "model.gcode"));
    CHECK(id.size() == 32);
    const std::string served = ps::resolve_localfile(t.s(t.granted_dir / "model.gcode"), roots, grants);
    CHECK_FALSE(served.empty());
    CHECK(ps::resolve_localfile("cap/" + id + "/model.gcode", roots, grants) == served);
    CHECK(ps::resolve_localfile("cap/" + id, roots, grants) == served);
    CHECK(ps::resolve_localfile("cap/" + std::string(32, '0') + "/model.gcode", roots, grants).empty());
    CHECK(ps::resolve_localfile("cap/../../outside/secret.txt", roots, grants).empty());
    CHECK(ps::resolve_localfile(t.s(t.outside / "secret.txt"), roots, grants).empty());
    CHECK(ps::resolve_granted_path(t.s(t.granted_dir / "model.gcode"), roots, grants) == served);
    CHECK(ps::resolve_granted_path(t.s(t.outside / "secret.txt"), roots, grants).empty());

    // Traversal from a granted directory does not escape.
    CHECK(ps::resolve_localfile(t.s(t.granted_dir) + "/../outside/secret.txt", roots, grants).empty());
    // A directory is never served.
    grants.grant(t.s(t.outside));
    CHECK(ps::resolve_localfile(t.s(t.outside), roots, grants).empty());

#ifdef _WIN32
    // Case games resolve to the same file: granted stays granted, refused stays refused.
    CHECK(ps::resolve_localfile(upper(t.s(t.granted_dir / "model.gcode")), roots, grants) == served);
    CHECK(ps::resolve_localfile(upper(t.s(t.outside / "secret.txt")), roots, grants).empty());
    // Backslashes and a granted file's alternate data stream.
    CHECK(ps::resolve_localfile((t.granted_dir / "model.gcode").string(), roots, grants) == served);
    CHECK(ps::resolve_localfile(t.s(t.granted_dir / "model.gcode") + ":hidden", roots, grants).empty());
    CHECK(ps::resolve_localfile(t.s(t.granted_dir / "model.gcode") + "::$DATA", roots, grants).empty());

    // 8.3 short names resolve to the long name (when the volume has them).
    {
        std::wstring longp = (t.granted_dir / "model.gcode").wstring();
        wchar_t      buf[MAX_PATH];
        DWORD        n = ::GetShortPathNameW(longp.c_str(), buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH && std::wstring(buf) != longp) {
            fs::path shortp(std::wstring(buf, n));
            CHECK(ps::resolve_localfile(slashes(shortp.string()), roots, grants) == served);
        }
    }

    // A junction pointing out of a granted place does not make its target readable.
    {
        const fs::path link = t.granted_dir / "link";
        const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + t.outside.string() + "\" >nul 2>&1";
        if (std::system(cmd.c_str()) == 0 && fs::exists(link / "secret.txt")) {
            CHECK(ps::resolve_localfile(t.s(link / "secret.txt"), roots, grants).empty());
            // ...while a granted file reached through a junction is still the granted file.
            const fs::path link2 = t.root / "link_to_granted";
            const std::string cmd2 = "cmd /c mklink /J \"" + link2.string() + "\" \"" + t.granted_dir.string() + "\" >nul 2>&1";
            if (std::system(cmd2.c_str()) == 0)
                CHECK(ps::resolve_localfile(t.s(link2 / "model.gcode"), roots, grants) == served);
            fs::remove(link2);
            fs::remove(link);
        } else {
            WARN("mklink /J unavailable; junction case skipped");
        }
    }
#endif
}

TEST_CASE("page server never serves the data dir's config and presets", "[PageServer]")
{
    Tree           t;
    ps::FileGrants grants;
    const auto     roots = t.roots();
    for (const fs::path& p : {t.data / "EdgeSlicer.conf", t.data / "EdgeSlicer.conf.bak", t.data / "user" / "default" / "machine" / "p.json"}) {
        grants.grant(t.s(p));
        INFO(p.string());
        CHECK(ps::resolve_localfile(t.s(p), roots, grants).empty());
        CHECK(ps::resolve_granted_path(t.s(p), roots, grants).empty());
    }
    // A *.conf anywhere is refused, granted or not.
    write_file(t.granted_dir / "printer.conf", "x");
    grants.grant(t.s(t.granted_dir / "printer.conf"));
    CHECK(ps::resolve_localfile(t.s(t.granted_dir / "printer.conf"), roots, grants).empty());
    // A granted plate file in a data-dir subfolder is fine.
    grants.grant(t.s(t.data / "tmp" / "plate_1.gcode"));
    CHECK_FALSE(ps::resolve_localfile(t.s(t.data / "tmp" / "plate_1.gcode"), roots, grants).empty());
}

TEST_CASE("page server resource URLs stay inside the resources dir", "[PageServer]")
{
    Tree t;
    const std::string res = t.s(t.res);
    CHECK_FALSE(ps::resolve_resource("/web/flutter_web/index.html", res).empty());
    CHECK_FALSE(ps::resolve_resource("/profiles/Snapmaker/U1_cover.png", res).empty());
    CHECK(ps::resolve_resource("/web/missing.html", res).empty());
    CHECK(ps::resolve_resource("/web/flutter_web", res).empty()); // directory
    for (const char* bad : {"/../data/EdgeSlicer.conf", "/web/../../data/EdgeSlicer.conf", "/web/flutter_web/index.html::$DATA",
                            "//server/share/x", "/C:/Windows/win.ini", "web/flutter_web/index.html", "/web/flutter_web/index.html."}) {
        INFO(bad);
        CHECK(ps::resolve_resource(bad, res).empty());
    }
}

TEST_CASE("page server grants keep the newest entries", "[PageServer]")
{
    ps::FileGrants    grants;
    const std::string first = grants.grant("C:/a/first.gcode");
    CHECK(grants.grant("C:/a/first.gcode") == first); // same file, same id
    for (int i = 0; i < (int) ps::FileGrants::capacity; ++i)
        grants.grant("C:/a/f" + std::to_string(i) + ".gcode");
    CHECK(grants.size() == ps::FileGrants::capacity);
    CHECK(grants.path_for_id(first).empty());
}

// ---- a real server --------------------------------------------------------------------------

namespace {

struct Reply
{
    int         status = 0;
    std::string head;
    std::string body;
    bool has_header(const std::string& name) const
    {
        std::string h = head, n = name;
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        return h.find("\r\n" + n + ":") != std::string::npos;
    }
};

Reply http_get(uint16_t port, const std::string& target, const std::string& extra_headers = "", const std::string& host = "",
               const std::string& method = "GET")
{
    using boost::asio::ip::tcp;
    boost::asio::io_context io;
    tcp::socket             sock(io);
    sock.connect(tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), port));
    const std::string req = method + " " + target + " HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) +
                            "\r\n" + extra_headers + "Connection: close\r\n\r\n";
    boost::asio::write(sock, boost::asio::buffer(req));
    std::string               all;
    char                      buf[4096];
    boost::system::error_code ec;
    for (;;) {
        size_t n = sock.read_some(boost::asio::buffer(buf), ec);
        all.append(buf, n);
        if (ec) break;
    }
    Reply r;
    const size_t split = all.find("\r\n\r\n");
    r.head             = all.substr(0, split);
    r.body             = split == std::string::npos ? "" : all.substr(split + 4);
    if (all.compare(0, 9, "HTTP/1.1 ") == 0)
        r.status = std::atoi(all.c_str() + 9);
    return r;
}

uint16_t free_port()
{
    boost::asio::io_context        io;
    boost::asio::ip::tcp::acceptor a(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), 0));
    return a.local_endpoint().port();
}

std::string b64url(const std::string& s)
{
    std::string out(boost::beast::detail::base64::encoded_size(s.size()), '\0');
    out.resize(boost::beast::detail::base64::encode(&out[0], s.data(), s.size()));
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::string pct(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char) c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// Points resources_dir()/data_dir() at the scratch tree for the lifetime of a test.
struct DirsOverride
{
    std::string old_res, old_data;
    DirsOverride(const Tree& t) : old_res(Slic3r::resources_dir()), old_data(Slic3r::data_dir())
    {
        Slic3r::set_resources_dir(t.s(t.res));
        Slic3r::set_data_dir(t.s(t.data));
    }
    ~DirsOverride()
    {
        Slic3r::set_resources_dir(old_res);
        Slic3r::set_data_dir(old_data);
    }
};

} // namespace

TEST_CASE("page server over HTTP: token, cookie, host, origin and file routes", "[PageServer][PageServerHttp]")
{
    Tree         t;
    DirsOverride dirs(t);
    ps::file_grants().clear();

    HttpServer server(free_port());
    server.set_request_handler(HttpServer::web_server_handle_request);
    server.enable_page_security(true);
    server.start();
    server.stop_health_check();
    const uint16_t    port   = server.get_port();
    const std::string secret = server.page_secret();
    const std::string cookie = "Cookie: " + ps::cookie_name(port) + "=" + secret + "\r\n";

    // Our web view's first load: token in the URL -> 200 and the cookie; no CORS headers.
    Reply r = http_get(port, "/web/flutter_web/index.html?path=2&edge_page_token=" + secret);
    CHECK(r.status == 200);
    CHECK(r.body == "<html>index</html>");
    CHECK(r.head.find("Set-Cookie: " + ps::cookie_name(port) + "=" + secret + "; Path=/; HttpOnly; SameSite=Strict") != std::string::npos);
    CHECK_FALSE(r.has_header("Access-Control-Allow-Origin"));
    // ...then everything the page loads rides on the cookie.
    CHECK(http_get(port, "/profiles/Snapmaker/U1_cover.png", cookie).status == 200);
    CHECK(http_get(port, "/", cookie).status == 200);

    // Anybody else.
    CHECK(http_get(port, "/web/flutter_web/index.html").status == 403);
    CHECK(http_get(port, "/web/flutter_web/index.html?edge_page_token=" + std::string(64, '0')).status == 403);
    CHECK(http_get(port, "/web/flutter_web/index.html", cookie, "evil.example:" + std::to_string(port)).status == 403);
    CHECK(http_get(port, "/web/flutter_web/index.html", cookie + "Origin: https://evil.example\r\n").status == 403);
    CHECK(http_get(port, "/web/flutter_web/index.html", cookie + "Origin: null\r\n").status == 403);
    CHECK(http_get(port, "/web/flutter_web/index.html", cookie + "Sec-Fetch-Site: cross-site\r\n").status == 403);
    Reply pre = http_get(port, "/web/flutter_web/index.html", "Origin: https://evil.example\r\nAccess-Control-Request-Method: GET\r\n", "", "OPTIONS");
    CHECK(pre.status == 403);
    CHECK_FALSE(pre.has_header("Access-Control-Allow-Origin"));
    CHECK(http_get(port, "/web/flutter_web/index.html", cookie + "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n").status == 200);

    // /localfile/: files outside the grants/resources are 403 even for our own web view.
    const std::string conf   = t.s(t.data / "EdgeSlicer.conf");
    const std::string secret_file = t.s(t.outside / "secret.txt");
    CHECK(http_get(port, "/localfile/" + pct(conf), cookie).status == 403);
    CHECK(http_get(port, "/localfile/" + pct(secret_file), cookie).status == 403);
    CHECK(http_get(port, "/localfile/" + pct(t.s(t.granted_dir)) + "%2F..%2Foutside%2Fsecret.txt", cookie).status == 403);
    CHECK(http_get(port, "/wcp_download/" + b64url(secret_file), cookie).status == 403);
    CHECK(http_get(port, "/wcp_download/" + b64url(conf), cookie).status == 403);
    CHECK(http_get(port, "/web/..%2F..%2Fdata%2FEdgeSlicer.conf", cookie).status == 404);

    // A handed-out file: served by capability URL, by path, and by /wcp_download/.
    const std::string url = server.localfile_url(t.s(t.granted_dir / "model.gcode"));
    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    REQUIRE(url.compare(0, origin.size(), origin) == 0);
    r = http_get(port, url.substr(origin.size()), cookie);
    CHECK(r.status == 200);
    CHECK(r.body == "G1 X1");
    CHECK(http_get(port, "/localfile/" + pct(t.s(t.granted_dir / "model.gcode")), cookie).status == 200);
    CHECK(http_get(port, "/wcp_download/" + b64url(t.s(t.granted_dir / "model.gcode")), cookie).status == 200);
    // ...but not to a foreign website, even one that learned the URL.
    CHECK(http_get(port, url.substr(origin.size())).status == 403);
    CHECK(http_get(port, url.substr(origin.size()), "Origin: https://evil.example\r\n" + cookie).status == 403);

    server.stop();
    ps::file_grants().clear();
}

TEST_CASE("login callback server keeps its open CORS and needs no token", "[PageServer][PageServerHttp]")
{
    HttpServer server(free_port());
    server.set_request_handler([](const std::string&) { return std::make_shared<HttpServer::ResponseNotFound>(); });
    server.start();
    server.stop_health_check();
    Reply r = http_get(server.get_port(), "/?ticket=x");
    CHECK(r.status == 404);
    CHECK(r.has_header("Access-Control-Allow-Origin"));
    server.stop();
}

TEST_CASE("page server does not share a port another process holds", "[PageServer][PageServerHttp]")
{
    using boost::asio::ip::tcp;
    boost::asio::io_context io;
    // An "old build" squatting on the port the way HttpServer used to bind: SO_REUSEADDR, listening.
    tcp::acceptor squatter(io);
    const uint16_t wanted = free_port();
    squatter.open(tcp::v4());
    squatter.set_option(tcp::acceptor::reuse_address(true));
    squatter.bind(tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), wanted));
    squatter.listen();

    HttpServer server(wanted);
    server.set_request_handler(HttpServer::web_server_handle_request);
    server.enable_page_security(true);
    server.start();
    server.stop_health_check();
    CHECK(server.get_port() != wanted);
    // The URLs we hand our web views follow the port we actually hold.
    CHECK(server.page_url("/x").find(":" + std::to_string(server.get_port()) + "/x?") != std::string::npos);

#ifdef _WIN32
    // And nobody can join us on our port, SO_REUSEADDR or not.
    tcp::acceptor             thief(io);
    boost::system::error_code ec;
    thief.open(tcp::v4(), ec);
    thief.set_option(tcp::acceptor::reuse_address(true), ec);
    thief.bind(tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), server.get_port()), ec);
    CHECK(bool(ec));
#endif
    server.stop();
}

#ifndef _WIN32
TEST_CASE("file_url_from_path percent-encodes characters special to URLs", "[FileUrl]")
{
    const std::string path = GENERATE(std::string("/opt/test#dir/resources/web/homepage/index.html"),
                                      std::string("/opt/test%20x/resources/web/homepage/index.html"),
                                      std::string("/opt/Orca Slicer/resources/web/homepage/index.html"),
                                      std::string("/opt/what?/resources/web/homepage/index.html"),
                                      std::string("/home/Jos\xC3\xA9/\xE8\xB5\x84\xE6\xBA\x90/resources/web/homepage/index.html"));

    const wxString url = file_url_from_path(boost::filesystem::path(path));
    CAPTURE(path, into_u8(url));

    // WebView::CreateWebView() and WebView::LoadUrl() re-parse the URL before loading it.
    const wxURI uri(wxURI(url).BuildURI());
    CHECK(uri.GetScheme() == "file");
    CHECK_FALSE(uri.HasQuery());
    CHECK_FALSE(uri.HasFragment());
    CHECK(into_u8(wxURI::Unescape(uri.GetPath())) == path);
    CHECK(Slic3r::untrusted::local_path_from_file_url(into_u8(url)) == path);
}

TEST_CASE("file_url_from_path of a plain path is the path behind file://", "[FileUrl]")
{
    const std::string path = "/opt/OrcaSlicer/resources/web/homepage/index.html";
    CHECK(into_u8(file_url_from_path(boost::filesystem::path(path))) == "file://" + path);
}

TEST_CASE("a query appended to a file URL stays separate from its path", "[FileUrl]")
{
    const std::string path = "/opt/test#dir%20x/resources/web/guide/0/index.html";
    const wxURI       uri(file_url_from_path(boost::filesystem::path(path)) + "?target=21&lang=de");
    CHECK(into_u8(wxURI::Unescape(uri.GetPath())) == path);
    CHECK(uri.GetQuery() == "target=21&lang=de");
    CHECK_FALSE(uri.HasFragment());
    CHECK(Slic3r::untrusted::local_path_from_file_url(into_u8(file_url_from_path(boost::filesystem::path(path)) + "?target=21&lang=de")) == path);
}

TEST_CASE("file_url_from_path encodes space, hash, percent and non-ASCII", "[FileUrl]")
{
    const std::string spaced = into_u8(file_url_from_path(boost::filesystem::path("/opt/Orca Slicer/index.html")));
    CHECK(spaced.find("%20") != std::string::npos);
    CHECK(spaced.find(' ') == std::string::npos);

    const std::string hashed = into_u8(file_url_from_path(boost::filesystem::path("/opt/test#dir/index.html")));
    CHECK(hashed.find("%23") != std::string::npos);
    CHECK(hashed.find('#') == std::string::npos);

    const std::string percent = into_u8(file_url_from_path(boost::filesystem::path("/opt/100%/index.html")));
    CHECK(percent.find("%25") != std::string::npos);

    const std::string utf8_path = "/home/Jos\xC3\xA9/index.html";
    const std::string utf8      = into_u8(file_url_from_path(boost::filesystem::path(utf8_path)));
    CHECK(upper(utf8).find("%C3%A9") != std::string::npos);
    CHECK(Slic3r::untrusted::local_path_from_file_url(utf8) == utf8_path);
}
#else
TEST_CASE("file_url_from_path of a Windows path has a drive letter and forward slashes", "[FileUrl]")
{
    CHECK(into_u8(file_url_from_path(boost::filesystem::path(L"C:\\Program Files\\OrcaSlicer\\resources\\web\\homepage\\index.html"))) ==
          "file:///C:/Program%20Files/OrcaSlicer/resources/web/homepage/index.html");
    CHECK(into_u8(file_url_from_path(boost::filesystem::path(L"D:\\#OneDrive\\OrcaSlicer\\resources\\web\\guide\\0\\index.html"))) ==
          "file:///D:/%23OneDrive/OrcaSlicer/resources/web/guide/0/index.html");
    CHECK(into_u8(file_url_from_path(boost::filesystem::path(L"D:\\100%\\OrcaSlicer\\resources\\web\\homepage\\index.html"))) ==
          "file:///D:/100%25/OrcaSlicer/resources/web/homepage/index.html");
    // Callers join the resources directory with a forward-slash relative path.
    CHECK(into_u8(file_url_from_path(boost::filesystem::path(L"D:\\#OneDrive\\OrcaSlicer\\resources/web/homepage/index.html"))) ==
          "file:///D:/%23OneDrive/OrcaSlicer/resources/web/homepage/index.html");
    CHECK(into_u8(file_url_from_path(boost::filesystem::path(L"\\\\server\\share\\OrcaSlicer\\resources\\web\\homepage\\index.html"))) ==
          "file://server/share/OrcaSlicer/resources/web/homepage/index.html");
}

TEST_CASE("a query appended to a Windows file URL stays separate from its path", "[FileUrl]")
{
    const wxURI uri(file_url_from_path(boost::filesystem::path(L"D:\\#OneDrive\\OrcaSlicer\\resources\\web\\guide\\0\\index.html")) +
                    "?target=21&lang=de");
    CHECK(wxURI::Unescape(uri.GetPath()) == "/D:/#OneDrive/OrcaSlicer/resources/web/guide/0/index.html");
    CHECK(uri.GetQuery() == "target=21&lang=de");
    CHECK_FALSE(uri.HasFragment());
    CHECK(Slic3r::untrusted::local_path_from_file_url(into_u8(
              file_url_from_path(boost::filesystem::path(L"D:\\#OneDrive\\OrcaSlicer\\resources\\web\\guide\\0\\index.html")) + "?lang=en")) ==
          "D:/#OneDrive/OrcaSlicer/resources/web/guide/0/index.html");
}
#endif

