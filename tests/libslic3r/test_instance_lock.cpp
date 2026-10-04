#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/InstanceLock.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"

#ifdef _WIN32
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

// Port of upstream Orca #15861's InstanceLock tests (Catch2 v2), plus the
// Edge rule that matters most for the hub and multi-window design: a save
// goes ahead when another instance holds the lock.

using namespace Slic3r;
using namespace std::chrono_literals;

namespace {

struct LockTempDir
{
    boost::filesystem::path path;
    LockTempDir()
    {
        path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("instance_lock_%%%%%%%%");
        boost::filesystem::create_directories(path);
    }
    ~LockTempDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

// Sets a process-wide knob for one test and restores it however the test ends.
template<typename T> struct ScopedStaticValue
{
    T &ref;
    T  saved;
    ScopedStaticValue(T &ref, T value) : ref(ref), saved(ref) { ref = value; }
    ~ScopedStaticValue() { ref = saved; }
};

struct ScopedDataDir
{
    std::string prev;
    explicit ScopedDataDir(const std::string &next) : prev(data_dir()) { set_data_dir(next); }
    ~ScopedDataDir() { set_data_dir(prev); }
};

// Holds the OS lock on a file through a handle of its own, as another instance
// would. The lock belongs to the handle on Windows and to the open file
// description elsewhere, so the guard's own handle is refused while this one
// holds it.
class OtherHolder
{
public:
    explicit OtherHolder(const std::string &path)
    {
#ifdef _WIN32
        boost::nowide::ofstream(path, std::ios::app).close();
        m_lock = boost::interprocess::file_lock(boost::nowide::widen(path).c_str());
        m_held = m_lock.try_lock();
#else
        m_fd   = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        m_held = m_fd >= 0 && ::flock(m_fd, LOCK_EX | LOCK_NB) == 0;
#endif
    }
    ~OtherHolder() { release(); }
    OtherHolder(const OtherHolder &)            = delete;
    OtherHolder &operator=(const OtherHolder &) = delete;

