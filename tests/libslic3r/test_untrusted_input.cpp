#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/UntrustedInput.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::untrusted;
namespace fs = boost::filesystem;

// Rules for input from outside the machine (UntrustedInput.hpp): "Open in" links, URLs a web page
// asks us to open, downloaded file names, archive entry names, and the settings a project brings.

// ---- URL parsing and host checks ----------------------------------------------------------------

TEST_CASE("parse_url takes an ordinary https URL apart", "[Untrusted][Url]")
{
    Url u;
    REQUIRE(parse_url("https://Files.Printables.com:443/media/prints/1/a.stl?x=1#frag", u));
    CHECK(u.scheme == "https");
    CHECK(u.host == "files.printables.com");
    CHECK(u.port == 443);
    CHECK(u.path == "/media/prints/1/a.stl");
    CHECK(u.query == "x=1");
    CHECK_FALSE(u.has_userinfo);
}

TEST_CASE("parse_url refuses what a spoofed or broken URL looks like", "[Untrusted][Url]")
{
    Url u;
    CHECK_FALSE(parse_url("", u));
    CHECK_FALSE(parse_url("printables.com/a.stl", u));
    CHECK_FALSE(parse_url("https:/printables.com/a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com /a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com\\@evil.tld/a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com\t/a.stl", u));
    CHECK_FALSE(parse_url("https:///a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com:99999/a", u));
    CHECK_FALSE(parse_url("https://printables.com:/a", u));
    CHECK_FALSE(parse_url("https://pr\xc3\xadntables.com/a", u)); // IDN not punycoded
    // userinfo is parsed, so callers can see the real host
    REQUIRE(parse_url("https://printables.com@evil.tld/a.stl", u));
    CHECK(u.has_userinfo);
    CHECK(u.host == "evil.tld");
}

TEST_CASE("host_is_or_under matches a domain and its subdomains only", "[Untrusted][Url]")
{
    CHECK(host_is_or_under("printables.com", "printables.com"));
    CHECK(host_is_or_under("files.printables.com", "printables.com"));
    CHECK(host_is_or_under("FILES.Printables.COM", "printables.com"));
    CHECK_FALSE(host_is_or_under("printables.com.evil.tld", "printables.com"));
    CHECK_FALSE(host_is_or_under("evilprintables.com", "printables.com"));
    CHECK_FALSE(host_is_or_under("printables.co", "printables.com"));
    CHECK_FALSE(host_is_or_under("", "printables.com"));
}

TEST_CASE("the model-site link checks match real URLs and nothing spoofed", "[Untrusted][Url]")
{
    // The old regex versions matched the whole string against a bare host, so none of these matched.
    CHECK(untrusted::is_printables_link("https://www.printables.com/model/12345-benchy"));
    CHECK(untrusted::is_printables_link("https://printables.com"));
    CHECK(untrusted::is_makerworld_link("https://makerworld.com/en/models/2077"));
    CHECK(untrusted::is_makerworld_link("https://makerworld.com.cn/zh/models/1"));
    CHECK(untrusted::is_thingiverse_link("https://www.thingiverse.com/thing:763622"));

    // The Utils.hpp spellings are the same checks.
    CHECK(Slic3r::is_printables_link("https://www.printables.com/model/1"));
    CHECK_FALSE(Slic3r::is_makerworld_link("https://makerworld.com.evil.tld/"));

    CHECK_FALSE(untrusted::is_printables_link("https://printables.com.evil.tld/model/1"));
    CHECK_FALSE(untrusted::is_printables_link("https://evil.tld/?https://printables.com"));
    CHECK_FALSE(untrusted::is_printables_link("https://printables.com@evil.tld/"));
    CHECK_FALSE(untrusted::is_printables_link("ftp://printables.com/"));
    CHECK_FALSE(untrusted::is_makerworld_link("https://makerworld.com.evil.tld/"));
    CHECK_FALSE(untrusted::is_makerworld_link("https://notmakerworld.com/"));
    CHECK_FALSE(untrusted::is_thingiverse_link("https://thingiverse.com.evil.tld/"));

    CHECK(is_makerworld_file_url("https://public-cdn.bblmw.com/models/a.3mf"));
    CHECK(is_makerworld_file_url("https://makerworld.bblmw.com/a.3mf"));
    CHECK_FALSE(is_makerworld_file_url("https://bblmw.com.evil.tld/a.3mf"));
    CHECK_FALSE(is_makerworld_file_url("https://files.printables.com/a.3mf"));
}

TEST_CASE("local and IP hosts are recognised", "[Untrusted][Url]")
{
    for (const char *h : {"127.0.0.1", "10.0.0.5", "192.168.1.20", "127.1", "2130706433", "0x7f.1", "[::1]", "localhost",
                          "foo.localhost", "printer.local", "nas.lan", "box.home.arpa", "printer"})
        CHECK(is_local_or_ip_host(h));
    for (const char *h : {"files.printables.com", "public-cdn.bblmw.com", "1password.com", "3dprint.example.org"})
        CHECK_FALSE(is_local_or_ip_host(h));
}

// ---- URLs a page asks us to open ------------------------------------------------------------------

TEST_CASE("only web links may be opened from a page", "[Untrusted][OpenUrl]")
{
    CHECK(is_safe_to_open_externally("https://wiki.snapmaker.com/en/page"));
    CHECK(is_safe_to_open_externally("http://example.org/"));
    CHECK(is_safe_to_open_externally("mailto:support@example.org"));

    CHECK_FALSE(is_safe_to_open_externally("file:///C:/Windows/System32/calc.exe"));
    CHECK_FALSE(is_safe_to_open_externally("FILE://server/share/x.exe"));
    CHECK_FALSE(is_safe_to_open_externally("C:\\Windows\\System32\\calc.exe"));
    CHECK_FALSE(is_safe_to_open_externally("\\\\server\\share\\x.exe"));
    CHECK_FALSE(is_safe_to_open_externally("calc.exe"));
    CHECK_FALSE(is_safe_to_open_externally("javascript:alert(1)"));
    CHECK_FALSE(is_safe_to_open_externally("ms-msdt:/id PCWDiagnostic"));
    CHECK_FALSE(is_safe_to_open_externally("search-ms:query=x&crumb=location:\\\\evil\\share"));
    CHECK_FALSE(is_safe_to_open_externally("edgeslicer://open?file=https%3A%2F%2Fx"));
    CHECK_FALSE(is_safe_to_open_externally("https://example.org/ \"&calc"));
    CHECK_FALSE(is_safe_to_open_externally("https://exa mple.org/"));
    CHECK_FALSE(is_safe_to_open_externally("mailto:x@y.org\" & calc"));
    CHECK_FALSE(is_safe_to_open_externally(""));
}

TEST_CASE("our own pages are recognised by origin", "[Untrusted][Bridge]")
{
    CHECK(is_page_server_url("http://127.0.0.1:13619/web/flutter_web/index.html?path=0&edge_page_token=ab", 13619));
    CHECK(is_page_server_url("http://localhost:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://127.0.0.1:13620/web/", 13619));
    CHECK_FALSE(is_page_server_url("https://127.0.0.1:13619/web/", 13619));
    CHECK_FALSE(is_page_server_url("http://127.0.0.1.evil.tld:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://user@127.0.0.1:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://192.168.1.20:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://127.0.0.1:13619/", 0));

    CHECK(local_path_from_file_url("file:///C:/Program%20Files/EdgeSlicer/resources/web/model/index.html?lang=en") ==
          "C:/Program Files/EdgeSlicer/resources/web/model/index.html");
    CHECK(local_path_from_file_url("file:///C:/Users/a%20b/%23x/%25y/index.html?lang=en") ==
          "C:/Users/a b/#x/%y/index.html");
    CHECK(local_path_from_file_url("file:///home/a%20b/%23x/%25y/index.html?lang=en") ==
          "/home/a b/#x/%y/index.html");
    CHECK(local_path_from_file_url("file:///usr/share/edgeslicer/web/x.html") == "/usr/share/edgeslicer/web/x.html");
    CHECK(local_path_from_file_url("file://localhost/C:/x.html") == "C:/x.html");
    CHECK(local_path_from_file_url("file://server/share/x.html").empty());
    CHECK(local_path_from_file_url("file:////server/share/x.html").empty());
    CHECK(local_path_from_file_url("file:///%2F%2Fserver/share/x.html").empty());
    CHECK(local_path_from_file_url("https://example.org/x.html").empty());
}

TEST_CASE("only document, picture and model attachments are launched", "[Untrusted][Attachment]")
{
    for (const char *n : {"manual.pdf", "Assembly.PNG", "notes.txt", "part.stl", "bom.xlsx", "guide.docx"})
        CHECK(is_safe_attachment_to_launch(n));
    for (const char *n : {"setup.exe", "run.bat", "x.cmd", "x.ps1", "x.vbs", "x.js", "x.lnk", "x.url", "x.scr", "x.hta", "x.msi",
                          "x.docm", "x.xlsm", "x.doc", "x.pdf.exe", "pdf", ".pdf", "noext"})
        CHECK_FALSE(is_safe_attachment_to_launch(n));
}

TEST_CASE("project-page images may only load from https", "[Untrusted][ProjectPage]")
{
    CHECK(is_safe_project_image_url("https://cdn.example.com/preview.png"));
    CHECK(is_safe_project_image_url("HTTPS://CDN.Example.COM/preview.png"));
    CHECK(is_safe_project_image_url("https://cdn.example.com:443/a/b.png?x=1#frag"));

    CHECK_FALSE(is_safe_project_image_url("http://127.0.0.1:9/x"));
    CHECK_FALSE(is_safe_project_image_url("http://example.com/preview.png"));
    CHECK_FALSE(is_safe_project_image_url("HTTP://example.com/preview.png"));
    CHECK_FALSE(is_safe_project_image_url("javascript:alert(1)"));
    CHECK_FALSE(is_safe_project_image_url("JAVASCRIPT:alert(1)"));
    CHECK_FALSE(is_safe_project_image_url("data:image/png;base64,aaaa"));
    CHECK_FALSE(is_safe_project_image_url("data:text/html,<img src=x>"));
    CHECK_FALSE(is_safe_project_image_url("file:///tmp/preview.png"));
    CHECK_FALSE(is_safe_project_image_url("https://user@evil.tld/preview.png"));
    CHECK_FALSE(is_safe_project_image_url(""));
    CHECK_FALSE(is_safe_project_image_url("preview.png"));
    CHECK_FALSE(is_safe_project_image_url("//cdn.example.com/preview.png"));
}

TEST_CASE("archive relative paths may not break out of HTML attributes", "[Untrusted][ProjectPage]")
{
    CHECK_FALSE(is_safe_archive_relative_path("foo\".png"));
    CHECK_FALSE(is_safe_archive_relative_path("foo'.png"));
    CHECK_FALSE(is_safe_archive_relative_path("foo<.png"));
    CHECK_FALSE(is_safe_archive_relative_path("foo>.png"));
    CHECK_FALSE(is_safe_archive_relative_path("x.png\"><img"));
    CHECK_FALSE(is_safe_archive_relative_path("Auxiliaries/Model Pictures/x.\"onclick=1.png"));
    CHECK(is_safe_archive_relative_path("Auxiliaries/Model Pictures/cover.png"));
}

