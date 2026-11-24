#include "FilamentRulesManager.hpp"

#include "Preset.hpp"
#include "Utils.hpp"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace {

constexpr const char* g_rules_file_name = "filament_rules.json";
constexpr const char* g_snapmaker_vendor_name = "Snapmaker";
constexpr int         g_schema_version = 1;

std::filesystem::path rules_path()
{
    std::filesystem::path system_path = std::filesystem::u8path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR /
                                        g_snapmaker_vendor_name / "filament" / g_rules_file_name;
    if (std::filesystem::exists(system_path))
        return system_path;

    return std::filesystem::u8path(Slic3r::resources_dir()) / "profiles" / g_snapmaker_vendor_name / "filament" /
           g_rules_file_name;
}

} // namespace

FilamentRulesManager& FilamentRulesManager::instance()
{
    FilamentRulesManager& manager = raw_instance();
    manager.init();
    return manager;
}

void FilamentRulesManager::uninit()
{
    raw_instance().release();
}

FilamentRulesManager& FilamentRulesManager::raw_instance()
{
    static FilamentRulesManager* s_instance = new FilamentRulesManager();
    return *s_instance;
}

FilamentRulesManager::StringList FilamentRulesManager::get_list(const std::string& section, const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Initialized)
        return {};

    auto section_it = m_sections.find(section);
    if (section_it == m_sections.end())
        return {};

    auto list_it = section_it->second.find(key);
    if (list_it == section_it->second.end())
        return {};

    return list_it->second;
}

void FilamentRulesManager::init()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Uninitialized)
        return;

    const std::filesystem::path path = rules_path();
    Sections                   sections;
    std::string                error;

    do
    {
        std::ifstream stream(path);
        if (!stream)
        {
            error = "failed to open file";
            break;
        }

        nlohmann::json root = nlohmann::json::parse(stream, nullptr, false);
        if (root.is_discarded())
        {
            error = "failed to parse JSON";
            break;
        }

        if (!root.is_object())
        {
            error = "root must be an object";
            break;
        }

        auto schema_version = root.find("schema_version");
        auto sections_node = root.find("sections");
        if (schema_version == root.end() || !schema_version->is_number_integer() || *schema_version != g_schema_version ||
            sections_node == root.end() || !sections_node->is_object())
        {
            error = "invalid schema";
            break;
        }

        bool valid = true;
        for (const auto& section_item : sections_node->items())
        {
            if (!section_item.value().is_object())
            {
                error = "section must be an object: " + section_item.key();
                valid = false;
                break;
            }

            Section lists;
            for (const auto& list_item : section_item.value().items())
            {
                if (!list_item.value().is_array())
                {
                    error = "list must be an array: " + list_item.key();
                    valid = false;
                    break;
                }

                StringList values;
                for (const auto& value : list_item.value())
                {
                    if (!value.is_string())
                    {
                        error = "list item must be a string: " + list_item.key();
                        valid = false;
                        break;
                    }

                    values.emplace_back(value.get_ref<const std::string&>());
                }

                if (!valid)
                    break;

                lists.emplace(list_item.key(), std::move(values));
            }

            if (!valid)
                break;

            sections.emplace(section_item.key(), std::move(lists));
        }

        if (!valid)
            break;

        m_sections = std::move(sections);
        m_state = State::Initialized;
    } while (false);

    if (m_state != State::Initialized)
    {
        m_state = State::Failed;
        BOOST_LOG_TRIVIAL(error) << "FilamentRulesManager failed to load " << path.u8string() << ": " << error;
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "FilamentRulesManager loaded: " << path.u8string();
}

void FilamentRulesManager::release()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == State::Terminated)
        return;

    m_sections.clear();
    m_sections.rehash(0);
    m_state = State::Terminated;
}

} // namespace Slic3r
