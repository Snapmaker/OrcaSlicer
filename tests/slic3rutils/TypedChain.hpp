// （Model/DynamicPrintConfig/PartPlateList(nullptr plater)/NotificationHub(nullptr q)/
//   SnapshotService/PlateController/SelectionQuery/ProjectSession/SlicingController/ObjectEditCommands/CalibrationFlow）
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

    Print                   fff_print;
    SLAPrint                sla_print;
    GCodeProcessorResult    gcode_result;
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
