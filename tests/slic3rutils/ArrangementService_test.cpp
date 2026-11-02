// ArrangementService 单测（阶段 F2，tasks 2.1-2.3）：无 wx 应用态，Catch2 直构服务 + FakeWorker。
// 覆盖抢占锁 CAS 三态与 can_arrange 三态；其余摆放域入口由逐字 diff + 手册 §6 手测兜底。
#include "slic3r/GUI/app/ArrangementService.hpp"
#include "slic3r/GUI/PartPlate.hpp"   // H1 2.3：PartPlateList 无头构造（typed SnapshotService ctor）
#include "slic3r/GUI/app/ArrangeRequest.hpp"   // F3：Request 值语义直用

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP：与既有服务单测同款 Windows/asio 前置块
#endif

#include "slic3r/GUI/app/SnapshotService.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

namespace {

// 测试用 Worker：可控 idle 状态，记录 push/cancel_all（不启线程）。
class FakeWorker : public Worker {
public:
    bool idle = true;
    int  push_count = 0;
    int  cancel_all_count = 0;

    bool push(std::unique_ptr<Job> job) override { ++push_count; return job != nullptr; }
    bool is_idle() const override { return idle; }
    void cancel() override {}
    void cancel_all() override { ++cancel_all_count; }
    void process_events() override {}
    bool wait_for_current_job(unsigned) override { return true; }
    bool wait_for_idle(unsigned) override { return true; }
    ~FakeWorker() override = default;
};

} // namespace

// —— 2.1 抢占锁 CAS 三态（原 last_arrange_job_is_finished 内联 / ArrangeJob::finalize 释放端）——

TEST_CASE("ArrangementService claim/release: exclusive claim then release reopens", "[ArrangementService]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    SnapshotService snapshots(nullptr, model, ppl);
    FakeWorker worker;
    ArrangementService svc(nullptr, &model, worker, snapshots);

    REQUIRE(svc.claim_arrange_running());        // 空锁：false→true
    REQUIRE_FALSE(svc.claim_arrange_running());  // 持锁期间再 claim：CAS 失败
    svc.release_arrange_running();               // ArrangeJob::finalize 释放端
    REQUIRE(svc.claim_arrange_running());        // 释放后可再次抢占
}

// —— 2.2 can_arrange 三态（原 priv 体 :4435-4438）——

TEST_CASE("ArrangementService can_arrange: empty model / busy worker / idle with objects", "[ArrangementService]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    SnapshotService snapshots(nullptr, model, ppl);
    FakeWorker worker;
    ArrangementService svc(nullptr, &model, worker, snapshots);

    REQUIRE_FALSE(svc.can_arrange());   // 空 Model → false
    model.add_object();
    worker.idle = false;
    REQUIRE_FALSE(svc.can_arrange());   // FakeWorker busy → false
    worker.idle = true;
    REQUIRE(svc.can_arrange());         // idle + 有对象 → true
}

// —— F3 1.5：ArrangeRequest 缺省值钉 AllPlates（原 DEFAULT 隐式初值语义）——

TEST_CASE("ArrangeRequest default scope is AllPlates", "[ArrangementService]")
{
    REQUIRE(ArrangeRequest{}.scope == ArrangeRequest::Scope::AllPlates);
    REQUIRE(ArrangeRequest::all_plates().scope == ArrangeRequest::Scope::AllPlates);
    REQUIRE(ArrangeRequest::current_plate().scope == ArrangeRequest::Scope::CurrentPlate);
}

// —— F3 1.5：OrientRequest 缺省值钉 Selection（原 DEFAULT=选中对象；与 Arrange 异构，禁统一——手册陷阱 1）——

TEST_CASE("OrientRequest default scope is Selection", "[ArrangementService]")
{
    REQUIRE(OrientRequest{}.scope == OrientRequest::Scope::Selection);
    REQUIRE(OrientRequest::selection().scope == OrientRequest::Scope::Selection);
    REQUIRE(OrientRequest::current_plate().scope == OrientRequest::Scope::CurrentPlate);
}

// —— F3 1.5：worker_idle 双态（原 slicing_host.worker_idle 直读 m_worker.is_idle，F3 改经服务 accessor 消费）——

TEST_CASE("ArrangementService worker_idle mirrors worker state", "[ArrangementService]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    SnapshotService snapshots(nullptr, model, ppl);
    FakeWorker worker;
    ArrangementService svc(nullptr, &model, worker, snapshots);

    worker.idle = true;
    REQUIRE(svc.worker_idle());
    worker.idle = false;
    REQUIRE_FALSE(svc.worker_idle());
}

// —— F3 1.5：arrange/orient(req) busy 守卫（worker busy → 无 push、无快照）；
//    idle 路径 replace_job 会反查 wxGetApp().plater()，不可无头构造（手册 §0 发现 3）——映射由逐字 diff + 手测兜底。

TEST_CASE("ArrangementService arrange/orient(req): busy guard means no push no snapshot", "[ArrangementService]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    SnapshotService snapshots(nullptr, model, ppl);
    FakeWorker worker;
    ArrangementService svc(nullptr, &model, worker, snapshots);

    worker.idle = false;
    const bool stack_empty_before = snapshots.undo_redo_stack_main().empty();
    svc.arrange(ArrangeRequest::all_plates());
    REQUIRE(worker.push_count == 0);                                          // 守卫拦截：无 push
    REQUIRE(snapshots.undo_redo_stack_main().empty() == stack_empty_before);  // 守卫拦截：无快照
    svc.orient(OrientRequest::current_plate());
    REQUIRE(worker.push_count == 0);                                          // orient 同款守卫
}
