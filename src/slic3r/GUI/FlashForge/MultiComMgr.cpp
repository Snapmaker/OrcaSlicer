#include "MultiComMgr.hpp"

#include <memory>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/platinfo.h>
#include <wx/stdpaths.h>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r/Utils.hpp"
#include "FFDiagnostics.hpp"
#include "FreeInDestructor.h"
#include "MultiComHelper.hpp"
#include "WanDevTokenMgr.hpp"

namespace Slic3r { namespace GUI {

namespace {

std::string sanitized_client_id_part(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isalnum(c);
    }), value.end());
    return value;
}

std::string get_device_client_id_part()
{
    std::string device_id;
    if (wxGetApp().app_config) {
        device_id = wxGetApp().app_config->get("slicer_uuid");
    }

    device_id = sanitized_client_id_part(device_id);
    if (device_id.empty()) {
        device_id = sanitized_client_id_part(boost::uuids::to_string(boost::uuids::random_generator()()));
    }

    constexpr size_t max_device_id_len = 16;
    if (device_id.size() > max_device_id_len) {
        device_id.resize(max_device_id_len);
    }
    return device_id;
}

std::string client_id_device_prefix(const std::string &client_id)
{
    constexpr const char *pc_prefix = "pc_";
    if (client_id.rfind(pc_prefix, 0) != 0) {
        return {};
    }

    const size_t device_id_start = std::strlen(pc_prefix);
    const size_t device_id_end = client_id.find('_', device_id_start);
    if (device_id_end == std::string::npos || device_id_end == device_id_start) {
        return {};
    }

    return client_id.substr(0, device_id_end + 1);
}

bool is_same_device_client_id(const std::string &lhs, const std::string &rhs)
{
    const std::string lhs_prefix = client_id_device_prefix(lhs);
    return !lhs_prefix.empty() && lhs_prefix == client_id_device_prefix(rhs);
}

} // namespace

MultiComMgr::MultiComMgr()
    : m_idNum(ComInvalidId + 1)
    , m_login(false)
    , m_httpOnline(false)
    , m_connOnline(false)
    , m_connFirstConnected(true)
    , m_loopCheckTimer(this)
{
    com_dev_data_t devData;
    devData.connectMode = COM_CONNECT_LAN;
    devData.devProduct = nullptr;
    devData.devDetail = nullptr;
    devData.lanGcodeList.gcodeCnt = 0;
    devData.lanGcodeList.gcodeDatas = nullptr;
    devData.wanGcodeList.gcodeCnt = 0;
    devData.wanGcodeList.gcodeDatas = nullptr;
    devData.wanTimeLapseVideoList.videoCnt = 0;
    devData.wanTimeLapseVideoList.videoDatas = nullptr;
    memset(&devData.lanDevInfo, 0, sizeof(devData.lanDevInfo));
    m_datMap.emplace(ComInvalidId, devData);
    Bind(wxEVT_TIMER, &MultiComMgr::onTimer, this);
}

bool MultiComMgr::initalize(const std::string &dllPath, const std::string &dataDir)
{
    if (networkIntfc() != nullptr) {
        return false;
    }
    wxFileName appFileName(wxStandardPaths::Get().GetExecutablePath());
    wxString appPathWithSep = appFileName.GetPathWithSep();
    std::string logFileDir = dataDir + "/FlashNetwork";
    // The marker file stays supported for anyone who already relies on it; Diagnostics raises the
    // level through setDebugLogging() instead, which needs no file and no restart.
    bool debug = m_debugLogging || wxFileName::FileExists(appPathWithSep + "FLASHNETWORK_DEBUG");
    m_dllPath    = dllPath;
    m_dataDir    = dataDir;
    m_logFileDir = logFileDir;

    fnet_log_settings_t logSettings;
    logSettings.fileDir = logFileDir.c_str();
    logSettings.expireHours = 72;
    logSettings.level = debug ? FNET_LOG_LEVEL_DEBUG : FNET_LOG_LEVEL_INFO;

    // fnet_initlize refuses to start without FlashForge's server-settings file (a missing, empty
    // or wrong-generation file all return FNET_ERROR), so find it before calling in. The first
    // candidate is <resources>/data - where the installer puts it and where FlashForge's own
    // client looks - so an install that has it behaves exactly as upstream does.
    const std::vector<std::string> datCandidates = Slic3r::GUI::ff_flashnetwork_dat_search_paths(
        Slic3r::resources_dir(), dllPath, dataDir, DAT_FILE_NAME);
    std::string serverSettingsPath;
    for (const std::string &candidate : datCandidates) {
        if (wxFileName::FileExists(wxString::FromUTF8(candidate.c_str()))) {
            serverSettingsPath = candidate;
            break;
        }
    }
    const bool datFound = !serverSettingsPath.empty();
    if (datFound) {
        BOOST_LOG_TRIVIAL(info) << "FlashNetwork: server settings " << serverSettingsPath;
    } else {
        std::string searched;
        for (const std::string &candidate : datCandidates)
            searched += (searched.empty() ? "" : ", ") + candidate;
        BOOST_LOG_TRIVIAL(error) << "FlashNetwork: " << DAT_FILE_NAME
                                 << " (FlashForge server settings) not found; looked in: " << searched
                                 << ". FlashNetwork will not initialise without it.";
        // Still make the call, with the path upstream would use, so the log carries the library's
        // own verdict and return code rather than our guess. FlashForge's client does not fall
        // back to running without the file, and neither do we.
        serverSettingsPath = datCandidates.empty() ? std::string(DAT_FILE_NAME) : datCandidates.front();
    }
    m_lastInitError.clear();
    m_networkIntfc.reset(new fnet::FlashNetworkIntfc(
        dllPath.c_str(), serverSettingsPath.c_str(), logSettings, datFound));
    if (!m_networkIntfc->isOk()) {
        m_lastInitError = m_networkIntfc->error();
        BOOST_LOG_TRIVIAL(error) << "initalize FlashNetwork failed: " << dllPath << ": " << m_lastInitError;
        m_networkIntfc.reset();
        return false;
    }
    const char *userAgentFormat = "Orca-Flashforge/%s (PC; %s)";
    // Ultra: identify as an Orca-Flashforge-compatible client with our own version
    std::string version = std::string(SLIC3R_VERSION);
    std::string osName = wxPlatformInfo::Get().GetOperatingSystemIdName().utf8_string();
    m_networkIntfc->setUserAgent((boost::format(userAgentFormat) % version % osName).str().c_str());
    m_wanDevMaintainThd.reset(new WanDevMaintainThd(m_networkIntfc.get()));
    m_wanDevMaintainThd->Bind(RELOGIN_HTTP_EVENT, &MultiComMgr::onReloginHttp, this);
    m_wanDevMaintainThd->Bind(GET_WAN_DEV_EVENT, &MultiComMgr::onUpdateWanDev, this);
    m_wanDevMaintainThd->Bind(COM_GET_USER_PROFILE_EVENT, &MultiComMgr::onUpdateUserProfile, this);

    auto queueEvent = [this](auto &event) { QueueEvent(event.Clone()); };
    m_sendGcodeThd.reset(new WanDevSendGcodeThd(m_networkIntfc.get()));
    m_sendGcodeThd->Bind(COM_SEND_GCODE_PROGRESS_EVENT, queueEvent);
    m_sendGcodeThd->Bind(COM_SEND_GCODE_FINISH_EVENT, queueEvent);

    m_threadPool.reset(new ComThreadPool(5, 30000));
    m_threadExitEvent.set(false);
    m_loopCheckTimer.Start(1000);

    auto onWanConnUnauthorized = [this](wxCommandEvent &) { maintianWanDev(COM_UNAUTHORIZED, false); };
    ComWanConn::inst()->Bind(WAN_CONN_STATUS_EVENT, &MultiComMgr::onWanConnStatus, this);
    ComWanConn::inst()->Bind(WAN_CONN_READ_EVENT, &MultiComMgr::onWanConnRead, this);
    ComWanConn::inst()->Bind(WAN_CONN_HTTP_UNAUTHORIZED, onWanConnUnauthorized);
    WanDevTokenMgr::inst()->Bind(COM_REFRESH_TOKEN_EVENT, &MultiComMgr::onRefreshToken, this);
    return true;
}

