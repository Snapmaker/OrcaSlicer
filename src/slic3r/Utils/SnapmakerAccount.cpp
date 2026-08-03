#include "SnapmakerAccount.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r {

using json = nlohmann::json;

bool sm_parse_account_response(const std::string& body, SMAccountProfile& profile, bool& auth_rejected)
{
    auth_rejected = false;
    auto str_or_empty = [](const json& j, const char* key) {
        return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
    };
    try {
        json response = json::parse(body);
        int  code     = -1;
        if (response.contains("code")) {
            const json& jcode = response["code"];
            if (jcode.is_number_integer())
                code = jcode.get<int>();
            else if (jcode.is_string()) {
                try {
                    code = std::stoi(jcode.get<std::string>());
                } catch (std::exception&) {}
            }
        }
        auth_rejected = code == SM_API_CODE_AUTHORIZATION_MISSING ||
                        code == SM_API_CODE_TOKEN_EXPIRED ||
                        code == SM_API_CODE_AUTHENTICATION_FAILED;
        if (!response.contains("data") || !response["data"].is_object())
            return false;
        const json& data = response["data"];
        if (data.contains("id")) {
            const json& jid = data["id"];
            if (jid.is_number_integer())
                profile.id = std::to_string(jid.get<long long>());
            else if (jid.is_string())
                profile.id = jid.get<std::string>();
        }
        profile.nickname = str_or_empty(data, "nickname");
        profile.icon     = str_or_empty(data, "icon");
        profile.account  = str_or_empty(data, "account");
        return code == SM_API_CODE_OK;
    } catch (std::exception&) {
        return false;
    }
}

std::string sm_account_api_base(const std::string& country_code)
{
    return country_code == "CN" ? "https://api.snapmaker.cn" : "https://id.snapmaker.com";
}

} // namespace Slic3r
