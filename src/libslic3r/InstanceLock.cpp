#include "InstanceLock.hpp"

#include "Utils.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Slic3r {

#ifdef _WIN32
struct InstanceLock::Native
{
    HANDLE handle{INVALID_HANDLE_VALUE};
    bool   locked{false};

    explicit Native(const std::string &path)
    {
        const std::wstring wide = boost::nowide::widen(path);
        // No sharing: another instance's CreateFileW fails immediately.
        handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
                return;
            throw std::system_error(static_cast<int>(err), std::system_category(), path);
        }
    }

    ~Native()
    {
        if (locked)
            unlock();
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }

    bool try_lock()
    {
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        OVERLAPPED ov{};
        if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
            locked = true;
            return true;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING || err == ERROR_SHARING_VIOLATION)
            return false;
        throw std::system_error(static_cast<int>(err), std::system_category(), "LockFileEx");
    }

    void unlock()
    {
        if (!locked || handle == INVALID_HANDLE_VALUE)
            return;
        OVERLAPPED ov{};
        UnlockFileEx(handle, 0, 1, 0, &ov);
        locked = false;
    }
};
#else
// flock belongs to this open file description, so another open/close of the
// lock file in this process (a backup walking the data dir) cannot drop it.
struct InstanceLock::Native
{
    int  fd{-1};
    bool locked{false};

    explicit Native(const std::string &path)
    {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), path);
    }

    ~Native()
    {
        if (locked)
            unlock();
        if (fd >= 0)
            ::close(fd);
    }

    bool try_lock()
    {
        if (fd < 0)
            return false;
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            locked = true;
            return true;
        }
        if (errno == EWOULDBLOCK || errno == EINTR)
            return false;
        throw std::system_error(errno, std::generic_category(), "flock");
    }

    void unlock()
    {
        if (!locked || fd < 0)
            return;
        ::flock(fd, LOCK_UN);
        locked = false;
    }
};
#endif

namespace {

enum class SessionState { NotAttempted, Held, ReadOnly };

SessionState                 s_session_state{SessionState::NotAttempted};
std::unique_ptr<InstanceLock> s_session_lock;
std::string                  s_session_error;

void set_session_error(const std::string &msg)
{
    s_session_error = msg;
    BOOST_LOG_TRIVIAL(warning) << msg;
}

} // namespace

InstanceLock::InstanceLock(const std::string &lock_file_path, std::chrono::milliseconds timeout)
    : m_path(lock_file_path)
{
    if (m_path.empty())
        return;
    try_lock(timeout);
}

InstanceLock::~InstanceLock() { unlock(); }

InstanceLock::InstanceLock(InstanceLock &&other) noexcept
    : m_path(std::move(other.m_path)), m_native(std::move(other.m_native)), m_locked(other.m_locked)
{
    other.m_locked = false;
}

InstanceLock &InstanceLock::operator=(InstanceLock &&other) noexcept
{
    if (this != &other) {
        unlock();
        m_path         = std::move(other.m_path);
        m_native       = std::move(other.m_native);
        m_locked       = other.m_locked;
        other.m_locked = false;
    }
    return *this;
}