// Raise or lower FlashNetwork's log level.
//
// The library takes its log settings in fnet_initlize and exports no way to change them after,
// so the only way to get debug logs out of a running session is to drop the interface and build
// a new one. That disconnects every LAN device, which the caller (Diagnostics) accepts: it
// re-runs its connection test straight afterwards and the device list repopulates on its own.
bool MultiComMgr::setDebugLogging(bool debug)
{
    if (networkIntfc() == nullptr) {
        // Nothing is up, so there is nothing to re-initalize. Remember the wish: the next
        // initalize() will honour it.
        m_debugLogging = debug;
        return false;
    }
    if (m_debugLogging == debug) {
        return true;
    }
    const std::string dllPath = m_dllPath;
    const std::string dataDir = m_dataDir;

    uninitalize();
    m_debugLogging = debug;
    if (!initalize(dllPath, dataDir)) {
        BOOST_LOG_TRIVIAL(error) << "setDebugLogging: re-initalize failed, FlashNetwork is down";
        return false;
    }
    return true;
}

std::string MultiComMgr::libraryVersion()
{
    fnet::FlashNetworkIntfc *intfc = networkIntfc();
    if (intfc == nullptr || intfc->getVersion == nullptr) {
        return std::string();
    }
    const char *version = intfc->getVersion();
    return version == nullptr ? std::string() : std::string(version);
}

void MultiComMgr::uninitalize()
{
    if (networkIntfc() == nullptr) {
        return;
    }
    m_loopCheckTimer.Stop();
    m_threadExitEvent.set(true);
    removeWanDev();
    for (auto &comPtr : m_comPtrs) {
        if (comPtr->connectMode() == COM_CONNECT_LAN) {
            comPtr->disconnect(0);
        }
        comPtr->joinThread();
    }
    WanDevTokenMgr::inst()->Unbind(COM_REFRESH_TOKEN_EVENT, &MultiComMgr::onRefreshToken, this);
    ComWanConn::inst()->Unbind(WAN_CONN_STATUS_EVENT, &MultiComMgr::onWanConnStatus, this);
    ComWanConn::inst()->Unbind(WAN_CONN_READ_EVENT, &MultiComMgr::onWanConnRead, this);
    m_threadPool.reset();
    m_sendGcodeThd->exit();
    m_sendGcodeThd.reset();
    m_wanDevMaintainThd->exit();
    m_wanDevMaintainThd.reset();
    m_networkIntfc.reset();
}

fnet::FlashNetworkIntfc *MultiComMgr::networkIntfc()
{
    return m_networkIntfc.get(); 
}

std::string MultiComMgr::homePageUrl() 
{ 
    if (networkIntfc() == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "homePageUrl: FlashNetwork Failed, can't connect server!";
        return "";
    }
    return m_networkIntfc->getHomePageUrl(); 
}

com_id_t MultiComMgr::addLanDev(const fnet_lan_dev_info_t &devInfo, const std::string &checkCode)
{
    if (networkIntfc() == nullptr) {
        return ComInvalidId;
    }
    com_ptr_t comPtr = std::make_shared<ComConnection>(m_idNum, checkCode, devInfo, networkIntfc());
    com_dev_data_t devData;
    devData.connectMode = COM_CONNECT_LAN;
    devData.lanDevInfo = devInfo;
    devData.devProduct = nullptr;
    devData.devDetail = nullptr;
    devData.lanGcodeList.gcodeCnt = 0;
    devData.lanGcodeList.gcodeDatas = nullptr;
    devData.wanGcodeList.gcodeCnt = 0;
    devData.wanGcodeList.gcodeDatas = nullptr;
    devData.wanTimeLapseVideoList.videoCnt = 0;
    devData.wanTimeLapseVideoList.videoDatas = nullptr;
    devData.devDetailUpdated = false;
    initConnection(comPtr, devData);
    return m_idNum++;
}

void MultiComMgr::removeLanDev(com_id_t id)
{
    auto it = m_ptrMap.left.find(id);
    if (it == m_ptrMap.left.end()) {
        return;
    }
    it->second->disconnect(0);
}

