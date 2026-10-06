#pragma once

// Which hub on this PC sends the notifications for a printer.
//
// A hub belongs to a data dir, and nothing stops a person running two: the normal install and a
// test build with its own --datadir, a second copy started from a scratch folder, a gate. Every
// one of them runs slicer windows that watch the same printers on the LAN, so every one of them
// sees the same HMS code and the same print start. Each hub's ring is its own business, but a
// notification is the person's: two hubs that both have somewhere to deliver (the same phone
// registered with both, the same ntfy topic in two copied settings files) sent it twice.
//
// So a hub delivers a printer's events only while it holds that printer's claim, a lock file in
// the machine's temp folder named after the printer. The first hub that has an event to deliver
// for the printer takes it and keeps it for as long as it runs; the operating system drops the
// lock when the process goes, however it goes, so there is nothing to clean up and no stale owner
// to time out. A hub that finds the claim taken still stores the event - its own page and its own
// phone link show it - and only skips the delivery.
//
// Only physical printers are claimed (a Bambu serial, a LAN Snapmaker). A print host or a
// connected printer is known by a preset-level id ("printhost", "connect") that two data dirs can
// share for two different machines, so those are never claimed and always delivered.
//
// A hub with nowhere to deliver to never asks, so a scratch hub with no phone and no destinations
// cannot take the claim away from the hub the person actually paired.

#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/nowide/fstream.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r {
namespace GUI {

class HubPushOwner
{
public:
    // `dir` is where the claim files live; empty means <temp>/EdgeSlicer-push-owner.
    explicit HubPushOwner(std::string dir = std::string()) : m_dir(std::move(dir)) {}

    // The printer kinds whose ids name one physical machine.
    static bool claimable_kind(const std::string& kind) { return kind == "bambu" || kind == "snapmaker"; }

    // The claim file's name for a printer id: letters, digits, '-' and '_' kept, the rest '_',
    // and a hash of the original so two ids that sanitise alike ("sm:A" and "sm_A") stay apart.
    static std::string file_name(const std::string& printer_id)
    {
        std::string safe;
        for (char c : printer_id) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            safe.push_back(ok ? c : '_');
            if (safe.size() >= 64) break;
        }
        unsigned long long h = 1469598103934665603ULL; // FNV-1a: stable across runs and builds
        for (unsigned char c : printer_id) { h ^= c; h *= 1099511628211ULL; }
        char hex[17];
        static const char* digits = "0123456789abcdef";
        for (int i = 15; i >= 0; --i) { hex[i] = digits[h & 0xf]; h >>= 4; }
        hex[16] = 0;
        return "printer-" + safe + "-" + hex + ".lock";
    }

    // True when this hub may deliver `printer_id`'s events: it holds the claim, or takes it now.
    // Never blocks. Any failure to even create the claim file answers true: a broken temp folder
    // must not silence the only hub there is.
    bool claim(const std::string& printer_id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_held.find(printer_id);
        if (it != m_held.end()) return true;
        try {
            boost::filesystem::path   root = m_dir.empty() ? boost::filesystem::temp_directory_path() / "EdgeSlicer-push-owner"
                                                           : boost::filesystem::path(m_dir);
            boost::system::error_code ec;
            boost::filesystem::create_directories(root, ec);
            const boost::filesystem::path file = root / file_name(printer_id);
            if (!boost::filesystem::exists(file, ec)) {
                boost::nowide::ofstream touch(file.string().c_str(), std::ios::binary | std::ios::app);
            }
            auto fl = std::make_unique<boost::interprocess::file_lock>(file.string().c_str());
            if (!fl->try_lock()) return false;
            m_held.emplace(printer_id, std::move(fl));
            return true;
        } catch (...) {
            return true;
        }
    }

    bool holds(const std::string& printer_id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_held.count(printer_id) != 0;
    }

    // Let go of every claim (the hub has nowhere left to deliver, or is shutting down).
    void release_all()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& kv : m_held) {
            try { kv.second->unlock(); } catch (...) {}
        }
        m_held.clear();
    }

    ~HubPushOwner() { release_all(); }

private:
    std::string                                                          m_dir;
    std::mutex                                                           m_mutex;
    std::map<std::string, std::unique_ptr<boost::interprocess::file_lock>> m_held;
};

} // namespace GUI
} // namespace Slic3r
