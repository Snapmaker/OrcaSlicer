// ArrangementService 实现（阶段 F2，手册 §2/§3）。commit ① 仅落地骨架与装配：
// 函数体随 tasks 2.3（claim/release/can_arrange）与 3.1/3.2（摆放域迁移）逐字迁入。
// 依赖规则：禁直接 include Plater/GUI_App 宿主头（经 Host 解引用；PartPlate.hpp 的传递包含与 E1/E2/F1 cpp 同款先例）。
#include "ArrangementService.hpp"
#include "ArrangeRequest.hpp"     // 静态工厂实现宿主（F3 §3：hpp-only，不新建 cpp）

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX                 // 防 min/max 宏泄漏（E2 同款 Windows/asio 前置块）
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN      // 防 winsock.h 拖入与 asio 的 winsock2 冲突
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP（WIN32_LEAN_AND_MEAN 下 asio socket_ops.ipp 需要；实测缺失）
#endif
// 注意：必须早于 PartPlate.hpp（经 Plater.hpp→... 引入 asio 相关头，E1/F1 实测）

#include <algorithm>              // std::find（find_new_position）

#include "SnapshotService.hpp"

#include "../Jobs/Worker.hpp"
#include "../Jobs/ArrangeJob.hpp"      // init_arrange_params / get_wipe_tower_arrangepoly 声明
#include "../Jobs/OrientJob.hpp"
#include "../Jobs/FillBedJob.hpp"
#include "../PartPlate.hpp"            // PartPlateList::get_curr_plate
#include "../I18N.hpp"                 // _u8L
#include "libslic3r/Model.hpp"
#include "libslic3r/Arrange.hpp"
#include "../Plater.hpp"    // 阶段 H1 2.1：Host 消亡后 m_q 门面直调需完整类型（hpp 仍零宿主头）

namespace Slic3r {
namespace GUI {
namespace app {

// —— ArrangeRequest/OrientRequest 静态工厂（F3 §2.4：一行实现，缺省值钉 AllPlates/Selection）——
ArrangeRequest ArrangeRequest::all_plates()    { return {}; }
ArrangeRequest ArrangeRequest::current_plate() { return {Scope::CurrentPlate}; }
OrientRequest  OrientRequest::selection()      { return {}; }
OrientRequest  OrientRequest::current_plate()  { return {Scope::CurrentPlate}; }

ArrangementService::ArrangementService(Plater* q, Model* model, Worker& worker, SnapshotService& snapshots)
    : m_q(q), m_model(model), m_worker(worker), m_snapshots(snapshots)
{
}

// 原 Plater::arrange（2026-10-19 基线 :7858-7865）逐字：worker idle 守卫 → 快照 → replace_job
// F3：前置语义由隐式 prepare-state 写点改为随调用 Request 入参
void ArrangementService::arrange(const ArrangeRequest& req)
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Arrange"));
        replace_job(w, std::make_unique<ArrangeJob>(req));
    }
}

// 原 Plater::orient（基线 :4205-4212）逐字；F3：Request 显式传递
void ArrangementService::orient(const OrientRequest& req)
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Orient"));
        replace_job(w, std::make_unique<OrientJob>(req));
    }
}

// 原 Plater::fill_bed_with_instances（基线 :6096-6103）逐字；快照名沿用 "Arrange"（不改）
void ArrangementService::fill_bed_with_instances()
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Arrange"));
        replace_job(w, std::make_unique<FillBedJob>());
    }
}

// 原 Plater::optimize_rotation（基线 :8001-8008）逐字；复用 OrientJob
// F3 D6：无写点、原隐式依赖残留 DEFAULT 初值——显式钉死 Selection（误传 CurrentPlate 会使 SLA 菜单摆向变盘级语义）
void ArrangementService::optimize_rotation()
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Optimize Rotation"));
        replace_job(w, std::make_unique<OrientJob>(OrientRequest::selection()));
    }
}