ComErrno MultiComMgr::addWanDev(const com_token_data_t &tokenData, bool isCheckVersionOnTestServer,
    com_add_wan_dev_data_t &addDevData, int tryCnt, int tryMsInterval)
{
    auto tryDo = [tryCnt, tryMsInterval](const std::function<ComErrno()> &func) {
        ComErrno ret = COM_ERROR;
        for (int i = 0; i < tryCnt; ++i) {
            ret = func();
            if (ret == COM_OK || ret == COM_UNAUTHORIZED) {
                return ret;
            } else if (i + 1 < tryCnt) {
                boost::this_thread::sleep_for(boost::chrono::milliseconds(tryMsInterval));
            }
        }
        return ret;
    };
    BOOST_LOG_TRIVIAL(info) << "MultiComMgr::addWanDev";
    if (networkIntfc() == nullptr || m_login) {
        return COM_ERROR;
    }
    m_clientId = generateClientId();
    ComErrno ret = tryDo([&]() {
        return MultiComUtils::getUserProfile(tokenData.accessToken, addDevData.userProfile, ComTimeoutWanA);
    });
    if (ret != COM_OK) {
        return ret;
    }
    ret = tryDo([&]() {
        return MultiComUtils::bindAccountRelp(m_clientId, tokenData.accessToken, addDevData.userProfile.email,
            addDevData.showUserPoints, ComTimeoutWanA);
    });
    if (ret != COM_OK) {
        return ret;
    }
    m_login = true;
    m_httpOnline = true;
    m_connOnline = false;
    m_connFirstConnected = true;
    m_blockCommandFailedUpdate = false;
    m_commandFailedUpdateTime = std_precise_clock::time_point::min();
    m_pendingSetUpdateWanDevTime = std_precise_clock::time_point::max();
    setMaintainThdReqHeader(isCheckVersionOnTestServer);
    MultiComHelper::inst()->loginInit(m_clientId, addDevData.userProfile.uid);
    WanDevTokenMgr::inst()->initalize(tokenData, networkIntfc()); // initialize global token
    initWanConnAsync(tokenData.accessToken);
    QueueEvent(new ComGetUserProfileEvent(COM_GET_USER_PROFILE_EVENT, addDevData.userProfile, ret));
    return ret;
}

void MultiComMgr::removeWanDev()
{
    BOOST_LOG_TRIVIAL(info) << "MultiComMgr::removeWanDev";
    if (!m_login) {
        return;
    }
    for (auto &comPtr : m_comPtrs) {
        if (comPtr->connectMode() == COM_CONNECT_WAN) {
            comPtr->disconnect(0);
        }
    }
    m_login = false;
    m_httpOnline = false;
    m_connOnline = false;
    m_wanDevMaintainThd->stop();
    WanDevTokenMgr::inst()->exit();
    ComWanConn::inst()->freeConn();
    QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, false, false, COM_OK));
}

ComErrno MultiComMgr::bindWanDev(const std::string &ip, unsigned short port, const std::string &serialNumber,
    unsigned short pid, const std::string &name, unsigned short bindType)
{
    if (!m_httpOnline || !m_connOnline) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_wan_dev_bind_data_t *bindData;
    int ret = m_networkIntfc->bindWanDev(m_clientId.c_str(), token.accessToken().c_str(),
        serialNumber.c_str(), pid, name.c_str(), bindType, &bindData, ComTimeoutWanA);
    fnet::FreeInDestructor freeBinData(bindData, m_networkIntfc->freeBindData);
    if (ret == FNET_OK) {
        m_threadPool->post([this, ip, port, serialNumber]() {
            for (int i = 0; i < 3 && !m_threadExitEvent.get(); ++i) {
                int ret = m_networkIntfc->notifyLanDevWanBind(
                    ip.c_str(), port, serialNumber.c_str(), ComTimeoutLanA);
                if (ret == FNET_OK) {
                    break;
                }
                m_threadExitEvent.waitTrue(3000);
            }
        });
        m_pendingSetUpdateWanDevTime = std_precise_clock::now();
    }
    return MultiComUtils::fnetRet2ComErrno(ret);
}

ComErrno MultiComMgr::unbindWanDev(const std::string &serialNumber, const std::string &devId)
{
    if (!m_httpOnline || !m_connOnline) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    int ret = m_networkIntfc->unbindWanDev(
        m_clientId.c_str(), token.accessToken().c_str(), devId.c_str(), ComTimeoutWanA);
    if (ret == FNET_OK) {
        std::string devTopic = getDevTopic(devId);
        if (!devTopic.empty()) {
            ComWanConn::inst()->unsubscribe(std::vector<std::string>(1, devTopic));
        }
        for (auto &comPtr : m_comPtrs) {
            if (comPtr->devId() == devId) {
                if (m_readyIdSet.find(comPtr->id()) != m_readyIdSet.end()) {
                    BOOST_LOG_TRIVIAL(info) << serialNumber << ", unbind disconnect";
                }
                comPtr->disconnect(0);
                break;
            }
        }
    }
    return MultiComUtils::fnetRet2ComErrno(ret);
}

com_id_list_t MultiComMgr::getReadyDevList()
{
    com_id_list_t idList;
    for (auto &id : m_readyIdSet) {
        idList.push_back(id);
    }
    return idList;
}

const com_dev_data_t &MultiComMgr::devData(com_id_t id, bool *valid /* = nullptr */)
{
    auto it = m_ptrMap.left.find(id);
    if (valid != nullptr) {
        *valid = (it != m_ptrMap.left.end());
    }
    if (it == m_ptrMap.left.end()) {
        return m_datMap.at(ComInvalidId);
    } else {
        return m_datMap.at(it->get_left());
    }
}

bool MultiComMgr::putCommand(com_id_t id, ComCommand *command)
{
    ComCommandPtr commandPtr(command);
    auto it = m_ptrMap.left.find(id);
    if (it == m_ptrMap.left.end()) {
        BOOST_LOG_TRIVIAL(error) << "putCommand, invalid com_id_t, " << id;
        return false;
    }
    if (it->second->connectMode() == COM_CONNECT_WAN && (!m_httpOnline || !m_connOnline)) {
        BOOST_LOG_TRIVIAL(error) << "putCommand, invalid com state, " << id;
        return false;
    }
    m_ptrMap.left.at(id)->putCommand(commandPtr);
    return true;
}

bool MultiComMgr::abortSendGcode(com_id_t id, int commandId)
{
    auto it = m_ptrMap.left.find(id);
    if (it == m_ptrMap.left.end()) {
        return false;
    }
    m_ptrMap.left.at(id)->abortSendGcode(commandId);
    return true;
}

bool MultiComMgr::wanSendGcode(const std::vector<std::string> &devIds,
    const std::vector<std::string> &devSerialNumbers, const com_send_gcode_data_t &sendGocdeData)
{
    if (!m_httpOnline || !m_connOnline) {
        return false;
    }
    std::vector<std::string> devTopics(devIds.size());
    for (size_t i = 0; i < devIds.size(); ++i) {
        devTopics[i] = getDevTopic(devIds[i]);
    }
    bool isSend = false;
    int  loop   = 3;
    while (loop--) {
        isSend = m_sendGcodeThd->startSendGcode(m_clientId, devIds, devSerialNumbers, devTopics, sendGocdeData);
        if (isSend)
            break;
    }
    return isSend;
}

