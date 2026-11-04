#include "libslic3r/ObjectID.hpp"   // ObjectBase（PartPlateList 基类）
#include "slic3r/GUI/PartPlate.hpp"       // PartPlateList 完整类型（get_plate_count / MAX_PLATES_COUNT）
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <string>
#include <vector>

#include <wx/wx.h>
#include <wx/app.h>
#include <wx/init.h>

#include "slic3r/GUI/app/PlateController.hpp"
#include "slic3r/GUI/app/NotificationHub.hpp"   // 阶段 I：PlateController 构造注入 NotificationHub&
#include "slic3r/GUI/app/SnapshotService.hpp"
#include "libslic3r/Geometry.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

namespace {

struct WxBootstrap {
    WxBootstrap()
    {
        wxApp::SetInstance(new wxApp());
        m_init = new wxInitializer();
    }
    ~WxBootstrap()
    {
        delete m_init;
        wxApp::SetInstance(nullptr);
    }
    wxInitializer* m_init = nullptr;
};
static WxBootstrap& wx_boot()   // 惰性：--list-tests（catch_discover_tests）不触发
{
    static WxBootstrap b;
    return b;
}

struct HeadlessPlates {
    Model                     model;
    DynamicPrintConfig        config;
    PartPlateList             ppl{nullptr, &model};
    NotificationHub           hub{nullptr, model, ppl};
    SnapshotService           snapshots{nullptr, model, ppl};
    PlateController           svc{nullptr, ppl, snapshots, hub, &config};
};

} // namespace

TEST_CASE("plate gates on fresh single-plate list", "[PlateController]")
{
    wx_boot();
    HeadlessPlates h;

    REQUIRE(h.svc.can_add_plate());
    REQUIRE_FALSE(h.svc.can_delete_plate());
}

TEST_CASE("bed_shape_bb maps scaled bbox to unscaled BoundingBoxf", "[PlateController]")
{
    wx_boot();
    HeadlessPlates h;
    ConfigOptionPoints opt;
    opt.values = {Vec2d(0, 0), Vec2d(200, 0), Vec2d(200, 200), Vec2d(0, 200)};
    h.config.set_key_value("printable_area", new ConfigOptionPoints(opt));

    const BoundingBoxf bb = h.svc.bed_shape_bb();
    REQUIRE(bb.min.x() == Catch::Approx(0.0).margin(1e-6));
    REQUIRE(bb.min.y() == Catch::Approx(0.0).margin(1e-6));
    REQUIRE(bb.max.x() == Catch::Approx(200.0).margin(1e-3));
    REQUIRE(bb.max.y() == Catch::Approx(200.0).margin(1e-3));
}

TEST_CASE("right_click_plate_index round-trips", "[PlateController]")
{
    wx_boot();
    HeadlessPlates h;
    h.svc.set_right_click_plate_index(3);
    REQUIRE(h.svc.right_click_plate_index() == 3);
    h.svc.set_right_click_plate_index(-1);
    REQUIRE(h.svc.right_click_plate_index() == -1);
}
