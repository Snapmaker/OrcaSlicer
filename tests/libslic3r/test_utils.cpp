#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"
#include <test_utils.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Slic3r;

TEST_CASE("A resolved input path still names the same file after the working directory changes", "[utils]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
                                        boost::filesystem::unique_path("cli_input_%%%%%%%%");
    boost::filesystem::create_directories(dir);
    const boost::filesystem::path model = dir / "model.3mf";
    {
        std::ofstream out(model.string());
        out << "3mf";
    }

    struct Cleanup
    {
        boost::filesystem::path path;
        ~Cleanup()
        {
            boost::system::error_code ec;
            boost::filesystem::remove_all(path, ec);
        }
    } cleanup{dir};

    // Resolve the bare name from the directory holding the file, then move away from it. The guard
    // restores the directory the test started in, wherever this leaves it.
    ScopedWorkingDirectory cwd(dir);
    const std::string      resolved = resolve_cli_input_path(model.filename().string());
    boost::filesystem::current_path(boost::filesystem::path(TEST_DATA_DIR));

    REQUIRE(boost::filesystem::exists(resolved));
    REQUIRE(boost::filesystem::equivalent(resolved, model));
    // Control: the bare name finds nothing from here, so resolving it this late would have failed.
    REQUIRE_FALSE(boost::filesystem::exists(model.filename().string()));
}

