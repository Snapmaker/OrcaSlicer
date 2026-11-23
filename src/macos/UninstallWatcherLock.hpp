#pragma once

#include "UninstallWatcherNames.hpp"

#include <cerrno>
#include <chrono>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace SnapmakerUninstall {

/** @brief Serializes registration and cleanup across app and watcher processes. */
class LifecycleLock
{
public:
    /**
     * @brief Acquires the persistent lock without passing it to spawned commands.
     * @param deadline Deadline for retrying acquisition after contention.
     */
    explicit LifecycleLock(std::chrono::steady_clock::time_point deadline)
    {
        const std::filesystem::path dir = lifecycle_dir();
        if (dir.empty() || !is_unredirected_user_path(dir))
        {
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(dir.parent_path(), ec);
        if (ec.value() != 0 || (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST))
        {
            return;
        }

        // Keep the inode outside Snapmaker_Orca: cleanup can delete that entire tree.
        // Never unlink this file, even after unlocking, or waiters could use different inodes.
        const std::filesystem::path path = dir / "lifecycle.lock";
        if (!is_unredirected_user_path(path))
        {
            return;
        }
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0)
        {
            return;
        }
        for (;;)
        {
            if (::flock(fd_, LOCK_EX | LOCK_NB) == 0)
            {
                locked_ = true;
                return;
            }
            if (errno != EWOULDBLOCK && errno != EINTR)
            {
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                timed_out_ = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    /** @brief Releases the lock and descriptor on all returning paths. */
    ~LifecycleLock()
    {
        if (fd_ >= 0)
        {
            if (locked_)
            {
                ::flock(fd_, LOCK_UN);
            }
            ::close(fd_);
        }
    }

    LifecycleLock(const LifecycleLock&) = delete;
    LifecycleLock& operator=(const LifecycleLock&) = delete;
    LifecycleLock(LifecycleLock&&) = delete;
    LifecycleLock& operator=(LifecycleLock&&) = delete;

    /** @brief Returns whether exclusive ownership was acquired. */
    bool locked() const { return locked_; }

    /** @brief Distinguishes retryable contention from path, permission, and descriptor errors. */
    bool timed_out() const { return timed_out_; }

private:
    int  fd_ = -1;
    bool locked_ = false;
    bool timed_out_ = false;
};

} // namespace SnapmakerUninstall
