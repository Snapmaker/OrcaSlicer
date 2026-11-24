#pragma once

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Slic3r {

class FilamentRulesManager
{
public:
    using StringList = std::list<std::string>;

    static FilamentRulesManager& instance();
    static void uninit();

    StringList get_list(const std::string& section, const std::string& key) const;

private:
    enum class State
    {
        Uninitialized,
        Initialized,
        Failed,
        Terminated
    };

    using Section = std::unordered_map<std::string, StringList>;
    using Sections = std::unordered_map<std::string, Section>;

    FilamentRulesManager() = default;
    ~FilamentRulesManager() = default;

    FilamentRulesManager(const FilamentRulesManager&) = delete;
    FilamentRulesManager& operator=(const FilamentRulesManager&) = delete;

    static FilamentRulesManager& raw_instance();

    void init();
    void release();

private:
    mutable std::mutex m_mutex;
    State              m_state{ State::Uninitialized };
    Sections           m_sections;
};

} // namespace Slic3r
