#include "FlashNetworkIntfc.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <boost/log/trivial.hpp>
#include "FFDiagnostics.hpp"

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace fnet {

using Slic3r::GUI::FFNetInitStage;
using Slic3r::GUI::ff_flashnetwork_init_error;

// EdgeSlicer: the DLL shipped since 2.4.0 (Flash Studio Desktop 1.7.13) reports 3.4.2 and exports
// the full fnet_* surface this wrapper binds. The original port pinned an exact "3.4.1" string,
// which should not gate loading. Accept any 3.x.y and record the loaded version in the log so a
// future ABI break is at least visible there.
static bool fnet_version_usable(const char *version)
{
    if (version == nullptr)
        return false;
    unsigned int major = 0, minor = 0, patch = 0;
    if (sscanf(version, "%u.%u.%u", &major, &minor, &patch) < 2)
        return false;
    return major == 3;
}

void FlashNetworkIntfc::fail(const std::string &reason)
{
    m_error = reason;
    BOOST_LOG_TRIVIAL(error) << "FlashNetwork: " << reason;
}

FlashNetworkIntfc::FlashNetworkIntfc(const char *libraryPath, const char *serverSettingsPath,
    const fnet_log_settings_t &logSettings, bool serverSettingsFound)
    : m_isOk(false)
    , m_libraryPath(libraryPath == nullptr ? "" : libraryPath)
{
    library_handle_t libraryHandle = loadLibrary(libraryPath);
    if (libraryHandle == INVALID_LIBRARY_HANDLE) {
        return;
    }
#define INIT_FUNC_PTR(ptr, func) \
    ptr = (decltype(&func)) getFuncPtr(libraryHandle, #func); \
    if (ptr == nullptr) { \
        fail(ff_flashnetwork_init_error(FFNetInitStage::MissingSymbol, m_libraryPath, #func, 0)); \
        return; \
    }
    INIT_FUNC_PTR(initlize, fnet_initlize);
    INIT_FUNC_PTR(uninitlize, fnet_uninitlize);
    INIT_FUNC_PTR(getVersion, fnet_getVersion);
    INIT_FUNC_PTR(getHomePageUrl, fnet_getHomePageUrl);
    INIT_FUNC_PTR(setUserAgent, fnet_setUserAgent);
    INIT_FUNC_PTR(getLanDevList, fnet_getLanDevList);
    INIT_FUNC_PTR(freeLanDevInfos, fnet_freeLanDevInfos);
    INIT_FUNC_PTR(getLanDevProduct, fnet_getLanDevProduct);
    INIT_FUNC_PTR(freeDevProduct, fnet_freeDevProduct);
    INIT_FUNC_PTR(getLanDevDetail, fnet_getLanDevDetail);
    INIT_FUNC_PTR(freeDevDetail, fnet_freeDevDetail);
    INIT_FUNC_PTR(getLanDevGcodeList, fnet_getLanDevGcodeList);
    INIT_FUNC_PTR(freeGcodeList, fnet_freeGcodeList);
    INIT_FUNC_PTR(getLanDevGcodeThumb, fnet_getLanDevGcodeThumb);
    INIT_FUNC_PTR(lanDevStartJob, fnet_lanDevStartJob);
    INIT_FUNC_PTR(ctrlLanDevTemp, fnet_ctrlLanDevTemp);
    INIT_FUNC_PTR(ctrlLanDevLight, fnet_ctrlLanDevLight);
    INIT_FUNC_PTR(ctrlLanDevAirFilter, fnet_ctrlLanDevAirFilter);
    INIT_FUNC_PTR(ctrlLanDevClearFan, fnet_ctrlLanDevClearFan);
    INIT_FUNC_PTR(ctrlLanDevMove, fnet_ctrlLanDevMove);
    INIT_FUNC_PTR(ctrlLanDevExtrude, fnet_ctrlLanDevExtrude);
    INIT_FUNC_PTR(ctrlLanDevHoming, fnet_ctrlLanDevHoming);
    INIT_FUNC_PTR(ctrlLanDevMatlStation, fnet_ctrlLanDevMatlStation);
    INIT_FUNC_PTR(ctrlLanDevIndepMatl, fnet_ctrlLanDevIndepMatl);
    INIT_FUNC_PTR(ctrlLanDevPrint, fnet_ctrlLanDevPrint);
    INIT_FUNC_PTR(ctrlLanDevJob, fnet_ctrlLanDevJob);
    INIT_FUNC_PTR(ctrlLanDevState, fnet_ctrlLanDevState);
    INIT_FUNC_PTR(ctrlLanDevErrorCode, fnet_ctrlLanDevErrorCode);
    INIT_FUNC_PTR(ctrlLanDevPlateDetect, fnet_ctrlLanDevPlateDetect);
    INIT_FUNC_PTR(ctrlLanDevFirstLayerDetect, fnet_ctrlLanDevFirstLayerDetect);
    INIT_FUNC_PTR(configLanDevMatlStation, fnet_configLanDevMatlStation);
    INIT_FUNC_PTR(configLanDevIndepMatl, fnet_configLanDevIndepMatl);
    INIT_FUNC_PTR(lanDevSendGcode, fnet_lanDevSendGcode);
    INIT_FUNC_PTR(notifyLanDevWanBind, fnet_notifyLanDevWanBind);
    INIT_FUNC_PTR(downloadFileMem, fnet_downloadFileMem);
    INIT_FUNC_PTR(downloadFileDisk, fnet_downloadFileDisk);
    INIT_FUNC_PTR(freeFileData, fnet_freeFileData);
    INIT_FUNC_PTR(getTokenByPassword, fnet_getTokenByPassword);
    INIT_FUNC_PTR(refreshToken, fnet_refreshToken);
    INIT_FUNC_PTR(freeToken, fnet_freeToken);
    INIT_FUNC_PTR(sendSMSCode, fnet_sendSMSCode);
    INIT_FUNC_PTR(getTokenBySMSCode, fnet_getTokenBySMSCode);
    INIT_FUNC_PTR(signOut, fnet_signOut);
    INIT_FUNC_PTR(getUserProfile, fnet_getUserProfile);
    INIT_FUNC_PTR(freeUserProfile, fnet_freeUserProfile);
    INIT_FUNC_PTR(bindWanDev, fnet_bindWanDev);
    INIT_FUNC_PTR(freeBindData, fnet_freeBindData);
    INIT_FUNC_PTR(unbindWanDev, fnet_unbindWanDev);
    INIT_FUNC_PTR(getWanDevList, fnet_getWanDevList);
    INIT_FUNC_PTR(freeWanDevList, fnet_freeWanDevList);
    INIT_FUNC_PTR(getWanDevProductDetail, fnet_getWanDevProductDetail);
    INIT_FUNC_PTR(getWanDevGcodeList, fnet_getWanDevGcodeList);
    INIT_FUNC_PTR(getWanDevTimeLapseVideoList, fnet_getWanDevTimeLapseVideoList);
    INIT_FUNC_PTR(freeTimeLapseVideoList, fnet_freeTimeLapseVideoList);
    INIT_FUNC_PTR(deleteTimeLapseVideo, fnet_deleteTimeLapseVideo);
    INIT_FUNC_PTR(wanDevAddJob, fnet_wanDevAddJob);
    INIT_FUNC_PTR(freeAddJobResult, fnet_freeAddJobResult);
    INIT_FUNC_PTR(wanDevSendGcodeClound, fnet_wanDevSendGcodeClound);
    INIT_FUNC_PTR(freeCloundGcodeData, fnet_freeCloundGcodeData);
    INIT_FUNC_PTR(wanDevAddCloundJob, fnet_wanDevAddCloundJob);
    INIT_FUNC_PTR(freeAddCloudJobResults, fnet_freeAddCloudJobResults);
    INIT_FUNC_PTR(bindAccountRelp, fnet_bindAccountRelp);
    INIT_FUNC_PTR(freeBindAccountRelpResult, fnet_freeBindAccountRelpResult);
    INIT_FUNC_PTR(uploadAiImageClound, fnet_uploadAiImageClound);
    INIT_FUNC_PTR(uploadLogFileCloud, fnet_uploadLogFileCloud);
    INIT_FUNC_PTR(freeCloundFileData, fnet_freeCloundFileData);
    INIT_FUNC_PTR(getUserAiPointsInfo, fnet_getUserAiPointsInfo);
    INIT_FUNC_PTR(freeUserAiPointsInfo, fnet_freeUserAiPointsInfo);
    INIT_FUNC_PTR(createAiJobPipeline, fnet_createAiJobPipeline);
    INIT_FUNC_PTR(freeAiJobPipelineInfo, fnet_freeAiJobPipelineInfo);
    INIT_FUNC_PTR(startAiModelJob, fnet_startAiModelJob);
    INIT_FUNC_PTR(freeStartAiModelJobResult, fnet_freeStartAiModelJobResult);
    INIT_FUNC_PTR(getAiModelJobState, fnet_getAiModelJobState);
    INIT_FUNC_PTR(freeAiModelJobState, fnet_freeAiModelJobState);
    INIT_FUNC_PTR(abortAiModelJob, fnet_abortAiModelJob);
    INIT_FUNC_PTR(getExistingAiModelJob, fnet_getExistingAiModelJob);
    INIT_FUNC_PTR(startAiImg2imgJob, fnet_startAiImg2imgJob);
    INIT_FUNC_PTR(startAiTxt2txtJob, fnet_startAiTxt2txtJob);
    INIT_FUNC_PTR(startAiTxt2imgJob, fnet_startAiTxt2imgJob);
    INIT_FUNC_PTR(freeStartAiGeneralJobResult, fnet_freeStartAiGeneralJobResult);
    INIT_FUNC_PTR(getAiImg2imgJobState, fnet_getAiImg2imgJobState);
    INIT_FUNC_PTR(getAiTxt2txtJobState, fnet_getAiTxt2txtJobState);
    INIT_FUNC_PTR(getAiTxt2imgJobState, fnet_getAiTxt2imgJobState);
    INIT_FUNC_PTR(freeAiGeneralJobState, fnet_freeAiGeneralJobState);
    INIT_FUNC_PTR(abortAiImg2imgJob, fnet_abortAiImg2imgJob);
    INIT_FUNC_PTR(abortAiTxt2txtJob, fnet_abortAiTxt2txtJob);
    INIT_FUNC_PTR(abortAiTxt2imgJob, fnet_abortAiTxt2imgJob);
    INIT_FUNC_PTR(userClickCount, fnet_userClickCount);
    INIT_FUNC_PTR(addPrintListModel, fnet_addPrintListModel);
    INIT_FUNC_PTR(removePrintListModel, fnet_removePrintListModel);
    INIT_FUNC_PTR(navLikeModel, fnet_navLikeModel);
    INIT_FUNC_PTR(reportModel, fnet_reportModel);
    INIT_FUNC_PTR(reportTrackingData, fnet_reportTrackingData);
    INIT_FUNC_PTR(reportTrackingDataBatch, fnet_reportTrackingDataBatch);
    INIT_FUNC_PTR(getSystemMessage, fnet_getSystemMessage);
    INIT_FUNC_PTR(postReadSystemMessage, fnet_postReadSystemMessage);
    INIT_FUNC_PTR(freeSystemMessage, fnet_freeSystemMessage);
    INIT_FUNC_PTR(getMonitorMessage, fnet_getMonitorMessage);
    INIT_FUNC_PTR(retrySliceTask, fnet_retrySliceTask);
    INIT_FUNC_PTR(cancelSliceTask, fnet_cancelSliceTask);
    INIT_FUNC_PTR(freeSliceState, fnet_freeSliceState);
    INIT_FUNC_PTR(freeJobInfo, fnet_freeJobInfo);
    INIT_FUNC_PTR(doBusGetRequest, fnet_doBusGetRequest);
    INIT_FUNC_PTR(doBusPostRequest, fnet_doBusPostRequest);
    INIT_FUNC_PTR(getMqttConfig, fnet_getMqttConfig);
    INIT_FUNC_PTR(freeMqttConfig, fnet_freeMqttConfig);
    INIT_FUNC_PTR(createConnection, fnet_createConnection);
    INIT_FUNC_PTR(freeConnection, fnet_freeConnection);
    INIT_FUNC_PTR(connectionStop, fnet_connectionStop);
    INIT_FUNC_PTR(connectionSend, fnet_connectionSend);
    INIT_FUNC_PTR(connectionSendMulti, fnet_connectionSendMulti);
    INIT_FUNC_PTR(connectionSubscribe, fnet_connectionSubscribe);
    INIT_FUNC_PTR(connectionUnsubscribe, fnet_connectionUnsubscribe);
    INIT_FUNC_PTR(freeWriteMultiResult, fnet_freeWriteMultiResult);
    INIT_FUNC_PTR(freeSyncLoginInfo, fnet_freeSyncLoginInfo);
    INIT_FUNC_PTR(freeSyncBindInfo, fnet_freeSyncBindInfo);
    INIT_FUNC_PTR(freeSyncOnlineInfo, fnet_freeSyncOnlineInfo);
    INIT_FUNC_PTR(allocString, fnet_allocString);
    INIT_FUNC_PTR(freeString, fnet_freeString);
#undef INIT_FUNC_PTR
    const char *versionPtr = getVersion();
    const std::string version = versionPtr == nullptr ? std::string() : std::string(versionPtr);
    BOOST_LOG_TRIVIAL(info) << "FlashNetwork: loaded " << m_libraryPath << ", library version "
                            << (version.empty() ? "(none)" : version);
    // Checked before fnet_initlize so a library with an ABI we do not speak is never driven.
    if (!fnet_version_usable(versionPtr)) {
        fail(ff_flashnetwork_init_error(FFNetInitStage::BadVersion, m_libraryPath, version, 0));
        return;
    }
    const std::string settings = serverSettingsPath == nullptr ? std::string() : std::string(serverSettingsPath);
    const int ret = initlize(serverSettingsPath, &logSettings);
    if (ret != FNET_OK) {
        fail(ff_flashnetwork_init_error(FFNetInitStage::InitFailed, m_libraryPath, settings, ret,
                                        serverSettingsFound));
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "FlashNetwork initialized, library version " << version
                            << ", server settings " << settings;
    m_isOk = true;
}

FlashNetworkIntfc::~FlashNetworkIntfc()
{
    if (m_isOk) {
        uninitlize();
    }
}

library_handle_t FlashNetworkIntfc::loadLibrary(const char *libraryPath)
{
    if (libraryPath == nullptr || *libraryPath == '\0') {
        fail(ff_flashnetwork_init_error(FFNetInitStage::LoadFailed, m_libraryPath, "no path given", 0));
        return INVALID_LIBRARY_HANDLE;
    }
#ifdef _WIN32
    // Sized from the input rather than a fixed buffer: a long install path used to be cut off.
    const int len = (int)strlen(libraryPath);
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, libraryPath, len, nullptr, 0);
    std::vector<wchar_t> wpath((size_t)wlen + 1, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, libraryPath, len, wpath.data(), wlen);
    library_handle_t handle = LoadLibraryW(wpath.data());
    if (handle == INVALID_LIBRARY_HANDLE) {
        const DWORD code = GetLastError();
        std::string sysText;
        LPSTR buf = nullptr;
        const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                             FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr, code, 0, (LPSTR)&buf, 0, nullptr);
        if (n != 0 && buf != nullptr) {
            sysText.assign(buf, n);
            while (!sysText.empty() && (sysText.back() == '\r' || sysText.back() == '\n' || sysText.back() == ' ' || sysText.back() == '.'))
                sysText.pop_back();
        }
        if (buf != nullptr)
            ::LocalFree(buf);
        fail(ff_flashnetwork_init_error(FFNetInitStage::LoadFailed, m_libraryPath, sysText, (long)code));
    }
    return handle;
#else
    library_handle_t handle = dlopen(libraryPath, RTLD_LAZY);
    if (handle == nullptr) {
        const char *dllError = dlerror();
        fail(ff_flashnetwork_init_error(FFNetInitStage::LoadFailed, m_libraryPath,
                                        dllError == nullptr ? "" : dllError, 0));
    }
    return handle;
#endif
}

void *FlashNetworkIntfc::getFuncPtr(library_handle_t libraryHandle, const char *funcName)
{
    // A missing export is reported (once, with its name) by the INIT_FUNC_PTR caller.
#ifdef _WIN32
    return (void *)GetProcAddress(libraryHandle, funcName);
#else
    return dlsym(libraryHandle, funcName);
#endif
}

} // namespace fnet
