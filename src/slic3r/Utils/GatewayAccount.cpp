#include "GatewayAccount.hpp"

#include <cstdint>
#include <optional>

namespace Slic3r { namespace Gateway {

namespace {

std::string get_string(const nlohmann::json& frame, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const auto it = frame.find(key);
        if (it != frame.end() && it->is_string() && !it->get<std::string>().empty())
            return it->get<std::string>();
    }
    return std::string{};
}

std::optional<bool> get_bool(const nlohmann::json& frame, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const auto it = frame.find(key);
        if (it != frame.end() && it->is_boolean())
            return it->get<bool>();
    }
    return std::nullopt;
}

// The wrapped /api/account shape carries the cloud profile, where the user id is a JSON number.
std::string get_string_or_number(const nlohmann::json& frame, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const auto it = frame.find(key);
        if (it == frame.end())
            continue;
        if (it->is_string() && !it->get<std::string>().empty())
            return it->get<std::string>();
        if (it->is_number_integer())
            return std::to_string(it->get<std::int64_t>());
    }
    return std::string{};
}

std::optional<bool> wrapped_code_succeeded(const nlohmann::json& frame)
{
    const auto it = frame.find("code");
    if (it == frame.end() || !it->is_number_integer())
        return std::nullopt;
    const std::int64_t code = it->get<std::int64_t>();
    return code == 0 || code == 200;
}

bool status_is_online(const nlohmann::json& frame)
{
    for (const char* key : {"status", "stauts"}) {
        const auto it = frame.find(key);
        if (it != frame.end() && it->is_string())
            return it->get<std::string>() == "online";
    }
    return false;
}

} // namespace

bool GatewayAccount::parse(const nlohmann::json& frame, AccountSnapshot& out)
{
    if (!frame.is_object())
        return false;

    // Legacy CLI builds answer /api/account with {code, data:{...cloud profile}}
    // instead of the documented flat frame. Unwrap it first.
    const auto wrapped_data = frame.find("data");
    if (wrapped_data != frame.end() && wrapped_data->is_object()) {
        AccountSnapshot snapshot;
        snapshot.token    = get_string(*wrapped_data, {"accessToken", "access_token", "token"});
        snapshot.userid   = get_string_or_number(*wrapped_data, {"userid", "uid", "id"});
        snapshot.nickname = get_string(*wrapped_data, {"nickname"});
        snapshot.account  = get_string(*wrapped_data, {"account"});
        snapshot.email    = get_string(*wrapped_data, {"email"});
        snapshot.is_login = wrapped_code_succeeded(frame).value_or(true) && (!snapshot.token.empty() || !snapshot.userid.empty());
        out               = std::move(snapshot);
        return true;
    }

    AccountSnapshot snapshot;
    snapshot.is_login = get_bool(frame, {"is_login", "isLogin"}).value_or(status_is_online(frame));
    snapshot.userid   = get_string(frame, {"userid", "id", "uid"});
    snapshot.token    = get_string(frame, {"token", "access_token", "accessToken"});
    snapshot.nickname = get_string(frame, {"nickname"});
    snapshot.account  = get_string(frame, {"account"});
    snapshot.email    = get_string(frame, {"email"});
    out               = std::move(snapshot);
    return true;
}

}} // namespace Slic3r::Gateway
