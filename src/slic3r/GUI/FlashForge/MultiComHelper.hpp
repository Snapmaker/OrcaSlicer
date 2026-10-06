#ifndef slic3r_GUI_MultiComHelper_hpp_
#define slic3r_GUI_MultiComHelper_hpp_

#include <cstdio>
#include <wx/event.h>
#include "ComThreadPool.hpp"
#include "MultiComDef.hpp"
#include "Singleton.hpp"

namespace Slic3r { namespace GUI {

class MultiComHelper : public wxEvtHandler, public Singleton<MultiComHelper>
{
public:
    static const int64_t InvalidRequestId = -1;

    MultiComHelper();

    void loginInit(const std::string &clientId, const std::string &uid);

    void userClickCount(const std::string &source, int msTimeout);

    void reportTrackingData(const com_tracking_common_data_t &commonData,
        const com_tracking_event_data_t &eventData, int msTimeout);

    void reportTrackingDataBatch(const com_tracking_common_data_t &commonData,
        const std::vector<com_tracking_event_data_t> &eventDatas, int msTimeout);

    void reportTrackingDataBatchSync(const com_tracking_common_data_t &commonData,
        const std::vector<com_tracking_event_data_t> &eventDatas, int msTimeout);

    int64_t addPrintListModel(const std::string &modelId, const std::string &language, int msTimeout);

    int64_t removePrintListModel(const std::string &modelId, int msTimeout);

    int64_t likeModel(const std::string& modelId, bool like, int msTimeout);

    int64_t reportModel(int selectedOptionId, const std::string &modelId,
        const std::string &extraMessage, int msTimeout);

    int64_t getSystemMessage(com_sys_msg_data_t& data, const std::string &language, int msTimeout);

    int64_t postReadSystemMessage(int id, int msTimeout);

    int64_t getMonitorMessage(std::string& data, const std::string& language, int msTimeout);

    int64_t retrySliceTask(const std::string& id, int msTimeout);

    int64_t cancelSliceTask(const std::string& id, int msTimeout);

    int64_t doBusGetRequest(const std::string &target, const std::string &language, int msTimeout);

    int64_t doBusGetRequestSystem(const std::string &target, const std::string &language, int msTimeout);

    int64_t doBusPostRequest(const std::string &target, const std::string &language,
        const std::string &postFields, int msTimeout);

    ComErrno singOut(int msTimeout);

    ComErrno getUserAiPointsInfo(com_user_ai_points_info_t &userAiPointsInfo, int msTimeout);

    ComErrno uploadAiImageClound(const std::string &filePath, const std::string &saveName,
        std::string &storeUrl, fnet_progress_callback_t callback, void *callbackData, int msTimeout);

    ComErrno uploadLogFileCloud(const std::string&       filePath,
                                const std::string&       saveName,
                                const std::string&       connectEmail,
                                fnet_progress_callback_t callback,
                                void*                    callbackData,
                                int                      msTimeout);

    ComErrno createAiJobPipeline(const std::string &entryType,
        com_ai_job_pipeline_info_t &pipelineInfo, int msTimeout); // entryType: text2text/img2img

    ComErrno startAiModelJob(int supplier, int64_t pipelineId, const std::string &imageUrl,
        const std::string &resultFormat, com_ai_model_job_result_t &jobResult, int msTimeout);

    ComErrno getAiModelJobState(int64_t jobId, com_ai_model_job_state_t &jobState,
        int msTimeout);

    ComErrno abortAiModelJob(int64_t jobId, int msTimeout);

    ComErrno getExistingAiModelJob(com_ai_model_job_result_t &jobResult, int msTimeout);

    ComErrno startAiImg2imgJob(int supplier, int64_t pipelineId, const std::string &imageUrl,
        com_ai_general_job_result_t &jobResult, int msTimeout);

    ComErrno startAiTxt2txtJob(int supplier, int64_t pipelineId, const std::string &prompt,
        com_ai_general_job_result_t &jobResult, int msTimeout);

    ComErrno startAiTxt2imgJob(int supplier, int64_t pipelineId, const std::string &prompt,
        com_ai_general_job_result_t &jobResult, int msTimeout);

    ComErrno getAiImg2imgJobState(int64_t jobId, com_ai_general_job_state_t &jobState, int msTimeout);

    ComErrno getAiTxt2txtJobState(int64_t jobId, com_ai_general_job_state_t &jobState, int msTimeout);

    ComErrno getAiTxt2imgJobState(int64_t jobId, com_ai_general_job_state_t &jobState, int msTimeout);

    ComErrno abortAiImg2imgJob(int64_t jobId, int msTimeout);

    ComErrno abortAiTxt2txtJob(int64_t jobId, int msTimeout);

    ComErrno abortAiTxt2imgJob(int64_t jobId, int msTimeout);

private:
    std::string m_clinetId;
    std::string m_uid;
    ComThreadPool m_threadPool;
    int64_t m_requestNum;
};

}} // namespace Slic3r::GUI

#endif
