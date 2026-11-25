#pragma once

#include "UninstallWatcherLifecycle.hpp"
#include "UninstallWatcherNames.hpp"

#include <fstream>
#include <iterator>
#include <string>

namespace SnapmakerUninstall {

/** @brief Reads a small watcher state or configuration file. */
inline std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (in.fail())
    {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/** @brief Replaces a file atomically; callers serialize writers with LifecycleLock. */
inline bool write_text_atomic(const std::filesystem::path& path, const std::string& text)
{
    const std::filesystem::path tmp = path.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (out.fail())
    {
        return false;
    }
    out << text;
    out.close();
    std::error_code ec;
    if (out.fail())
    {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec.value() != 0)
    {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

/** @brief Accepts canonical UUID text used to identify one registration. */
inline bool valid_registration_id(const std::string& id)
{
    if (id.size() != 36)
    {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (id[i] != '-')
            {
                return false;
            }
        }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
        {
            return false;
        }
    }
    return true;
}

/** @brief Reads a registration UUID, rejecting absent, malformed, or escaped identifiers. */
inline std::string registration_id_from_state(const std::string& text)
{
    const std::size_t key = text.find("\"registration_id\"");
    if (key == std::string::npos)
    {
        return {};
    }
    const std::size_t colon = text.find_first_not_of(" \t\r\n", key + 17);
    if (colon == std::string::npos || text[colon] != ':')
    {
        return {};
    }
    const std::size_t quote = text.find_first_not_of(" \t\r\n", colon + 1);
    if (quote == std::string::npos || text[quote] != '"' || text.size() - quote < 38)
    {
        return {};
    }
    const std::string id = text.substr(quote + 1, 36);
    return text[quote + 37] == '"' && valid_registration_id(id) ? id : std::string();
}

/** @brief Serializes an observed installation and its registration identity. */
inline std::string watch_state_text(const std::filesystem::path& bundle, const std::string& id)
{
    if (!is_managed_install_path(bundle) || !valid_registration_id(id))
    {
        return {};
    }
    std::string escaped;
    for (char c : bundle.string())
    {
        if (c == '\\' || c == '"')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return "{\n  \"registration_id\": \"" + id + "\",\n  \"bundle_path\": \"" + escaped +
           "\",\n  \"seen_bundle\": true\n}\n";
}

/** @brief Stops an older retry from overwriting a registration that has superseded it. */
inline bool registration_attempt_is_current(const std::string& published_id, const std::string& current_id)
{
    return published_id.empty() || (valid_registration_id(published_id) && published_id == current_id);
}

/** @brief Persists a required reload across failed attempts and process restarts; caller holds LifecycleLock. */
class PendingReload
{
public:
    /** @brief Loads the marker; missing markers are normal, redirected or non-file markers are errors. */
    PendingReload()
    {
        const std::filesystem::path dir = helper_dir();
        if (dir.empty())
        {
            return;
        }
        path_ = dir / ".reload.pending";
        if (!is_unredirected_user_path(path_))
        {
            return;
        }
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::symlink_status(path_, ec);
        if (status.type() == std::filesystem::file_type::not_found)
        {
            valid_ = ec.value() == 0 || ec == std::errc::no_such_file_or_directory;
            return;
        }
        valid_ = ec.value() == 0 && std::filesystem::is_regular_file(status);
        required_ = valid_;
    }

    /** @brief Reports whether the marker could be read safely. */
    bool valid() const { return valid_; }
    /** @brief Reports whether a previous attempt requires a reload regardless of matching disk files. */
    bool required() const { return required_; }

    /** @brief Persists reload intent before helper or plist changes. */
    bool require()
    {
        if (valid_ && required_)
        {
            return true;
        }
        if (!valid_ || !write_text_atomic(path_, "pending\n"))
        {
            return false;
        }
        required_ = true;
        return true;
    }

    /** @brief Clears intent only after an acknowledged registration; failures preserve the marker. */
    bool complete(AgentUpdateResult result)
    {
        if (!valid_ || result != AgentUpdateResult::Registered)
        {
            return false;
        }
        if (!required_)
        {
            return true;
        }
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        if (ec.value() != 0)
        {
            return false;
        }
        required_ = false;
        return true;
    }

private:
    std::filesystem::path path_;
    bool valid_ = false;
    bool required_ = false;
};

} // namespace SnapmakerUninstall
