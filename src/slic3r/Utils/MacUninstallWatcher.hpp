#pragma once

#include <atomic>
#include <string>

#include <pthread.h>

namespace Slic3r { namespace GUI {

/** @brief Owns a background registration worker; GUI-thread start/stop calls must be serialized. */
class MacUninstallWatcherRegistration
{
public:
    MacUninstallWatcherRegistration() = default;
    /** @brief Cancels and joins the worker before its state is destroyed. */
    ~MacUninstallWatcherRegistration();

    MacUninstallWatcherRegistration(const MacUninstallWatcherRegistration&) = delete;
    MacUninstallWatcherRegistration& operator=(const MacUninstallWatcherRegistration&) = delete;
    MacUninstallWatcherRegistration(MacUninstallWatcherRegistration&&) = delete;
    MacUninstallWatcherRegistration& operator=(MacUninstallWatcherRegistration&&) = delete;

    /** @brief Captures the executable on the GUI thread and starts asynchronous registration. */
    void start();
    /** @brief Cancels pending registration, lets a committed update finish, and joins the worker. */
    void stop();

private:
    /** @brief Registers one LaunchAgent, retrying lock contention until cancellation. */
    static void* run(void* context);

    std::string       executable_;
    std::atomic<bool> stopping_{false};
    pthread_t         worker_{};
    bool              running_ = false;
};

}} // namespace Slic3r::GUI
