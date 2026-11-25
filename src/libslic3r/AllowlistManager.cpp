#include "AllowlistManager.hpp"

#include "Preset.hpp"
#include "Utils.hpp"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace {

constexpr const char* g_allowlist_file_name = "filament_allow_list.json";
constexpr const char* g_snapmaker_vendor_name = "Snapmaker";
constexpr int         g_schema_version = 1;

std::filesystem::path user_allowlist_path()
{
    return std::filesystem::u8path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR / g_snapmaker_vendor_name / "filament" /
           g_allowlist_file_name;
}

std::filesystem::path resource_allowlist_path()
{
    return std::filesystem::u8path(Slic3r::resources_dir()) / "profiles" / g_snapmaker_vendor_name / "filament" /
           g_allowlist_file_name;
}

} // namespace

AllowlistManager& AllowlistManager::instance()
{
    AllowlistManager& manager = raw_instance();
    manager.init();
    return manager;
}

void AllowlistManager::uninit()
{
    raw_instance().release();
}

AllowlistManager& AllowlistManager::raw_instance()
{
    static AllowlistManager* s_instance = new AllowlistManager();
    return *s_instance;
}

AllowlistManager::StringList AllowlistManager::get_list(const std::string& section, const std::string& key) const
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

void AllowlistManager::init()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Uninitialized)
        return;

    const std::filesystem::path user_path     = user_allowlist_path();
    const std::filesystem::path resource_path = resource_allowlist_path();
    const std::filesystem::path path          = std::filesystem::exists(user_path) ? user_path : resource_path;
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
        BOOST_LOG_TRIVIAL(error) << "AllowlistManager failed to load " << path.u8string() << ": " << error;
        return;
    }

    if (path == user_path)
    {
        // A deployed user copy can predate newly added sections/keys. Keep its
        // existing values authoritative, but inherit missing entries from the
        // bundled allow-list.
        auto merge_missing_entries = [&](const std::filesystem::path& fallback_path) {
            std::ifstream stream(fallback_path);
            if (!stream)
                return;

            nlohmann::json root = nlohmann::json::parse(stream, nullptr, false);
            if (root.is_discarded() || !root.is_object())
                return;

            auto sections_node = root.find("sections");
            if (sections_node == root.end() || !sections_node->is_object())
                return;

            auto read_list = [](const nlohmann::json& value) {
                StringList values;
                for (const auto& item : value) {
                    if (item.is_string())
                        values.emplace_back(item.get_ref<const std::string&>());
                }
                return values;
            };

            for (const auto& section_item : sections_node->items())
            {
                if (!section_item.value().is_object())
                    continue;

                auto section_it = m_sections.find(section_item.key());
                if (section_it == m_sections.end()) {
                    Section lists;
                    for (const auto& list_item : section_item.value().items()) {
                        if (list_item.value().is_array())
                            lists.emplace(list_item.key(), read_list(list_item.value()));
                    }
                    m_sections.emplace(section_item.key(), std::move(lists));
                    continue;
                }

                for (const auto& list_item : section_item.value().items())
                {
                    if (!list_item.value().is_array() || section_it->second.find(list_item.key()) != section_it->second.end())
                        continue;
                    section_it->second.emplace(list_item.key(), read_list(list_item.value()));
                }
            }
        };
        merge_missing_entries(resource_path);
    }

    BOOST_LOG_TRIVIAL(info) << "AllowlistManager loaded: " << path.u8string();
}

void AllowlistManager::release()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == State::Terminated)
        return;

    m_sections.clear();
    m_sections.rehash(0);
    m_state = State::Terminated;
}

} // namespace Slic3r
