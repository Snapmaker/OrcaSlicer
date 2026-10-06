#include "WanDevSendGcodeThd.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <boost/algorithm/hex.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <openssl/md5.h>
#include "ComCommand.hpp"
#include "ComWanConn.hpp"
#include "FreeInDestructor.h"
#include "MultiComUtils.hpp"
#include "WanDevTokenMgr.hpp"

namespace Slic3r { namespace GUI {

WanDevSendGcodeThd::WanDevSendGcodeThd(fnet::FlashNetworkIntfc *networkIntfc)
    : m_exitThread(false)
    , m_thread(boost::bind(&WanDevSendGcodeThd::run, this))
    , m_networkIntfc(networkIntfc)
{
}

void WanDevSendGcodeThd::exit()
{
    abortSendGcode();
    m_exitThread = true;
    m_sendGcodeEvent.set(true);
    m_thread.join();
}

bool WanDevSendGcodeThd::startSendGcode(const std::string &clientId, const std::vector<std::string> &devIds,
    const std::vector<std::string> &devSerialNumbers, const std::vector<std::string> &devTopics,
    const com_send_gcode_data_t &sendGocdeData)
{
    if (m_sendGcodeEvent.get()) {
        return false;
    }
    m_clientId = clientId;
    m_devIds = devIds;
    m_devSerialNumberMap.clear();
    m_devTopicMap.clear();
    for (size_t i = 0; i < devIds.size(); ++i) {
        m_devSerialNumberMap.emplace(devIds[i], devSerialNumbers[i]);
        m_devTopicMap.emplace(devIds[i], devTopics[i]);
    }
    m_comSendGcodeData = sendGocdeData;
    m_materialMappings = MultiComUtils::comMaterialMappings2Fnet(m_comSendGcodeData.materialMappings);
    m_sendGcodeData.gcodeFilePath = m_comSendGcodeData.gcodeFilePath.c_str();
    m_sendGcodeData.thumbFilePath = m_comSendGcodeData.thumbFilePath.c_str();
    m_sendGcodeData.gcodeDstName = m_comSendGcodeData.gcodeDstName.c_str();
    m_sendGcodeData.printNow = m_comSendGcodeData.printNow;
    m_sendGcodeData.levelingBeforePrint = m_comSendGcodeData.levelingBeforePrint;
    m_sendGcodeData.flowCalibration = m_comSendGcodeData.flowCalibration;
    m_sendGcodeData.firstLayerInspection = m_comSendGcodeData.firstLayerInspection;
    m_sendGcodeData.timeLapseVideo = m_comSendGcodeData.timeLapseVideo;
    m_sendGcodeData.useMatlStation = m_comSendGcodeData.useMatlStation;
    m_sendGcodeData.gcodeToolCnt = (int)m_comSendGcodeData.materialMappings.size();
    m_sendGcodeData.materialMappings = m_materialMappings.data();
    m_sendGcodeData.callback = callback;
    m_sendGcodeData.callbackData = this;
    m_progress = 0.0;
    m_callbackRet = 0;
    m_sendGcodeEvent.set(true);
    return true;
}

bool WanDevSendGcodeThd::abortSendGcode()
{
    if (!m_sendGcodeEvent.get()) {
        return false;
    }
    m_callbackRet = 1;
    return true;
}

void WanDevSendGcodeThd::run()
{
    while (!m_exitThread) {
        m_sendGcodeEvent.waitTrue();
        if (!m_exitThread) {
            ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
            const char *accessToken = token.accessToken().c_str();

            fnet_clound_gcode_data_t *cloundGcodeData;
            int fnetRet = m_networkIntfc->wanDevSendGcodeClound(
                m_clientId.c_str(), accessToken, &m_sendGcodeData, &cloundGcodeData, ComTimeoutWanB);
            fnet::FreeInDestructor freeCloundGcodeData(
                cloundGcodeData, m_networkIntfc->freeCloundGcodeData);

            std::map<std::string, ComCloundJobErrno> errorMap;
            if (fnetRet == FNET_OK) {
                fnetRet = startCloundJob(accessToken, cloundGcodeData, errorMap);
            }
            ComErrno ret = MultiComUtils::fnetRet2ComErrno(fnetRet);
            QueueEvent(new ComSendGcodeFinishEvent(COM_SEND_GCODE_FINISH_EVENT, errorMap, ret));
        }
        m_sendGcodeEvent.set(false);
    }
}

int WanDevSendGcodeThd::startCloundJob(const char *accessToken, const fnet_clound_gcode_data_t *cloundGcodeData,
    std::map<std::string, ComCloundJobErrno> &errorMap)
{
    std::vector<const char *> devIds;
    for (auto &devId : m_devIds) {
        devIds.push_back(devId.c_str());
    }
    boost::filesystem::path gcodeDstPath(m_sendGcodeData.gcodeDstName);
    boost::filesystem::path gcodePath(m_sendGcodeData.gcodeFilePath);
    boost::filesystem::path thumbPath(m_sendGcodeData.thumbFilePath);
    std::string gcodeType = gcodePath.extension().string().substr(1);
    std::string gcodeMd5 = getFileMd5(m_sendGcodeData.gcodeFilePath);
    std::string thumbName = gcodeDstPath.replace_extension(".").string() + gcodeType;
    std::string thumbType = thumbPath.extension().string().substr(1);
    std::string thumbMd5 = getFileMd5(m_sendGcodeData.thumbFilePath);

    fnet_clound_job_data_t jobData;
    jobData.devIds = devIds.data();
    jobData.devSerialNumbers = nullptr;
    jobData.jobIds = nullptr;
    jobData.devCnt = devIds.size();
    jobData.gcodeName = m_sendGcodeData.gcodeDstName;
    jobData.gcodeType = gcodeType.c_str();
    jobData.gcodeMd5 = gcodeMd5.c_str();
    jobData.gcodeSize = boost::filesystem::file_size(m_sendGcodeData.gcodeFilePath);
    jobData.thumbName = thumbName.c_str();
    jobData.thumbType = thumbType.c_str();
    jobData.thumbMd5 = thumbMd5.c_str();
    jobData.thumbSize = boost::filesystem::file_size(m_sendGcodeData.thumbFilePath);
    jobData.bucketName = cloundGcodeData->bucketName;
    jobData.endpoint = cloundGcodeData->endpoint;
    jobData.gcodeStorageKey = cloundGcodeData->gcodeStorageKey;
    jobData.gcodeStorageUrl = cloundGcodeData->gcodeStorageUrl;
    jobData.thumbStorageKey = cloundGcodeData->thumbStorageKey;
    jobData.thumbStorageUrl = cloundGcodeData->thumbStorageUrl;
    jobData.printNow = m_sendGcodeData.printNow;
    jobData.plateNo              = m_comSendGcodeData.currentPlaterIndex;
    jobData.unionId              = m_comSendGcodeData.unionId.c_str();
    jobData.levelingBeforePrint = m_sendGcodeData.levelingBeforePrint;
    jobData.flowCalibration = m_sendGcodeData.flowCalibration;
    jobData.firstLayerInspection = m_sendGcodeData.firstLayerInspection;
    jobData.timeLapseVideo = m_sendGcodeData.timeLapseVideo;
    jobData.useMatlStation = m_sendGcodeData.useMatlStation;
    jobData.gcodeToolCnt = m_sendGcodeData.gcodeToolCnt;
    jobData.materialMappings = m_sendGcodeData.materialMappings;

    fnet_add_clound_job_result_t *results;
    int resultCnt;
    int fnetRet = m_networkIntfc->wanDevAddCloundJob(
        m_clientId.c_str(), accessToken, &jobData, &results, &resultCnt, ComTimeoutWanB);
    if (fnetRet != FNET_OK) {
        return fnetRet;
    }
    fnet::FreeInDestructorArg freeResults(results, m_networkIntfc->freeAddCloudJobResults, resultCnt);
    if (resultCnt != m_devIds.size()) {
        BOOST_LOG_TRIVIAL(error) << "invalid resultCnt: " << resultCnt << " devCnt: " << m_devIds.size();
        return FNET_ERROR;
    }
    for (int i = 0; i < resultCnt; ++i) {
        if (m_devSerialNumberMap.find(results[i].devId) == m_devSerialNumberMap.end()) {
            BOOST_LOG_TRIVIAL(error) << "invalid devId: " << results[i].devId;
            return FNET_ERROR;
        }
    }
    sendStartCloundJob(results, resultCnt, jobData, errorMap);
    return fnetRet;
}

std::string WanDevSendGcodeThd::getFileMd5(const char *filePath)
{
    std::ifstream fs;
    fs.open(filePath, std::fstream::binary);
    if (!fs.is_open()) {
        return std::string();
    }
    MD5_CTX md5;
    if (MD5_Init(&md5) == 0) {
        return std::string();
    }
    std::vector<char> buf(4096);
    do {
        fs.read(buf.data(), buf.size());
        if (fs.gcount() > 0) {
            MD5_Update(&md5, buf.data(), fs.gcount());
        }
    } while (fs.good());

    unsigned char hash[MD5_DIGEST_LENGTH];
    MD5_Final(hash, &md5);

    std::string result;
    boost::algorithm::hex_lower(std::begin(hash), std::end(hash), std::back_inserter(result));
    return result;
}

void WanDevSendGcodeThd::sendStartCloundJob(const fnet_add_clound_job_result_t *results, int resultCnt,
    fnet_clound_job_data_t &jobData, std::map<std::string, ComCloundJobErrno> &errorMap)
{
    for (int i = 0; i < resultCnt; i += 30) {
        std::vector<std::string> topics;
        std::vector<const char *> devIds;
        std::vector<const char *> devSerialNumbers;
        std::vector<const char *> jobIds;
        for (size_t j = 0; j < 30 && i + j < resultCnt; ++j) {
            if (results[i + j].error == FNET_ADD_CLOUND_JOB_OK) {
                topics.push_back(m_devTopicMap.at(results[i + j].devId));
                devIds.push_back(results[i + j].devId);
                devSerialNumbers.push_back(m_devSerialNumberMap.at(results[i + j].devId).c_str());
                jobIds.push_back(results[i + j].jobId);
            } else {
                errorMap.emplace(results[i + j].devId, convertError(results[i + j].error));
            }
        }
        jobData.devIds = devIds.data();
        jobData.devSerialNumbers = devSerialNumbers.data();
        jobData.jobIds = jobIds.data();
        jobData.devCnt = devIds.size();
        std::set<int> failedIndices;
        ComErrno ret = ComWanConn::inst()->sendStartCloundJob(topics, jobData, failedIndices);
        for (size_t i = 0; i < devIds.size(); ++i) {
            if (ret != COM_OK || failedIndices.find(i) != failedIndices.end()) {
                errorMap.emplace(devIds[i], COM_CLOUND_JOB_CONN_SEND_ERROR);
            } else {
                errorMap.emplace(devIds[i], COM_CLOUND_JOB_OK);
            }
        }
    }
}

ComCloundJobErrno WanDevSendGcodeThd::convertError(fnet_add_clound_job_error_t error)
{
    switch (error) {
    case FNET_ADD_CLOUND_JOB_OK:
        return COM_CLOUND_JOB_OK;
    case FNET_ADD_CLOUND_JOB_DEVICE_BUSY:
        return COM_CLOUND_JOB_DEVICE_BUSY;
    case FNET_ADD_CLOUND_JOB_DEVICE_NOT_FOUND:
        return COM_CLOUND_JOB_DEVICE_NOT_FOUND;
    case FNET_ADD_CLOUND_JOB_SERVER_INTERNAL_ERROR:
        return COM_CLOUND_JOB_SERVER_INTERNAL_ERROR;
    default:
        return COM_CLOUND_JOB_UNKNOWN_ERROR;
    }
}

int WanDevSendGcodeThd::callback(long long now, long long total, void *callbackData)
{
    WanDevSendGcodeThd *inst = (WanDevSendGcodeThd *)callbackData;
    if (total != 0) {
        double progress = (double)now / total;
        if (progress - inst->m_progress > 0.025) {
            inst->QueueEvent(new ComSendGcodeProgressEvent(COM_SEND_GCODE_PROGRESS_EVENT, now, total));
            inst->m_progress = progress;
        }
    }
    return inst->m_callbackRet;
}

}} // namespace Slic3r::GUI
