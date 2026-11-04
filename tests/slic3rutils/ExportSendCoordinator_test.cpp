#include "slic3r/GUI/app/ExportSendCoordinator.hpp"
#include "slic3r/GUI/app/ProjectSession.hpp"
#include "slic3r/GUI/app/SnapshotService.hpp"
#include "slic3r/GUI/I18N.hpp"               // _L（join_gcode_filename Untitled 用例）
#include "TypedChain.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;



TEST_CASE("ExportSendCoordinator constructs with TypedChain (smoke)", "[ExportSendCoordinator]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    ExportSendCoordinator ex(nullptr, &tc.model, &tc.config, tc.session, tc.slicing, tc.ppl, tc.hub, nullptr);
    REQUIRE_NOTHROW(ex.is_not_exporting());
}


TEST_CASE("join_gcode_filename: folder + export_all joins folder/project/extension", "[ExportSendCoordinator]")
{
    boost::filesystem::path folder("d:/ws/proj");
    auto got = ExportSendCoordinator::join_gcode_filename(folder, "MyProject", "_plate_1", ".gcode",
                                                          /*only_filename=*/false, /*export_all=*/true);
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