bool MultiComMgr::abortWanSendGcode()
{
    return m_sendGcodeThd->abortSendGcode();
}

const std::unordered_map<std::string, std::vector<std::string>>& MultiComMgr::getDevUnupdateList() { 
    return m_unUpdateDevList;
}

void MultiComMgr::initConnection(const com_ptr_t &comPtr, const com_dev_data_t &devData)
{
    m_comPtrs.push_back(comPtr);
    m_ptrMap.insert(com_ptr_map_val_t(comPtr->id(), comPtr.get()));
    m_datMap.emplace(comPtr->id(), devData);
    if (devData.connectMode == COM_CONNECT_WAN) {
        m_devIdMap.emplace(devData.wanDevInfo.devId, comPtr->id());
    }
    auto queueEvent = [this](auto &event) { QueueEvent(event.Clone()); };
    comPtr->Bind(COM_CONNECTION_READY_EVENT, &MultiComMgr::onConnectionReady, this);
    comPtr->Bind(COM_CONNECTION_EXIT_EVENT, &MultiComMgr::onConnectionExit, this);
    comPtr->Bind(COM_DEV_DETAIL_UPDATE_EVENT, &MultiComMgr::onDevDetailUpdate, this);
    comPtr->Bind(COM_GET_DEV_GCODE_LIST_EVENT, &MultiComMgr::onGetDevGcodeList, this);
    comPtr->Bind(COM_GET_GCODE_THUMB_EVENT, [this](auto &event){ QueueEvent(event.MoveClone()); });
    comPtr->Bind(COM_GET_TIME_LAPSE_VIDEO_LIST_EVENT, &MultiComMgr::onGetDevTimeLapseVideoList, this);
    comPtr->Bind(COM_DELETE_TIME_LAPSE_VIDEO_EVENT, queueEvent);
    comPtr->Bind(COM_START_JOB_EVENT, queueEvent);
    comPtr->Bind(COM_SEND_GCODE_PROGRESS_EVENT, queueEvent);
    comPtr->Bind(COM_SEND_GCODE_FINISH_EVENT, queueEvent);
    comPtr->Bind(COMMAND_FAILED_EVENT, &MultiComMgr::onCommandFailed, this);
    comPtr->connect();
}

void MultiComMgr::onTimer(const wxTimerEvent &event)
{
    if (event.GetId() != m_loopCheckTimer.GetId()) {
        return;
    }
    if (!m_pendingWanDevDatas.empty()) {
        if (!m_httpOnline || !m_connOnline) {
            m_pendingWanDevDatas.clear();
            return;
        }
        std::vector<std::string> devTopics;
        for (auto it = m_pendingWanDevDatas.begin(); it != m_pendingWanDevDatas.end();) {
            const com_wan_dev_info_t &wanDevInfo = it->wanDevInfo;
            if (m_devIdMap.find(wanDevInfo.devId) == m_devIdMap.end()) {
                com_ptr_t comPtr = std::make_shared<ComConnection>(m_idNum++, m_clientId,
                    wanDevInfo.serialNumber, wanDevInfo.devId, wanDevInfo.devTopic, networkIntfc());
                initConnection(comPtr, *it);
                if (!wanDevInfo.devTopic.empty()) {
                    devTopics.push_back(wanDevInfo.devTopic);
                }
                it = m_pendingWanDevDatas.erase(it);
            } else {
                ++it;
            }
        }
        if (!devTopics.empty()) {
            ComWanConn::inst()->updateDetail(devTopics);
            ComWanConn::inst()->subscribe(devTopics);
        }
    }
    if (m_connOnline && m_httpOnline) {
        for (auto comId : m_readyIdSet) {
            com_dev_data_t &devData = m_datMap.at(comId);
            if (devData.connectMode == COM_CONNECT_WAN) {
                std::chrono::duration<double> duration = std_precise_clock::now() - m_devAliveTimeMap.at(comId);
                if (duration.count() > 20 && devData.wanDevInfo.status != "offline") {
                    devData.wanDevInfo.status = "offline";
                    QueueEvent(new ComWanDevInfoUpdateEvent(COM_WAN_DEV_INFO_UPDATE_EVENT, comId));
                    BOOST_LOG_TRIVIAL(warning) << devData.wanDevInfo.devId << ", "
                        << devData.wanDevInfo.serialNumber << ", timeout offline";
                }
            }
        }
        if (m_pendingSetUpdateWanDevTime != std_precise_clock::time_point::max()) {
            std::chrono::duration<double> duration = std_precise_clock::now() - m_pendingSetUpdateWanDevTime;
            if (duration.count() > 3) {
                m_wanDevMaintainThd->setUpdateWanDev();
                m_pendingSetUpdateWanDevTime = std_precise_clock::time_point::max();
            }
        }
    }
}

void MultiComMgr::onReloginHttp(ReloginHttpEvent &event)
{
    if (!m_login || event.ret != COM_OK && event.ret != COM_UNAUTHORIZED) {
        m_networkIntfc->freeWanDevList(event.devInfos, event.devCnt);
        return;
    }
    if (event.ret == COM_UNAUTHORIZED) {
        m_networkIntfc->freeWanDevList(event.devInfos, event.devCnt);
        maintianWanDev(event.ret, true);
        return;
    }
    m_httpOnline = true;
    updateWanDevDetail();

    GetWanDevEvent updateWanDevEvent;
    updateWanDevEvent.SetEventType(GET_WAN_DEV_EVENT);
    updateWanDevEvent.ret = event.ret;
    updateWanDevEvent.clientId = event.clientId;
    updateWanDevEvent.devInfos = event.devInfos;
    updateWanDevEvent.devCnt = event.devCnt;
    onUpdateWanDev(updateWanDevEvent);

    QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, true, m_connOnline, COM_OK));
    QueueEvent(new ComGetUserProfileEvent(COM_GET_USER_PROFILE_EVENT, event.userProfile, COM_OK));
}

