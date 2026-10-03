#include <catch2/catch.hpp>

#include "slic3r/GUI/PrinterTimelapse.hpp"

#include <cstring>
#include <deque>
#include <map>
#include <string>

using namespace Slic3r::GUI::Timelapse;
using nlohmann::json;

// ---- names -----------------------------------------------------------------------------------------

TEST_CASE("Timelapse file names: bare names only", "[Timelapse]")
{
    CHECK(valid_name("video_2026-10-01_12-00-00.mp4"));
    CHECK(valid_name("timelapse_Benchy PLA_2026.mp4"));
    CHECK(valid_name("Zeitraffer äöü.avi"));
    CHECK_FALSE(valid_name(""));
    CHECK_FALSE(valid_name("."));
    CHECK_FALSE(valid_name(".."));
    CHECK_FALSE(valid_name("../secret.mp4"));
    CHECK_FALSE(valid_name("sub/clip.mp4"));
    CHECK_FALSE(valid_name("sub\\clip.mp4"));
    CHECK_FALSE(valid_name("clip.mp4#thumbnail"));
    CHECK_FALSE(valid_name("c:clip.mp4"));
    CHECK_FALSE(valid_name(std::string("clip\0.mp4", 9)));
    CHECK_FALSE(valid_name("clip.mp4 "));
    CHECK_FALSE(valid_name("clip."));
    CHECK_FALSE(valid_name(std::string(256, 'a')));
    CHECK(valid_name(std::string(251, 'a') + ".mp4"));
}

TEST_CASE("Timelapse types: videos and their Content-Type", "[Timelapse]")
{
    CHECK(is_video_name("a.mp4"));
    CHECK(is_video_name("a.MP4"));
    CHECK(is_video_name("a.avi"));
    CHECK(is_video_name("a.mkv"));
    CHECK_FALSE(is_video_name("a.jpg"));
    CHECK_FALSE(is_video_name("mp4"));
    CHECK(mime_for("a.mp4") == "video/mp4");
    CHECK(mime_for("a.AVI") == "video/x-msvideo");
    CHECK(mime_for("a.mkv") == "video/x-matroska");
    CHECK(mime_for("a.jpg") == "image/jpeg");
    CHECK(mime_for("a.png") == "image/png");
    CHECK(mime_for("a.bin") == "application/octet-stream");
}

TEST_CASE("Timelapse routes: the three paths and nothing else", "[Timelapse]")
{
    std::string printer, what;
    REQUIRE(match_route("/printers/sm%3AABC/timelapses", printer, what));
    CHECK(printer == "sm%3AABC");
    CHECK(what == "list");
    REQUIRE(match_route("/printers/01P00A123/timelapses/thumbnail", printer, what));
    CHECK(printer == "01P00A123");
    CHECK(what == "thumbnail");
    REQUIRE(match_route("/printers/host/timelapses/video", printer, what));
    CHECK(what == "video");
    CHECK_FALSE(match_route("/printers//timelapses", printer, what));
    CHECK_FALSE(match_route("/printers/host/control", printer, what));
    CHECK_FALSE(match_route("/printers/host/timelapses/", printer, what));
    CHECK_FALSE(match_route("/printers/host/timelapses/delete", printer, what));
    CHECK_FALSE(match_route("/printers/a/b/timelapses", printer, what));
    CHECK_FALSE(match_route("/plates/0/timelapses", printer, what));
}

// ---- listings --------------------------------------------------------------------------------------

