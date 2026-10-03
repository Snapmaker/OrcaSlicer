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
// path in this process would drop an fcntl lock). flock is inherited across
// fork-without-exec; O_CLOEXEC only covers exec. Windows: CreateFileW with
// FILE_SHARE_READ|WRITE|DELETE, then LockFileEx. Share-mode 0 is not used:
// the old instance's OnExit handle, Defender, the indexer or OneDrive would
// otherwise defeat the wait and force a spurious read-only session. The OS
// releases the lock when the holder exits. The lock file is created once and
// kept; deleting it on release would let a third instance lock a fresh inode
// while the second still holds the old one.
//
// try_acquire never waits longer than `timeout` (default 1.5 s — the previous
// instance releases only after OnExit teardown). Startup must not block
// forever. If another instance holds the lock this process is read-only, but
// that is not permanent: allows_saves() retries a non-blocking acquire and
// restores writes once the lock is free. Hidden / hub instances take the same
// lock non-blocking so they cannot save alongside the holder. On filesystems
// that do not support flock/LockFileEx (NFS/SMB/FUSE ENOLCK/EOPNOTSUPP),
// saves stay allowed and a warning is recorded. Tests and the CLI never call
// try_acquire_data_dir, so allows_saves() stays true until release_data_dir().
class InstanceLock
{
public:
    static constexpr std::chrono::milliseconds default_timeout{1500};

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
    bool unsupported() const { return m_unsupported; }

    static std::string lock_path_for_data_dir(const std::string &data_dir);

    // Session lock. `claim` false (empty path / test reset) leaves NotAttempted
    // so later tests stay writable. Hidden / hub still claim with timeout 0.
    static bool try_acquire_data_dir(const std::string &       data_dir,
                                     bool                      claim   = true,
                                     std::chrono::milliseconds timeout = default_timeout);
    static void release_data_dir();
    static bool holds_data_dir();
    // True while Held, NotAttempted (tests/CLI) or Unsupported (no lock API).
    // ReadOnly retries a non-blocking acquire. Released (OnExit / release)
    // stays false.
    static bool allows_saves();
    static bool is_read_only();
    static bool lock_unsupported();
    static const std::string &last_error();

private:
    struct Native;
    std::string              m_path;
    std::unique_ptr<Native>  m_native;
    bool                     m_locked{false};
    bool                     m_unsupported{false};

    bool try_lock(std::chrono::milliseconds timeout);
    void unlock();
};

// True only for a Stage A leftover produced by write_file_atomically:
// `<name>.<pid>.<launch_ns>.<launch_rnd>.<n>.tmp` with exactly four decimal
// fields after the original name, launch_ns at least 9 digits (steady_clock
// nanoseconds), pid in (0, max-pid], no overflow. Never matches .bak, date
// stamps like photo.<pid>.2024.06.01.tmp, or names with fewer fields.
bool is_atomic_write_temp_name(const std::string &filename, unsigned *pid_out = nullptr);

// True when `pid` still has a process. Used by the scavenger so a live
// writer's temp is left alone.
bool process_is_alive(unsigned pid);

// Remove matching leftover temps whose pid is not a live writer. No-op when
// `lock_held` is false. Never follows directory symlinks out of the start
// dir, never touches .bak or names that fail the pattern.
size_t scavenge_stale_atomic_temps(const std::vector<std::string> &dirs, bool lock_held);

} // namespace Slic3r
