#ifndef slic3r_SnapmakerAccount_hpp_
#define slic3r_SnapmakerAccount_hpp_

#include <string>

namespace Slic3r {

// Error codes of the Snapmaker account API, as enumerated by the bundled web
// UI (resources/web/flutter_web): 110001 login_failed, 110002
// authorization_missing, 110003 token_expired, 110004 authentication_failed.
// The API reports these with HTTP 200, so the transport status proves nothing
// about whether a token was accepted.
static constexpr int SM_API_CODE_OK                    = 200;
static constexpr int SM_API_CODE_AUTHORIZATION_MISSING = 110002;
static constexpr int SM_API_CODE_TOKEN_EXPIRED         = 110003;
static constexpr int SM_API_CODE_AUTHENTICATION_FAILED = 110004;

struct SMAccountProfile
{
    std::string id;
    std::string nickname;
    std::string icon;
    std::string account;
};

// Parses a reply from the accounts/current endpoint. Returns true when the
// body is a success envelope (code 200 with a data object), filling whichever
// profile fields are present; sets auth_rejected for the codes that mean the
// token itself was refused, so callers can tell a definitive rejection from a
// transient failure. Callers decide what else to require: restoring a stored
// session demands a usable account id, while a fresh login proceeds unless the
// token was refused. "code" and "id" are tolerated as integers or numeric
// strings, since only the id.snapmaker.com envelope has been observed live.
bool sm_parse_account_response(const std::string& body, SMAccountProfile& profile, bool& auth_rejected);

// Returns the account API base for a country code as stored in AppConfig.
std::string sm_account_api_base(const std::string& country_code);

} // namespace Slic3r

#endif // slic3r_SnapmakerAccount_hpp_
