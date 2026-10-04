#pragma once

#include <chrono>
#include <mutex>
#include <string>

namespace Slic3r {

// Scoped write lock on a file shared by every running instance on one data
// dir (port of upstream Orca #15861): the app config, the user preset tree.
// Threads of this process are serialised through a recursive mutex, other
// processes through an advisory OS file lock on `lock_file_path`. The lock
// file is created on first use and kept; the OS releases the lock when its
// holder exits, so a crashed instance never leaves a stale lock behind.
//
// Best effort, taken around each save and nothing longer: when the lock file
// cannot be opened or locked, or another instance still holds it after
// `timeout`, the guard keeps only the in-process mutex, locked() reports
// false, a warning is logged and the write proceeds. A hung or busy instance
// never blocks another one from saving, and no write is ever refused. This
// is what Edge's multi-window / phone-hub design needs: hidden hub instances,
// visible windows and a test build beside the installed one all share the
// data dir, each stays fully writable, and AppConfig::save holds the guard
// across its merge_shared_from_disk() read and its write so two instances'
// read-merge-write cycles do not interleave. For `cooldown` after a failed
// attempt, guards on that file leave it alone so a stuck holder costs one
// wait, not one per save.
//
// Not the single-instance hand-off lock (Utils/InstanceRouting.hpp,
// GUI/InstanceCheck.cpp): that decides whether a launch hands its files to a
// running window; this one only orders writes.
//
// Lock order: a PresetCollection mutex may be held when a guard is taken,
// never the reverse; the guards sit at the leaf writers for that reason.
class InstanceLock
{
public:
    // Long against a critical section of milliseconds, short against the GUI
    // thread, which is where most guards are taken.
    static constexpr std::chrono::milliseconds default_timeout{1000};
    // Mutable so tests can shorten it.
    static inline std::chrono::milliseconds cooldown{10000};

    // An empty path makes the guard a no-op.
    explicit InstanceLock(const std::string &lock_file_path, std::chrono::milliseconds timeout = default_timeout);
    ~InstanceLock();

    InstanceLock(const InstanceLock &)            = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;

    // True while this process holds the cross-process file lock.
    bool locked() const { return m_locked; }

private:
    struct Slot;
    static Slot &slot_for(const std::string &lock_file_path);
    static bool  open_lock_file(Slot &slot, const std::string &lock_file_path);
    static void  defer(Slot &slot, const std::string &reason);

    Slot                                  *m_slot{nullptr};
    std::unique_lock<std::recursive_mutex> m_slot_guard;
    bool                                   m_locked{false};
};

} // namespace Slic3r