void MultiComMgr::onUpdateWanDev(const GetWanDevEvent &event)
{
    fnet::FreeInDestructorArg freeDevInfos(event.devInfos, m_networkIntfc->freeWanDevList, event.devCnt);
    if (m_clientId != event.clientId || !m_httpOnline || !m_connOnline) {
        return;
    }
    if (event.ret != COM_OK) {
        maintianWanDev(event.ret, false);
        return;
    }
    m_unUpdateDevList.clear();
    std::map<std::string, fnet_wan_dev_info_t *> devInfoMap;
    for (int i = 0; i < event.devCnt; ++i) {
        const char *devId = event.devInfos[i].devId;
        if (strlen(devId) == 0 || devInfoMap.find(devId) != devInfoMap.end()) {
            BOOST_LOG_TRIVIAL(fatal) << devId << ", empty devId/duplicated devId";
        } else {
            devInfoMap.emplace(devId, &event.devInfos[i]);
            if (std::string(event.devInfos[i].updateInfo.status) == "device") {
                if (std::string(event.devInfos[i].model) == "AD5X") {
                    m_unUpdateDevList["AD5X"].emplace_back(event.devInfos[i].name);
                } else if (std::string(event.devInfos[i].model) == "Adventurer 5M" ||
                           std::string(event.devInfos[i].model) == "Adventurer 5M Pro") {
                    m_unUpdateDevList["5M/5MP"].emplace_back(event.devInfos[i].name);     
                }
            }
        }
    }

    std::vector<std::string> removedDevTopics;
    for (auto &comPtr : m_comPtrs) {
        if (comPtr->connectMode() == COM_CONNECT_WAN && devInfoMap.find(comPtr->devId()) == devInfoMap.end()) {
            comPtr.get()->disconnect(0);
            if (!comPtr->devTopic().empty()) {                                     
                removedDevTopics.push_back(comPtr->devTopic());
            }
        }
    }
    if (!removedDevTopics.empty()) {
        ComWanConn::inst()->unsubscribe(removedDevTopics);
    }
    m_pendingWanDevDatas.clear();
    std::vector<std::string> addedDevTopics;
    for (auto &item : devInfoMap) {
        const fnet_wan_dev_info_t &wanDevInfo = *item.second;
        auto it = m_devIdMap.find(wanDevInfo.devId);
        if (it == m_devIdMap.end()) {
            com_ptr_t comPtr = std::make_shared<ComConnection>(m_idNum++, m_clientId,
                wanDevInfo.serialNumber, wanDevInfo.devId, wanDevInfo.devTopic, networkIntfc());
            initConnection(comPtr, makeWanDevData(&wanDevInfo));
            if (strlen(wanDevInfo.devTopic) != 0) {
                addedDevTopics.push_back(wanDevInfo.devTopic);
            }
        } else if (m_ptrMap.left.at(it->second)->isDisconnect()) {
            m_pendingWanDevDatas.push_back(makeWanDevData(&wanDevInfo));
        }
    }
    if (!addedDevTopics.empty()) {
        ComWanConn::inst()->updateDetail(addedDevTopics);
        ComWanConn::inst()->subscribe(addedDevTopics);
    }
}

void MultiComMgr::onUpdateUserProfile(const ComGetUserProfileEvent &event)
{
    if (!m_httpOnline || !m_connOnline) {
        return;
    }
    if (event.ret == COM_UNAUTHORIZED) {
        maintianWanDev(event.ret, false);
    } else if (event.ret != COM_OK) {
        m_wanDevMaintainThd->setUpdateUserProfile();
    } else {
        QueueEvent(event.Clone());
    }
}

void MultiComMgr::onConnectionReady(const ComConnectionReadyEvent &event)
{
    com_dev_data_t &devData = m_datMap.at(event.id);
    devData.devProduct = event.devProduct;
    if (devData.devDetail == nullptr) {
        devData.devDetail = event.devDetail;
    } else {
        m_networkIntfc->freeDevDetail(event.devDetail);
    }
    m_readyIdSet.insert(event.id);
    if (devData.connectMode == COM_CONNECT_WAN) {
        m_devAliveTimeMap[event.id] = std_precise_clock::now();
    }
    QueueEvent(event.Clone());

    const std::string &serialNumber = m_ptrMap.left.at(event.id)->serialNumber();
    BOOST_LOG_TRIVIAL(info) << serialNumber << ", connection_ready";
    BOOST_LOG_TRIVIAL(info) << "devices count: " << m_readyIdSet.size();
}

void MultiComMgr::onConnectionExit(const ComConnectionExitEvent &event)
{
    if (m_readyIdSet.find(event.id) != m_readyIdSet.end()) {
        const std::string &serialNumber = m_ptrMap.left.at(event.id)->serialNumber();
        BOOST_LOG_TRIVIAL(info) << "devices count: " << m_readyIdSet.size();
        BOOST_LOG_TRIVIAL(info) << serialNumber << ", connection_exit";
    }
    ComConnection *comConnection = m_ptrMap.left.at(event.id);
    comConnection->joinThread();
    com_dev_data_t &devData = m_datMap.at(event.id);
    m_networkIntfc->freeDevProduct(devData.devProduct);
    m_networkIntfc->freeDevDetail(devData.devDetail);
    m_networkIntfc->freeGcodeList(devData.lanGcodeList.gcodeDatas, devData.lanGcodeList.gcodeCnt);
    m_networkIntfc->freeGcodeList(devData.wanGcodeList.gcodeDatas, devData.wanGcodeList.gcodeCnt);
    m_networkIntfc->freeTimeLapseVideoList(devData.wanTimeLapseVideoList.videoDatas,
        devData.wanTimeLapseVideoList.videoCnt);
    m_readyIdSet.erase(event.id);
    if (comConnection->connectMode() == COM_CONNECT_WAN) {
        m_devAliveTimeMap.erase(event.id);
        m_devIdMap.erase(devData.wanDevInfo.devId);
    }
    m_datMap.erase(event.id);
    m_ptrMap.left.erase(event.id);
    m_comPtrs.remove_if([comConnection](auto &ptr) { return ptr.get() == comConnection; });
    QueueEvent(event.Clone());
}

void MultiComMgr::onDevDetailUpdate(const ComDevDetailUpdateEvent &event)
{
    if (m_ptrMap.left.at(event.id)->isDisconnect()) {
        m_networkIntfc->freeDevDetail(event.devDetail);
        return;
    }
    com_dev_data_t &devData = m_datMap.at(event.id);
    m_networkIntfc->freeDevDetail(devData.devDetail);
    devData.devDetail = event.devDetail;
    devData.devDetailUpdated = true;
    if (m_readyIdSet.find(event.id) != m_readyIdSet.end()) {
        QueueEvent(event.Clone());
    }
    if (devData.connectMode != COM_CONNECT_WAN
     || devData.wanDevInfo.name == devData.devDetail->name
     && devData.wanDevInfo.status == devData.devDetail->status
     && devData.wanDevInfo.location == devData.devDetail->location) {
        return;
    }
    BOOST_LOG_TRIVIAL(info) << devData.devDetail->name << " status---" << devData.devDetail->status;
    devData.wanDevInfo.name = devData.devDetail->name;
    devData.wanDevInfo.status = devData.devDetail->status;
    devData.wanDevInfo.location = devData.devDetail->location;
    if (m_readyIdSet.find(event.id) != m_readyIdSet.end()) {
        QueueEvent(new ComWanDevInfoUpdateEvent(COM_WAN_DEV_INFO_UPDATE_EVENT, event.id));
    }
}

