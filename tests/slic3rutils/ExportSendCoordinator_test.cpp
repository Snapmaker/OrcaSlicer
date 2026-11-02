// ExportSendCoordinator 单测（阶段 E2，手册 §2 / design D7）：无 wx 对话框核心，Catch2 + TypedChain 直构。
// join_gcode_filename 纯函数用例与 exporting 谓词语义用例随任务 2.1-2.3 落地（TDD：先行红、实现后绿）。
// H1 2.3 typed 化改造：ExportSendCoordinatorHost 随 Host 消亡移除——TypedChain 无头闭包直构。
#include "slic3r/GUI/app/ExportSendCoordinator.hpp"
#include "slic3r/GUI/app/ProjectSession.hpp"
#include "slic3r/GUI/app/SnapshotService.hpp"
#include "slic3r/GUI/I18N.hpp"               // _L（join_gcode_filename Untitled 用例）
#include "TypedChain.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

// ExportSendCoordinator 不可拷贝/移动（ProjectSession& 成员）——各用例直接构造。

// —— 1.4 骨架冒烟：TypedChain 直构服务（EX→PS typed 注入，design D1）——

TEST_CASE("ExportSendCoordinator constructs with TypedChain (smoke)", "[ExportSendCoordinator]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    ExportSendCoordinator ex(nullptr, &tc.model, &tc.config, tc.session, tc.slicing, tc.ppl, tc.hub, nullptr);
    REQUIRE_NOTHROW(ex.is_not_exporting());
}

// —— 2.1 join_gcode_filename 拼装纯函数（手册 §2.1 表 4；原 priv::get_export_gcode_filename :4549 分支结构）——

TEST_CASE("join_gcode_filename: folder + export_all joins folder/project/extension", "[ExportSendCoordinator]")
{
    boost::filesystem::path folder("d:/ws/proj");
    auto got = ExportSendCoordinator::join_gcode_filename(folder, "MyProject", "_plate_1", ".gcode",
                                                          /*only_filename=*/false, /*export_all=*/true);
    // 原实现：folder / std::string((project_name + extension).mb_str(wxConvUTF8)) 后 from_path（盘符路径分隔符随平台）
    boost::filesystem::path expected = folder / std::string("MyProject.gcode");
    REQUIRE(got.ToStdString() == expected.string());
}

TEST_CASE("join_gcode_filename: folder + only_filename keeps project_name + plate suffix", "[ExportSendCoordinator]")
{
    auto got = ExportSendCoordinator::join_gcode_filename(boost::filesystem::path("d:/ws/proj"), "MyProject", "_plate_1", ".gcode",
                                                          /*only_filename=*/true, /*export_all=*/false);
    REQUIRE(got.ToStdString() == std::string("MyProject_plate_1.gcode"));
}

TEST_CASE("join_gcode_filename: empty folder + Untitled falls back to object name", "[ExportSendCoordinator]")
{
    auto got = ExportSendCoordinator::join_gcode_filename(boost::filesystem::path(), _L("Untitled"), "_plate_1", ".gcode",
                                                          /*only_filename=*/true, /*export_all=*/false,
                                                          /*untitled_object_name=*/"cube");
    REQUIRE(got.ToStdString() == std::string("cube_plate_1.gcode"));
}

// —— 2.2 exporting 状态谓词（原 priv::exporting_status :1227 + 装配 lambda :1465-1472 语义）——

TEST_CASE("exporting predicates: default state is NOT_EXPORTING", "[ExportSendCoordinator]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    ExportSendCoordinator ex(nullptr, &tc.model, &tc.config, tc.session, tc.slicing, tc.ppl, tc.hub, nullptr);

    REQUIRE(ex.is_not_exporting());
    REQUIRE_FALSE(ex.exporting_active());
    REQUIRE_FALSE(ex.is_exporting_to_removable());
    REQUIRE_FALSE(ex.is_exporting_to_local());
    REQUIRE(ex.is_publishing() == false);
}

TEST_CASE("exporting predicates: int bridge matches ExportingStatus enum order", "[ExportSendCoordinator]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    ExportSendCoordinator ex(nullptr, &tc.model, &tc.config, tc.session, tc.slicing, tc.ppl, tc.hub, nullptr);

    ex.set_exporting_status(1);   // EXPORTING_TO_REMOVABLE（原枚举 :759-762 顺序）
    REQUIRE(ex.exporting_active());
    REQUIRE(ex.is_exporting_to_removable());
    REQUIRE(ex.exporting_to_removable());
    REQUIRE_FALSE(ex.is_not_exporting());
    REQUIRE_FALSE(ex.is_exporting_to_local());

    ex.set_exporting_status(2);   // EXPORTING_TO_LOCAL
    REQUIRE(ex.exporting_active());
    REQUIRE(ex.is_exporting_to_local());
    REQUIRE(ex.exporting_to_local());
    REQUIRE_FALSE(ex.is_exporting_to_removable());

    ex.set_not_exporting();
    REQUIRE(ex.is_not_exporting());
    REQUIRE_FALSE(ex.exporting_active());

    ex.set_is_publishing(true);
    REQUIRE(ex.is_publishing());
}
