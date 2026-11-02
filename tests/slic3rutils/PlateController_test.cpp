// PlateController 单测（阶段 D1）：无 wx 核心决策面。
// 测试面边界（手册 §4 步骤 2 执行期裁定）：PartPlateList 的变更操作（create/duplicate/
// delete_plate 真实执行）经 PartPlate::init 等触达 GUI 深层单例（wxGetApp/OpenGL 纹理），
// 无头测试进程不可达——盘变更路径由手测覆盖；本文件只测不驱动真实盘变更的决策面。
// H1 2.3 typed 化改造：HostRecorder 随 Host 消亡移除（调用顺序契约改为结构直连——
// 无槽可错接）；slicing 切片守卫例退休（m_slicing 为 late_bind 指针，无头不可构造
// SlicingController 实例，切片拒绝路径移手测 ⑥——erratum §11 同批）。
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

// 无头 wx 引导（惰性）：盘查询链路（PartPlateList 构造等）触达 wx 基础设施。
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

// 无头 PlateController + 依赖闭包（q/partplate_list/snapshots/config；slicing/layout/refresh
// 不构造——late_bind 不调用，成员保持 nullptr）。
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

    // 初始单盘（PartPlateList 构造即含 1 盘）：可加、不可删（count>1 不满足；slicing 未绑=未切片）
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
    // 原数学：Polygon::new_scale(values).bounding_box() → BoundingBoxf(unscale(min), unscale(max))
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
