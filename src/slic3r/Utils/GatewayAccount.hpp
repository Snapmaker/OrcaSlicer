#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace Gateway {

// Login state as owned by the snapmaker_connection CLI. The CLI is the single
// source of truth; the C++ side only mirrors the frames below and never issues
// login or logout requests.
struct AccountSnapshot
{
    bool        is_login{false};
    std::string userid;
    std::string nickname;
    std::string account;
    std::string email;
    std::string token;
};

// Account-facing parsing for the connection gateway. Both the cold-start
// GET /api/account response and the notify.account.changed params use the same
// flat frame with alias fields from different client generations.
class GatewayAccount
{
public:
    // frame: /api/account response body or notify.account.changed params.
    // Accepts both the documented flat frame and the legacy {code, data}
    // wrapper emitted by current CLI builds. Returns false when the frame is
    // not a JSON object; individual fields fall back to their aliases and then
    // to safe defaults.
    static bool parse(const nlohmann::json& frame, AccountSnapshot& out);
};

}} // namespace Slic3r::Gateway