void MultiComMgr::onGetDevGcodeList(const ComGetDevGcodeListEvent &event)
{
    com_dev_data_t &devData = m_datMap.at(event.id);
    if (devData.connectMode == COM_CONNECT_LAN) {
        m_networkIntfc->freeGcodeList(devData.lanGcodeList.gcodeDatas, devData.lanGcodeList.gcodeCnt);
        devData.lanGcodeList = event.lanGcodeList;
    } else {
        m_networkIntfc->freeGcodeList(devData.wanGcodeList.gcodeDatas, devData.wanGcodeList.gcodeCnt);
        devData.wanGcodeList = event.wanGcodeList;
    }
    QueueEvent(event.Clone());
}

void MultiComMgr::onGetDevTimeLapseVideoList(const ComGetTimeLapseVideoListEvent &event)
{
    com_dev_data_t &devData = m_datMap.at(event.id);
    m_networkIntfc->freeTimeLapseVideoList(devData.wanTimeLapseVideoList.videoDatas,
        devData.wanTimeLapseVideoList.videoCnt);
    devData.wanTimeLapseVideoList = event.wanTimeLapseVideoList;
    QueueEvent(event.Clone());
}

void MultiComMgr::onCommandFailed(const CommandFailedEvent &event)
{
    if (!m_httpOnline || !m_connOnline) {
        return;
    }
    if (event.fatalError || event.ret == COM_UNAUTHORIZED) {
        maintianWanDev(event.ret, false);
    } else if (!m_blockCommandFailedUpdate) {
        m_blockCommandFailedUpdate = true;
        m_threadPool->post([this]() {
            if (m_commandFailedUpdateTime != std_precise_clock::time_point::min()) {
                std::chrono::duration<double> duration = std_precise_clock::now() - m_commandFailedUpdateTime;
                int waitTime = 600000 - duration.count() * 1000;
                if (waitTime > 0) {
                    m_threadExitEvent.waitTrue(waitTime);
                }
            }
            if (!m_threadExitEvent.get()) {
                m_wanDevMaintainThd->setUpdateWanDev();
                m_commandFailedUpdateTime = std_precise_clock::now();
            }
            m_blockCommandFailedUpdate = false;
        });
    }
}

void MultiComMgr::onWanConnStatus(const WanConnStatusEvent &event)
{
    if (!m_login) {
        return;
    }
    switch (event.status) {
    case FNET_CONN_STATUS_CONNECTED:
        m_connOnline = true;
        QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, true, m_httpOnline, COM_OK));
        if (!m_connFirstConnected) {
            m_wanDevMaintainThd->setUpdateUserProfile();
            m_wanDevMaintainThd->setUpdateWanDev();
            ComWanConn::inst()->subscribe(std::vector<std::string>(1, m_mqttConfig.userTopic));
            ComWanConn::inst()->subscribe(m_mqttConfig.commonTopics);
            subscribeWanDevTopic();
            updateWanDevDetail();
        }
        m_connFirstConnected = false;
        break;
    case FNET_CONN_STATUS_DISCONNECTED:
        m_connOnline = false;
        setWanDevOffline();
        QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, true, false, COM_ERROR));
        break;
    }
}

