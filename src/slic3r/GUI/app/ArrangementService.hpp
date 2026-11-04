#pragma once

#include <atomic>
#include <utility>
#include <vector>

#include "libslic3r/Point.hpp"   // Vec2f（get_empty_cells 签名）
#include "ArrangeRequest.hpp"    // F3：Request 值类型（arrange/orient 签名）

namespace Slic3r {
class Model;
class ModelInstance;
class BuildVolume;

namespace GUI {
class Plater;
class PartPlateList;
class Worker;

namespace app {
class SnapshotService;

using ModelInstancePtrs = std::vector<ModelInstance*>;


class ArrangementService {
public:
    ArrangementService(Plater* q, Model* model, Worker& worker, SnapshotService& snapshots);

    void arrange(const ArrangeRequest& req);   // F3：Request 显式传递（原无参版随旧隐式写点一并消亡）
    void orient(const OrientRequest& req);
    void fill_bed_with_instances();
    void optimize_rotation();                  // F3 D6：签名不变，内部钉 OrientRequest::selection()
    void find_new_position(const ModelInstancePtrs& instances);
    static std::vector<Vec2f> get_empty_cells(const Vec2f step, PartPlateList& plates);
    bool can_arrange() const;
    bool worker_idle() const;   // F3 新增：原 slicing_host.worker_idle 直读 m_worker，改经服务 accessor 消费（cpp 内联一行转发；hpp 仅前向声明 Worker 不可行内定义）

    bool claim_arrange_running();    // 原 last_arrange_job_is_finished：CAS false→true
    void release_arrange_running();  // 原 ArrangeJob::finalize 的 store(false)

private:
    Plater*          m_q{nullptr};      // 仅传给 init_arrange_params/get_wipe_tower_arrangepoly，不解引用
    Model*           m_model{nullptr};
    Worker&          m_worker;
    SnapshotService& m_snapshots;
    std::atomic<bool> m_arrange_running{false};
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
