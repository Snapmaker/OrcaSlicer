#ifndef slic3r_GUI_MultiComMgr_hpp_
#define slic3r_GUI_MultiComMgr_hpp_

#include <atomic>
#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <boost/bimap.hpp>
#include <wx/event.h>
#include <wx/timer.h>
#include "ComConnection.hpp"
#include "ComThreadPool.hpp"
#include "ComWanConn.hpp"
#include "FlashNetworkIntfc.h"
#include "MultiComDef.hpp"
#include "MultiComEvent.hpp"
#include "Singleton.hpp"
#include "WaitEvent.hpp"
#include "WanDevMaintainThd.hpp"
#include "WanDevSendGcodeThd.hpp"

namespace boost { namespace interprocess {
    class file_lock;
}}

namespace Slic3r { namespace GUI {

class MultiComMgr : public wxEvtHandler, public Singleton<MultiComMgr>
{
public:
    MultiComMgr();

    bool initalize(const std::string &dllPath, const std::string &dataDir);

    // FlashNetwork fixes its log level in fnet_initlize and offers no runtime setter, so raising
    // it means tearing the stack down and bringing it back up. Diagnostics does exactly that, so
    // a user can produce debug logs without creating the FLASHNETWORK_DEBUG marker file by hand
    // and restarting. Returns false when the stack was never up, or the re-init failed (in which
    // case it is left down and the caller should say so).
    bool setDebugLogging(bool debug);

    bool debugLogging() const { return m_debugLogging; }

    // Where FlashNetwork writes its logs: <data dir>/FlashNetwork. Empty before initalize().
    const std::string &logFileDir() const { return m_logFileDir; }

    // The library version string (fnet_getVersion), or empty when nothing is loaded.
    std::string libraryVersion();

    // Why the last initalize() failed, naming the step (load, missing export, version, or
    // fnet_initlize with its return code and settings path). Empty after a success.
    const std::string &lastInitError() const { return m_lastInitError; }

    void uninitalize();

    fnet::FlashNetworkIntfc *networkIntfc();

    std::string homePageUrl();
    
    com_id_t addLanDev(const fnet_lan_dev_info_t &devInfo, const std::string &checkCode);

    void removeLanDev(com_id_t id);

    ComErrno addWanDev(const com_token_data_t &tokenData, bool isCheckVersionOnTestServer,
        com_add_wan_dev_data_t &addDevData, int tryCnt, int tryMsInterval);

    void removeWanDev();

    ComErrno bindWanDev(const std::string &ip, unsigned short port, const std::string &serialNumber,
        unsigned short pid, const std::string &name, unsigned short bindType);

    ComErrno unbindWanDev(const std::string &serialNumber, const std::string &devId);

    com_id_list_t getReadyDevList();

    const com_dev_data_t &devData(com_id_t id, bool *valid = nullptr);

    bool putCommand(com_id_t id, ComCommand *command); // this method takes ownership of the command, i.e. it will delete it itself

    bool abortSendGcode(com_id_t id, int commandId);

    bool wanSendGcode(const std::vector<std::string> &devIds, const std::vector<std::string> &devSerialNumbers,
        const com_send_gcode_data_t &sendGocdeData);

    bool abortWanSendGcode();

    const std::unordered_map<std::string, std::vector<std::string>>& getDevUnupdateList();

private:
    using std_precise_clock = std::chrono::high_resolution_clock;

    typedef std::map<com_id_t, std_precise_clock::time_point> dev_alive_time_map_t;

    typedef std::shared_ptr<ComConnection> com_ptr_t;

    typedef boost::bimap<com_id_t, ComConnection*> com_ptr_map_t;

    typedef boost::bimap<com_id_t, ComConnection*>::value_type com_ptr_map_val_t;
    
    void initConnection(const com_ptr_t &comPtr, const com_dev_data_t &devData);

    void onTimer(const wxTimerEvent &event);

    void onReloginHttp(ReloginHttpEvent &event);

    void onUpdateWanDev(const GetWanDevEvent &event);
    
    void onUpdateUserProfile(const ComGetUserProfileEvent &event);

    void onConnectionReady(const ComConnectionReadyEvent &event);

    void onConnectionExit(const ComConnectionExitEvent &event);

    void onDevDetailUpdate(const ComDevDetailUpdateEvent &event);

    void onGetDevGcodeList(const ComGetDevGcodeListEvent &event);

    void onGetDevTimeLapseVideoList(const ComGetTimeLapseVideoListEvent &event);

    void onCommandFailed(const CommandFailedEvent &event);

    void onWanConnStatus(const WanConnStatusEvent &event);

    void onWanConnRead(const WanConnReadEvent &event);

    void onRefreshToken(const ComRefreshTokenEvent &event);

    std::string generateClientId();

    std::string getDevTopic(const std::string &devId);

    com_dev_data_t makeWanDevData(const fnet_wan_dev_info_t *wanDevInfo);

    void maintianWanDev(ComErrno ret, bool needLogout);

    void setMaintainThdReqHeader(bool isCheckVersionTestServer);

    void initWanConnAsync(const std::string &accessToken);

    void setWanDevOffline();

    void subscribeWanDevTopic();

    void updateWanDevDetail();

    void freeConnReadData(const WanConnReadEvent &event);

private:
    int                                      m_idNum;
    bool                                     m_login;
    bool                                     m_httpOnline;
    bool                                     m_connOnline;
    bool                                     m_connFirstConnected;
    std::string                              m_clientId;
    com_mqtt_config_t                        m_mqttConfig;
    std::list<com_ptr_t>                     m_comPtrs;
    com_ptr_map_t                            m_ptrMap;
    std::map<com_id_t, com_dev_data_t>       m_datMap;
    std::set<com_id_t>                       m_readyIdSet;
    std::unordered_map<std::string, std::vector<std::string>> m_unUpdateDevList;
    std::map<std::string, com_id_t>          m_devIdMap;
    dev_alive_time_map_t                     m_devAliveTimeMap;
    std::list<com_dev_data_t>                m_pendingWanDevDatas;
    wxTimer                                  m_loopCheckTimer;
    std::atomic_bool                         m_blockCommandFailedUpdate;
    std_precise_clock::time_point            m_commandFailedUpdateTime;
    std_precise_clock::time_point            m_pendingSetUpdateWanDevTime;
    std::unique_ptr<WanDevMaintainThd>       m_wanDevMaintainThd;
    std::unique_ptr<WanDevSendGcodeThd>      m_sendGcodeThd;
    std::unique_ptr<fnet::FlashNetworkIntfc> m_networkIntfc;
    // Remembered so setDebugLogging() can re-initalize with a different log level.
    std::string m_dllPath;
    std::string m_dataDir;
    std::string m_logFileDir;
    std::string m_lastInitError;
    bool        m_debugLogging { false };
    std::unique_ptr<ComThreadPool>           m_threadPool;
    WaitEvent                                m_threadExitEvent;
};

}} // namespace Slic3r::GUI

#endif