TEST_CASE("resolve_cli_input_path completes a relative path against the working directory", "[utils]")
{
    ScopedWorkingDirectory        cwd(boost::filesystem::temp_directory_path());
    // Read back rather than reusing temp_directory_path(): changing to it resolves any symlink.
    const boost::filesystem::path here = boost::filesystem::current_path();

    SECTION("a bare name") {
        REQUIRE(resolve_cli_input_path("model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ./ prefix is dropped") {
        REQUIRE(resolve_cli_input_path("./model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ../ traversal is collapsed") {
        REQUIRE(resolve_cli_input_path("../model.3mf") == (here.parent_path() / "model.3mf").make_preferred().string());
    }
}

TEST_CASE("resolve_cli_input_path leaves inputs that must not be completed unchanged", "[utils]")
{
    SECTION("an absolute path") {
        const boost::filesystem::path absolute = (boost::filesystem::temp_directory_path() / "model.3mf").make_preferred();
        REQUIRE(resolve_cli_input_path(absolute.string()) == absolute.string());
    }
#ifdef _WIN32
    // Every absolute form Windows accepts opens today, so each must come back byte for byte:
    // normalizing them would rewrite the forward slashes and rebuild the \\?\ and UNC prefixes.
    SECTION("an absolute Windows path of any form") {
        for (const std::string absolute : {R"(C:\models\model.3mf)",
                                           R"(C:/models/model.3mf)",
                                           R"(\\server\share\model.3mf)",
                                           R"(\\?\C:\models\model.3mf)"})
            REQUIRE(resolve_cli_input_path(absolute) == absolute);
    }
#endif
    // These are downloaded rather than opened, and completing one would produce a path, not a URL.
    // Edge registers edgeslicer:// and still accepts every older scheme this fork has shipped.
    SECTION("a custom open protocol URL") {
        for (const std::string url : {"edgeslicer://open/?file=https://example.com/model.3mf",
                                      "ultraone://open/?file=https://example.com/model.3mf",
                                      "Snapmaker_Orca://open/?file=https://example.com/model.3mf",
                                      "snapmaker-orca://open/?file=https://example.com/model.3mf",
                                      "orcaslicer://open/?file=https://example.com/model.3mf",
                                      "prusaslicer://open/?file=https://example.com/model.3mf",
                                      "bambustudio://open/?file=https://example.com/model.3mf",
                                      "cura://open/?file=https://example.com/model.3mf"})
            REQUIRE(resolve_cli_input_path(url) == url);
    }
    SECTION("an empty argument") { REQUIRE(resolve_cli_input_path("").empty()); }
}

namespace {

struct ScopedTempDir
{
    boost::filesystem::path path;
    ScopedTempDir()
    {
        path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("atomic_%%%%%%%%");
        boost::filesystem::create_directories(path);
    }
    ~ScopedTempDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

std::string slurp(const boost::filesystem::path &file)
{
    std::string content;
    load_string_file(file, content);
    return content;
}

} // namespace

TEST_CASE("write_file_atomically writes the full content and leaves no temporary", "[utils][atomic]")
{
    ScopedTempDir                     dir;
    const boost::filesystem::path     target = dir.path / "preset.json";
    const std::string                 body   = "{\n  \"name\": \"atomic\"\n}\n";

    std::string err;
    REQUIRE(write_file_atomically(target.string(), body, &err));
    REQUIRE(err.empty());
    REQUIRE(slurp(target) == body);

    size_t entries = 0;
    for (auto &entry : boost::filesystem::directory_iterator(dir.path)) {
        (void) entry;
        ++entries;
    }
    REQUIRE(entries == 1);
}

namespace {

void plant_atomic_temp_blockers(const std::string &target, int count)
{
    const std::string peek    = atomic_write_temp_path(target, false);
    const auto        tmp_pos = peek.rfind(".tmp");
    REQUIRE(tmp_pos != std::string::npos);
    const auto dot = peek.rfind('.', tmp_pos - 1);
    REQUIRE(dot != std::string::npos);
    const unsigned    n      = static_cast<unsigned>(std::stoul(peek.substr(dot + 1, tmp_pos - dot - 1)));
    const std::string prefix = peek.substr(0, dot + 1);
    for (int i = 0; i < count; ++i)
        boost::filesystem::create_directory(prefix + std::to_string(n + static_cast<unsigned>(i)) + ".tmp");
}

} // namespace

TEST_CASE("write_file_atomically leaves the original file intact when the write fails", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target   = dir.path / "preset.json";
    const std::string             original = "keep-me";
    REQUIRE(write_file_atomically(target.string(), original));

    // Occupy the whole EEXIST retry budget so the save cannot create a temp.
    plant_atomic_temp_blockers(target.string(), ATOMIC_WRITE_TEMP_ATTEMPTS);

    std::string err;
    REQUIRE_FALSE(write_file_atomically(target.string(), "replacement", &err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(slurp(target) == original);
}

TEST_CASE("atomic write temp names are unique per call and include the process id", "[utils][atomic]")
{
    const std::string a = atomic_write_temp_path("preset.json");
    const std::string b = atomic_write_temp_path("preset.json");
    REQUIRE(a != b);
    REQUIRE(a.find(std::to_string(get_current_pid())) != std::string::npos);
    REQUIRE(b.find(std::to_string(get_current_pid())) != std::string::npos);
    REQUIRE(a.find(".tmp") != std::string::npos);
    REQUIRE(b.find(".tmp") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("rename_file replaces an existing POSIX file and reports success", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path from = dir.path / "from.json";
    const boost::filesystem::path to   = dir.path / "to.json";
    {
        std::ofstream out_from(from.string());
        out_from << "new-bytes";
        std::ofstream out_to(to.string());
        out_to << "old-bytes";
    }

    const std::error_code ec = rename_file(from.string(), to.string());
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(boost::filesystem::exists(from));
    REQUIRE(slurp(to) == "new-bytes");
}

TEST_CASE("posix remove-then-rename fallback replaces after a refused first rename", "[utils][atomic]")
{
    REQUIRE(posix_rename_worth_retrying(EEXIST));
    REQUIRE(posix_rename_worth_retrying(EPERM));
    REQUIRE(posix_rename_worth_retrying(EBUSY));
    REQUIRE_FALSE(posix_rename_worth_retrying(ENOENT));
    REQUIRE_FALSE(posix_rename_worth_retrying(EXDEV));
    REQUIRE_FALSE(posix_rename_worth_retrying(ENOTDIR));
    REQUIRE_FALSE(posix_rename_worth_retrying(EISDIR));

    ScopedTempDir                 dir;
    const boost::filesystem::path from = dir.path / "from.json";
    const boost::filesystem::path to   = dir.path / "to.json";
    {
        std::ofstream out_from(from.string());
        out_from << "new-bytes";
        std::ofstream out_to(to.string());
        out_to << "old-bytes";
    }

    const std::error_code ec = posix_rename_retry_after_replace_refused(from.string(), to.string(), EEXIST);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(boost::filesystem::exists(from));
    REQUIRE(slurp(to) == "new-bytes");

    const boost::filesystem::path keep_from = dir.path / "keep_from.json";
    const boost::filesystem::path keep_to   = dir.path / "keep_to.json";
    {
        std::ofstream out_from(keep_from.string());
        out_from << "left";
        std::ofstream out_to(keep_to.string());
        out_to << "right";
    }
    const std::error_code skip = posix_rename_retry_after_replace_refused(keep_from.string(), keep_to.string(), ENOENT);
    REQUIRE(skip);
    REQUIRE(skip.value() == ENOENT);
    REQUIRE(slurp(keep_from) == "left");
    REQUIRE(slurp(keep_to) == "right");
}

namespace {

struct ScopedRenameHook
{
    explicit ScopedRenameHook(AtomicPosixRenameFn fn) { set_atomic_posix_rename_hook(fn); }
    ~ScopedRenameHook() { set_atomic_posix_rename_hook(nullptr); }
};

int refuse_existing_dest(const char *from, const char *to)
{
    boost::system::error_code bec;
    if (boost::filesystem::is_regular_file(to, bec)) {
        errno = EPERM;
        return -1;
    }
    return boost::nowide::rename(from, to);
}

int fail_tmp_renames(const char *from, const char *to)
{
    const std::string f(from);
    if (f.size() >= 4 && f.compare(f.size() - 4, 4, ".tmp") == 0) {
        errno = EPERM;
        return -1;
    }
    return boost::nowide::rename(from, to);
}

int fail_tmp_and_restore(const char *from, const char *to)
{
    const std::string f(from);
    const bool is_tmp = f.size() >= 4 && f.compare(f.size() - 4, 4, ".tmp") == 0;
    const bool is_bak = f.size() >= 11 && f.compare(f.size() - 11, 11, ".atomic.bak") == 0;
    if (is_tmp || is_bak) {
        errno = EPERM;
        return -1;
    }
    return boost::nowide::rename(from, to);
}

int fail_target_to_bak(const char *from, const char *to)
{
    const std::string t(to);
    if (t.size() >= 11 && t.compare(t.size() - 11, 11, ".atomic.bak") == 0) {
        errno = EACCES;
        return -1;
    }
    boost::system::error_code bec;
    if (boost::filesystem::is_regular_file(to, bec)) {
        errno = EPERM;
        return -1;
    }
    return boost::nowide::rename(from, to);
}

struct ScopedInspectHook
{
    explicit ScopedInspectHook(AtomicWriteTempInspectFn fn) { set_atomic_write_temp_inspect_hook(fn); }
    ~ScopedInspectHook() { set_atomic_write_temp_inspect_hook(nullptr); }
};

struct ScopedCreateHook
{
    explicit ScopedCreateHook(AtomicWriteTempInspectFn fn) { set_atomic_write_temp_create_hook(fn); }
    ~ScopedCreateHook() { set_atomic_write_temp_create_hook(nullptr); }
};

static mode_t s_inspected_tmp_mode  = 0;
static mode_t s_created_tmp_mode    = 0;

void inspect_tmp_mode(const char *, int fd)
{
    struct stat st;
    if (::fstat(fd, &st) == 0)
        s_inspected_tmp_mode = st.st_mode & 0777;
}

void inspect_create_mode(const char *, int fd)
{
    struct stat st;
    if (::fstat(fd, &st) == 0)
        s_created_tmp_mode = st.st_mode & 07777;
}

size_t count_atomic_baks(const boost::filesystem::path &dir)
{
    size_t n = 0;
    for (auto &entry : boost::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.size() >= 11 && name.compare(name.size() - 11, 11, ".atomic.bak") == 0)
            ++n;
    }
    return n;
}

bool dir_holds_payload(const boost::filesystem::path &dir, const std::string &payload)
{
    for (auto &entry : boost::filesystem::directory_iterator(dir)) {
        if (!boost::filesystem::is_regular_file(entry))
            continue;
        if (slurp(entry.path()) == payload)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("posix fallback keeps the target when the source is missing", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path from = dir.path / "missing.json";
    const boost::filesystem::path to   = dir.path / "to.json";
    {
        std::ofstream out_to(to.string());
        out_to << "old-bytes";
    }

    PosixRenameFallbackFate fate = PosixRenameFallbackFate::NotAttempted;
    const std::error_code   ec   = posix_rename_retry_after_replace_refused(from.string(), to.string(), EEXIST, &fate);
    REQUIRE(ec);
    REQUIRE(ec.value() == ENOENT);
    REQUIRE(fate == PosixRenameFallbackFate::TargetRestored);
    REQUIRE(boost::filesystem::exists(to));
    REQUIRE(slurp(to) == "old-bytes");
}

TEST_CASE("posix fallback keeps old or new contents when the second rename fails", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "preset.json";
    REQUIRE(write_file_atomically(target.string(), "old-bytes"));

    ScopedRenameHook hook(fail_tmp_renames);
    std::string      err;
    REQUIRE_FALSE(write_file_atomically(target.string(), "new-bytes", &err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(err.find("target intact") != std::string::npos);
    REQUIRE((dir_holds_payload(dir.path, "old-bytes") || dir_holds_payload(dir.path, "new-bytes")));
    REQUIRE(boost::filesystem::exists(target));
    REQUIRE(slurp(target) == "old-bytes");
}

TEST_CASE("rename_file and write_file_atomically succeed when replace is refused", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    ScopedRenameHook              hook(refuse_existing_dest);

    const boost::filesystem::path from = dir.path / "from.json";
    const boost::filesystem::path to   = dir.path / "to.json";
    {
        std::ofstream out_from(from.string());
        out_from << "new-bytes";
        std::ofstream out_to(to.string());
        out_to << "old-bytes";
    }
    const std::error_code ec = rename_file(from.string(), to.string());
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(boost::filesystem::exists(from));
    REQUIRE(slurp(to) == "new-bytes");

    const boost::filesystem::path target = dir.path / "preset.json";
    REQUIRE(write_file_atomically(target.string(), "old-bytes"));
    REQUIRE(write_file_atomically(target.string(), "new-bytes"));
    REQUIRE(slurp(target) == "new-bytes");
}

TEST_CASE("write_file_atomically fails a dangling symlink instead of replacing it", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path missing = dir.path / "gone.json";
    const boost::filesystem::path link    = dir.path / "link.json";
    boost::filesystem::create_symlink(missing, link);

    std::string err;
    REQUIRE_FALSE(write_file_atomically(link.string(), "payload", &err));
    REQUIRE(err.find("symlink") != std::string::npos);
    REQUIRE(boost::filesystem::is_symlink(link));
    REQUIRE_FALSE(boost::filesystem::exists(missing));
}

TEST_CASE("write_file_atomically through a symlink keeps the link and updates the target", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path real = dir.path / "real.json";
    const boost::filesystem::path link = dir.path / "link.json";
    REQUIRE(write_file_atomically(real.string(), "old"));
    boost::filesystem::create_symlink(real, link);

    REQUIRE(write_file_atomically(link.string(), "new-through-link"));
    REQUIRE(boost::filesystem::is_symlink(link));
    REQUIRE(slurp(real) == "new-through-link");
    REQUIRE(slurp(link) == "new-through-link");
}

TEST_CASE("write_file_atomically preserves a 0600 mode", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "secret.json";
    REQUIRE(write_file_atomically(target.string(), "first"));
    REQUIRE(::chmod(target.string().c_str(), S_IRUSR | S_IWUSR) == 0);

    REQUIRE(write_file_atomically(target.string(), "second"));
    struct stat st;
    REQUIRE(::stat(target.string().c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0600);
    REQUIRE(slurp(target) == "second");
}

TEST_CASE("posix fallback keeps temp and bak when both renames fail", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "preset.json";
    REQUIRE(write_file_atomically(target.string(), "old-bytes"));

    ScopedRenameHook hook(fail_tmp_and_restore);
    std::string      err;
    REQUIRE_FALSE(write_file_atomically(target.string(), "new-bytes", &err));
    REQUIRE(err.find("target removed") != std::string::npos);

    bool saw_tmp = false;
    bool saw_unique_bak = false;
    const std::string plain_bak = target.string() + ".atomic.bak";
    REQUIRE_FALSE(boost::filesystem::exists(plain_bak));
    for (auto &entry : boost::filesystem::directory_iterator(dir.path)) {
        const std::string name = entry.path().filename().string();
        if (!boost::filesystem::is_regular_file(entry))
            continue;
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            REQUIRE(slurp(entry.path()) == "new-bytes");
            saw_tmp = true;
        }
        if (name.size() >= 11 && name.compare(name.size() - 11, 11, ".atomic.bak") == 0) {
            REQUIRE(slurp(entry.path()) == "old-bytes");
            REQUIRE(name.find(std::to_string(get_current_pid())) != std::string::npos);
            REQUIRE(entry.path().string() != plain_bak);
            saw_unique_bak = true;
        }
    }
    REQUIRE(saw_tmp);
    REQUIRE(saw_unique_bak);
    REQUIRE_FALSE(boost::filesystem::exists(target));
}

TEST_CASE("posix fallback reports when the target cannot be moved aside", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "preset.json";
    REQUIRE(write_file_atomically(target.string(), "old-bytes"));

    ScopedRenameHook hook(fail_target_to_bak);
    std::string      err;
    REQUIRE_FALSE(write_file_atomically(target.string(), "new-bytes", &err));
    REQUIRE(err.find("cannot move target aside") != std::string::npos);
    REQUIRE(err.find("target intact") != std::string::npos);
    REQUIRE(boost::filesystem::exists(target));
    REQUIRE(slurp(target) == "old-bytes");
}

TEST_CASE("atomic write temp is 0600 while writing and a new file gets the umask mode", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "fresh.json";
    s_inspected_tmp_mode = 0;
    ScopedInspectHook             hook(inspect_tmp_mode);
    REQUIRE(write_file_atomically(target.string(), "payload"));
    REQUIRE(s_inspected_tmp_mode == 0600);

    const mode_t mask = ::umask(0);
    ::umask(mask);
    struct stat st;
    REQUIRE(::stat(target.string().c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == (0666 & ~mask));
    REQUIRE(slurp(target) == "payload");
}

TEST_CASE("write_file_atomically preserves 0644 and 0640 modes", "[utils][atomic]")
{
    ScopedTempDir dir;
    const struct {
        const char *name;
        mode_t      want;
    } cases[] = {{"mode-644.json", 0644}, {"mode-640.json", 0640}};
    for (const auto &c : cases) {
        const boost::filesystem::path target = dir.path / c.name;
        REQUIRE(write_file_atomically(target.string(), "first"));
        REQUIRE(::chmod(target.string().c_str(), c.want) == 0);
        REQUIRE(write_file_atomically(target.string(), "second"));
        struct stat st;
        REQUIRE(::stat(target.string().c_str(), &st) == 0);
        REQUIRE((st.st_mode & 0777) == c.want);
        REQUIRE(slurp(target) == "second");
    }
}

TEST_CASE("write_file_atomically keeps suid sgid and sticky bits", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "special.json";
    REQUIRE(write_file_atomically(target.string(), "first"));
    const mode_t want = 04644; // suid + 0644
    REQUIRE(::chmod(target.string().c_str(), want) == 0);
    REQUIRE(write_file_atomically(target.string(), "second"));
    struct stat st;
    REQUIRE(::stat(target.string().c_str(), &st) == 0);
    REQUIRE((st.st_mode & 07777) == want);
    REQUIRE(slurp(target) == "second");
}

TEST_CASE("an existing target's temp is created 0600 before fchmod", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "secret.json";
    REQUIRE(write_file_atomically(target.string(), "first"));
    REQUIRE(::chmod(target.string().c_str(), 0644) == 0);

    s_created_tmp_mode = 0;
    ScopedCreateHook              hook(inspect_create_mode);
    REQUIRE(write_file_atomically(target.string(), "second"));
    REQUIRE((s_created_tmp_mode & 0777) == 0600);
    REQUIRE(slurp(target) == "second");
}

TEST_CASE("a successful fallback save leaves no atomic.bak", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    ScopedRenameHook              hook(refuse_existing_dest);
    const boost::filesystem::path target = dir.path / "preset.json";
    REQUIRE(write_file_atomically(target.string(), "old-bytes"));
    REQUIRE(write_file_atomically(target.string(), "new-bytes"));
    REQUIRE(slurp(target) == "new-bytes");
    REQUIRE(count_atomic_baks(dir.path) == 0);
}

TEST_CASE("write_file_atomically never clears the process umask", "[utils][atomic]")
{
#ifdef __linux__
    ScopedTempDir dir;
    struct ScopedUmask
    {
        const mode_t prev;
        explicit ScopedUmask(mode_t mask) : prev(::umask(mask)) {}
        ~ScopedUmask() { ::umask(prev); }
    } umask_022{0022};

    constexpr int     kWrites = 256;
    std::atomic<bool> writing{true};
    std::atomic<int>  samples{0};
    std::atomic<int>  saw_zero{0};

    std::thread checker([&] {
        const int fd = ::open("/proc/self/status", O_RDONLY);
        if (fd < 0)
            return;
        char buf[8192];
        while (writing.load(std::memory_order_relaxed)) {
            if (::lseek(fd, 0, SEEK_SET) < 0)
                break;
            const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            if (n <= 0)
                continue;
            buf[n] = '\0';
            const char *p = std::strstr(buf, "Umask:");
            if (p != nullptr) {
                p += 6;
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (p[0] == '0' && p[1] == '0' && p[2] == '0' && p[3] == '0')
                    saw_zero.fetch_add(1, std::memory_order_relaxed);
            }
            samples.fetch_add(1, std::memory_order_relaxed);
        }
        ::close(fd);
    });

    for (int i = 0; i < kWrites; ++i) {
        const boost::filesystem::path target = dir.path / ("umask-" + std::to_string(i) + ".json");
        REQUIRE(write_file_atomically(target.string(), "x"));
    }
    writing.store(false, std::memory_order_relaxed);
    checker.join();

    REQUIRE(samples.load() > 0);
    REQUIRE(saw_zero.load() == 0);
#else
    SUCCEED("Umask: is only on /proc/self/status");
#endif
}

TEST_CASE("a leftover temp name is retried and left untouched", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "preset.json";
    REQUIRE(write_file_atomically(target.string(), "old-bytes"));

    const boost::filesystem::path leftover = atomic_write_temp_path(target.string(), /*consume=*/false);
    {
        std::ofstream out(leftover.string());
        out << "planted-leftover";
    }

    REQUIRE(write_file_atomically(target.string(), "new-bytes"));
    REQUIRE(slurp(target) == "new-bytes");
    REQUIRE(boost::filesystem::exists(leftover));
    REQUIRE(slurp(leftover) == "planted-leftover");
}
#endif

TEST_CASE("write_file_atomically removes the temporary when rename fails", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target = dir.path / "preset.json";
    boost::filesystem::create_directory(target);

    std::string err;
    REQUIRE_FALSE(write_file_atomically(target.string(), "replacement", &err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(boost::filesystem::is_directory(target));

    size_t tmp_count = 0;
    for (auto &entry : boost::filesystem::directory_iterator(dir.path)) {
        if (entry.path().extension() == ".tmp")
            ++tmp_count;
    }
    REQUIRE(tmp_count == 0);
}

TEST_CASE("a failed binary atomic write keeps the previous cache", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target   = dir.path / "vendor.cbor";
    const std::string             original = std::string("\x00\x01\x02""keep-cbor", 12);
    REQUIRE(write_file_atomically(target.string(), original, nullptr, true));

    plant_atomic_temp_blockers(target.string(), ATOMIC_WRITE_TEMP_ATTEMPTS);

    std::string err;
    REQUIRE_FALSE(write_file_atomically(target.string(), "replacement-cbor", &err, true));
    REQUIRE_FALSE(err.empty());
    REQUIRE(slurp(target) == original);
}

TEST_CASE("AppConfig save round-trips through the atomic helper", "[utils][atomic][AppConfig]")
{
    ScopedTempDir dir;
    struct ScopedDataDir
    {
        std::string prev;
        explicit ScopedDataDir(const std::string &next) : prev(data_dir()) { set_data_dir(next); }
        ~ScopedDataDir() { set_data_dir(prev); }
    } data{dir.path.string()};
    save_main_thread_id();

    AppConfig writer;
    writer.set("atomic_roundtrip_key", "atomic-value");
    writer.save();
    REQUIRE_FALSE(writer.dirty());
    REQUIRE(boost::filesystem::exists(writer.config_path()));
#ifdef WIN32
    REQUIRE(boost::filesystem::exists(writer.config_path() + ".bak"));
#endif

    AppConfig reader;
    const std::string load_err = reader.load();
    REQUIRE(load_err.empty());
    REQUIRE(reader.get("atomic_roundtrip_key") == "atomic-value");
}