TEST_CASE("Moonraker timelapse root: videos, their preview by stem, frames ignored, newest first", "[Timelapse]")
{
    const json answer = json::parse(R"({"result": [
        {"path": "timelapse_benchy_202610011200.mp4", "modified": 1759320000.5, "size": 1048576, "permissions": "rw"},
        {"path": "timelapse_benchy_202610011200.jpg", "modified": 1759320001.0, "size": 20480},
        {"path": "timelapse_cube_202609301000.mp4", "modified": 1759226400.0, "size": 2048},
        {"path": "timelapse_cube_202609301000.png", "modified": 1759226400.0, "size": 999},
        {"path": "timelapse_cube_202609301000.jpg", "modified": 1759226400.0, "size": 1000},
        {"path": "frames/frame000001.jpg", "modified": 1759320000.0, "size": 100},
        {"path": "frames/old.mp4", "modified": 1759999999.0, "size": 100},
        {"path": "notes.txt", "modified": 1759999999.0, "size": 10},
        {"filename": "old_moonraker.mkv", "modified": 1700000000, "size": "77"},
        {"path": "../escape.mp4", "modified": 1, "size": 1}
    ]})");
    const std::vector<File> files = parse_moonraker_list(answer);
    REQUIRE(files.size() == 3);
    CHECK(files[0].name == "timelapse_benchy_202610011200.mp4");
    CHECK(files[0].size == 1048576);
    CHECK(files[0].time == 1759320000);
    CHECK(files[0].has_thumbnail);
    CHECK(files[0].thumbnail == "timelapse_benchy_202610011200.jpg");
    CHECK(files[1].name == "timelapse_cube_202609301000.mp4");
    CHECK(files[1].thumbnail == "timelapse_cube_202609301000.jpg"); // .jpg wins over .png
    CHECK(files[2].name == "old_moonraker.mkv");
    CHECK(files[2].size == 77);
    CHECK_FALSE(files[2].has_thumbnail);

    const json out = files_json(files);
    REQUIRE(out.size() == 3);
    CHECK(out[0]["name"] == "timelapse_benchy_202610011200.mp4");
    CHECK(out[0]["mime"] == "video/mp4");
    CHECK(out[0]["has_thumbnail"] == true);
    CHECK(out[0]["duration_s"].is_null());
    CHECK(out[2]["mime"] == "video/x-matroska");

    CHECK(parse_moonraker_list(json::parse(R"({"error": {"code": 400}})")).empty());
    CHECK(parse_moonraker_list(json::parse("[]")).empty());
    CHECK(parse_moonraker_list(json::parse(R"([{"path": "a.mp4", "modified": 5, "size": 1}])")).size() == 1);
}

TEST_CASE("Bambu LIST_INFO reply: name, path, time, size; videos only", "[Timelapse]")
{
    const json reply = json::parse(R"({"file_lists": [
        {"name": "video_2026-09-29_08-00-00.mp4", "path": "/timelapse/video_2026-09-29_08-00-00.mp4", "time": 1759132800, "size": 52428800},
        {"name": "video_2026-10-01_09-30-00.mp4", "path": "/timelapse/video_2026-10-01_09-30-00.mp4", "time": 1759311000, "size": 1000},
        {"name": "old.avi", "time": 1600000000, "size": 5},
        {"name": "thumbnail.jpg", "path": "/timelapse/thumbnail/x.jpg", "time": 1, "size": 1},
        {"name": "../x.mp4", "path": "/x.mp4", "time": 1, "size": 1}
    ]})");
    const std::vector<File> files = parse_bambu_list(reply);
    REQUIRE(files.size() == 3);
    CHECK(files[0].name == "video_2026-10-01_09-30-00.mp4");
    CHECK(files[0].path == "/timelapse/video_2026-10-01_09-30-00.mp4");
    CHECK(files[1].size == 52428800);
    CHECK(files[2].name == "old.avi");
    CHECK(files[2].path.empty());
    CHECK(files[2].has_thumbnail);
    CHECK(find_file(files, "old.avi") == &files[2]);
    CHECK(find_file(files, "nope.mp4") == nullptr);
    CHECK(parse_bambu_list(json::object()).empty());
}

// ---- Range ---------------------------------------------------------------------------------------------

TEST_CASE("Range: one byte range against the file size", "[Timelapse]")
{
    const std::uint64_t total = 1000;
    ByteRange r = parse_range("", total);
    CHECK(r.kind == ByteRange::None);

    r = parse_range("bytes=0-99", total);
    REQUIRE(r.kind == ByteRange::Satisfiable);
    CHECK(r.start == 0);
    CHECK(r.end == 99);
    CHECK(r.length() == 100);
    CHECK(content_range(r, total) == "bytes 0-99/1000");

    r = parse_range(" bytes=500- ", total);
    REQUIRE(r.kind == ByteRange::Satisfiable);
    CHECK(r.start == 500);
    CHECK(r.end == 999);

    r = parse_range("bytes=-100", total); // the last 100
    REQUIRE(r.kind == ByteRange::Satisfiable);
    CHECK(r.start == 900);
    CHECK(r.end == 999);

    r = parse_range("bytes=-5000", total); // more than the file: all of it
    REQUIRE(r.kind == ByteRange::Satisfiable);
    CHECK(r.start == 0);

    r = parse_range("bytes=990-5000", total); // an end past the file is clipped
    REQUIRE(r.kind == ByteRange::Satisfiable);
    CHECK(r.end == 999);

    r = parse_range("BYTES=0-0", total);
    REQUIRE(r.kind == ByteRange::Satisfiable);
    CHECK(r.length() == 1);

    CHECK(parse_range("bytes=1000-", total).kind == ByteRange::Unsatisfiable);
    CHECK(parse_range("bytes=1000-2000", total).kind == ByteRange::Unsatisfiable);
    CHECK(parse_range("bytes=-0", total).kind == ByteRange::Unsatisfiable);
    CHECK(parse_range("bytes=0-", 0).kind == ByteRange::Unsatisfiable);
    CHECK(content_range(parse_range("bytes=2000-", total), total) == "bytes */1000");

    // Ignored (whole file): several ranges, another unit, garbage, an inverted range.
    CHECK(parse_range("bytes=0-1,5-9", total).kind == ByteRange::None);
    CHECK(parse_range("items=0-1", total).kind == ByteRange::None);
    CHECK(parse_range("bytes=abc", total).kind == ByteRange::None);
    CHECK(parse_range("bytes=-", total).kind == ByteRange::None);
    CHECK(parse_range("bytes=5-2", total).kind == ByteRange::None);
    CHECK(parse_range("bytes=+5-9", total).kind == ByteRange::None);
}