TEST_CASE("attachment paths stay inside the project auxiliary directory", "[Untrusted][Attachment]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_attach_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path root    = dir / "Auxiliaries";
    const fs::path others  = root / "Others";
    fs::create_directories(others);
    const fs::path inside  = others / "note.txt";
    const fs::path missing = others / "missing.txt";
    const fs::path outside = dir / "secret.txt";
    {
        boost::nowide::ofstream out(inside.string());
        out << "inside";
    }
    {
        boost::nowide::ofstream out(outside.string());
        out << "outside";
    }

    CHECK(is_safe_attachment_path(root, inside));
    CHECK(is_safe_attachment_path(root, missing));
    CHECK(is_safe_attachment_to_launch(inside.filename().string()));

    CHECK_FALSE(is_safe_attachment_path(root, root));
    CHECK_FALSE(is_safe_attachment_path(root, others / ".." / ".." / "secret.txt"));
    CHECK_FALSE(is_safe_attachment_path(root, outside));
    CHECK_FALSE(is_safe_attachment_path(root, dir / "Auxiliaries2" / "note.txt"));
    CHECK_FALSE(is_safe_attachment_path(root, fs::path("Others") / "note.txt"));
    CHECK_FALSE(is_safe_attachment_path(root, fs::path()));
    CHECK_FALSE(is_safe_attachment_path(fs::path(), inside));

#ifndef _WIN32
    {
        const fs::path link = others / "link.txt";
        try {
            fs::create_symlink(outside, link);
            // Extraction would replace a dest-file symlink; opening must follow it.
            CHECK(is_path_within_root(root, link));
            CHECK_FALSE(is_safe_attachment_path(root, link));
        } catch (const std::exception &) {}
    }
#endif

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- "Open in" links -------------------------------------------------------------------------------

TEST_CASE("parse_open_link understands every scheme we accept", "[Untrusted][Link]")
{
    const std::string file = "https%3A%2F%2Ffiles.printables.com%2Fmedia%2Fprints%2F1%2Fstls%2Fa%20b.stl";
    for (const char *scheme : {"edgeslicer", "orcaslicer", "prusaslicer", "bambustudio", "cura", "ultraone", "snapmaker-orca", "Snapmaker_Orca"}) {
        for (const char *sep : {"://open?file=", "://open/?file="}) {
            const OpenLink l = parse_open_link(std::string(scheme) + sep + file);
            INFO(scheme << sep);
            REQUIRE(l.ok);
            CHECK(l.file_url == "https://files.printables.com/media/prints/1/stls/a b.stl");
            CHECK(l.name.empty());
        }
    }
    const OpenLink named = parse_open_link("edgeslicer://open?file=" + file + "&name=My%20Benchy.3mf");
    REQUIRE(named.ok);
    CHECK(named.name == "My Benchy.3mf");

    const OpenLink bbs = parse_open_link("bambustudioopen://https%3A%2F%2Fpublic-cdn.bblmw.com%2Fa.3mf");
    REQUIRE(bbs.ok);
    CHECK(bbs.scheme == "bambustudioopen");
    CHECK(bbs.file_url == "https://public-cdn.bblmw.com/a.3mf");
}

TEST_CASE("parse_open_link refuses other schemes and actions", "[Untrusted][Link]")
{
    CHECK_FALSE(parse_open_link("edgeslicer://run?file=x").ok);
    CHECK_FALSE(parse_open_link("edgeslicer://open").ok);
    CHECK_FALSE(parse_open_link("edgeslicer://open?file=").ok);
    CHECK_FALSE(parse_open_link("evilslicer://open?file=https%3A%2F%2Fx").ok);
    CHECK_FALSE(parse_open_link("C:\\models\\a.3mf").ok);
    CHECK_FALSE(parse_open_link("https://files.printables.com/a.stl").ok);
}

TEST_CASE("percent_decode", "[Untrusted][Link]")
{
    CHECK(percent_decode("a%20b%2Fc") == "a b/c");
    CHECK(percent_decode("100%") == "100%");
    CHECK(percent_decode("%zz%4") == "%zz%4");
    CHECK(percent_decode("a+b") == "a+b");
}

// ---- download decisions -----------------------------------------------------------------------------

TEST_CASE("model downloads from the known sites are allowed", "[Untrusted][Download]")
{
    for (const char *url : {"https://files.printables.com/media/prints/1/stls/a.stl",
                            "https://media.printables.com/media/prints/1/a.3mf",
                            "https://public-cdn.bblmw.com/models/a.3mf",
                            "https://makerworld.bblmw.com/a.3mf",
                            "https://cdn.thingiverse.com/assets/a.zip",
                            "https://www.thingiverse.com/download:123456",
                            "https://public.resource.snapmaker.com/models/a.3mf",
                            "https://files.cults3d.com/a.step"}) {
        INFO(url);
        CHECK(check_model_download(url, "edgeslicer").verdict == DownloadVerdict::Allow);
    }
    // MakerWorld's signed storage only for the MakerWorld schemes.
    CHECK(check_model_download("https://makerworld.s3.us-west-2.amazonaws.com/a.3mf", "bambustudioopen").verdict == DownloadVerdict::Allow);
    CHECK(check_model_download("https://mw.oss-cn-hangzhou.aliyuncs.com/a.3mf", "bambustudio").verdict == DownloadVerdict::Allow);
    CHECK(check_model_download("https://makerworld.s3.us-west-2.amazonaws.com/a.3mf", "edgeslicer").verdict == DownloadVerdict::Ask);
}

TEST_CASE("model downloads from other public hosts need the user", "[Untrusted][Download]")
{
    const DownloadCheck c = check_model_download("https://models.example.org/a.3mf", "edgeslicer");
    CHECK(c.verdict == DownloadVerdict::Ask);
    CHECK(c.host == "models.example.org");
    CHECK(check_model_download("https://files.printables.com.evil.tld/a.stl").verdict == DownloadVerdict::Ask);
    CHECK(check_model_download("https://evilprintables.com/a.stl").verdict == DownloadVerdict::Ask);
}

TEST_CASE("model downloads that are never allowed", "[Untrusted][Download]")
{
    for (const char *url : {"http://files.printables.com/a.stl",               // not https
                            "file:///C:/Users/me/secret.3mf",                 // local file
                            "ftp://files.printables.com/a.stl",
                            "https://printables.com@evil.tld/a.stl",          // userinfo spoof
                            "https://127.0.0.1:13619/localfile/x.3mf",        // this machine
                            "https://192.168.1.20/a.3mf",                     // the LAN
                            "https://[::1]/a.3mf",
                            "https://localhost/a.3mf",
                            "https://printer.local/a.3mf",
                            "https://files.printables.com/a.exe",             // not a model
                            "https://files.printables.com/a.3mf.bat",
                            "https://files.printables.com/a%2Eexe",
                            "\\\\server\\share\\a.3mf",
                            "not a url"}) {
        INFO(url);
        CHECK(check_model_download(url, "edgeslicer").verdict == DownloadVerdict::Refuse);
    }
}

TEST_CASE("model extensions", "[Untrusted][Download]")
{
    for (const char *n : {"a.3mf", "A.STL", "a.step", "a.stp", "a.obj", "a.zip"})
        CHECK(has_model_extension(n));
    for (const char *n : {"a.exe", "a.3mf.exe", "a.gcode", "3mf", ".3mf", "a.amf", "a"})
        CHECK_FALSE(has_model_extension(n));
}

TEST_CASE("download file names are plain names in the download folder", "[Untrusted][Filename]")
{
    CHECK(sanitize_download_filename("Benchy.3mf") == "Benchy.3mf");
    CHECK(sanitize_download_filename("../../evil.3mf") == "evil.3mf");
    CHECK(sanitize_download_filename("..\\..\\AppData\\Roaming\\evil.3mf") == "evil.3mf");
    CHECK(sanitize_download_filename("C:\\Windows\\evil.3mf") == "evil.3mf");
    CHECK(sanitize_download_filename("C:evil.3mf") == "C_evil.3mf");
    CHECK(sanitize_download_filename("..") == "");
    CHECK(sanitize_download_filename(".") == "");
    CHECK(sanitize_download_filename("...") == "");
    CHECK(sanitize_download_filename("a/..") == "");
    CHECK(sanitize_download_filename(".hidden.3mf") == "hidden.3mf");
    CHECK(sanitize_download_filename("model.3mf. . .") == "model.3mf");
    CHECK(sanitize_download_filename("a<b>c:d\"e|f?g*h.stl") == "a_b_c_d_e_f_g_h.stl");
    CHECK(sanitize_download_filename(std::string("a\x01" "b\n.stl")) == "a_b_.stl");
    CHECK(sanitize_download_filename("NUL.3mf") == "_NUL.3mf");
    CHECK(sanitize_download_filename("con") == "_con");
    CHECK(sanitize_download_filename("Com1.stl") == "_Com1.stl");
    CHECK(sanitize_download_filename("console.stl") == "console.stl");
    const std::string longname = sanitize_download_filename(std::string(400, 'x') + ".3mf");
    CHECK(longname.size() <= 150);
    CHECK(longname.substr(longname.size() - 4) == ".3mf");
    for (const std::string &n : {std::string("a/b\\c.3mf"), std::string("../../x"), std::string("C:\\x\\y.stl")})
        CHECK(sanitize_download_filename(n).find_first_of("/\\:") == std::string::npos);
}

namespace {

void touch_download_file(const fs::path &path)
{
    boost::nowide::ofstream out(path.string());
    out << "existing";
}

std::string read_download_file(const fs::path &path)
{
    boost::nowide::ifstream in(path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct DownloadScratch
{
    fs::path dir;
    DownloadScratch()
    {
        dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_download_%%%%%%%%");
        fs::create_directories(dir);
    }
    ~DownloadScratch()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("find_unused_filename keeps a name nothing uses", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    std::string     name;
    REQUIRE(find_unused_filename(scratch.dir, "model.3mf", {}, name));
    CHECK(name == "model.3mf");
}

TEST_CASE("find_unused_filename versions a name an existing file uses instead of overwriting", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    touch_download_file(scratch.dir / "model.3mf");
    std::string name;
    REQUIRE(find_unused_filename(scratch.dir, "model.3mf", {}, name));
    CHECK(name == "model(1).3mf");
    CHECK(read_download_file(scratch.dir / "model.3mf") == "existing");
    CHECK_FALSE(fs::exists(scratch.dir / name));
}

TEST_CASE("find_unused_filename versions a name that maps onto an existing file once sanitized", "[Untrusted][Filename]")
{
    // Probe after sanitizing: "my:model.3mf" becomes "my_model.3mf". Skipping that step
    // would look for a different name and overwrite my_model.3mf at rename time.
    DownloadScratch scratch;
    touch_download_file(scratch.dir / "my_model.3mf");
    for (const char *input : {"my?model.3mf", "my:model.3mf", "my*model.3mf"}) {
        INFO(input);
        std::string name;
        REQUIRE(find_unused_filename(scratch.dir, input, {}, name));
        CHECK(name == "my_model(1).3mf");
        CHECK(read_download_file(scratch.dir / "my_model.3mf") == "existing");
    }
}

TEST_CASE("find_unused_filename treats the marker of another download as used", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    touch_download_file(download_marker_path(scratch.dir, "model.3mf"));
    std::string name;
    REQUIRE(find_unused_filename(scratch.dir, "model.3mf", {}, name));
    CHECK(name == "model(1).3mf");
}

TEST_CASE("find_unused_filename ignores the marker of the download asking", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    const fs::path  own_marker = download_marker_path(scratch.dir, "model.3mf");
    touch_download_file(own_marker);
    std::string name;
    REQUIRE(find_unused_filename(scratch.dir, "model.3mf", own_marker, name));
    CHECK(name == "model.3mf");
}

TEST_CASE("find_unused_filename keeps traversal, absolute, and drive-letter names inside the folder", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    touch_download_file(scratch.dir / "evil.3mf");
    const fs::path parent = scratch.dir.parent_path();

    struct Case { const char *input; const char *expected; };
    const Case cases[] = {
        {"../evil.3mf", "evil(1).3mf"},
        {"..\\..\\AppData\\Roaming\\evil.3mf", "evil(1).3mf"},
        {"/tmp/evil.3mf", "evil(1).3mf"},
        {"C:\\Windows\\evil.3mf", "evil(1).3mf"},
        {"C:evil.3mf", "C_evil.3mf"},
        {"a/b\\c.3mf", "c.3mf"},
    };
    for (const Case &c : cases) {
        INFO(c.input);
        std::string name;
        REQUIRE(find_unused_filename(scratch.dir, c.input, {}, name));
        CHECK(name == c.expected);
        CHECK(name.find_first_of("/\\") == std::string::npos);
        CHECK(fs::path(name).filename() == fs::path(name));
        CHECK(read_download_file(scratch.dir / "evil.3mf") == "existing");
        CHECK_FALSE(fs::exists(parent / name));
    }
}

TEST_CASE("find_unused_filename rejects empty names and names that sanitize to empty", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    for (const char *input : {"", ".", "..", "../..", "dir/", "..\\", " ", ". .", "..."}) {
        INFO(input);
        std::string name = "sentinel";
        CHECK_FALSE(find_unused_filename(scratch.dir, input, {}, name));
        CHECK(name.empty());
    }
}

TEST_CASE("find_unused_filename defuses Windows reserved names and trailing dots or spaces", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    std::string     name;
    REQUIRE(find_unused_filename(scratch.dir, "NUL.3mf", {}, name));
    CHECK(name == "_NUL.3mf");
    REQUIRE(find_unused_filename(scratch.dir, "con", {}, name));
    CHECK(name == "_con");
    REQUIRE(find_unused_filename(scratch.dir, "Com1.stl", {}, name));
    CHECK(name == "_Com1.stl");
    REQUIRE(find_unused_filename(scratch.dir, "console.stl", {}, name));
    CHECK(name == "console.stl");
    REQUIRE(find_unused_filename(scratch.dir, "model.3mf. . .", {}, name));
    CHECK(name == "model.3mf");
    REQUIRE(find_unused_filename(scratch.dir, ".hidden.3mf", {}, name));
    CHECK(name == "hidden.3mf");

    touch_download_file(scratch.dir / "_NUL.3mf");
    REQUIRE(find_unused_filename(scratch.dir, "NUL.3mf", {}, name));
    CHECK(name == "_NUL(1).3mf");
}

TEST_CASE("find_unused_filename caps a very long name and keeps the extension", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    std::string     name;
    REQUIRE(find_unused_filename(scratch.dir, std::string(400, 'x') + ".3mf", {}, name));
    CHECK(name.size() <= 150);
    CHECK(name.substr(name.size() - 4) == ".3mf");
    CHECK(name.find_first_of("/\\:") == std::string::npos);

    touch_download_file(scratch.dir / name);
    std::string unused;
    REQUIRE(find_unused_filename(scratch.dir, std::string(400, 'x') + ".3mf", {}, unused));
    CHECK(unused != name);
    CHECK(unused.substr(unused.size() - 4) == ".3mf");
}

TEST_CASE("find_unused_filename strips NUL and control characters", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    std::string     name;
    REQUIRE(find_unused_filename(scratch.dir, std::string("a\0b.3mf", 7), {}, name));
    CHECK(name == "a_b.3mf");
    REQUIRE(find_unused_filename(scratch.dir, std::string("a\x01" "b\n.stl"), {}, name));
    CHECK(name == "a_b_.stl");
    CHECK(name.find('\0') == std::string::npos);
}

TEST_CASE("find_unused_filename versions a name with no extension", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    std::string     name;
    REQUIRE(find_unused_filename(scratch.dir, "readme", {}, name));
    CHECK(name == "readme");
    touch_download_file(scratch.dir / "readme");
    REQUIRE(find_unused_filename(scratch.dir, "readme", {}, name));
    CHECK(name == "readme(1)");
}

TEST_CASE("find_unused_filename gives up after the version cap", "[Untrusted][Filename]")
{
    CHECK(FIND_UNUSED_FILENAME_MAX_VERSION == 999);
    DownloadScratch scratch;
    touch_download_file(scratch.dir / "model.3mf");
    touch_download_file(scratch.dir / "model(1).3mf");
    std::string name;
    REQUIRE(find_unused_filename(scratch.dir, "model.3mf", {}, name, 2));
    CHECK(name == "model(2).3mf");

    touch_download_file(scratch.dir / name);
    REQUIRE_FALSE(find_unused_filename(scratch.dir, "model.3mf", {}, name, 2));
    CHECK(name == "model(2).3mf");
}

TEST_CASE("open_exclusive_write cannot create the same marker twice", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    const fs::path  marker = scratch.dir / "model.3mf.1.download";
    FILE           *first  = open_exclusive_write(marker);
    REQUIRE(first != nullptr);
    FILE *second = open_exclusive_write(marker);
    CHECK(second == nullptr);
    fclose(first);
    CHECK(fs::exists(marker));
    FILE *again = open_exclusive_write(marker);
    CHECK(again == nullptr);
}

TEST_CASE("rename_no_replace fails when the destination already exists", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    const fs::path  src  = scratch.dir / "src.3mf";
    const fs::path  dest = scratch.dir / "dest.3mf";
    touch_download_file(src);
    touch_download_file(dest);
    boost::system::error_code ec;
    CHECK_FALSE(rename_no_replace(src, dest, ec));
    CHECK(ec == boost::system::errc::file_exists);
    CHECK(read_download_file(dest) == "existing");
    CHECK(fs::exists(src));
    CHECK(read_download_file(src) == "existing");

    const fs::path dest2 = scratch.dir / "new.3mf";
    REQUIRE(rename_no_replace(src, dest2, ec));
    CHECK_FALSE(fs::exists(src));
    CHECK(read_download_file(dest2) == "existing");
}

#ifndef _WIN32
TEST_CASE("find_unused_filename treats a dangling symlink as taken", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    const fs::path  dangling = scratch.dir / "model.3mf";
    fs::create_symlink(scratch.dir / "no_such_target.3mf", dangling);
    REQUIRE_FALSE(fs::exists(dangling));
    std::string name;
    REQUIRE(find_unused_filename(scratch.dir, "model.3mf", {}, name));
    CHECK(name == "model(1).3mf");
}
#endif

TEST_CASE("claim_unused_download_name recreates a removed marker when the old path is still ignored", "[Untrusted][Filename]")
{
    // FileGet pause with m_written==0 removes the marker but used to keep m_tmp_path.
    // Resume then passed that stale path as ignored_marker; claim must still succeed.
    DownloadScratch scratch;
    std::string     name;
    FILE           *first = claim_unused_download_name(scratch.dir, "model.3mf", {}, name);
    REQUIRE(first != nullptr);
    fclose(first);
    const fs::path marker = download_marker_path(scratch.dir, name);
    REQUIRE(fs::exists(marker));
    boost::system::error_code ec;
    fs::remove(marker, ec);
    REQUIRE_FALSE(fs::exists(marker));

    FILE *again = claim_unused_download_name(scratch.dir, "model.3mf", marker, name);
    REQUIRE(again != nullptr);
    fclose(again);
    CHECK(name == "model.3mf");
    CHECK(fs::exists(marker));
}

TEST_CASE("claim_unused_download_name gives distinct names to concurrent claimants", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    constexpr int   N = 8;
    std::vector<std::string> names(N);
    std::vector<FILE *>      files(N, nullptr);
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            files[i] = claim_unused_download_name(scratch.dir, "model.3mf", {}, names[i]);
        });
    }
    for (std::thread &t : threads)
        t.join();

    std::set<std::string> unique;
    for (int i = 0; i < N; ++i) {
        REQUIRE(files[i] != nullptr);
        fclose(files[i]);
        REQUIRE_FALSE(names[i].empty());
        REQUIRE(unique.insert(names[i]).second);
    }
    CHECK(unique.size() == static_cast<size_t>(N));
}

TEST_CASE("place_download_file versions when the destination already exists", "[Untrusted][Filename]")
{
    DownloadScratch scratch;
    std::string     name;
    FILE           *marker_file = claim_unused_download_name(scratch.dir, "model.3mf", {}, name);
    REQUIRE(marker_file != nullptr);
    fclose(marker_file);
    const fs::path marker = download_marker_path(scratch.dir, name);
    touch_download_file(scratch.dir / "model.3mf");
    touch_download_file(marker);

    fs::path dest;
    boost::system::error_code ec;
    REQUIRE(place_download_file(marker, scratch.dir, name, dest, ec));
    CHECK(name == "model(1).3mf");
    CHECK(dest.filename() == "model(1).3mf");
    CHECK(read_download_file(dest) == "existing");
    CHECK(read_download_file(scratch.dir / "model.3mf") == "existing");
    CHECK_FALSE(fs::exists(marker));
}

TEST_CASE("downloaded bytes must match the file type", "[Untrusted][Download]")
{
    const std::string zip = std::string("PK\x03\x04", 4) + std::string(60, 'z');
    CHECK(content_matches_extension("a.3mf", zip, 64));
    CHECK(content_matches_extension("a.zip", zip, 64));
    CHECK_FALSE(content_matches_extension("a.3mf", "MZ\x90\x00", 4));
    CHECK_FALSE(content_matches_extension("a.3mf", "<html>", 6));

    std::string stl(84, '\0');
    stl[80] = 2; // two triangles
    CHECK(content_matches_extension("a.stl", stl, 84 + 2 * 50));
    CHECK_FALSE(content_matches_extension("a.stl", stl, 84 + 3 * 50));
    CHECK(content_matches_extension("a.stl", "solid cube\nfacet normal 0 0 1\n", 30));
    CHECK_FALSE(content_matches_extension("a.stl", "<!DOCTYPE html>", 15));

    CHECK(content_matches_extension("a.step", "ISO-10303-21;\nHEADER;", 20));
    CHECK(content_matches_extension("a.stp", "\xEF\xBB\xBFISO-10303-21;", 20));
    CHECK_FALSE(content_matches_extension("a.step", "MZ", 2));

    CHECK(content_matches_extension("a.obj", "v 0 0 0\nv 1 0 0\nf 1 2 3\n", 24));
    CHECK_FALSE(content_matches_extension("a.obj", std::string("MZ\x90\0", 4), 4));
    CHECK_FALSE(content_matches_extension("a.exe", zip, 64));
}

// ---- archive entry names ------------------------------------------------------------------------------

TEST_CASE("archive entry names may not leave the extraction folder", "[Untrusted][ZipSlip]")
{
    for (const char *p : {"readme.txt", "Other Files/readme.txt", "Metadata/plate_1.gcode", "a/b/c.png", "..a/b", "a..b"})
        CHECK(is_safe_archive_relative_path(p));
    for (const char *p : {"", "../x", "a/../../x", "..", ".", "./x", "a/./b", "/etc/passwd", "a//b", "a/", "..\\x",
                          "a\\b", "C:/x", "C:x", "a/.../b", "a/.. /b", "x\x01y", "foo\".png", "foo'.png", "a<b", "a>b"})
        CHECK_FALSE(is_safe_archive_relative_path(p));
}

// The Windows ANSI code page maps these look-alikes to '.', '/', '\\' and ':' when a UTF-8 name is
// narrowed (WideCharToMultiByte best-fit), so a name made of them is "../x" to a narrow fopen.
// The validator refuses them on every platform; ordinary non-ASCII names stay fine.
TEST_CASE("archive entry names refuse look-alikes of dot, slash, backslash and colon", "[Untrusted][ZipSlip]")
{
    // U+FF0E U+FF0E U+FF0F "evil.txt" (fullwidth "../evil.txt")
    CHECK_FALSE(is_safe_archive_relative_path("\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\x8F" "evil.txt"));
    CHECK_FALSE(is_safe_archive_relative_path("sub/\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\x8F" "evil.txt"));
    // U+FF0E U+FF0E U+FF3C (fullwidth reverse solidus)
    CHECK_FALSE(is_safe_archive_relative_path("\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\xBC" "evil.txt"));
    CHECK_FALSE(is_safe_archive_relative_path("C\xEF\xBC\x9A" "evil.txt"));    // U+FF1A fullwidth colon
    CHECK_FALSE(is_safe_archive_relative_path("a\xE2\x88\x95" "b"));            // U+2215 division slash
    CHECK_FALSE(is_safe_archive_relative_path("a\xE2\x81\x84" "b"));            // U+2044 fraction slash
    CHECK_FALSE(is_safe_archive_relative_path("a\xE2\x88\x96" "b"));            // U+2216 set minus
    CHECK_FALSE(is_safe_archive_relative_path("a\xE2\x80\xA4\xE2\x80\xA4" "b")); // U+2024 one dot leader
    CHECK_FALSE(is_safe_archive_relative_path("a\xEF\xB9\x92" "b"));            // U+FE52 small full stop

    CHECK(is_safe_archive_relative_path("caf\xC3\xA9/printer.json"));              // e acute
    CHECK(is_safe_archive_relative_path("\xE6\xB5\x8B\xE8\xAF\x95/\xE6\xB5\x8B.json")); // CJK
    CHECK(is_safe_archive_relative_path("a\xE2\x80\xA6" "b"));                  // U+2026 ellipsis is an ordinary character
}

TEST_CASE("archive entry names are normalised before they are judged", "[Untrusted][ZipSlip]")
{
    auto normalized = [](const std::string &raw) {
        std::string out;
        return normalize_archive_entry_path(raw, out) == ArchiveEntryName::Ok ? out : std::string("<not ok>");
    };
    // Harmless spellings from PowerShell 5.1 Compress-Archive / .NET zippers (backslashes) and bsdtar ("./").
    CHECK(normalized("a\\b.json") == "a/b.json");
    CHECK(normalized("a\\b\\c.json") == "a/b/c.json");
    CHECK(normalized("./a/b.json") == "a/b.json");
    CHECK(normalized(".\\a\\b.json") == "a/b.json");
    CHECK(normalized("a//b.json") == "a/b.json");
    CHECK(normalized("a/./b.json") == "a/b.json");
    CHECK(normalized("a\\\\b.json") == "a/b.json");
    CHECK(normalized("dir/") == "dir");
    CHECK(normalized("dir\\") == "dir");
    CHECK(normalized(".//a/") == "a");
    CHECK(normalized("..a/b..") == "..a/b..");
    CHECK(normalized("caf\xC3\xA9\\\xE6\xB5\x8B.json") == "caf\xC3\xA9/\xE6\xB5\x8B.json");

    // Nothing to extract.
    std::string out;
    for (const char *p : {"./", ".", ".\\", "./.", ".//"})
        CHECK(normalize_archive_entry_path(p, out) == ArchiveEntryName::Skip);

    // Everything the strict check refused is still refused, in every spelling.
    for (const char *p : {"", "..", "../x", "a/../x", "a/..", ".\\..\\x", "a\\..\\..\\x", "./../x", "a//../../x",
                          "/x", "//x", "\\x", "\\\\x", "\\\\server\\share\\x", "//server/share/x", "\\\\?\\C:\\x", "/C:/x",
                          "C:", "C:x", "C:/x", "C:\\x", ".\\C:\\x", "a/b:stream", "a\\b:stream",
                          "a/.../b", "a/.. /b", "a/. /b", "x\x01y", "x\ty",
                          "\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\x8F" "x", "a\\\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\xBC" "x"}) {
        INFO(p);
        CHECK(normalize_archive_entry_path(p, out) == ArchiveEntryName::Reject);
    }
    // Over-long names, before and after normalisation.
    CHECK(normalize_archive_entry_path(std::string(1025, 'a'), out) == ArchiveEntryName::Reject);
    CHECK(normalize_archive_entry_path("./" + std::string(1025, 'a'), out) == ArchiveEntryName::Reject);
    CHECK(normalize_archive_entry_path(std::string(5000, 'a'), out) == ArchiveEntryName::Reject);
    CHECK(normalize_archive_entry_path(std::string(1024, 'a'), out) == ArchiveEntryName::Ok);

    CHECK(archive_entry_leaf("a/b/c.json") == "c.json");
    CHECK(archive_entry_leaf("c.json") == "c.json");
}

// ---- settings in project / preset files ---------------------------------------------------------------

static DynamicPrintConfig config_with_post_process(const std::vector<std::string> &values)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("post_process", new ConfigOptionStrings(values));
    return cfg;
}

TEST_CASE("post_process is flagged unless the user already has the same script", "[Untrusted][Settings]")
{
    TrustedValues none;
    CHECK(find_untrusted_settings(config_with_post_process({}), "project settings", none).empty());
    CHECK(find_untrusted_settings(config_with_post_process({"", "  \r\n "}), "project settings", none).empty());

    const DynamicPrintConfig evil = config_with_post_process({"C:\\edge-test-does-not-exist\\marker.exe --flag"});
    auto found = find_untrusted_settings(evil, "project settings", none);
    REQUIRE(found.size() == 1);
    CHECK(found[0].key == "post_process");
    CHECK(found[0].risk == SettingRisk::RunsPrograms);
    CHECK(found[0].value == "C:\\edge-test-does-not-exist\\marker.exe --flag");

    // The user's own script (same lines, whitespace aside) is not flagged.
    TrustedValues mine;
    mine.post_process.insert(joined_post_process(config_with_post_process({"C:\\tools\\thumbs.exe  \r\n\r\nC:\\tools\\b.exe"})));
    CHECK(find_untrusted_settings(config_with_post_process({"  C:\\tools\\thumbs.exe", "C:\\tools\\b.exe "}), "x", mine).empty());
    CHECK(find_untrusted_settings(config_with_post_process({"C:\\tools\\thumbs.exe"}), "x", mine).size() == 1);
}

TEST_CASE("filename_format is flagged when it leaves the output folder", "[Untrusted][Settings]")
{
    CHECK_FALSE(filename_format_leaves_folder("{input_filename_base}_{filament_type[0]}_{print_time}.gcode"));
    CHECK_FALSE(filename_format_leaves_folder("{input_filename_base}_{layer_height/2}.gcode"));
    CHECK_FALSE(filename_format_leaves_folder("[input_filename_base]{if total_toolchanges>0}_mm{endif}.gcode"));
    CHECK_FALSE(filename_format_leaves_folder("{is_extruder_used[0] ? \"a\" : \"b\"}.gcode"));
    CHECK(filename_format_leaves_folder("..\\..\\{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("../{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("sub/{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("C:{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("{\"C:/x/\" + input_filename_base}.gcode"));

    DynamicPrintConfig cfg;
    cfg.set_key_value("filename_format", new ConfigOptionString("../../Startup/{input_filename_base}.gcode"));
    TrustedValues none;
    auto found = find_untrusted_settings(cfg, "p", none);
    REQUIRE(found.size() == 1);
    CHECK(found[0].risk == SettingRisk::WritesOutsideOutputFolder);
    TrustedValues mine;
    mine.filename_format.insert("../../Startup/{input_filename_base}.gcode");
    CHECK(find_untrusted_settings(cfg, "p", mine).empty());
}

TEST_CASE("print-host endpoints and network bed files are always flagged", "[Untrusted][Settings]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host", new ConfigOptionString("http://attacker.invalid"));
    cfg.set_key_value("print_host_webui", new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey", new ConfigOptionString("k"));
    cfg.set_key_value("bed_custom_texture", new ConfigOptionString("\\\\attacker.invalid\\share\\bed.png"));
    cfg.set_key_value("bed_custom_model", new ConfigOptionString("C:/Users/me/bed.stl"));
    auto found = find_untrusted_settings(cfg, "p", TrustedValues{});
    std::vector<std::string> keys;
    for (const auto &f : found) {
        CHECK(f.risk == SettingRisk::NetworkEndpoint);
        keys.push_back(f.key);
    }
    CHECK(keys == std::vector<std::string>{"print_host", "printhost_apikey", "bed_custom_texture"});
}

TEST_CASE("neutralize_settings puts back the baseline or the default", "[Untrusted][Settings]")
{
    DynamicPrintConfig cfg = config_with_post_process({"C:\\evil.exe"});
    cfg.set_key_value("filename_format", new ConfigOptionString("../x.gcode"));
    cfg.set_key_value("print_host", new ConfigOptionString("http://attacker.invalid"));
    DynamicPrintConfig baseline = config_with_post_process({"C:\\tools\\mine.exe"});
    baseline.set_key_value("print_host", new ConfigOptionString("http://my-printer.lan"));

    auto found = find_untrusted_settings(cfg, "p", TrustedValues{});
    REQUIRE(found.size() == 3);
    CHECK(neutralize_settings(cfg, found, &baseline) == 3);
    CHECK(joined_post_process(cfg) == "C:\\tools\\mine.exe");
    CHECK(cfg.opt_string("filename_format") == print_config_def.get("filename_format")->get_default_value<ConfigOptionString>()->value);
    CHECK(cfg.opt_string("print_host").empty()); // network keys never come from the baseline
    CHECK(find_untrusted_settings(cfg, "p", TrustedValues{}).empty() == false); // mine.exe is not in an empty trusted set...
    TrustedValues mine;
    mine.post_process.insert("C:\\tools\\mine.exe");
    CHECK(find_untrusted_settings(cfg, "p", mine).empty()); // ...but is the user's own

    DynamicPrintConfig cfg2 = config_with_post_process({"C:\\evil.exe"});
    auto found2 = find_untrusted_settings(cfg2, "p", TrustedValues{});
    neutralize_settings(cfg2, found2, nullptr);
    CHECK(joined_post_process(cfg2).empty());
}

// ---- a project 3MF that carries scripts ----------------------------------------------------------------

namespace {

struct Scratch
{
    fs::path root;
    Scratch()
    {
        root = fs::temp_directory_path() / ("snorca_untrusted_" + std::to_string(get_current_pid()));
        fs::create_directories(root);
        set_temporary_dir(root.string());
    }
    ~Scratch()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

const std::string FIXTURE = std::string(TEST_DATA_DIR) + "/untrusted_3mf/post_process_project.3mf";

struct Loaded
{
    Model                 model;
    DynamicPrintConfig    config;
    std::vector<Preset *> presets;
    ~Loaded()
    {
        for (Preset *p : presets)
            delete p;
    }
};

void load_fixture(Loaded &out)
{
    ConfigSubstitutionContext subs{ForwardCompatibilitySubstitutionRule::Enable};
    En3mfType                 type = En3mfType::From_BBS;
    PlateDataPtrs             plates;
    Semver                    version;
    out.model = Model::read_from_archive(FIXTURE, &out.config, &subs, type,
                                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::LoadAuxiliary |
                                             LoadStrategy::AddDefaultInstances,
                                         &plates, &out.presets, &version);
    release_PlateData_list(plates);
}

const Preset *find_embedded(const std::vector<Preset *> &presets, const std::string &name)
{
    for (const Preset *p : presets)
        if (p->name == name)
            return p;
    return nullptr;
}

} // namespace

TEST_CASE("a project 3MF can carry post-processing scripts, and the guard finds all of them", "[Untrusted][Project]")
{
    Scratch scratch;
    Loaded  loaded;
    load_fixture(loaded);

    // The project settings and the embedded print preset both carry a script; the embedded
    // printer preset points the printer tab at a foreign web UI.
    CHECK(joined_post_process(loaded.config) == "C:\\edge-test-does-not-exist\\edge_pp_marker_project.exe");
    const Preset *evil_process = find_embedded(loaded.presets, "Evil Process");
    const Preset *evil_printer = find_embedded(loaded.presets, "Evil Printer");
    REQUIRE(evil_process != nullptr);
    REQUIRE(evil_printer != nullptr);
    CHECK(joined_post_process(evil_process->config) == "C:\\edge-test-does-not-exist\\edge_pp_marker_embedded.exe");
    CHECK(evil_printer->config.opt_string("print_host_webui") == "http://attacker.invalid/ui");

    // What Plater's guard sees, with a user who has no scripts of their own.
    const TrustedValues none;
    auto project = find_untrusted_settings(loaded.config, "project settings", none);
    std::vector<std::string> keys;
    for (const auto &f : project)
        keys.push_back(f.key);
    CHECK(keys == std::vector<std::string>{"post_process", "filename_format", "print_host", "bed_custom_texture"});
    CHECK(find_untrusted_settings(evil_process->config, evil_process->name, none).size() == 1);
    CHECK(find_untrusted_settings(evil_printer->config, evil_printer->name, none).size() == 2);

    // Stripped, nothing is left to run.
    neutralize_settings(loaded.config, project, nullptr);
    CHECK(joined_post_process(loaded.config).empty());
    CHECK(find_untrusted_settings(loaded.config, "project settings", none).empty());
}

TEST_CASE("the 3MF reader keeps archive entries inside their folders", "[Untrusted][ZipSlip]")
{
    Scratch scratch;
    Loaded  loaded;
    load_fixture(loaded);

    const fs::path aux(loaded.model.get_auxiliary_file_temp_path());
    // The ordinary attachment is extracted...
    CHECK(fs::exists(aux / "Other Files" / "readme.txt"));
    // ...the entries that climb out ("Auxiliaries/../../x", "Metadata/../../x") are not.
    CHECK_FALSE(fs::exists((aux / ".." / ".." / "edge_zipslip_aux.txt").lexically_normal()));
    const fs::path backup(loaded.model.get_backup_path());
    CHECK_FALSE(fs::exists((backup / "Metadata" / ".." / ".." / "edge_zipslip_meta.gcode").lexically_normal()));
    // An entry whose own name is harmless ("Auxiliaries/benign.txt") but whose Unicode Path extra
    // field climbs out: the reader's older "/../" check only looked at the name.
    CHECK_FALSE(fs::exists((aux / ".." / ".." / "edge_zipslip_unicode.txt").lexically_normal()));
    CHECK_FALSE(fs::exists(aux / "benign.txt"));
    // Nothing named like the markers anywhere in the scratch tree.
    for (fs::recursive_directory_iterator it(scratch.root), end; it != end; ++it)
        CHECK(it->path().filename().string().find("edge_zipslip") == std::string::npos);
}

// ---- Preset import and "Import config from G-code" wiring --------------------------------------
//
// Owner decision (follow-up to #123): a preset JSON/bundle import and "Import config from G-code"
// must get the same "Untrusted settings in file" prompt as project loads, instead of silently
// removing scripts - reusing the guard's decision logic (UntrustedSettingsGuard.cpp installs
// PresetBundle::untrusted_config_filter, see GUI_App::init_app_config / GUI_App startup). What
// differs from a project load is: (1) both of these paths route through the SAME PresetBundle hook
// (PresetBundle.cpp: import_json_presets and load_config_file both call untrusted_config_filter
// before the value becomes a preset), and (2) they must NOT strip print-host / network settings
// (guard_untrusted_settings's strip_network=false for these two paths, since an imported preset or
// a G-code's config may legitimately be the user's own printer).
//
// The prompt-vs-silent branch itself (RemoteAccess::dialog_mode()) needs a live GUI_App / wx event
// loop and is not GUI-independent, so it is not covered here. What IS GUI-independent, and was not
// covered by any existing test, is: the PresetBundle plumbing actually invokes the filter for both
// import paths (nothing here bypasses UntrustedSettingsGuard), and the risk-based split the guard
// applies (RunsPrograms / WritesOutsideOutputFolder go on the "ask" list; NetworkEndpoint does not,
// so strip_network=false really does leave network settings alone).

namespace {

// A private data_dir(), never the user's real config, so PresetCollection::load_preset's disk
// writes land somewhere disposable. Mirrors test_preset_load_determinism.cpp's pattern.
struct ScratchDataDir
{
    fs::path    root;
    std::string saved;
    ScratchDataDir() : saved(data_dir())
    {
        root = fs::temp_directory_path() / fs::unique_path("orca_untrusted_import_%%%%%%%%");
        fs::create_directories(root);
        set_data_dir(root.string());
    }
    ~ScratchDataDir()
    {
        set_data_dir(saved);
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

fs::path write_temp_file(const std::string &name, const std::string &contents)
{
    const fs::path dir = fs::temp_directory_path() / "edgeslicer_untrusted_import_tests";
    fs::create_directories(dir);
    const fs::path path = dir / name;
    boost::nowide::ofstream ofs(path.string());
    ofs << contents;
    return path;
}

// load_from_gcode_file (Config.cpp) rejects a CONFIG_BLOCK with fewer than 80 key/value pairs;
// pad with a harmless repeated key the way test_gcode_import.cpp does.
std::string gcode_with_post_process(const std::string &script)
{
    std::ostringstream oss;
    oss << "; generated by " << SLIC3R_APP_NAME << " on 2026-01-01 at 00:00:00 UTC\n";
    oss << "; CONFIG_BLOCK_START\n";
    oss << "; post_process = " << script << "\n";
    for (int i = 0; i < 80; ++i)
        oss << "; layer_height = 0.2\n";
    oss << "; CONFIG_BLOCK_END\n";
    oss << "G28\n";
    return oss.str();
}

std::string json_escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}

std::string print_preset_json_with_post_process(const std::string &name, const std::string &script)
{
    // Minimal process-type preset: no "inherits", so import_json_presets falls back to
    // prints.default_preset_for(config), which a bare PresetBundle() already has.
    std::ostringstream oss;
    oss << "{\n"
        << "  \"type\": \"process\",\n"
        << "  \"name\": \"" << json_escape(name) << "\",\n"
        << "  \"from\": \"User\",\n"
        << "  \"version\": \"1.0.0.0\",\n"
        << "  \"print_settings_id\": \"" << json_escape(name) << "\",\n"
        << "  \"post_process\": [\"" << json_escape(script) << "\"]\n"
        << "}\n";
    return oss.str();
}

} // namespace

TEST_CASE("load_config_file (\"Import config from G-code\") routes the config through the untrusted filter", "[Untrusted][Import]")
{
    ScratchDataDir scratch;
    const fs::path path = write_temp_file("edge_untrusted_import.gcode",
                                          gcode_with_post_process("C:\\edge-test-does-not-exist\\edge_pp_marker_gcode.exe"));

    PresetBundle bundle;
    // load_config_file_config (called after the filter) splits the config across all three
    // collections and saves each as a preset; with m_dir_path empty (a bare PresetBundle()) that
    // write resolves relative to the process's CWD instead of this test's scratch dir. Redirect
    // all three the same way PresetBundle::update_user_presets_directory does at real startup.
    bundle.prints.update_user_presets_directory(scratch.root.string(), "print");
    bundle.filaments.update_user_presets_directory(scratch.root.string(), "filament");
    bundle.printers.update_user_presets_directory(scratch.root.string(), "printer");

    std::vector<std::pair<std::string, std::string>> filter_calls; // (source, post_process seen)
    bundle.untrusted_config_filter = [&filter_calls](const std::string &source, DynamicPrintConfig &config) {
        filter_calls.emplace_back(source, joined_post_process(config));
    };

    // Any exception past the filter call is not this test's concern - only that the filter
    // itself fired with the file's real content.
    try {
        bundle.load_config_file(path.string(), ForwardCompatibilitySubstitutionRule::EnableSilent);
    } catch (const std::exception &) {
    }

    REQUIRE(filter_calls.size() == 1);
    CHECK(filter_calls[0].first == path.string());
    CHECK(filter_calls[0].second == "C:\\edge-test-does-not-exist\\edge_pp_marker_gcode.exe");

    fs::remove(path);
}

TEST_CASE("preset JSON import routes the config through the untrusted filter", "[Untrusted][Import]")
{
    ScratchDataDir scratch;
    const std::string name = "Edge Untrusted Import Preset";
    const fs::path    path = write_temp_file("edge_untrusted_import.json",
                                          print_preset_json_with_post_process(name, "C:\\edge-test-does-not-exist\\edge_pp_marker_preset.exe"));

    PresetBundle bundle;
    // A bare PresetBundle() has prints.m_dir_path empty, so Preset::save (called after the filter,
    // once the value is a preset) would resolve its target path relative to the process's CWD
    // ("base/<name>.json") instead of anywhere under this test's scratch dir. Point it at the
    // scratch dir the same way PresetBundle::update_user_presets_directory does at real startup.
    bundle.prints.update_user_presets_directory(scratch.root.string(), "print");

    std::vector<std::pair<std::string, std::string>> filter_calls;
    bundle.untrusted_config_filter = [&filter_calls](const std::string &source, DynamicPrintConfig &config) {
        filter_calls.emplace_back(source, joined_post_process(config));
    };

    PresetsConfigSubstitutions substitutions;
    std::string                file = path.string();
    int                         overwrite = 0;
    std::vector<std::string>   result;
    bundle.import_json_presets(substitutions, file, [](const std::string &) { return 1; /* overwrite */ },
                               ForwardCompatibilitySubstitutionRule::EnableSilent, overwrite, result);

    REQUIRE(filter_calls.size() == 1);
    CHECK(filter_calls[0].first == name);
    CHECK(filter_calls[0].second == "C:\\edge-test-does-not-exist\\edge_pp_marker_preset.exe");

    fs::remove(path);
}

TEST_CASE("strip_network=false keeps print-host settings out of the ask list an import path would build", "[Untrusted][Import]")
{
    // What UntrustedSettingsGuard::guard_untrusted_settings does for strip_network=false (preset
    // imports and "Import config from G-code", per PR #123 and the follow-up): every finding is
    // still used to strip/neutralize the config, but only non-NetworkEndpoint findings go on the
    // "ask" list the dialog is built from. A project load (strip_network=true) has no such
    // exclusion and always drops network settings without asking.
    DynamicPrintConfig cfg = config_with_post_process({"C:\\edge-test-does-not-exist\\edge_pp_marker_mixed.exe"});
    cfg.set_key_value("print_host", new ConfigOptionString("http://attacker.invalid"));
    cfg.set_key_value("printhost_apikey", new ConfigOptionString("secret-key"));

    const TrustedValues none;
    auto found = find_untrusted_settings(cfg, "imported preset", none);
    REQUIRE(found.size() == 3);

    // The guard's own filter, reproduced here since it is a plain vector filter with no GUI
    // dependency: strip_network=false removes NetworkEndpoint entries from what gets asked about.
    std::vector<UntrustedSetting> ask;
    for (const UntrustedSetting &s : found)
        if (s.risk != SettingRisk::NetworkEndpoint)
            ask.push_back(s);
    REQUIRE(ask.size() == 1);
    CHECK(ask[0].key == "post_process");

    // Network settings are still real findings (so a project load with strip_network=true would
    // still remove them); an import path with strip_network=false only keeps them off the prompt,
    // it never asks the caller to neutralize them here. Confirms both keys were in "found".
    std::vector<std::string> network_keys;
    for (const UntrustedSetting &s : found)
        if (s.risk == SettingRisk::NetworkEndpoint)
            network_keys.push_back(s.key);
    CHECK(network_keys == std::vector<std::string>{"print_host", "printhost_apikey"});
}

// ---- OBJ texcoord hardening (Orca #15948, OBJ part; DRC N/A on Edge) ----------------------------
//
// Malformed or mixed-format OBJ texture coordinates used to OOB-read in load_obj, drop `vt u v w`
// lines (shifting every later vt index), and resolve negative vt indices with /3 instead of /2.

namespace {

struct ObjScratch
{
    fs::path dir;
    ObjScratch()
    {
        dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_obj_%%%%%%%%");
        fs::create_directories(dir);
    }
    ~ObjScratch()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path write(const std::string &name, const std::string &contents) const
    {
        const fs::path path = dir / name;
        boost::nowide::ofstream ofs(path.string());
        ofs << contents;
        ofs.close();
        return path;
    }
};

struct LoadedObj
{
    bool         ok{false};
    TriangleMesh mesh;
    ObjInfo      info;
    std::string  message;
};

// A tetrahedron with a material and two texture coordinates, (0.25, 0.5) and (0.75, 1).
// Only the first face and the vt lines are varied; the other three faces reference vt 1.
LoadedObj load_textured_tetrahedron(const std::string &first_face, const std::string &vts = "vt 0.25 0.5\nvt 0.75 1\n")
{
    ObjScratch     scratch;
    scratch.write("a.mtl", "newmtl a\nKd 1 0 0\n");
    std::string body = "mtllib a.mtl\n"
                       "v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\n";
    body += vts;
    body += "usemtl a\n";
    body += first_face;
    body += "\n";
    body += "f 1/1 2/1 4/1\nf 1/1 4/1 3/1\nf 2/1 3/1 4/1\n";
    const fs::path obj = scratch.write("mesh.obj", body);
    LoadedObj      loaded;
    loaded.ok = load_obj(obj.string().c_str(), &loaded.mesh, loaded.info, loaded.message);
    return loaded;
}

void check_uv(const Vec2f &uv, float x, float y)
{
    CHECK(uv.x() == Approx(x).margin(1e-6f));
    CHECK(uv.y() == Approx(y).margin(1e-6f));
}

} // namespace

TEST_CASE("A face with a vt index beyond the table loads, and the UV falls back to (0,0)", "[obj][untrusted]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1/1000000000 3/1 2/1");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    check_uv(uv[0], 0.f, 0.f);
    check_uv(uv[1], 0.25f, 0.5f);
}

TEST_CASE("A face without vt loads among faces that have them", "[obj][untrusted]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1 2 3");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    // One UV entry per face, so later faces keep their own coordinates.
    REQUIRE(loaded.info.uvs.size() == 4);
    for (const Vec2f &uv : loaded.info.uvs.front())
        check_uv(uv, 0.f, 0.f);
    check_uv(loaded.info.uvs[1][0], 0.25f, 0.5f);
}

TEST_CASE("A negative vt index counts back from the last texture coordinate (/2, not /3)", "[obj][untrusted]")
{
    // -1 is the most recent vt (0.5, 0.6), -2 the one before it (0.3, 0.4), -3 the first (0.1, 0.2).
    const LoadedObj loaded = load_textured_tetrahedron("f 1/-1 2/-2 3/-3", "vt 0.1 0.2\nvt 0.3 0.4\nvt 0.5 0.6\n");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    check_uv(uv[0], 0.5f, 0.6f);
    check_uv(uv[1], 0.3f, 0.4f);
    check_uv(uv[2], 0.1f, 0.2f);
}

TEST_CASE("vt u v w is kept, and a later face's vt index still points at the right entry", "[obj][untrusted]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1/1 3/2 2/2", "vt 0.5 0.25 0.0\nvt 0.75 1 0\n");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    check_uv(uv[0], 0.5f, 0.25f);
    check_uv(uv[1], 0.75f, 1.f);
}

TEST_CASE("Mixed vt u v and vt u v w lines keep the indices stable", "[obj][untrusted]")
{
    // The w on the first vt used to drop that line, so vt 2 resolved to the third coordinate.
    const LoadedObj loaded = load_textured_tetrahedron("f 1/2 3/3 2/-1", "vt 0.1 0.2 0\nvt 0.25 0.5\nvt 0.75 1\n");

    REQUIRE(loaded.ok);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    check_uv(uv[0], 0.25f, 0.5f);
    check_uv(uv[1], 0.75f, 1.f);
    check_uv(uv[2], 0.75f, 1.f);
}

// ---- 3MF XML entries larger than expat's int (Orca #15958) ------------------------------------
//
// The three XML_GetBuffer / XML_ParseBuffer sites (Prusa fingerprint probe, Prusa model-config
// extract, BBS XML extract) used to cast m_uncomp_size to int. An entry whose central directory
// claims more than INT_MAX bytes then allocated a truncated buffer and extracted the declared
// size into it. Fail the load instead of truncating.

TEST_CASE("xml_entry_size_ok rejects sizes that cannot be passed to expat as int", "[3mf][untrusted]")
{
    const std::uint64_t int_max = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    CHECK(xml_entry_size_ok(0));
    CHECK(xml_entry_size_ok(1));
    CHECK(xml_entry_size_ok(int_max));
    CHECK_FALSE(xml_entry_size_ok(int_max + 1));
    CHECK_FALSE(xml_entry_size_ok((std::uint64_t(1) << 32) + 16));
}

namespace {

// Writes a single-entry zip whose central directory carries a zip64 record declaring an
// uncompressed size beyond what the 32-bit expat buffer API can take. The deflated payload
// inflates to 64 bytes. Expat's internal buffer is at least ~2 KiB, so 64 bytes never
// overflows anything; a removed guard is caught by the "Found invalid size" log CHECK,
// not by a buffer overflow. Keep the payload under 1 KiB so a mutation fails cleanly
// (main's unguarded code only overflows above about 2 KiB). Built by hand because miniz
// never writes a size that disagrees with the data.
void write_zip_with_oversized_entry(const fs::path &path, const std::string &entry)
{
    const std::string xml          = std::string(64, 'x');
    const std::uint64_t claimed_size = (std::uint64_t(1) << 32) + 16;
    REQUIRE(xml.size() > static_cast<size_t>(static_cast<int>(claimed_size)));
    size_t            comp_len = 0;
    void             *comp     = tdefl_compress_mem_to_heap(xml.data(), xml.size(), &comp_len, TDEFL_DEFAULT_MAX_PROBES);
    REQUIRE(comp != nullptr);
    const std::string deflated(static_cast<const char *>(comp), comp_len);
    mz_free(comp);
    const std::uint32_t crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()));

    std::string out;
    auto        put = [&out](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    // local file header, with the true sizes
    put(0x04034b50, 4);
    put(45, 2);
    put(0, 2);
    put(8, 2);
    put(0, 2);
    put(0, 2);
    put(crc, 4);
    put(deflated.size(), 4);
    put(xml.size(), 4);
    put(entry.size(), 2);
    put(0, 2);
    out += entry + deflated;
    // central directory header, sizes deferred to the zip64 extra field
    const size_t cd_offset = out.size();
    put(0x02014b50, 4);
    put(45, 2);
    put(45, 2);
    put(0, 2);
    put(8, 2);
    put(0, 2);
    put(0, 2);
    put(crc, 4);
    put(0xFFFFFFFF, 4);
    put(0xFFFFFFFF, 4);
    put(entry.size(), 2);
    put(20, 2);
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(0, 4);
    put(0, 4);
    out += entry;
    put(0x0001, 2);
    put(16, 2);
    put(claimed_size, 8);
    put(deflated.size(), 8);
    const size_t cd_size = out.size() - cd_offset;
    // end of central directory
    put(0x06054b50, 4);
    put(0, 2);
    put(0, 2);
    put(1, 2);
    put(1, 2);
    put(cd_size, 4);
    put(cd_offset, 4);
    put(0, 2);

    boost::nowide::ofstream f(path.string(), std::ios::binary);
    REQUIRE(f.good());
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    REQUIRE(f.good());
}

bool zip_entry_claims_oversize(const fs::path &path, const std::string &entry)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, path.string()))
        return false;
    const int index = mz_zip_reader_locate_file(&archive, entry.c_str(), nullptr, 0);
    mz_zip_archive_file_stat stat;
    const bool ok = index >= 0 && mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &stat) &&
                    !xml_entry_size_ok(stat.m_uncomp_size);
    close_zip_reader(&archive);
    return ok;
}

