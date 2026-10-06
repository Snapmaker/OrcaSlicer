#ifndef slic3r_GUI_MultiComUtils_hpp_
#define slic3r_GUI_MultiComUtils_hpp_

#include <string>
#include <wx/string.h>
#include "FlashNetworkIntfc.h"
#include "MultiComDef.hpp"

namespace Slic3r { namespace GUI {

class MultiComUtils
{
public:
    static ComErrno getLanDevList(std::vector<fnet_lan_dev_info> &devInfos);

    static ComErrno getTokenByPassword(const std::string &userName, const std::string &password,
        const std::string &language, com_token_data_t &tokenData, std::string &message, int msTimeout);

    static ComErrno refreshToken(const std::string &refreshToken, com_token_data_t &tokenData, int msTimeout);

    static ComErrno sendSMSCode(const std::string &phoneNumber, const std::string &language,
        std::string &message, int msTimeout);

    static ComErrno getTokenBySMSCode(const std::string &userName, const std::string &SMSCode,
        const std::string &language, com_token_data_t &tokenData, std::string &message, int msTimeout);

    static ComErrno getUserProfile(const std::string &accessToken, com_user_profile_t &userProfile,
        int msTimeout);

    static ComErrno bindAccountRelp(const std::string &clientId, const std::string &accessToken,
        const std::string &email, bool &showUserPoints, int msTimeout);

    static ComErrno getMqttConfig(const std::string &clientId, const std::string &accessToken,
        com_mqtt_config_t &mqttConfig, int msTimeout);

    static ComErrno downloadFileMem(const std::string &url, std::vector<char> &bytes,
        fnet_progress_callback_t callback, void *callbackData, int msConnectTimeout, int msTimeout);

    static ComErrno downloadFileDisk(const std::string &url, const wxString &saveName,
        fnet_progress_callback_t callback, void *callbackData, int msConnectTimeout, int msTimeout);

    static ComErrno fnetRet2ComErrno(int networkRet);

    static std::vector<fnet_material_mapping_t> comMaterialMappings2Fnet(
        const std::vector<com_material_mapping_t> &comMaterialMappings);
};

}} // namespace Slic3r::GUI

#endif