bool InstanceLock::try_lock(std::chrono::milliseconds timeout)
{
    if (m_path.empty())
        return false;
    try {
        const boost::filesystem::path parent = boost::filesystem::path(m_path).parent_path();
        if (!parent.empty())
            boost::filesystem::create_directories(parent);
        m_native = std::make_unique<Native>(m_path);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (m_native->try_lock()) {
                m_locked = true;
                return true;
            }
            if (timeout.count() <= 0 || std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        m_native.reset();
        return false;
    } catch (const std::exception &e) {
        m_native.reset();
        BOOST_LOG_TRIVIAL(warning) << "InstanceLock: cannot lock " << m_path << ": " << e.what();
        return false;
    }
}

void InstanceLock::unlock()
{
    if (m_native) {
        try {
            m_native->unlock();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "InstanceLock: unlock failed: " << e.what();
        }
        m_native.reset();
    }
    m_locked = false;
}

std::string InstanceLock::lock_path_for_data_dir(const std::string &data_dir)
{
    return (boost::filesystem::path(data_dir) / "instance.lock").make_preferred().string();
}

bool InstanceLock::try_acquire_data_dir(const std::string &data_dir, bool claim, std::chrono::milliseconds timeout)
{
    s_session_error.clear();
    if (!claim || data_dir.empty()) {
        s_session_state = SessionState::NotAttempted;
        s_session_lock.reset();
        return false;
    }
    s_session_lock = std::make_unique<InstanceLock>(lock_path_for_data_dir(data_dir), timeout);
    if (s_session_lock->locked()) {
        s_session_state = SessionState::Held;
        BOOST_LOG_TRIVIAL(info) << "InstanceLock: acquired " << s_session_lock->m_path;
        return true;
    }
    s_session_state = SessionState::ReadOnly;
    set_session_error("Another EdgeSlicer instance is using this data directory; "
                      "this instance will not save config, presets or printers.");
    s_session_lock.reset();
    return false;
}

void InstanceLock::release_data_dir()
{
    s_session_lock.reset();
    s_session_state = SessionState::NotAttempted;
    s_session_error.clear();
}

bool InstanceLock::holds_data_dir() { return s_session_state == SessionState::Held && s_session_lock && s_session_lock->locked(); }

bool InstanceLock::allows_saves() { return s_session_state != SessionState::ReadOnly; }

bool InstanceLock::is_read_only() { return s_session_state == SessionState::ReadOnly; }

const std::string &InstanceLock::last_error() { return s_session_error; }

namespace {

bool all_digits(const std::string &s)
{
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

// Split `name` on '.' from the right. A Stage A temp is
// `<stem>.<pid>.<launch_ns>.<launch_rnd>.<n>.tmp`.
bool parse_atomic_temp(const std::string &filename, unsigned *pid_out)
{
    static constexpr const char kExt[] = ".tmp";
    if (filename.size() <= 4 || filename.compare(filename.size() - 4, 4, kExt) != 0)
        return false;
    const std::string body = filename.substr(0, filename.size() - 4);
    std::vector<std::string> parts;
    std::string              cur;
    for (char c : body) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    // stem (at least one part) + pid + launch_ns + launch_rnd + n
    if (parts.size() < 5)
        return false;
    const std::string &n         = parts[parts.size() - 1];
    const std::string &launch_rnd = parts[parts.size() - 2];
    const std::string &launch_ns = parts[parts.size() - 3];
    const std::string &pid       = parts[parts.size() - 4];
    if (!all_digits(n) || !all_digits(launch_rnd) || !all_digits(launch_ns) || !all_digits(pid))
        return false;
    if (pid_out)
        *pid_out = static_cast<unsigned>(std::strtoul(pid.c_str(), nullptr, 10));
    return true;
}

void scavenge_dir(const boost::filesystem::path &dir, size_t &removed, int depth)
{
    if (depth > 6)
        return;
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(dir, ec))
        return;
    for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        const boost::filesystem::path &p = it->path();
        boost::system::error_code      st_ec;
        if (boost::filesystem::is_directory(p, st_ec)) {
            scavenge_dir(p, removed, depth + 1);
            continue;
        }
        if (!boost::filesystem::is_regular_file(p, st_ec))
            continue;
        unsigned pid = 0;
        if (!parse_atomic_temp(p.filename().string(), &pid))
            continue;
        if (process_is_alive(pid))
            continue;
        boost::system::error_code rm_ec;
        boost::filesystem::remove(p, rm_ec);
        if (!rm_ec) {
            ++removed;
            BOOST_LOG_TRIVIAL(info) << "InstanceLock: scavenged stale temp " << p.string();
        }
    }
}

} // namespace

bool is_atomic_write_temp_name(const std::string &filename, unsigned *pid_out)
{
    return parse_atomic_temp(filename, pid_out);
}

bool process_is_alive(unsigned pid)
{
    if (pid == 0)
        return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno != ESRCH;
#endif
}

size_t scavenge_stale_atomic_temps(const std::vector<std::string> &dirs, bool lock_held)
{
    if (!lock_held)
        return 0;
    size_t removed = 0;
    for (const std::string &dir : dirs) {
        if (dir.empty())
            continue;
        scavenge_dir(boost::filesystem::path(dir), removed, 0);
    }
    return removed;
}

} // namespace Slic3r
