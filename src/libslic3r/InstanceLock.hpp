#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

// Per-user-data-dir lock so two EdgeSlicer instances do not write the same
// config, preset or printer files at once. This is not the single-instance
// hand-off lock (Utils/InstanceRouting.hpp / GUI/InstanceCheck.cpp): a second
// launch that should open a file in the existing instance still goes through
// that path and never reaches this lock.
//
// POSIX: flock(2) on our own fd (not fcntl — another open/close of the same
// path in this process would drop an fcntl lock). Windows: CreateFileW with
// no sharing, then LockFileEx. The OS releases the lock when the holder
// exits, so a crash never leaves a stale lock behind. The lock file is
// created once and kept; deleting it on release would let a third instance
// lock a fresh inode while the second still holds the old one.
//
// try_acquire never waits longer than `timeout` (startup must not block
// forever). If another instance holds the lock, this process enters
// read-only / no-save mode: allows_saves() is false, writers return false
// and log, and the GUI shows a warning. Hidden / hub instances do not claim
// the lock (same rule as claims_instance_lock), so they cannot starve a
// visible instance. Tests and the CLI never call try_acquire_data_dir, so
// allows_saves() stays true.
class InstanceLock
{
public:
    static constexpr std::chrono::milliseconds default_timeout{250};

    // Empty path: no-op. Otherwise try to lock `lock_file_path` for this
    // object's lifetime. Never waits longer than `timeout`.
    explicit InstanceLock(const std::string &           lock_file_path,
                          std::chrono::milliseconds     timeout = default_timeout);
    ~InstanceLock();

    InstanceLock(const InstanceLock &)            = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;
    InstanceLock(InstanceLock &&other) noexcept;
    InstanceLock &operator=(InstanceLock &&other) noexcept;

    bool locked() const { return m_locked; }

    static std::string lock_path_for_data_dir(const std::string &data_dir);

    // Session lock. `claim` false (hidden / hub) leaves the lock alone and
    // does not flip the process into read-only mode.
    static bool try_acquire_data_dir(const std::string &       data_dir,
                                     bool                      claim   = true,
                                     std::chrono::milliseconds timeout = default_timeout);
    static void release_data_dir();
    static bool holds_data_dir();
    // False only after a claimed acquire failed (another instance holds the
    // data dir). NotAttempted (tests, CLI, hidden) stays writable.
    static bool allows_saves();
    static bool is_read_only();
    static const std::string &last_error();

private:
    struct Native;
    std::string              m_path;
    std::unique_ptr<Native>  m_native;
    bool                     m_locked{false};

    bool try_lock(std::chrono::milliseconds timeout);
    void unlock();
};

// True for a Stage A leftover: `<name>.<pid>.<launch_ns>.<launch_rnd>.<n>.tmp`
// with four decimal fields after the original name. Never matches .bak.
bool is_atomic_write_temp_name(const std::string &filename, unsigned *pid_out = nullptr);

// True when `pid` still has a process. Used by the scavenger so a live
// writer's temp is left alone.
bool process_is_alive(unsigned pid);

// Remove matching leftover temps whose pid is not a live writer. No-op when
// `lock_held` is false. Never touches .bak or names that fail the pattern.
// Walks each directory a few levels down (data dir, user/, process, machine).
size_t scavenge_stale_atomic_temps(const std::vector<std::string> &dirs, bool lock_held);

} // namespace Slic3r
