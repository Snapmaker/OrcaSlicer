#include "MultiComHelper.hpp"
#include <boost/log/trivial.hpp>
#include "MultiComEvent.hpp"
#include "MultiComMgr.hpp"
#include "MultiComUtils.hpp"
#include "WanDevTokenMgr.hpp"

namespace Slic3r { namespace GUI {

MultiComHelper::MultiComHelper()
    : m_threadPool(5, 30000)
    , m_requestNum(0)
{
}

void MultiComHelper::loginInit(const std::string &clientId, const std::string &uid)
{
    m_clinetId = clientId;
    m_uid = uid;
}

void MultiComHelper::userClickCount(const std::string &source, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return;
    }
    m_threadPool.post([=]() {
        ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->userClickCount(
            m_clinetId.c_str(), token.accessToken().c_str(), source.c_str(), msTimeout));
        if (ret != COM_OK) {
            BOOST_LOG_TRIVIAL(error) << "aiModelClickCount error, " << (int)ret;
        }
    });
}

void MultiComHelper::reportTrackingData(const com_tracking_common_data_t &commonData,
    const com_tracking_event_data_t &eventData, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return;
    }
    m_threadPool.post([=]() {
        fnet_tracking_common_data_t fnetCommonData;
        fnetCommonData.uid = commonData.uid.c_str();
        fnetCommonData.did = commonData.did.c_str();
        fnetCommonData.sid = commonData.sid.c_str();
        fnetCommonData.netType = commonData.netType.c_str();
        fnetCommonData.oper = commonData.oper.c_str();
        fnetCommonData.ext = commonData.ext.c_str();

        fnet_tracking_event_data_t fnetEventData;
        fnetEventData.eventType = eventData.eventType.c_str();
        fnetEventData.eventId = eventData.eventId.c_str();
        fnetEventData.eventName = eventData.eventName.c_str();
        fnetEventData.pageId = eventData.pageId.c_str();
        fnetEventData.moduleId = eventData.moduleId.c_str();
        fnetEventData.reqId = eventData.reqId.c_str();
        fnetEventData.expIds = eventData.expIds.c_str();
        fnetEventData.objectType = eventData.objectType.c_str();
        fnetEventData.objectId = eventData.objectId.c_str();
        fnetEventData.extend = eventData.extend.c_str();
        fnetEventData.timestamp = eventData.timestamp.c_str();

        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->reportTrackingData(m_clinetId.c_str(),
            &fnetCommonData, &fnetEventData, msTimeout));
        if (ret != COM_OK) {
            BOOST_LOG_TRIVIAL(error) << "reportTrackingData error, " << (int)ret;
        }
    });
}

void MultiComHelper::reportTrackingDataBatch(const com_tracking_common_data_t &commonData,
    const std::vector<com_tracking_event_data_t> &eventDatas, int msTimeout)
{
    m_threadPool.post([=]() {
        reportTrackingDataBatchSync(commonData, eventDatas, msTimeout);
    });
}

void MultiComHelper::reportTrackingDataBatchSync(const com_tracking_common_data_t &commonData,
    const std::vector<com_tracking_event_data_t> &eventDatas, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return;
    }
    fnet_tracking_common_data_t fnetCommonData;
    fnetCommonData.uid = commonData.uid.c_str();
    fnetCommonData.did = commonData.did.c_str();
    fnetCommonData.sid = commonData.sid.c_str();
    fnetCommonData.netType = commonData.netType.c_str();
    fnetCommonData.oper = commonData.oper.c_str();
    fnetCommonData.ext = commonData.ext.c_str();

    std::vector<fnet_tracking_event_data_t> fnetEventDatas(eventDatas.size());
    for (size_t i = 0; i < eventDatas.size(); ++i) {
        fnetEventDatas[i].eventType = eventDatas[i].eventType.c_str();
        fnetEventDatas[i].eventId = eventDatas[i].eventId.c_str();
        fnetEventDatas[i].eventName = eventDatas[i].eventName.c_str();
        fnetEventDatas[i].pageId = eventDatas[i].pageId.c_str();
        fnetEventDatas[i].moduleId = eventDatas[i].moduleId.c_str();
        fnetEventDatas[i].reqId = eventDatas[i].reqId.c_str();
        fnetEventDatas[i].expIds = eventDatas[i].expIds.c_str();
        fnetEventDatas[i].objectType = eventDatas[i].objectType.c_str();
        fnetEventDatas[i].objectId = eventDatas[i].objectId.c_str();
        fnetEventDatas[i].extend = eventDatas[i].extend.c_str();
        fnetEventDatas[i].timestamp = eventDatas[i].timestamp.c_str();
    }
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->reportTrackingDataBatch(m_clinetId.c_str(),
        &fnetCommonData, fnetEventDatas.data(), fnetEventDatas.size(), msTimeout));
    if (ret != COM_OK) {
        BOOST_LOG_TRIVIAL(error) << "reportTrackingData error, " << (int)ret;
    }
}