void MultiComMgr::onWanConnRead(const WanConnReadEvent &event)
{
    auto isSpecialType = [](fnet_conn_read_data_type_t type) {
        return type == FNET_CONN_READ_SYS_NOTIFY
            || type == FNET_CONN_READ_UPDATE_NOTIFY
            || type == FNET_CONN_READ_SYNC_USER_PROFILE
            || type == FNET_CONN_READ_SYNC_UNREGISTER_USER
            || type == FNET_CONN_READ_SYNC_LOGIN
            || type == FNET_CONN_READ_SYNC_BIND_DEVICE
            || type == FNET_CONN_READ_SYNC_UNBIND_DEVICE;
    };
    if (!m_httpOnline && !isSpecialType(event.readData.type) || !m_connOnline) {
        freeConnReadData(event);
        return;
    }
    auto procRepeatLogin = [this](const fnet_conn_read_data_t &readData) {
        fnet_sync_login_info_t *loginInfo = (fnet_sync_login_info_t *)readData.data;
        if (loginInfo == nullptr || loginInfo->clientType == nullptr || loginInfo->clientId == nullptr) {
            return;
        }

        const std::string other_client_type = loginInfo->clientType;
        const std::string other_client_id = loginInfo->clientId;
        if (other_client_type == "pc" && other_client_id != m_clientId) {
            if (is_same_device_client_id(other_client_id, m_clientId)) {
                BOOST_LOG_TRIVIAL(info) << "Ignore repeated pc login from same device, client_id=" << other_client_id;
                return;
            }
            maintianWanDev(COM_OK, true);
        }
    };
    auto procDevOffline = [this](const fnet_conn_read_data_t &readData) {
        BOOST_LOG_TRIVIAL(warning) << "receive device offline, " << ((fnet_sync_online_info_t *)readData.data)->devId;
        auto it = m_devIdMap.find(((fnet_sync_online_info_t *)readData.data)->devId);
        if (it != m_devIdMap.end()) {
            m_datMap.at(it->second).wanDevInfo.status = "offline";
            if (m_readyIdSet.find(it->second) != m_readyIdSet.end()) {
                QueueEvent(new ComWanDevInfoUpdateEvent(COM_WAN_DEV_INFO_UPDATE_EVENT, it->second));
            }
        }
    };
    auto procDevDetailUpdate = [this](const fnet_conn_read_data_t &readData) {
        auto it = m_devIdMap.find(((fnet_dev_detail_t *)readData.data)->devId);
        if (it != m_devIdMap.end()) {
            ComDevDetailUpdateEvent devDetailUpdateEvent(COM_DEV_DETAIL_UPDATE_EVENT,
                it->second, ComInvalidCommandId, (fnet_dev_detail_t *)readData.data);
            onDevDetailUpdate(devDetailUpdateEvent);
            m_devAliveTimeMap[it->second] = std_precise_clock::now(); // may receive a push before ready
        } else {
            m_networkIntfc->freeDevDetail((fnet_dev_detail_t *)readData.data);
        }
    };
    auto procCloudSliceUpdate = [this](const fnet_conn_read_data_t& readData) {
        auto it = m_devIdMap.find(((fnet_slice_state_t*) readData.data)->deviceId);
        if (it != m_devIdMap.end()) {
            auto evt = new ComCloudSliceUpdateEvent(COM_CONN_CLOUD_SLICE_EVENT, it->second, ComInvalidCommandId,
                                                         (fnet_slice_state_t*) readData.data);
            QueueEvent(evt);
        } else {
            m_networkIntfc->freeSliceState((fnet_slice_state_t*) readData.data);
        }
    };
    auto procJobInfoUpdate = [this](const fnet_conn_read_data_t& readData) {
        auto it = m_devIdMap.find(((fnet_job_info_t*) readData.data)->deviceId);
        if (it != m_devIdMap.end()) {
            auto evt = new ComJobInfoUpdateEvent(COM_CONN_JOB_INFO_EVENT, it->second, ComInvalidCommandId, (fnet_job_info_t*) readData.data);
            QueueEvent(evt);
        } else {
            m_networkIntfc->freeJobInfo((fnet_job_info_t*) readData.data);
        }
    };
    auto procDevKeepAlive = [this](const fnet_conn_read_data_t &readData) {
        auto it = m_devIdMap.find((char *)readData.data);
        if (it != m_devIdMap.end()) {
            com_dev_data_t &devData = m_datMap.at(it->second);
            if (devData.devDetail != nullptr && devData.devDetailUpdated) {
                devData.wanDevInfo.status = devData.devDetail->status;
                if (m_readyIdSet.find(it->second) != m_readyIdSet.end()) {
                    QueueEvent(new ComWanDevInfoUpdateEvent(COM_WAN_DEV_INFO_UPDATE_EVENT, it->second));
                }
            }
            m_devAliveTimeMap[it->second] = std_precise_clock::now();
        }
    };
    switch (event.readData.type) {
    case FNET_CONN_READ_SYS_NOTIFY:
        QueueEvent(new ComConnSysNotifyEvent(COM_CONN_SYS_NOTIFY_EVENT, (char *)event.readData.data));
        break;
    case FNET_CONN_READ_UPDATE_NOTIFY: 
        QueueEvent(new ComConnSysNotifyEvent(COM_CONN_UPDATE_NOTIFY_EVENT, (char*) event.readData.data)); 
        break;
    case FNET_CONN_READ_SYNC_USER_PROFILE:
        m_wanDevMaintainThd->setUpdateUserProfile();
        break;
    case FNET_CONN_READ_SYNC_UNREGISTER_USER:
        maintianWanDev(COM_OK, true);
        break;
    case FNET_CONN_READ_SYNC_LOGIN:
        procRepeatLogin(event.readData);
        break;
    case FNET_CONN_READ_SYNC_BIND_DEVICE:
    case FNET_CONN_READ_SYNC_UNBIND_DEVICE:
        m_wanDevMaintainThd->setUpdateWanDev();
        m_pendingSetUpdateWanDevTime = std_precise_clock::time_point::max();
        break;
    case FNET_CONN_READ_SYNC_OFFLINE:
        procDevOffline(event.readData);
        break;
    case FNET_CONN_READ_DEVICE_DETAIL:
        procDevDetailUpdate(event.readData);
        break;
    case FNET_CONN_READ_SLICE_STATE: 
        procCloudSliceUpdate(event.readData);
        break;
    case FNET_CONN_READ_JOB_DOWNLOAD:
    case FNET_CONN_READ_JOB_UNZIP: 
        procJobInfoUpdate(event.readData); 
        break;
    case FNET_CONN_READ_DEVICE_KEEP_ALIVE:
        procDevKeepAlive(event.readData);
        break;
    }
    if (event.readData.type == FNET_CONN_READ_DEVICE_DETAIL) {
        m_networkIntfc->freeString(event.readData.topic);
    } else {
        freeConnReadData(event);
    }
}

void MultiComMgr::onRefreshToken(const ComRefreshTokenEvent &event)
{
    if (!m_login || event.ret != COM_OK && event.ret != COM_UNAUTHORIZED) {
        return;
    }
    if (event.ret == COM_UNAUTHORIZED) {
        maintianWanDev(event.ret, true);
        return;
    }
    QueueEvent(event.Clone());
}

std::string MultiComMgr::generateClientId()
{
    std::string uuidStr = boost::uuids::to_string(boost::uuids::random_generator()());
    uuidStr = sanitized_client_id_part(uuidStr);
    return "pc_" + get_device_client_id_part() + "_" + uuidStr;
}

std::string MultiComMgr::getDevTopic(const std::string &devId)
{
    auto devIdIt = m_devIdMap.find(devId);
    if (devIdIt == m_devIdMap.end()) {
        return std::string();
    }
    auto comIdIt = m_datMap.find(devIdIt->second);
    if (comIdIt == m_datMap.end()) {
        return std::string();
    }
    if (comIdIt->second.connectMode != COM_CONNECT_WAN) {
        return std::string();
    }
    return comIdIt->second.wanDevInfo.devTopic;
}

com_dev_data_t MultiComMgr::makeWanDevData(const fnet_wan_dev_info_t *wanDevInfo)
{
    com_dev_data_t devData;
    devData.connectMode = COM_CONNECT_WAN;
    devData.wanDevInfo.devId = wanDevInfo->devId;
    devData.wanDevInfo.name = wanDevInfo->name;
    devData.wanDevInfo.model = wanDevInfo->model;
    devData.wanDevInfo.imageUrl = wanDevInfo->imageUrl;
    devData.wanDevInfo.status = "offline";
    devData.wanDevInfo.location = wanDevInfo->location;
    devData.wanDevInfo.serialNumber = wanDevInfo->serialNumber;
    devData.wanDevInfo.devTopic = wanDevInfo->devTopic;
    devData.wanDevInfo.updateInfo.status = wanDevInfo->updateInfo.status;
    devData.wanDevInfo.updateInfo.title = wanDevInfo->updateInfo.title;
    devData.wanDevInfo.updateInfo.content = wanDevInfo->updateInfo.content;
    devData.wanDevInfo.updateInfo.tips = wanDevInfo->updateInfo.tips;
    devData.devProduct = nullptr;
    devData.devDetail = nullptr;
    devData.lanGcodeList.gcodeCnt = 0;
    devData.lanGcodeList.gcodeDatas = nullptr;
    devData.wanGcodeList.gcodeCnt = 0;
    devData.wanGcodeList.gcodeDatas = nullptr;
    devData.wanTimeLapseVideoList.videoCnt = 0;
    devData.wanTimeLapseVideoList.videoDatas = nullptr;
    devData.devDetailUpdated = false;
    memset(&devData.lanDevInfo, 0, sizeof(devData.lanDevInfo));
    return devData;
}