// load_bbs_3mf / load_3mf / check_3mf_from_prusa only log "Found invalid size" from the INT_MAX
// guard. A 64-byte payload never overflows expat's ~2 KiB buffer, so without the guard the
// load may still fail for other reasons. The tests must assert this exact message or they
// cannot fail when the guard is removed.
struct InvalidSizeLog
{
    std::vector<std::string> lines;
    InvalidSizeLog()
    {
        // 4 == boost::log::trivial::error (see Utils.hpp set_log_observer).
        set_log_observer([this](int, const std::string &msg) { lines.push_back(msg); }, 4);
    }
    ~InvalidSizeLog() { set_log_observer({}, 0); }
    bool saw_invalid_size() const
    {
        for (const std::string &line : lines)
            if (line.find("Found invalid size") != std::string::npos)
                return true;
        return false;
    }
};

} // namespace

TEST_CASE("3MF XML entries declaring more than an int can hold fail to load", "[3mf][untrusted]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_3mf_intmax_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path path = dir / "oversized.3mf";

    SECTION("BBS importer")
    {
        write_zip_with_oversized_entry(path, "_rels/.rels");
        REQUIRE(zip_entry_claims_oversize(path, "_rels/.rels"));
        Model                      model;
        DynamicPrintConfig         config;
        ConfigSubstitutionContext  ctxt{ForwardCompatibilitySubstitutionRule::Enable};
        PlateDataPtrs              plates;
        std::vector<Preset *>      project_presets;
        bool                       is_bbl_3mf = false;
        Semver                     file_version;
        bool                       loaded     = true;
        InvalidSizeLog             log;
        REQUIRE_NOTHROW(loaded = load_bbs_3mf(path.string().c_str(), &config, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf,
                                             &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
        CHECK_FALSE(loaded);
        CHECK(log.saw_invalid_size());
        release_PlateData_list(plates);
    }
    SECTION("PrusaSlicer importer")
    {
        write_zip_with_oversized_entry(path, "Metadata/Slic3r_PE_model.config");
        REQUIRE(zip_entry_claims_oversize(path, "Metadata/Slic3r_PE_model.config"));
        Model                     model;
        DynamicPrintConfig        config;
        ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Disable};
        bool                      loaded = true;
        InvalidSizeLog            log;
        REQUIRE_NOTHROW(loaded = load_3mf(path.string().c_str(), config, ctxt, &model, false));
        CHECK_FALSE(loaded);
        CHECK(log.saw_invalid_size());
    }
    SECTION("PrusaSlicer fingerprint probe")
    {
        write_zip_with_oversized_entry(path, "3D/3dmodel.model");
        REQUIRE(zip_entry_claims_oversize(path, "3D/3dmodel.model"));
        InvalidSizeLog  log;
        PrusaFileParser parser;
        CHECK_FALSE(parser.check_3mf_from_prusa(path.string()));
        CHECK(log.saw_invalid_size());
    }

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- confined zip extraction (Orca #15957 / D3: reject the whole archive) -----------------------
//
// PresetUpdater / extract_archive_confined used to skip a bad entry and keep extracting. A
// hostile bundle with one traversal, absolute, drive-letter, backslash, or symlink entry is
// now refused as a whole (D3). install_plugin still flattens names to the basename and skips
// symlink entries (macOS dylib version links) instead of refusing the zip. A dest-file symlink
// is replaced rather than followed; a symlink-to-dir extraction root is followed. Pass-2
// stages to sibling .part files and only then renames, so a mid-extract I/O failure leaves
// pre-existing dest files untouched (content and presence).

namespace {

void write_zip_entries(const fs::path &zip_file, const std::vector<std::pair<std::string, std::string>> &entries)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, zip_file.string()));
    for (const auto &entry : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, entry.first.c_str(), entry.second.data(), entry.second.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    REQUIRE(close_zip_writer(&zip));
}

// miniz refuses a name starting with '/', so write a same-length placeholder and patch it in place.
void rename_zip_entry(const fs::path &zip_file, const std::string &from, const std::string &to)
{
    REQUIRE(from.size() == to.size());
    std::string bytes;
    {
        boost::nowide::ifstream in(zip_file.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    size_t count = 0;
    for (size_t pos = bytes.find(from); pos != std::string::npos; pos = bytes.find(from, pos + to.size()), ++count)
        bytes.replace(pos, from.size(), to);
    REQUIRE(count == 2); // local header + central directory
    boost::nowide::ofstream out(zip_file.string(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Mark the last central-directory entry as a Unix symlink (mode 0120777 in the high 16 bits).
void mark_last_zip_entry_symlink(const fs::path &zip_file)
{
    std::string bytes;
    {
        boost::nowide::ifstream in(zip_file.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const char   sig[] = {'P', 'K', 1, 2};
    size_t       last  = std::string::npos;
    for (size_t pos = 0; (pos = bytes.find(std::string(sig, 4), pos)) != std::string::npos; pos += 4)
        last = pos;
    REQUIRE(last != std::string::npos);
    REQUIRE(last + 42 <= bytes.size());
    const std::uint32_t attr = (0120777u << 16);
    bytes[last + 38]         = static_cast<char>(attr & 0xFF);
    bytes[last + 39]         = static_cast<char>((attr >> 8) & 0xFF);
    bytes[last + 40]         = static_cast<char>((attr >> 16) & 0xFF);
    bytes[last + 41]         = static_cast<char>((attr >> 24) & 0xFF);
    boost::nowide::ofstream out(zip_file.string(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_text_file(const fs::path &file)
{
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("is_path_within_root confines a candidate to the extraction root", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_within_root_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path root = dir / "root";
    fs::create_directories(root);

    CHECK(is_path_within_root(root, root / "a"));
    CHECK(is_path_within_root(root, root / "a" / "b.json"));
    CHECK(is_path_within_root(root, root));
    CHECK(is_path_within_root(fs::path(root.string() + "/"), root / "a"));
    CHECK(is_path_within_root(fs::path(root.string() + std::string(1, static_cast<char>(fs::path::preferred_separator))), root / "vendor.json"));

    CHECK_FALSE(is_path_within_root(root, root / ".." / "x"));
    CHECK_FALSE(is_path_within_root(root, dir / "root2" / "a"));
    CHECK_FALSE(is_path_within_root(root, dir / "x"));

    const std::string with_nul("evil\0.txt", 9);
    CHECK_FALSE(is_path_within_root(root, fs::path(with_nul)));
    const fs::path joined_nul = root / with_nul;
    if (joined_nul.string().find('\0') != std::string::npos || joined_nul.generic_string().find('\0') != std::string::npos)
        CHECK_FALSE(is_path_within_root(root, joined_nul));

#ifndef _WIN32
    {
        const fs::path real_root = dir / "real_root";
        const fs::path link_root = dir / "link_root";
        fs::create_directories(real_root);
        try {
            fs::create_symlink(real_root, link_root);
            CHECK(is_path_within_root(link_root, link_root / "a"));
            CHECK(is_path_within_root(link_root, real_root / "a"));
        } catch (const std::exception &) {}
    }
#endif

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("extract_archive_confined writes a well-formed nested archive under the target", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_ok_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path target   = dir / "cache";
    fs::create_directories(target);
    write_zip_entries(zip_file, {{"vendor/", ""},
                                 {"vendor/machine/", ""},
                                 {"vendor.json", "{\"a\":1}"},
                                 {"vendor/machine/printer.json", "{\"b\":2}"}});

    std::string err;
    REQUIRE(extract_archive_confined(zip_file, target, err));
    CHECK(err.empty());
    CHECK(fs::is_directory(target / "vendor"));
    CHECK(read_text_file(target / "vendor.json") == "{\"a\":1}");
    CHECK(read_text_file(target / "vendor" / "machine" / "printer.json") == "{\"b\":2}");

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

#ifndef _WIN32
TEST_CASE("extract_archive_confined into a symlink-to-dir root writes into the real target", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_rootlink_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path real_root = dir / "real";
    const fs::path link_root = dir / "link";
    const fs::path zip_file  = dir / "bundle.zip";
    fs::create_directories(real_root);
    try {
        fs::create_symlink(real_root, link_root);
    } catch (const std::exception &) {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
        return;
    }
    write_zip_entries(zip_file, {{"vendor.json", "{\"a\":1}"}, {"vendor/machine/printer.json", "{\"b\":2}"}});

    std::string err;
    REQUIRE(extract_archive_confined(zip_file, link_root, err));
    CHECK(err.empty());
    CHECK(read_text_file(real_root / "vendor.json") == "{\"a\":1}");
    CHECK(read_text_file(real_root / "vendor" / "machine" / "printer.json") == "{\"b\":2}");
    CHECK(read_text_file(link_root / "vendor.json") == "{\"a\":1}");

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("extract_archive_confined rejects an intermediate directory symlink that escapes", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_midlink_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path root    = dir / "root";
    const fs::path outside = dir / "outside";
    const fs::path zip_file = dir / "bundle.zip";
    fs::create_directories(root);
    fs::create_directories(outside);
    try {
        fs::create_symlink(outside, root / "vendor");
    } catch (const std::exception &) {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
        return;
    }
    write_zip_entries(zip_file, {{"vendor/a.json", "escaped"}});

    std::string err;
    CHECK_FALSE(extract_archive_confined(zip_file, root, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(fs::exists(outside / "a.json"));
    CHECK(fs::is_empty(outside));
    CHECK(fs::is_symlink(fs::symlink_status(root / "vendor")));

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

TEST_CASE("extract_archive_confined rejects a hostile archive and writes nothing", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_bad_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path target   = dir / "cache";
    fs::create_directories(target);

    const char *hostile[] = {"../evil.txt", "..\\evil.txt", "sub/../../evil.txt", "C:/evil.txt", "C:evil.txt", "\\evil.txt",
                             // drive letter with a backslash, UNC, a bare "..", a "..\\.." pair
                             "C:\\evil.txt", "\\\\server\\share\\evil.txt", "..", "sub/..", "..\\..\\evil.txt",
                             // spellings that are normalised (".\", "./", "a//b") must not hide a ".." segment
                             ".\\..\\evil.txt", "a\\..\\..\\evil.txt", "./../evil.txt", "a/./../../evil.txt", "a//../../evil.txt",
                             "\\\\?\\C:\\evil.txt", ".\\C:\\evil.txt", ".\\a\\b:stream",
                             // fullwidth ".." + "/" (U+FF0E U+FF0E U+FF0F): the ANSI best-fit mapping makes this "../"
                             "\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\x8F" "evil.txt",
                             "sub/\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\x8F\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\x8F" "evil.txt",
                             // fullwidth ".." + reverse solidus (U+FF3C), division slash (U+2215)
                             "\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\xBC" "evil.txt",
                             "\xEF\xBC\x8E\xEF\xBC\x8E\xE2\x88\x95" "evil.txt"};
    for (const char *name : hostile) {
        INFO(name);
        write_zip_entries(zip_file, {{"normal.json", "{}"}, {name, "escaped"}});
        std::string err;
        CHECK_FALSE(extract_archive_confined(zip_file, target, err));
        CHECK_FALSE(err.empty());
        CHECK_FALSE(fs::exists(dir / "evil.txt"));
        CHECK(fs::is_empty(target));
    }

    // Absolute POSIX path (miniz will not write a leading '/', so patch the name in place).
    {
        const std::string absolute    = (dir / "evil.txt").generic_string();
        const std::string placeholder = "#" + absolute.substr(1);
        write_zip_entries(zip_file, {{"normal.json", "{}"}, {placeholder, "escaped"}});
        rename_zip_entry(zip_file, placeholder, absolute);
        std::string err;
        CHECK_FALSE(extract_archive_confined(zip_file, target, err));
        CHECK_FALSE(fs::exists(dir / "evil.txt"));
        CHECK(fs::is_empty(target));
    }

    // Absolute paths that name another place: a UNC share spelled with slashes, and a drive-rooted path.
    for (const char *absolute : {"//server/share/evil.txt", "/C:/evil.txt", "/etc/evil.txt"}) {
        INFO(absolute);
        const std::string name        = absolute;
        const std::string placeholder = "#" + name.substr(1);
        write_zip_entries(zip_file, {{"normal.json", "{}"}, {placeholder, "escaped"}});
        rename_zip_entry(zip_file, placeholder, name);
        std::string err;
        CHECK_FALSE(extract_archive_confined(zip_file, target, err));
        CHECK(fs::is_empty(target));
    }

    // Directory entry that climbs out.
    {
        write_zip_entries(zip_file, {{"vendor/", ""}, {"../outside/", ""}});
        std::string err;
        CHECK_FALSE(extract_archive_confined(zip_file, target, err));
        CHECK_FALSE(fs::exists(dir / "outside"));
        CHECK(fs::is_empty(target));
    }

    // Symlink entry: a normal file first so skip-and-continue would have already written it.
    {
        write_zip_entries(zip_file, {{"normal.json", "{}"}, {"link", "../evil.txt"}});
        mark_last_zip_entry_symlink(zip_file);
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);
        REQUIRE(open_zip_reader(&archive, zip_file.string()));
        mz_zip_archive_file_stat st;
        REQUIRE(mz_zip_reader_file_stat(&archive, 1, &st));
        CHECK(zip_entry_is_symlink(st));
        close_zip_reader(&archive);

        std::string err;
        CHECK_FALSE(extract_archive_confined(zip_file, target, err));
        CHECK_FALSE(fs::exists(target / "normal.json"));
        CHECK(fs::is_empty(target));
        CHECK_FALSE(fs::exists(dir / "evil.txt"));
    }

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// A UTF-8 entry name is interpreted as UTF-8 and written through the wide API on Windows (no round
// trip through the ANSI code page). CJK characters are not in the en-US code page, so a narrow
// path could not even name this file. The content has '\n' and a NUL: the wide writer must open the
// file in binary mode.
TEST_CASE("extract_archive_confined writes non-ASCII entry names and binary content exactly", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_utf8_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path target   = dir / "cache";
    fs::create_directories(target);

    const std::string name_dir  = "caf\xC3\xA9";                             // "cafe" with an e acute
    const std::string name_file = "\xE6\xB5\x8B\xE8\xAF\x95.json";         // two CJK characters
    const std::string content("line1\nline2\r\nline3\n\0end\n", 24);
    REQUIRE(content.size() == 24);
    write_zip_entries(zip_file, {{name_dir + "/" + name_file, content}});

    std::string err;
    REQUIRE(extract_archive_confined(zip_file, target, err));
#ifdef _WIN32
    const fs::path expected = target / fs::path(boost::nowide::widen(name_dir)) / fs::path(boost::nowide::widen(name_file));
#else
    const fs::path expected = target / name_dir / name_file;
#endif
    REQUIRE(fs::exists(expected));
    boost::filesystem::ifstream in(expected, std::ios::binary);
    const std::string           got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(got == content);

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// Archives from PowerShell 5.1 Compress-Archive (backslashes), bsdtar ("./" prefixes and a "./" entry)
// and tools that write "a//b" are extracted, not refused.
TEST_CASE("extract_archive_confined normalises backslash, ./ and // spellings", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_norm_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    boost::system::error_code ec;

    SECTION("backslash-separated archive")
    {
        const fs::path target = dir / "cache";
        fs::create_directories(target);
        write_zip_entries(zip_file, {{"vendor\\", ""},
                                     {"vendor\\machine\\", ""},
                                     {"vendor.json", "{\"a\":1}"},
                                     {"vendor\\machine\\printer.json", "{\"b\":2}"}});
        std::string err;
        REQUIRE(extract_archive_confined(zip_file, target, err));
        CHECK(fs::is_directory(target / "vendor" / "machine"));
        CHECK(read_text_file(target / "vendor.json") == "{\"a\":1}");
        CHECK(read_text_file(target / "vendor" / "machine" / "printer.json") == "{\"b\":2}");
        // No file with a backslash in its name was created.
        for (fs::directory_iterator it(target), end; it != end; ++it)
            CHECK(it->path().filename().string().find('\\') == std::string::npos);
    }

    SECTION("./ prefixes and a bare ./ entry")
    {
        const fs::path target = dir / "cache";
        fs::create_directories(target);
        write_zip_entries(zip_file, {{"./", ""}, {"./a/", ""}, {"./a/b.json", "{\"b\":1}"}, {"./c.json", "{\"c\":2}"}});
        std::string err;
        REQUIRE(extract_archive_confined(zip_file, target, err));
        CHECK(read_text_file(target / "a" / "b.json") == "{\"b\":1}");
        CHECK(read_text_file(target / "c.json") == "{\"c\":2}");
    }

    SECTION("a//b and interior ./ segments")
    {
        const fs::path target = dir / "cache";
        fs::create_directories(target);
        write_zip_entries(zip_file, {{"a//b.json", "{\"b\":1}"}, {"x/./y//z.json", "{\"z\":3}"}});
        std::string err;
        REQUIRE(extract_archive_confined(zip_file, target, err));
        CHECK(read_text_file(target / "a" / "b.json") == "{\"b\":1}");
        CHECK(read_text_file(target / "x" / "y" / "z.json") == "{\"z\":3}");
    }

    SECTION("a bad entry among normalised ones still refuses the whole archive")
    {
        const fs::path target = dir / "cache";
        fs::create_directories(target);
        for (const char *bad : {".\\..\\x", "a\\..\\..\\x"}) {
            INFO(bad);
            write_zip_entries(zip_file, {{"./ok.json", "{}"}, {"sub\\fine.json", "{}"}, {bad, "escaped"}});
            std::string err;
            CHECK_FALSE(extract_archive_confined(zip_file, target, err));
            CHECK_FALSE(err.empty());
            CHECK(fs::is_empty(target));
            CHECK_FALSE(fs::exists(dir / "x"));
        }
    }

    SECTION("two entries that normalise to the same path: the last one in the archive wins")
    {
        const fs::path target = dir / "cache";
        fs::create_directories(target);
        write_zip_entries(zip_file, {{"a\\b.json", "first"}, {"a/b.json", "second"}, {"./a//b.json", "third"}});
        std::string err;
        REQUIRE(extract_archive_confined(zip_file, target, err));
        CHECK(read_text_file(target / "a" / "b.json") == "third");
        // Same names, other order: still the last one.
        write_zip_entries(zip_file, {{"./a//b.json", "third"}, {"a/b.json", "second"}, {"a\\b.json", "first"}});
        const fs::path target2 = dir / "cache2";
        fs::create_directories(target2);
        REQUIRE(extract_archive_confined(zip_file, target2, err));
        CHECK(read_text_file(target2 / "a" / "b.json") == "first");
        // No staging leftovers.
        for (const fs::path &t : {target, target2})
            for (fs::recursive_directory_iterator it(t), end; it != end; ++it)
                CHECK(it->path().filename().string().find(".part") == std::string::npos);
    }

    fs::remove_all(dir, ec);
}

TEST_CASE("extract_archive_confined pass-2 failure leaves pre-existing files intact", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_keep_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path cache    = dir / "cache";
    fs::create_directories(cache);
    {
        boost::nowide::ofstream keep((cache / "keep.json").string());
        keep << "OLD";
    }
    {
        boost::nowide::ofstream blocker((cache / "blocker").string());
        blocker << "block";
    }
    write_zip_entries(zip_file, {{"keep.json", "NEW"}, {"newdir/n.json", "n"}, {"blocker/x.json", "x"}});

    std::string err;
    CHECK_FALSE(extract_archive_confined(zip_file, cache, err));
    CHECK_FALSE(err.empty());
    CHECK(fs::exists(cache / "keep.json"));
    CHECK(read_text_file(cache / "keep.json") == "OLD");
    CHECK(fs::is_regular_file(cache / "blocker"));
    CHECK(read_text_file(cache / "blocker") == "block");
    CHECK_FALSE(fs::exists(cache / "blocker" / "x.json"));
    CHECK_FALSE(fs::is_directory(cache / "blocker"));
    CHECK_FALSE(fs::exists(cache / "newdir"));
    CHECK_FALSE(fs::exists(cache / "newdir" / "n.json"));

    for (fs::recursive_directory_iterator it(cache), end; it != end; ++it) {
        const std::string name = it->path().filename().string();
        CHECK(name.find(".part") == std::string::npos);
        CHECK(name.find(".bak-extract") == std::string::npos);
    }

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("extract_archive_confined keeps x.json.part and x.json as distinct files", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_partname_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path target   = dir / "cache";
    fs::create_directories(target);
    write_zip_entries(zip_file, {{"x.json.part", "PART"}, {"x.json", "JSON"}});

    std::string err;
    REQUIRE(extract_archive_confined(zip_file, target, err));
    CHECK(err.empty());
    CHECK(read_text_file(target / "x.json.part") == "PART");
    CHECK(read_text_file(target / "x.json") == "JSON");

    for (fs::directory_iterator it(target), end; it != end; ++it) {
        const std::string name = it->path().filename().string();
        if (name != "x.json" && name != "x.json.part")
            CHECK(name.find(".part") == std::string::npos);
    }

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("extract_archive_confined accepts a dest path with a trailing slash", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_slash_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path cache    = dir / "cache";
    fs::create_directories(cache);
    write_zip_entries(zip_file, {{"vendor/", ""},
                                 {"vendor/machine/", ""},
                                 {"vendor.json", "{\"a\":1}"},
                                 {"vendor/machine/printer.json", "{\"b\":2}"}});

    const fs::path dest_slash(cache.string() + "/");
    std::string    err;
    REQUIRE(extract_archive_confined(zip_file, dest_slash, err));
    CHECK(err.empty());
    CHECK(read_text_file(cache / "vendor.json") == "{\"a\":1}");
    CHECK(read_text_file(cache / "vendor" / "machine" / "printer.json") == "{\"b\":2}");

    const fs::path dest_pref(cache.string() + std::string(1, static_cast<char>(fs::path::preferred_separator)));
    write_zip_entries(zip_file, {{"extra.json", "e"}});
    REQUIRE(extract_archive_confined(zip_file, dest_pref, err));
    CHECK(read_text_file(cache / "extra.json") == "e");

    // lexically_normal("cache/./") leaves a trailing separator; strip it again so files
    // land in cache, not in a "." child. Same for "cache/." .
    const fs::path dest_dotslash(cache.string() + "/./");
    write_zip_entries(zip_file, {{"dotslash.json", "d"}});
    REQUIRE(extract_archive_confined(zip_file, dest_dotslash, err));
    CHECK(err.empty());
    CHECK(read_text_file(cache / "dotslash.json") == "d");

    const fs::path dest_dot = cache / ".";
    write_zip_entries(zip_file, {{"dot.json", "e2"}});
    REQUIRE(extract_archive_confined(zip_file, dest_dot, err));
    CHECK(read_text_file(cache / "dot.json") == "e2");

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

#ifndef _WIN32
TEST_CASE("extract_archive_confined does not write through a pre-existing .part symlink", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_partlink_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path target   = dir / "cache";
    const fs::path outside  = dir / "outside";
    fs::create_directories(target);
    fs::create_directories(outside);
    try {
        fs::create_symlink(outside / "x.json", target / "x.json.part");
    } catch (const std::exception &) {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
        return;
    }
    write_zip_entries(zip_file, {{"x.json", "payload"}});

    std::string err;
    REQUIRE(extract_archive_confined(zip_file, target, err));
    CHECK(read_text_file(target / "x.json") == "payload");
    CHECK_FALSE(fs::exists(outside / "x.json"));
    CHECK(fs::is_symlink(fs::symlink_status(target / "x.json.part")));

    for (fs::directory_iterator it(target), end; it != end; ++it) {
        const std::string name = it->path().filename().string();
        if (name != "x.json" && name != "x.json.part")
            CHECK(name.find(".part") == std::string::npos);
    }

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

#ifndef _WIN32
TEST_CASE("extract_archive_confined replaces a destination symlink instead of writing through it", "[Untrusted][ZipSlip]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_confined_link_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path zip_file = dir / "bundle.zip";
    const fs::path target   = dir / "cache";
    const fs::path outside  = dir / "outside";
    fs::create_directories(target);
    fs::create_directories(outside);
    write_zip_entries(zip_file, {{"vendor.json", "{\"a\":1}"}});

    SECTION("a dangling symlink")
    {
        try {
            fs::create_symlink(outside / "vendor.json", target / "vendor.json");
        } catch (const std::exception &) {
            // create_symlink can fail without privileges; the rest of the suite still covers reject-whole-archive.
            boost::system::error_code ec;
            fs::remove_all(dir, ec);
            return;
        }
        std::string err;
        CHECK(extract_archive_confined(zip_file, target, err));
        CHECK_FALSE(fs::exists(outside / "vendor.json"));
        CHECK_FALSE(fs::is_symlink(fs::symlink_status(target / "vendor.json")));
        CHECK(read_text_file(target / "vendor.json") == "{\"a\":1}");
    }
    SECTION("a symlink to an existing file")
    {
        {
            boost::nowide::ofstream out((outside / "vendor.json").string());
            out << "original";
        }
        try {
            fs::create_symlink(outside / "vendor.json", target / "vendor.json");
        } catch (const std::exception &) {
            boost::system::error_code ec;
            fs::remove_all(dir, ec);
            return;
        }
        std::string err;
        CHECK(extract_archive_confined(zip_file, target, err));
        CHECK(read_text_file(outside / "vendor.json") == "original");
        CHECK_FALSE(fs::is_symlink(fs::symlink_status(target / "vendor.json")));
        CHECK(read_text_file(target / "vendor.json") == "{\"a\":1}");
    }

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