int64_t MultiComHelper::addPrintListModel(const std::string &modelId, const std::string &language, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return InvalidRequestId;
    }
    int64_t requestId = m_requestNum++;
    m_threadPool.post([=]() {
        ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
        char *message;
        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->addPrintListModel(m_clinetId.c_str(),
            token.accessToken().c_str(), language.c_str(), modelId.c_str(), &message, msTimeout));
        fnet::FreeInDestructor freeMessage(message, intfc->freeString);
        if (message != nullptr) {
            QueueEvent(new ComBusRequestEvent(COM_ADD_PRINT_LIST_MODEL_EVENT, requestId, message, ret));
        } else {
            QueueEvent(new ComBusRequestEvent(COM_ADD_PRINT_LIST_MODEL_EVENT, requestId, "", ret));
        }
    });
    return requestId;
}

int64_t MultiComHelper::removePrintListModel(const std::string &modelId, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return InvalidRequestId;
    }
    int64_t requestId = m_requestNum++;
    m_threadPool.post([=]() {
        ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->removePrintListModel(m_clinetId.c_str(),
            token.accessToken().c_str(), modelId.c_str(), msTimeout));
        QueueEvent(new ComBusRequestEvent(COM_REMOVE_PRINT_LIST_MODEL_EVENT, requestId, "", ret));
    });
    return requestId;
}

int64_t MultiComHelper::likeModel(const std::string& modelId, bool like, int msTimeout) 
{ 
    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return InvalidRequestId;
    }
    int64_t requestId = m_requestNum++;
    m_threadPool.post([=]() {
        ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
        ComErrno          ret   = MultiComUtils::fnetRet2ComErrno(
            intfc->navLikeModel(m_clinetId.c_str(), token.accessToken().c_str(), modelId.c_str(), like, msTimeout));
        QueueEvent(new ComBusRequestEvent(COM_LIKE_MODEL_EVENT, requestId, "", ret));
    });
    return requestId;
}

int64_t MultiComHelper::reportModel(int selectedOptionId, const std::string &modelId,
    const std::string &extraMessage, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return InvalidRequestId;
    }
    int64_t requestId = m_requestNum++;
    m_threadPool.post([=]() {
        ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
        fnet_report_model_data_t reportData;
        reportData.selectedOptionId = selectedOptionId;
        reportData.modelId = modelId.c_str();
        reportData.extraMessage = extraMessage.c_str();
        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->reportModel(m_clinetId.c_str(),
            token.accessToken().c_str(), &reportData, msTimeout));
        QueueEvent(new ComBusRequestEvent(COM_REPORT_MODEL_EVENT, requestId, "", ret));
    });
    return requestId;
}