    bool held() const { return m_held; }
    void release()
    {
#ifdef _WIN32
        if (m_held)
            m_lock.unlock();
        m_lock = boost::interprocess::file_lock();
#else
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = -1;
#endif
        m_held = false;
    }

private:
#ifdef _WIN32
    boost::interprocess::file_lock m_lock;
#else
    int m_fd{-1};
#endif
    bool m_held{false};
};

std::string slurp_file(const boost::filesystem::path &file)
{
    std::string content;
    load_string_file(file, content);
    return content;
}

} // namespace

TEST_CASE("InstanceLock creates its lock file and holds it for the guard's scope", "[InstanceLock]")
{
    LockTempDir       dir;
    const std::string path = (dir.path / "shared.lock").string();
    {
        InstanceLock lock(path);
        REQUIRE(lock.locked());
        REQUIRE(boost::filesystem::exists(path));
    }
    // Released: a fresh guard gets the lock at once instead of waiting out a timeout.
    const auto   started = std::chrono::steady_clock::now();
    InstanceLock again(path, 5000ms);
    REQUIRE(again.locked());
    REQUIRE(std::chrono::steady_clock::now() - started < 4000ms);
}

TEST_CASE("InstanceLock nests within one thread", "[InstanceLock]")
{
    LockTempDir       dir;
    const std::string path = (dir.path / "shared.lock").string();
    InstanceLock      outer(path);
    {
        InstanceLock inner(path, 100ms);
        REQUIRE(inner.locked());
    }
    // The inner guard leaving does not release the outer one.
    REQUIRE(outer.locked());
}

TEST_CASE("InstanceLock is a no-op for an empty path and survives an unwritable one", "[InstanceLock]")
{
    LockTempDir  dir;
    InstanceLock none("");
    REQUIRE_FALSE(none.locked());
    // The directory does not exist, so the lock file cannot be created; the
    // guard still constructs and the write it guards can go ahead.
    InstanceLock unwritable((dir.path / "missing" / "shared.lock").string(), 100ms);
    REQUIRE_FALSE(unwritable.locked());
}

TEST_CASE("InstanceLock serialises the threads of one process", "[InstanceLock]")
{
    LockTempDir       dir;
    const std::string path = (dir.path / "shared.lock").string();

    std::atomic<bool> holder_ready{false};
    std::atomic<bool> holder_released{false};
    std::thread       holder([&] {
        InstanceLock lock(path);
        holder_ready = true;
        std::this_thread::sleep_for(150ms);
        holder_released = true;
    });
    while (!holder_ready)
        std::this_thread::yield();

    bool released_before_acquire = false;
    {
        InstanceLock lock(path);
        released_before_acquire = holder_released;
    }
    holder.join();
    REQUIRE(released_before_acquire);
}

TEST_CASE("InstanceLock gives up on a lock another instance holds, then retries after the cool-down", "[InstanceLock]")
{
    LockTempDir       dir;
    const std::string path = (dir.path / "shared.lock").string();
    ScopedStaticValue<std::chrono::milliseconds> cooldown(InstanceLock::cooldown, 300ms);

    OtherHolder other(path);
    REQUIRE(other.held());

    const auto started = std::chrono::steady_clock::now();
    bool       locked_while_other_holds;
    {
        InstanceLock lock(path, 100ms);
        locked_while_other_holds = lock.locked();
    }
    const auto first_wait = std::chrono::steady_clock::now() - started;
    // The timed-out wait starts a cool-down: the next guard does not touch the file.
    const auto started2 = std::chrono::steady_clock::now();
    bool       locked_during_cooldown;
    {
        InstanceLock lock(path, 5000ms);
        locked_during_cooldown = lock.locked();
    }
    const auto cooldown_wait = std::chrono::steady_clock::now() - started2;
    other.release();

    REQUIRE_FALSE(locked_while_other_holds);
    REQUIRE(first_wait < 4000ms);
    REQUIRE_FALSE(locked_during_cooldown);
    REQUIRE(cooldown_wait < 4000ms);
    std::this_thread::sleep_for(400ms);
    InstanceLock lock(path);
    REQUIRE(lock.locked());
}

TEST_CASE("AppConfig::save goes ahead while another instance holds the config lock", "[InstanceLock][AppConfig]")
{
    LockTempDir   dir;
    ScopedDataDir data{dir.path.string()};
    save_main_thread_id();
    ScopedStaticValue<std::chrono::milliseconds> cooldown(InstanceLock::cooldown, 100ms);

    AppConfig writer;
    REQUIRE_FALSE(writer.lock_path().empty());
    OtherHolder other(writer.lock_path());
    REQUIRE(other.held());

    writer.set("instance_lock_key", "written-anyway");
    const auto started = std::chrono::steady_clock::now();
    writer.save();
    const auto waited = std::chrono::steady_clock::now() - started;
    other.release();

    // Never refused: the write landed after at most the lock timeout.
    REQUIRE_FALSE(writer.dirty());
    REQUIRE(waited < InstanceLock::default_timeout + 3000ms);
    AppConfig reader;
    REQUIRE(reader.load().empty());
    REQUIRE(reader.get("instance_lock_key") == "written-anyway");
}

TEST_CASE("Preset and physical printer saves go ahead while another instance holds the preset lock", "[InstanceLock][Preset]")
{
    LockTempDir   dir;
    ScopedDataDir data{dir.path.string()};
    ScopedStaticValue<std::chrono::milliseconds> cooldown(InstanceLock::cooldown, 100ms);

    const std::string lock_path = user_presets_lock_path();
    REQUIRE(lock_path == (dir.path / "user.lock").string());
    REQUIRE(user_presets_lock_path(/*read_only=*/true).empty());
    OtherHolder other(lock_path);
    REQUIRE(other.held());

    Preset preset(Preset::TYPE_PRINT, "Locked out");
    preset.file = (dir.path / "user" / "default" / "process" / "Locked out.json").string();
    preset.config.set_key_value("layer_height", new ConfigOptionFloat(0.15));
    REQUIRE(preset.save(nullptr));
    REQUIRE(boost::filesystem::exists(preset.file));
    REQUIRE(boost::filesystem::exists(boost::filesystem::path(preset.file).replace_extension(".info")));
    REQUIRE(slurp_file(preset.file).find("\"layer_height\"") != std::string::npos);

    DynamicPrintConfig printer_config;
    printer_config.set_key_value("preset_names", new ConfigOptionStrings());
    PhysicalPrinter    printer("Locked out printer", printer_config);
    printer.file = (dir.path / "printer.json").string();
    printer.save(nullptr);
    REQUIRE(boost::filesystem::exists(printer.file));

    preset.remove_files();
    REQUIRE_FALSE(boost::filesystem::exists(preset.file));
    other.release();
}
