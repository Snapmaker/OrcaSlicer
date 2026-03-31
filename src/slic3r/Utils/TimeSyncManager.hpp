#ifndef slic3r_TimeSyncManager_hpp_
#define slic3r_TimeSyncManager_hpp_

#include <shared_mutex>
#include <chrono>
#include <nlohmann/json.hpp>

namespace Slic3r {

class TimeSyncManager {
public:
    TimeSyncManager() = default;
    ~TimeSyncManager() = default;

    // 为请求添加时间字段
    void addTimeFields(nlohmann::json& request);

    // 处理响应，更新时间同步状态
    void updateFromResponse(const nlohmann::json& response);

    // 重置时间同步状态（用于重连）
    void reset();

    // 获取同步状态
    bool isSynced() const;

    // 获取当前系统时间（毫秒）
    static int64_t getCurrentTimeMs();

private:
    mutable std::shared_mutex mutex_;
    int64_t delta_time_ = 0;      // 客户端与设备的时间差（ms）
    int64_t duration_ = 0;         // 消息往返时长（ms）
    int64_t last_cli_time_ = 0;    // 上次请求的客户端时间
    bool is_synced_ = false;       // 是否已完成首次同步
    int sync_fail_count_ = 0;      // 连续同步失败次数

    // 计算设备时间
    int64_t calculateDevTime() const;
};

} // namespace Slic3r

#endif // slic3r_TimeSyncManager_hpp_