TEST_CASE("Range: the forwarded form, and an upstream Content-Range", "[Timelapse]")
{
    CHECK(normalize_range(" bytes=0-99") == "bytes=0-99");
    CHECK(normalize_range("bytes=100-") == "bytes=100-");
    CHECK(normalize_range("bytes=-20") == "bytes=-20");
    CHECK(normalize_range("bytes=-0").empty());
    CHECK(normalize_range("bytes=0-1,4-5").empty());
    CHECK(normalize_range("").empty());

    std::uint64_t a = 0, b = 0, t = 0;
    REQUIRE(parse_content_range("bytes 100-199/1000", a, b, t));
    CHECK(a == 100);
    CHECK(b == 199);
    CHECK(t == 1000);
    CHECK_FALSE(parse_content_range("bytes */1000", a, b, t));
    CHECK_FALSE(parse_content_range("bytes 5-1/10", a, b, t));
    CHECK_FALSE(parse_content_range("bytes 0-10/10", a, b, t));

    const std::string cd = content_disposition("Zeit \"raffer\" ä.mp4");
    CHECK(cd.find("attachment; filename=\"Zeit _raffer_ __.mp4\"") == 0);
    CHECK(cd.find("filename*=UTF-8''Zeit%20%22raffer%22%20%C3%A4.mp4") != std::string::npos);
    CHECK(url_encode("a b+c/d.mp4") == "a%20b%2Bc%2Fd.mp4");
}

TEST_CASE("Upstream address and answer head", "[Timelapse]")
{
    HttpTarget t;
    REQUIRE(parse_base_url("http://192.168.1.50", t));
    CHECK(t.host == "192.168.1.50");
    CHECK(t.port == 80);
    CHECK_FALSE(t.tls);
    REQUIRE(parse_base_url("http://127.0.0.1:18189/", t));
    CHECK(t.port == 18189);
    REQUIRE(parse_base_url("https://u1.local:7125/path", t));
    CHECK(t.tls);
    CHECK(t.host == "u1.local");
    CHECK(t.port == 7125);
    REQUIRE(parse_base_url("http://[fe80::1]:8080", t));
    CHECK(t.host == "fe80::1");
    CHECK(t.port == 8080);
    CHECK_FALSE(parse_base_url("ftp://x", t));
    CHECK_FALSE(parse_base_url("http://", t));
    CHECK_FALSE(parse_base_url("http://x:0", t));
    CHECK_FALSE(parse_base_url("http://x:70000", t));

    UpstreamHead h;
    REQUIRE(parse_upstream_head("HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\ncontent-length: 100\r\n"
                                "Content-Range: bytes 0-99/1000\r\nAccept-Ranges: bytes\r\n",
                                h));
    CHECK(h.status == 206);
    CHECK(h.has_length);
    CHECK(h.content_length == 100);
    CHECK(h.content_range == "bytes 0-99/1000");
    CHECK(h.content_type == "video/mp4");
    CHECK_FALSE(h.chunked);
    REQUIRE(parse_upstream_head("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n", h));
    CHECK(h.chunked);
    CHECK_FALSE(h.has_length);
    CHECK_FALSE(parse_upstream_head("garbage\r\n", h));
}

// ---- the tunnel framing ------------------------------------------------------------------------------

