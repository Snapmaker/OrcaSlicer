// TypedChain — H1 2.3 typed 化后的无头测试构造链（tests 共享）。
// 用途：批 2/3 typed ctor 注入后，FakeHost 替身模式失效；本链以真实无头对象
// （Model/DynamicPrintConfig/PartPlateList(nullptr plater)/NotificationHub(nullptr q)/
//   SnapshotService/PlateController/SelectionQuery/ProjectSession/SlicingController/ObjectEditCommands/CalibrationFlow）
// 构成服务依赖闭包，供各服务单测直构。
// 约定：
//  - FakeWorker 由本头提供（可控 idle/计数，不启线程）——TypedChain ctor 直收。
//  - LayoutManager/SceneRefreshCoordinator 不可无头构造（Sidebar/wx 依赖）——对应 late_bind
//    不调用，成员保持 nullptr；消费点已加空守卫（erratum §11 批 3 收口登记）。
//  - Plater* 全部 nullptr：q 桥接面（preset_bundle/app_config/...）在无头态不可触达，
//    触达即 AV——依赖 q 桥的用例不得保留（erratum §11 追记：急切求值教训 ⑭）。
//  - 成员声明序 = 依赖序（C++ 初始化序）；fff_print/sla_print/gcode_result 仅被存引用，
//    声明在后合法（slicing ctor 体不使用）。
#pragma once

#include "slic3r/GUI/app/NotificationHub.hpp"
#include "slic3r/GUI/app/SnapshotService.hpp"
#include "slic3r/GUI/app/PlateController.hpp"
#include "slic3r/GUI/app/SelectionQuery.hpp"
#include "slic3r/GUI/app/ProjectSession.hpp"
#include "slic3r/GUI/app/SlicingController.hpp"
#include "slic3r/GUI/app/ObjectEditCommands.hpp"
#include "slic3r/GUI/app/CalibrationFlow.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"

#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Preset.hpp"

#include <atomic>
#include <memory>

namespace Slic3r {
namespace GUI {

// 测试用 Worker：可控 idle 状态，记录 push/cancel（不启线程）。
struct FakeWorker : Worker {
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
};

namespace app {

struct TypedChain {
    // —— 依赖序（声明序 = 构造序；引用成员不可空）——
    Model                   model;
    std::atomic<bool>       fusync{false};
    DynamicPrintConfig      config;
    PartPlateList           ppl;
    NotificationHub         hub;
    SnapshotService         snapshots;
    PlateController         plates;
    SelectionQuery          selq;
    SlicingController       slicing;
    ProjectSession          session;
    ObjectEditCommands      objcmds;
    CalibrationFlow         calib;

    explicit TypedChain(Worker& worker)
        : ppl(nullptr, &model)
        , hub(nullptr, model, ppl)
        , snapshots(nullptr, model, ppl)
        , plates(nullptr, ppl, snapshots, hub, &config)
        , selq(nullptr, &model)
        , slicing(nullptr, ppl, model, &config, hub, snapshots, plates, worker, nullptr,
                  fff_print, sla_print, gcode_result)
        , session(nullptr, &model, &config, snapshots, slicing, ppl, hub, plates, nullptr)
        , objcmds(nullptr, &model, &config, selq, session, slicing, hub, snapshots, ppl, worker)
        , calib(hub, nullptr, model, ppl, nullptr, session, slicing, objcmds)
    {}

    // —— 值成员（SlicingController 注入引用的目标；声明序在后——ctor 仅存引用，合法）——
    Print                   fff_print;
    SLAPrint                sla_print;
    GCodeProcessorResult    gcode_result;
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