// 原 Plater::find_new_position（2026-10-19 基线 :2798-2829）逐字；零活调用，仍按主文档迁移
void ArrangementService::find_new_position(const ModelInstancePtrs &instances)
{
    arrangement::ArrangePolygons movable, fixed;
    arrangement::ArrangeParams arr_params = init_arrange_params(m_q);

    for (const ModelObject *mo : m_model->objects)
        for (ModelInstance *inst : mo->instances) {
            auto it = std::find(instances.begin(), instances.end(), inst);
            arrangement::ArrangePolygon arrpoly;
            inst->get_arrange_polygon(&arrpoly);

            if (it == instances.end())
                fixed.emplace_back(std::move(arrpoly));
            else {
                arrpoly.setter = [it](const arrangement::ArrangePolygon &p) {
                    if (p.is_arranged() && p.bed_idx == 0) {
                        Vec2d t = p.translation.cast<double>();
                        (*it)->apply_arrange_result(t, p.rotation);
                    }
                };
                movable.emplace_back(std::move(arrpoly));
            }
        }

    if (auto wt = get_wipe_tower_arrangepoly(*m_q))
        fixed.emplace_back(*wt);

    arrangement::arrange(movable, fixed, m_q->build_volume().polygon(), arr_params);

    for (auto & m : movable)
        m.apply();
}

// 原 Plater::get_empty_cells（基线 :8028-8057）逐字；唯一差异：宿主反查改为调用方传入 PartPlateList&
std::vector<Vec2f> ArrangementService::get_empty_cells(const Vec2f step, PartPlateList& plates)
{
    PartPlate* plate = plates.get_curr_plate();
    BoundingBoxf3 build_volume = plate->get_build_volume();
    Vec2d vmin(build_volume.min.x(), build_volume.min.y()), vmax(build_volume.max.x(), build_volume.max.y());
    BoundingBoxf bbox(vmin, vmax);
    std::vector<Vec2f> cells;
    auto min_x = step(0)/2;// start_point.x() - step(0) * int((start_point.x() - bbox.min.x()) / step(0));
    auto min_y = step(1)/2;// start_point.y() - step(1) * int((start_point.y() - bbox.min.y()) / step(1));
    auto& exclude_box3s = plate->get_exclude_areas();
    std::vector<BoundingBoxf> exclude_boxs;
    for (auto& box : exclude_box3s) {
        Vec2d vmin(box.min.x(), box.min.y()), vmax(box.max.x(), box.max.y());
        exclude_boxs.emplace_back(vmin, vmax);
    }
    for (float x = min_x + bbox.min.x(); x < bbox.max.x() - step(0) / 2; x += step(0))
        for (float y = min_y + bbox.min.y(); y < bbox.max.y() - step(1) / 2; y += step(1)) {
            bool in_exclude = false;
            BoundingBoxf cell(Vec2d(x - step(0) / 2, y - step(1) / 2), Vec2d(x + step(0) / 2, y + step(1) / 2));
            for (auto& box : exclude_boxs) {
                if (box.overlap(cell)) {
                    in_exclude = true;
                    break;
                }
            }
            if(in_exclude)
                continue;
            cells.emplace_back(x, y);
        }
    return cells;
}

// 原 Plater::priv::can_arrange（2026-10-19 基线 :4435-4438）逐字
bool ArrangementService::can_arrange() const
{
    return !m_model->objects.empty() && m_worker.is_idle();
}

// F3 新增：worker_idle 槽消费改经服务 accessor（hpp 仅前向声明 Worker，行内定义需完整类型——故落 cpp 一行转发）
bool ArrangementService::worker_idle() const
{
    return m_worker.is_idle();
}

// 原 Plater::last_arrange_job_is_finished 内联（hpp :853-857）逐字：CAS false→true
bool ArrangementService::claim_arrange_running()
{
    bool prevRunning = false;
    return m_arrange_running.compare_exchange_strong(prevRunning, true);
}

// 原 ArrangeJob::finalize（Jobs/ArrangeJob.cpp:737）释放端逐字
void ArrangementService::release_arrange_running()
{
    m_arrange_running.store(false);
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