TEST_CASE("Tunnel samples: JSON head, then the payload after the blank line", "[Timelapse]")
{
    const std::string payload = std::string("\xFF\xD8\n\nab\n", 7);
    const std::string sample  = R"({"result":1,"sequence":4,"reply":{"size":7,"path":"/a.mp4#thumbnail"}})" "\n\n" + payload;
    Message           m       = split_sample((const unsigned char*) sample.data(), sample.size());
    REQUIRE(m.ok);
    CHECK(m.is_reply);
    CHECK(m.result == CONTINUE);
    CHECK(m.sequence == 4);
    CHECK(m.body["path"] == "/a.mp4#thumbnail");
    REQUIRE(m.size == payload.size());
    CHECK(std::string((const char*) m.data, m.size) == payload);

    const std::string notify = R"({"cmdtype":256,"sequence":0,"notify":{"x":1}})";
    m = split_sample((const unsigned char*) notify.data(), notify.size());
    REQUIRE(m.ok);
    CHECK_FALSE(m.is_reply);
    CHECK(m.cmdtype == 256);
    CHECK(m.size == 0);

    const std::string bad = "not json\n\n";
    CHECK_FALSE(split_sample((const unsigned char*) bad.data(), bad.size()).ok);

    const json req = json::parse(request_text(LIST_INFO, 7, json{ { "type", "timelapse" } }));
    CHECK(req["cmdtype"] == 1);
    CHECK(req["sequence"] == 7);
    CHECK(req["req"]["type"] == "timelapse");
}

TEST_CASE("Tunnel MD5 is the standard digest in upper-case hex", "[Timelapse]")
{
    Md5 md5;
    md5.update("a", 1);
    md5.update("bc", 2);
    CHECK(md5.hex() == "900150983CD24FB0D6963F7D28E17F72");
    Md5 empty;
    CHECK(empty.hex() == "D41D8CD98F00B204E9800998ECF8427E");
}

TEST_CASE("Tunnel results become an HTTP status and a code", "[Timelapse]")
{
    CHECK(tunnel_failure(-2).status == 501);
    CHECK(tunnel_failure(-2).code == "no_tunnel");
    CHECK(tunnel_failure(ERROR_RES_BUSY).code == "busy");
    CHECK(tunnel_failure(ERROR_RES_BUSY).status == 409);
    CHECK(tunnel_failure(STORAGE_UNAVAILABLE).code == "no_storage");
    CHECK(tunnel_failure(FILE_NO_EXIST).status == 404);
    CHECK(tunnel_failure(FILE_TYPE_ERR).code == "unsupported");
    CHECK(tunnel_failure(TUNNEL_TIMEOUT).status == 504);
    CHECK(tunnel_failure(42).status == 502);
}

// ---- a pretend printer behind the BambuSource API -----------------------------------------------------

