#include "TimeSyncManager.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

int64_t TimeSyncManager::getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void TimeSyncManager::addTimeFields(nlohmann::json& request) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    int64_t current_time = getCurrentTimeMs();

    // 检测时钟回拨
    if (last_cli_time_ > 0 && current_time < last_cli_time_) {
        BOOST_LOG_TRIVIAL(warning) << "[TimeSyncManager] Clock rollback detected, resetting sync state";
        // 直接重置，不需要释放锁
        delta_time_ = 0;
        duration_ = 0;
        last_cli_time_ = 0;
        is_synced_ = false;
        sync_fail_count_ = 0;
    }

    int64_t dev_time_value = is_synced_ ? calculateDevTime() : -1;
    request["cli_time"] = current_time;
    request["dev_time"] = dev_time_value;

    BOOST_LOG_TRIVIAL(info) << "[TimeSyncManager] 添加时间字段: cli_time=" << current_time
                            << ", dev_time=" << dev_time_value
                            << ", is_synced=" << is_synced_;

    last_cli_time_ = current_time;
}

void TimeSyncManager::updateFromResponse(const nlohmann::json& response) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // 检查必需字段
    if (!response.contains("cli_time") || !response.contains("dev_time")) {
        BOOST_LOG_TRIVIAL(warning) << "[TimeSyncManager] Response missing time fields";
        sync_fail_count_++;
        if (sync_fail_count_ >= 3) {
            BOOST_LOG_TRIVIAL(error) << "[TimeSyncManager] 3 consecutive sync failures, resetting";
            is_synced_ = false;
            sync_fail_count_ = 0;
        }
        return;
    }

    // 检查字段类型
    if (!response["cli_time"].is_number() || !response["dev_time"].is_number()) {
        BOOST_LOG_TRIVIAL(error) << "[TimeSyncManager] Invalid time field types";
        sync_fail_count_++;
        if (sync_fail_count_ >= 3) {
            is_synced_ = false;
            sync_fail_count_ = 0;
        }
        return;
    }

    int64_t cli_time1 = last_cli_time_;
    int64_t cli_time2 = getCurrentTimeMs();
    int64_t dev_time = response["dev_time"].get<int64_t>();

    // 更新同步参数
    duration_ = cli_time2 - cli_time1;
    delta_time_ = cli_time2 - dev_time;
    is_synced_ = true;
    sync_fail_count_ = 0;

    BOOST_LOG_TRIVIAL(info) << "[TimeSyncManager] 时间同步更新: duration=" << duration_
                            << "ms, delta=" << delta_time_ << "ms"
                            << ", cli_time1=" << cli_time1
                            << ", cli_time2=" << cli_time2
                            << ", dev_time=" << dev_time;
}

void TimeSyncManager::reset() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    delta_time_ = 0;
    duration_ = 0;
    last_cli_time_ = 0;
    is_synced_ = false;
    sync_fail_count_ = 0;
    BOOST_LOG_TRIVIAL(info) << "[TimeSyncManager] Sync state reset";
}

bool TimeSyncManager::isSynced() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return is_synced_;
}

int64_t TimeSyncManager::calculateDevTime() const {
    int64_t current_time = getCurrentTimeMs();
    return current_time - delta_time_ + duration_;
}

} // namespace Slic3r