void MultiComMgr::maintianWanDev(ComErrno ret, bool needLogout)
{
    if (!m_login) {
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "MultiComMgr::maintianWanDev " << (int)ret;
    if (needLogout) {
        removeWanDev();
        QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, false, false, ret));
        return;
    }
    if (ret != COM_OK && m_httpOnline) {
        m_httpOnline = false;
        m_wanDevMaintainThd->setReloginHttp();
        setWanDevOffline();
        QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, true, false, ret));
    }
}

void MultiComMgr::initWanConnAsync(const std::string &accessToken)
{
    if (!m_threadPool || networkIntfc() == nullptr) {
        return;
    }

    const std::string clientId = m_clientId;
    m_threadPool->post([this, clientId, accessToken]() {
        if (m_threadExitEvent.get()) {
            return;
        }

        com_mqtt_config_t mqttConfig;
        ComErrno ret = MultiComUtils::getMqttConfig(clientId, accessToken, mqttConfig, ComTimeoutWanA);
        if (ret != COM_OK) {
            BOOST_LOG_TRIVIAL(warning) << "MultiComMgr::initWanConnAsync getMqttConfig failed: " << (int) ret;
            if (ret == COM_UNAUTHORIZED) {
                maintianWanDev(ret, false);
            } else if (m_login && m_clientId == clientId) {
                m_connOnline = false;
                QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, true, false, ret));
            }
            return;
        }

        if (m_threadExitEvent.get() || !m_login || m_clientId != clientId) {
            return;
        }

        m_mqttConfig = mqttConfig;
        ret = ComWanConn::inst()->createConn(networkIntfc(), clientId.c_str());
        if (ret != COM_OK) {
            BOOST_LOG_TRIVIAL(warning) << "MultiComMgr::initWanConnAsync createConn failed: " << (int) ret;
            if (m_login && m_clientId == clientId) {
                m_connOnline = false;
                QueueEvent(new ComWanDevMaintainEvent(COM_WAN_DEV_MAINTAIN_EVENT, true, false, ret));
            }
            return;
        }

        if (m_threadExitEvent.get() || !m_login || m_clientId != clientId) {
            if (!m_login && m_clientId == clientId) {
                ComWanConn::inst()->freeConn();
            }
            return;
        }

        ComWanConn::inst()->subscribe(std::vector<std::string>(1, m_mqttConfig.userTopic));
        ComWanConn::inst()->subscribe(m_mqttConfig.commonTopics);
        ComWanConn::inst()->syncLogin(m_mqttConfig.userTopic);
        WanDevTokenMgr::inst()->start();
        m_wanDevMaintainThd->setUpdateWanDev();
    });
}

void MultiComMgr::setMaintainThdReqHeader(bool isCheckVersionOnTestServer)
{
#ifdef _WIN32
    int64_t appId = 31;
    int64_t platId = 14;
    if (isCheckVersionOnTestServer) {
        appId = 46;
        platId = 20;
    }
#else // Mac OS
    int64_t appId = 31;
    int64_t platId = 15;
    if (isCheckVersionOnTestServer) {
        appId = 46;
        platId = 19;
    }
#endif
    m_wanDevMaintainThd->setReqHeaders(m_clientId, appId, platId);
}

void MultiComMgr::setWanDevOffline()
{
    for (auto &item : m_ptrMap.left) {
        if (item.second->connectMode() == COM_CONNECT_WAN) {
            com_dev_data_t &devData = m_datMap.at(item.first);
            devData.wanDevInfo.status = "offline";
            QueueEvent(new ComWanDevInfoUpdateEvent(COM_WAN_DEV_INFO_UPDATE_EVENT, item.first));
        }
    }
}

void MultiComMgr::subscribeWanDevTopic()
{
    if (!m_connOnline) {
        return;
    }
    std::vector<std::string> devTopics;
    for (auto &item : m_ptrMap.left) {
        if (item.second->connectMode() == COM_CONNECT_WAN
        && !item.second->isDisconnect()
        && !item.second->devTopic().empty()) {
            devTopics.push_back(item.second->devTopic());
        }
    }
    if (!devTopics.empty()) {
        ComWanConn::inst()->subscribe(devTopics);
    }
}

void MultiComMgr::updateWanDevDetail()
{
    if (!m_httpOnline || !m_connOnline) {
        return;
    }
    std::vector<std::string> devTopics;
    for (auto &item : m_ptrMap.left) {
        if (item.second->connectMode() == COM_CONNECT_WAN
        && !item.second->isDisconnect()
        && !item.second->devTopic().empty()) {
            devTopics.push_back(item.second->devTopic());
        }
    }
    if (!devTopics.empty()) {
        ComWanConn::inst()->updateDetail(devTopics);
    }
}

void MultiComMgr::freeConnReadData(const WanConnReadEvent &event)
{
    switch (event.readData.type) {
    case FNET_CONN_READ_SYS_NOTIFY:
    case FNET_CONN_READ_UPDATE_NOTIFY:
    case FNET_CONN_READ_DEVICE_KEEP_ALIVE:
        m_networkIntfc->freeString((char *)event.readData.data);
        break;
    case FNET_CONN_READ_SYNC_USER_PROFILE:
    case FNET_CONN_READ_SYNC_UNREGISTER_USER:
        break;
    case FNET_CONN_READ_SYNC_LOGIN:
        m_networkIntfc->freeSyncLoginInfo((fnet_sync_login_info_t *)event.readData.data);
        break;
    case FNET_CONN_READ_SYNC_BIND_DEVICE:
    case FNET_CONN_READ_SYNC_UNBIND_DEVICE:
        m_networkIntfc->freeSyncBindInfo((fnet_sync_bind_info *)event.readData.data);
        break;
    case FNET_CONN_READ_SYNC_ONLINE:
    case FNET_CONN_READ_SYNC_OFFLINE:
        m_networkIntfc->freeSyncOnlineInfo((fnet_sync_online_info_t *)event.readData.data);
        break;
    }
    m_networkIntfc->freeString(event.readData.topic);
}

}} // namespace Slic3r::GUI