namespace {

// What a printer's storage holds and how it misbehaves. One at a time (the API is plain C).
struct FakePrinter
{
    std::map<std::string, std::string> files;      // path -> bytes
    std::map<std::string, std::string> pictures;   // path -> thumbnail bytes
    int                                open_result { 0 };  // StartStreamEx: 0, or 1 = no conversation free
    int                                list_result { 0 };
    bool                               corrupt_md5 { false };
    size_t                             chunk { 4 };  // download chunk size
    size_t                             thumb_chunk { 3 };
    int                                would_block_reads { 2 }; // before each answer
    std::deque<std::string>            out;        // samples waiting to be read
    std::string                        current;    // the sample handed out last
    std::vector<json>                  requests;
    int                                creates { 0 }, closes { 0 };
    int                                pending_blocks { 0 };
};
FakePrinter* g_fake = nullptr;

std::string sample_of(int result, int seq, const json& reply, const std::string& payload = std::string())
{
    json root;
    root["result"]   = result;
    root["sequence"] = seq;
    root["reply"]    = reply;
    return root.dump() + "\n\n" + payload;
}

std::string md5_of(const std::string& s)
{
    Md5 m;
    m.update(s.data(), s.size());
    return m.hex();
}

int f_create(Bambu_Tunnel* t, char const* url)
{
    ++g_fake->creates;
    *t = (Bambu_Tunnel) g_fake;
    return std::strncmp(url, "bambu:///local/", 15) == 0 ? 0 : -1;
}
void f_set_logger(Bambu_Tunnel, Logger, void*) {}
int  f_open(Bambu_Tunnel) { return 0; }
int  f_start_stream_ex(Bambu_Tunnel, int type) { return type == CTRL_TYPE ? g_fake->open_result : -1; }
void f_close(Bambu_Tunnel) { ++g_fake->closes; }
void f_destroy(Bambu_Tunnel) {}

int f_send(Bambu_Tunnel, int ctrl, char const* data, int len)
{
    if (ctrl != CTRL_TYPE) return -1;
    const json root = json::parse(std::string(data, len));
    g_fake->requests.push_back(root);
    const int   cmd = root["cmdtype"], seq = root["sequence"];
    const json& req = root["req"];
    FakePrinter& p  = *g_fake;
    // A notification first: the client must skip it.
    p.out.push_back(json{ { "cmdtype", 0x100 }, { "sequence", 0 }, { "notify", json::object() } }.dump());
    if (cmd == LIST_INFO) {
        if (p.list_result != 0) { p.out.push_back(sample_of(p.list_result, seq, json::object())); return 0; }
        json lists = json::array();
        for (const auto& kv : p.files) {
            const std::string name = kv.first.substr(kv.first.find_last_of('/') + 1);
            lists.push_back(json{ { "name", name }, { "path", kv.first }, { "time", 1759000000 + (int) lists.size() }, { "size", kv.second.size() } });
        }
        p.out.push_back(sample_of(0, seq, json{ { "file_lists", lists } }));
    } else if (cmd == SUB_FILE) {
        const json& paths = req["paths"];
        for (size_t i = 0; i < paths.size(); ++i) {
            const std::string want = paths[i];
            const std::string path = want.substr(0, want.find('#'));
            const bool        last = i + 1 == paths.size();
            auto              it   = p.pictures.find(path);
            if (it == p.pictures.end()) { // no picture: a zero-size reply
                p.out.push_back(sample_of(last ? 0 : 1, seq, json{ { "path", want }, { "size", 0 } }));
                continue;
            }
            const std::string& pic = it->second;
            for (size_t off = 0; off < pic.size(); off += p.thumb_chunk) {
                const std::string part = pic.substr(off, p.thumb_chunk);
                const bool        more = off + p.thumb_chunk < pic.size();
                p.out.push_back(sample_of(last && !more ? 0 : 1, seq,
                                          json{ { "path", want }, { "size", part.size() }, { "continue", more }, { "mimetype", "image/jpeg" } }, part));
            }
        }
    } else if (cmd == FILE_DOWNLOAD) {
        const std::string path = req.value("path", "");
        auto              it   = p.files.find(path);
        if (it == p.files.end()) { p.out.push_back(sample_of(FILE_NO_EXIST, seq, json::object())); return 0; }
        const std::string& bytes = it->second;
        for (size_t off = 0; off < bytes.size() || off == 0; off += p.chunk) {
            const std::string part = bytes.substr(off, p.chunk);
            const bool        last = off + p.chunk >= bytes.size();
            json              r{ { "offset", off }, { "total", bytes.size() }, { "size", part.size() } };
            if (last) r["file_md5"] = p.corrupt_md5 ? std::string(32, '0') : md5_of(bytes);
            p.out.push_back(sample_of(last ? 0 : 1, seq, r, part));
            if (last) break;
        }
    }
    p.pending_blocks = p.would_block_reads;
    return 0;
}

int f_read(Bambu_Tunnel, Bambu_Sample* s)
{
    FakePrinter& p = *g_fake;
    if (p.pending_blocks > 0) { --p.pending_blocks; return Bambu_would_block; }
    if (p.out.empty()) return Bambu_would_block;
    p.current = p.out.front();
    p.out.pop_front();
    s->buffer = (const unsigned char*) p.current.data();
    s->size   = (int) p.current.size();
    return 0;
}

BambuLib fake_lib()
{
    BambuLib lib;
    std::memset(&lib, 0, sizeof(lib));
    lib.Bambu_Create        = f_create;
    lib.Bambu_SetLogger     = f_set_logger;
    lib.Bambu_Open          = f_open;
    lib.Bambu_StartStreamEx = f_start_stream_ex;
    lib.Bambu_SendMessage   = f_send;
    lib.Bambu_ReadSample    = f_read;
    lib.Bambu_Close         = f_close;
    lib.Bambu_Destroy       = f_destroy;
    return lib;
}

struct FakeScope
{
    FakePrinter p;
    FakeScope() { g_fake = &p; }
    ~FakeScope() { g_fake = nullptr; }
};

const char* URL = "bambu:///local/192.0.2.10.?port=6000&user=bblp&passwd=x&device=TEST";

} // namespace

