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

ArrangeRequest ArrangeRequest::all_plates()    { return {}; }
ArrangeRequest ArrangeRequest::current_plate() { return {Scope::CurrentPlate}; }
OrientRequest  OrientRequest::selection()      { return {}; }
OrientRequest  OrientRequest::current_plate()  { return {Scope::CurrentPlate}; }

ArrangementService::ArrangementService(Plater* q, Model* model, Worker& worker, SnapshotService& snapshots)
    : m_q(q), m_model(model), m_worker(worker), m_snapshots(snapshots)
{
}

void ArrangementService::arrange(const ArrangeRequest& req)
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Arrange"));
        replace_job(w, std::make_unique<ArrangeJob>(req));
    }
}

void ArrangementService::orient(const OrientRequest& req)
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Orient"));
        replace_job(w, std::make_unique<OrientJob>(req));
    }
}

void ArrangementService::fill_bed_with_instances()
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Arrange"));
        replace_job(w, std::make_unique<FillBedJob>());
    }
}

void ArrangementService::optimize_rotation()
{
    auto &w = m_worker;
    if (w.is_idle()) {
        m_snapshots.take_snapshot(_u8L("Optimize Rotation"));
        replace_job(w, std::make_unique<OrientJob>(OrientRequest::selection()));
    }
}

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

bool ArrangementService::can_arrange() const
{
    return !m_model->objects.empty() && m_worker.is_idle();
}

bool ArrangementService::worker_idle() const
{
    return m_worker.is_idle();
}

bool ArrangementService::claim_arrange_running()
{
    bool prevRunning = false;
    return m_arrange_running.compare_exchange_strong(prevRunning, true);
}

void ArrangementService::release_arrange_running()
{
    m_arrange_running.store(false);
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