int64_t MultiComHelper::getSystemMessage(com_sys_msg_data_t& data, const std::string& language, int msTimeout) 
{
    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken           token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_sys_msg_data_t* fnetInfo;
    ComErrno                    ret = MultiComUtils::fnetRet2ComErrno(
        intfc->getSystemMessage(m_clinetId.c_str(), token.accessToken().c_str(), language.c_str(), &fnetInfo, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    if (fnetInfo == nullptr) {
        return COM_ERROR;
    }
    fnet::FreeInDestructor freeDevInfos(fnetInfo, intfc->freeSystemMessage);
    data.content = fnetInfo->content;
    data.id = fnetInfo->id;
    data.title = fnetInfo->title;
    data.linkUrl = fnetInfo->linkUrl;
    data.showCount = fnetInfo->showCount;
    data.duration  = fnetInfo->duration;
    return ret;
}

int64_t MultiComHelper::getMonitorMessage(std::string& data, const std::string& language, int msTimeout)
{
    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken    token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_sys_msg_data_t* fnetInfo;
    ComErrno             ret = MultiComUtils::fnetRet2ComErrno(
        intfc->getMonitorMessage(m_clinetId.c_str(), token.accessToken().c_str(), language.c_str(), &fnetInfo, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    if (fnetInfo == nullptr) {
        return COM_ERROR;
    }
    fnet::FreeInDestructor freeDevInfos(fnetInfo, intfc->freeSystemMessage);
    data   = fnetInfo->content;
    return ret;
}

int64_t MultiComHelper::retrySliceTask(const std::string& id, int msTimeout) 
{
    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    ComErrno          ret   = MultiComUtils::fnetRet2ComErrno(
        intfc->retrySliceTask(m_clinetId.c_str(), token.accessToken().c_str(), id.c_str(), msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    return ret;
}

int64_t MultiComHelper::cancelSliceTask(const std::string& id, int msTimeout) 
{
    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    ComErrno          ret   = MultiComUtils::fnetRet2ComErrno(
        intfc->cancelSliceTask(m_clinetId.c_str(), token.accessToken().c_str(), id.c_str(), msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    return ret;
}

int64_t MultiComHelper::postReadSystemMessage(int id, int msTimeout) {
    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken    token = WanDevTokenMgr::inst()->getScopedToken();
    ComErrno             ret = MultiComUtils::fnetRet2ComErrno(
        intfc->postReadSystemMessage(m_clinetId.c_str(), token.accessToken().c_str(), id, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    return ret;
}

int64_t MultiComHelper::doBusGetRequest(const std::string &target, const std::string &language, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return InvalidRequestId;
    }
    int64_t requestId = m_requestNum++;
    m_threadPool.post([=]() {
        ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
        char *responseData;
        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->doBusGetRequest(m_clinetId.c_str(),
            token.accessToken().c_str(), language.c_str(), target.c_str(), &responseData, msTimeout));
        fnet::FreeInDestructor freeResponseData(responseData, intfc->freeString);
        if (responseData != nullptr) {
            QueueEvent(new ComBusGetRequestEvent(COM_BUS_GET_REQUEST_EVENT, requestId, "", responseData, ret));
        } else {
            QueueEvent(new ComBusGetRequestEvent(COM_BUS_GET_REQUEST_EVENT, requestId, "", "", ret));
        }
    });
    return requestId;
}

int64_t MultiComHelper::doBusGetRequestSystem(const std::string &target, const std::string &language, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return InvalidRequestId;
    }
    int64_t requestId = m_requestNum++;
    m_threadPool.post([=]() {
        char *responseData;
        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->doBusGetRequest(m_clinetId.c_str(),
            nullptr, language.c_str(), target.c_str(), &responseData, msTimeout));
        fnet::FreeInDestructor freeResponseData(responseData, intfc->freeString);
        if (responseData != nullptr) {
            QueueEvent(new ComBusGetRequestEvent(COM_BUS_GET_REQUEST_EVENT, requestId, "", responseData, ret));
        } else {
            QueueEvent(new ComBusGetRequestEvent(COM_BUS_GET_REQUEST_EVENT, requestId, "", "", ret));
        }
        });
    return requestId;
}

int64_t MultiComHelper::doBusPostRequest(const std::string &target, const std::string &language,
    const std::string &postFields, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return InvalidRequestId;
    }
    int64_t requestId = m_requestNum++;
    m_threadPool.post([=]() {
        ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
        int code;
        ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->doBusPostRequest(m_clinetId.c_str(),
            token.accessToken().c_str(), language.c_str(), target.c_str(), postFields.c_str(), &code, msTimeout));
        QueueEvent(new ComBusPostRequestEvent(COM_BUS_POST_REQUEST_EVENT, requestId, "", code, ret));
    });
    return requestId;
}

ComErrno MultiComHelper::singOut(int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    return MultiComUtils::fnetRet2ComErrno(intfc->signOut(token.accessToken().c_str(), msTimeout));
}

ComErrno MultiComHelper::getUserAiPointsInfo(com_user_ai_points_info_t &userAiPointsInfo, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_user_ai_points_info_t *fnetUserAiPointsInfo;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->getUserAiPointsInfo(
        m_clinetId.c_str(), token.accessToken().c_str(), &fnetUserAiPointsInfo, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeDevInfos(fnetUserAiPointsInfo, intfc->freeUserAiPointsInfo);
    userAiPointsInfo.totalPoints = fnetUserAiPointsInfo->totalPoints;
    userAiPointsInfo.modelGenPoints = fnetUserAiPointsInfo->modelGenPoints;
    userAiPointsInfo.img2imgPoints = fnetUserAiPointsInfo->img2imgPoints;
    userAiPointsInfo.txt2txtPoints = fnetUserAiPointsInfo->txt2txtPoints;
    userAiPointsInfo.txt2imgPoints = fnetUserAiPointsInfo->txt2imgPoints;
    userAiPointsInfo.remainingFreeCount = fnetUserAiPointsInfo->remainingFreeCount;
    userAiPointsInfo.freeRetriesPerProcess = fnetUserAiPointsInfo->freeRetriesPerProcess;
    return ret;
}

ComErrno MultiComHelper::uploadAiImageClound(const std::string &filePath, const std::string &saveName,
    std::string &storeUrl, fnet_progress_callback_t callback, void *callbackData, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    const char *accessToken = token.accessToken().c_str();
    fnet_upload_file_data_t uploadFileData;
    uploadFileData.filePath = filePath.c_str();
    uploadFileData.saveName = saveName.c_str();
    uploadFileData.callback = callback;
    uploadFileData.callbackData = callbackData;
    fnet_clound_file_data_t *cloundFileData;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->uploadAiImageClound(
        m_clinetId.c_str(), accessToken, m_uid.c_str(), &uploadFileData, &cloundFileData, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeCloundFileData(cloundFileData, intfc->freeCloundFileData);
    storeUrl = cloundFileData->storageUrl;
    return ret;
}

ComErrno MultiComHelper::uploadLogFileCloud(const std::string&       filePath,
                                            const std::string&       saveName,
                                            const std::string&       connectEmail,
                                            fnet_progress_callback_t callback,
                                            void*                    callbackData,
                                            int                      msTimeout)
{
    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken       token       = WanDevTokenMgr::inst()->getScopedToken();
    const char*             accessToken = token.accessToken().c_str();
    fnet_upload_file_data_t uploadFileData;
    uploadFileData.filePath     = filePath.c_str();
    uploadFileData.saveName     = saveName.c_str();
    uploadFileData.callback     = callback;
    uploadFileData.callbackData = callbackData;
    ComErrno                 ret = MultiComUtils::fnetRet2ComErrno(
        intfc->uploadLogFileCloud(m_clinetId.c_str(), accessToken, &uploadFileData, connectEmail.c_str(), msTimeout));
    return ret;
}

ComErrno MultiComHelper::createAiJobPipeline(const std::string &entryType,
    com_ai_job_pipeline_info_t &pipelineInfo, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_ai_job_pipeline_info_t *fnetPipelineInfo;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->createAiJobPipeline(
        m_clinetId.c_str(), token.accessToken().c_str(), entryType.c_str(), &fnetPipelineInfo, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freePipelineInfo(fnetPipelineInfo, intfc->freeAiJobPipelineInfo);
    pipelineInfo.id = fnetPipelineInfo->id;
    pipelineInfo.isFree = fnetPipelineInfo->isFree;
    return ret;
}

ComErrno MultiComHelper::startAiModelJob(int supplier, int64_t pipelineId, const std::string &imageUrl,
    const std::string &resultFormat, com_ai_model_job_result_t &jobResult, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_start_ai_model_job_data_t jobData;
    jobData.supplier = supplier;
    jobData.pipelineId = pipelineId;
    jobData.imageUrl = imageUrl.c_str();
    jobData.resultFormat = resultFormat.c_str();
    fnet_start_ai_model_job_result *fnetJobResult;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->startAiModelJob(
        m_clinetId.c_str(), token.accessToken().c_str(), &jobData, &fnetJobResult, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobResult(fnetJobResult, intfc->freeStartAiModelJobResult);
    jobResult.status = fnetJobResult->status;
    jobResult.jobId = fnetJobResult->jobId;
    jobResult.posInQueue = fnetJobResult->posInQueue;
    jobResult.queueLength = fnetJobResult->queueLength;
    jobResult.isOldJob = fnetJobResult->isOldJob;
    return ret;
}

ComErrno MultiComHelper::getAiModelJobState(int64_t jobId, com_ai_model_job_state_t &jobState, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_ai_model_job_state_t *fnetJobState;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->getAiModelJobState(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, &fnetJobState, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobState(fnetJobState, intfc->freeAiModelJobState);
    jobState.status = fnetJobState->status;
    jobState.jobId = fnetJobState->jobId;
    jobState.posInQueue = fnetJobState->posInQueue;
    jobState.queueLength = fnetJobState->queueLength;
    jobState.externalJobId = fnetJobState->externalJobId;
    jobState.models.resize(fnetJobState->modelCnt);
    for (int i = 0; i < fnetJobState->modelCnt; ++i) {
        jobState.models[i].modelType = fnetJobState->models[i].modelType;
        jobState.models[i].modelUrl = fnetJobState->models[i].modelUrl;
    }
    return ret;
}

ComErrno MultiComHelper::abortAiModelJob(int64_t jobId, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->abortAiModelJob(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    return ret;
}

ComErrno MultiComHelper::getExistingAiModelJob(com_ai_model_job_result_t &jobResult, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_start_ai_model_job_result *fnetJobResult;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->getExistingAiModelJob(
        m_clinetId.c_str(), token.accessToken().c_str(), &fnetJobResult, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobResult(fnetJobResult, intfc->freeStartAiModelJobResult);
    jobResult.status = fnetJobResult->status;
    jobResult.jobId = fnetJobResult->jobId;
    jobResult.posInQueue = fnetJobResult->posInQueue;
    jobResult.queueLength = fnetJobResult->queueLength;
    jobResult.isOldJob = fnetJobResult->isOldJob;
    return ret;
}

ComErrno MultiComHelper::startAiImg2imgJob(int supplier, int64_t pipelineId, const std::string &imageUrl,
    com_ai_general_job_result_t &jobResult, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_start_ai_general_job_data_t jobData;
    jobData.supplier = supplier;
    jobData.pipelineId = pipelineId;
    jobData.prompt = "";
    jobData.imageUrl = imageUrl.c_str();
    fnet_start_ai_general_job_result_t *fnetJobResult;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->startAiImg2imgJob(
        m_clinetId.c_str(), token.accessToken().c_str(), &jobData, &fnetJobResult, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobResult(fnetJobResult, intfc->freeStartAiGeneralJobResult);
    jobResult.status = fnetJobResult->status;
    jobResult.jobId = fnetJobResult->jobId;
    jobResult.posInQueue = fnetJobResult->posInQueue;
    jobResult.queueLength = fnetJobResult->queueLength;
    jobResult.remainingFreeRetries = fnetJobResult->remainingFreeRetries;
    return ret;
}

ComErrno MultiComHelper::startAiTxt2txtJob(int supplier, int64_t pipelineId, const std::string &prompt,
    com_ai_general_job_result_t &jobResult, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_start_ai_general_job_data_t jobData;
    jobData.supplier = supplier;
    jobData.pipelineId = pipelineId;
    jobData.prompt = prompt.c_str();
    jobData.imageUrl = "";
    fnet_start_ai_general_job_result_t *fnetJobResult;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->startAiTxt2txtJob(
        m_clinetId.c_str(), token.accessToken().c_str(), &jobData, &fnetJobResult, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobResult(fnetJobResult, intfc->freeStartAiGeneralJobResult);
    jobResult.status = fnetJobResult->status;
    jobResult.jobId = fnetJobResult->jobId;
    jobResult.posInQueue = fnetJobResult->posInQueue;
    jobResult.queueLength = fnetJobResult->queueLength;
    jobResult.remainingFreeRetries = fnetJobResult->remainingFreeRetries;
    return ret;
}

ComErrno MultiComHelper::startAiTxt2imgJob(int supplier, int64_t pipelineId, const std::string &prompt,
    com_ai_general_job_result_t &jobResult, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_start_ai_general_job_data_t jobData;
    jobData.supplier = supplier;
    jobData.pipelineId = pipelineId;
    jobData.prompt = prompt.c_str();
    jobData.imageUrl = "";
    fnet_start_ai_general_job_result_t *fnetJobResult;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->startAiTxt2imgJob(
        m_clinetId.c_str(), token.accessToken().c_str(), &jobData, &fnetJobResult, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobResult(fnetJobResult, intfc->freeStartAiGeneralJobResult);
    jobResult.status = fnetJobResult->status;
    jobResult.jobId = fnetJobResult->jobId;
    jobResult.posInQueue = fnetJobResult->posInQueue;
    jobResult.queueLength = fnetJobResult->queueLength;
    jobResult.remainingFreeRetries = fnetJobResult->remainingFreeRetries;
    return ret;
}

ComErrno MultiComHelper::getAiImg2imgJobState(int64_t jobId, com_ai_general_job_state_t &jobState, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_ai_general_job_state_t *fnetJobState;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->getAiImg2imgJobState(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, &fnetJobState, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobState(fnetJobState, intfc->freeAiGeneralJobState);
    jobState.status = fnetJobState->status;
    jobState.jobId = fnetJobState->jobId;
    jobState.posInQueue = fnetJobState->posInQueue;
    jobState.queueLength = fnetJobState->queueLength;
    jobState.externalJobId = fnetJobState->externalJobId;
    jobState.datas.resize(fnetJobState->dataCnt);
    for (int i = 0; i < fnetJobState->dataCnt; ++i) {
        jobState.datas[i].content = fnetJobState->datas[i].content;
        jobState.datas[i].imageUrl = fnetJobState->datas[i].imageUrl;
    }
    return ret;
}

ComErrno MultiComHelper::getAiTxt2txtJobState(int64_t jobId, com_ai_general_job_state_t &jobState, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_ai_general_job_state_t *fnetJobState;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->getAiTxt2txtJobState(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, &fnetJobState, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobState(fnetJobState, intfc->freeAiGeneralJobState);
    jobState.status = fnetJobState->status;
    jobState.jobId = fnetJobState->jobId;
    jobState.posInQueue = fnetJobState->posInQueue;
    jobState.queueLength = fnetJobState->queueLength;
    jobState.externalJobId = fnetJobState->externalJobId;
    jobState.datas.resize(fnetJobState->dataCnt);
    for (int i = 0; i < fnetJobState->dataCnt; ++i) {
        jobState.datas[i].content = fnetJobState->datas[i].content;
        jobState.datas[i].imageUrl = fnetJobState->datas[i].imageUrl;
    }
    return ret;
}

ComErrno MultiComHelper::getAiTxt2imgJobState(int64_t jobId, com_ai_general_job_state_t &jobState, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    fnet_ai_general_job_state_t *fnetJobState;
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->getAiTxt2imgJobState(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, &fnetJobState, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    fnet::FreeInDestructor freeJobState(fnetJobState, intfc->freeAiGeneralJobState);
    jobState.status = fnetJobState->status;
    jobState.jobId = fnetJobState->jobId;
    jobState.posInQueue = fnetJobState->posInQueue;
    jobState.queueLength = fnetJobState->queueLength;
    jobState.externalJobId = fnetJobState->externalJobId;
    jobState.datas.resize(fnetJobState->dataCnt);
    for (int i = 0; i < fnetJobState->dataCnt; ++i) {
        jobState.datas[i].content = fnetJobState->datas[i].content;
        jobState.datas[i].imageUrl = fnetJobState->datas[i].imageUrl;
    }
    return ret;
}

ComErrno MultiComHelper::abortAiImg2imgJob(int64_t jobId, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->abortAiImg2imgJob(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    return ret;
}

ComErrno MultiComHelper::abortAiTxt2txtJob(int64_t jobId, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->abortAiTxt2txtJob(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    return ret;
}

ComErrno MultiComHelper::abortAiTxt2imgJob(int64_t jobId, int msTimeout)
{
    fnet::FlashNetworkIntfc *intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        return COM_ERROR;
    }
    ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
    ComErrno ret = MultiComUtils::fnetRet2ComErrno(intfc->abortAiTxt2imgJob(
        m_clinetId.c_str(), token.accessToken().c_str(), jobId, msTimeout));
    if (ret != COM_OK) {
        return ret;
    }
    return ret;
}

}} // namespace Slic3r::GUI