TEST_CASE("Bambu tunnel: list, thumbnails and a download against a pretend printer", "[Timelapse]")
{
    FakeScope fake;
    fake.p.files["/timelapse/video_a.mp4"]    = "0123456789abcdef-0123";
    fake.p.files["/timelapse/video_b.mp4"]    = "xy";
    fake.p.pictures["/timelapse/video_a.mp4"] = std::string("\xFF\xD8picture-a", 11);
    const BambuLib lib = fake_lib();

    TunnelSession s(lib);
    REQUIRE(s.open(URL) == 0);
    REQUIRE(s.is_open());

    std::vector<File> files;
    REQUIRE(list_timelapses(s, files) == SUCCESS);
    REQUIRE(files.size() == 2);
    CHECK(files[0].name == "video_b.mp4"); // newer time
    CHECK(files[1].path == "/timelapse/video_a.mp4");
    CHECK(fake.p.requests.back()["req"]["type"] == "timelapse");
    CHECK(fake.p.requests.back()["req"]["api_version"] == 2);

    SECTION("thumbnails: one request for a batch, a picture split over replies, a file without one skipped")
    {
        std::map<std::string, std::string> pics;
        REQUIRE(fetch_thumbnails(s, files, pics) == SUCCESS);
        CHECK(pics.size() == 1);
        CHECK(pics["video_a.mp4"] == std::string("\xFF\xD8picture-a", 11));
        CHECK(fake.p.requests.back()["req"]["paths"][0] == "/timelapse/video_b.mp4#thumbnail");
    }
    SECTION("download: chunks in order, size and MD5 checked")
    {
        std::string got;
        std::uint64_t seen_total = 0;
        REQUIRE(download(s, files[1], [&](std::uint64_t off, std::uint64_t total, const unsigned char* d, std::size_t n) {
                    CHECK(off == got.size());
                    seen_total = total;
                    got.append((const char*) d, n);
                    return true;
                }) == SUCCESS);
        CHECK(got == "0123456789abcdef-0123");
        CHECK(seen_total == got.size());
        CHECK(fake.p.requests.back()["req"]["path"] == "/timelapse/video_a.mp4");
        // The session is still usable for the next request.
        std::vector<File> again;
        CHECK(list_timelapses(s, again) == SUCCESS);
    }
    SECTION("download: a wrong MD5 is FILE_CHECK_ERR")
    {
        fake.p.corrupt_md5 = true;
        CHECK(download(s, files[1], nullptr) == FILE_CHECK_ERR);
    }
    SECTION("download: a reader that stops closes the session")
    {
        CHECK(download(s, files[1], [](std::uint64_t, std::uint64_t, const unsigned char*, std::size_t) { return false; }) == TUNNEL_STOPPED);
        CHECK_FALSE(s.is_open());
    }
    SECTION("download: a file that went away")
    {
        File gone = files[1];
        gone.path = "/timelapse/gone.mp4";
        CHECK(download(s, gone, nullptr) == FILE_NO_EXIST);
    }
}

TEST_CASE("Bambu tunnel: a busy printer, no SD card, a placeholder library, silence", "[Timelapse]")
{
    FakeScope fake;
    BambuLib  lib = fake_lib();
    SECTION("no conversation free")
    {
        fake.p.open_result = 1;
        TunnelSession s(lib);
        CHECK(s.open(URL) == ERROR_RES_BUSY);
        CHECK_FALSE(s.is_open());
    }
    SECTION("no storage")
    {
        fake.p.list_result = STORAGE_UNAVAILABLE;
        TunnelSession s(lib);
        REQUIRE(s.open(URL) == 0);
        std::vector<File> files;
        CHECK(list_timelapses(s, files) == STORAGE_UNAVAILABLE);
        CHECK(tunnel_failure(STORAGE_UNAVAILABLE).code == "no_storage");
    }
    SECTION("the placeholder library exports nothing")
    {
        BambuLib empty;
        std::memset(&empty, 0, sizeof(empty));
        TunnelSession s(empty);
        CHECK(s.open(URL) == -2);
    }
    SECTION("a printer that never answers times out")
    {
        TunnelSession s(lib);
        REQUIRE(s.open(URL) == 0);
        fake.p.would_block_reads = 1000000;
        int r = s.request(LIST_INFO, json::object(), nullptr, 200);
        CHECK(r == TUNNEL_TIMEOUT);
        CHECK_FALSE(s.is_open());
    }
}
