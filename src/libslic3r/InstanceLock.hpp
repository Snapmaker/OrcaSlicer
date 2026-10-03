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
// otherwise defeat the wait and force a spurious read-only session. CreateFileW
// ACCESS_DENIED is a permission error, not "another instance". The OS
// releases the lock when the holder exits. The lock file is created once and
// kept; deleting it on release would let a third instance lock a fresh inode
// while the second still holds the old one.
//
// Visible instances take a long-held session lock (default wait 1.5 s — the
// previous instance releases only after OnExit teardown). Hidden / hub
// instances never hold that lock for life: they take it only around each
// gated write (non-blocking try, write, release). If the try fails, that
// write is refused. A visible wait / later re-acquire that collides with a
// hidden save window retries every 5 ms; the next attempt wins once the
// hidden write drops the lock. On filesystems that do not support
// flock/LockFileEx (NFS/SMB/FUSE ENOLCK/EOPNOTSUPP), saves stay allowed
// and a warning is recorded. Tests and the CLI never call
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
    bool permission_denied() const { return m_denied; }

    static std::string lock_path_for_data_dir(const std::string &data_dir);

    // Long-held session lock for a visible instance. `claim` false (empty
    // path / test reset) leaves NotAttempted so later tests stay writable.
    static bool try_acquire_data_dir(const std::string &       data_dir,
                                     bool                      claim   = true,
                                     std::chrono::milliseconds timeout = default_timeout);
    // Hidden / hub: remember the data dir but do not hold the lock. Writes
    // go through WriteScope (non-blocking try, release in the destructor).
    static bool enable_transient_saves(const std::string &data_dir);
    static bool is_transient();
    static void release_data_dir();
    static bool holds_data_dir();
    // True while Held, NotAttempted (tests/CLI), Unsupported, or Transient
    // (hidden may still attempt a per-save lock). ReadOnly retries a
    // non-blocking acquire. Released stays false.
    static bool allows_saves();
    // Session is ReadOnly right now. Does not re-acquire.
    static bool is_read_only();
    static bool lock_unsupported();
    static bool lock_permission_denied();
    static std::string last_error();
    // True when the most recently constructed WriteScope refused the write
    // (busy, permission, released). Survives the scope destructor so a
    // caller can pick a dialog after Preset::save / save_current_preset.
    static bool last_write_refused();

    // RAII gate around a write. The session mutex is held only in the
    // constructor and destructor, not across the write. Transient sessions
    // take the flock in the constructor and release it in the destructor
    // when the last nested scope exits. Nesting is OK (refcount). Use this
    // around every gated write; save_current_preset keeps one scope across
    // the disk write and the in-memory commit.
    class WriteScope
    {
    public:
        WriteScope();
        ~WriteScope();
        WriteScope(const WriteScope &)            = delete;
        WriteScope &operator=(const WriteScope &) = delete;
        bool        allows() const { return m_allows; }

    private:
        bool m_allows{false};
        bool m_owns_transient{false};
    };

private:
    struct Native;
    std::string             m_path;
    std::unique_ptr<Native> m_native;
    bool                    m_locked{false};
    bool                    m_unsupported{false};
    bool                    m_denied{false};

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
