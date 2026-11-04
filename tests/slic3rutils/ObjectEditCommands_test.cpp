#include "slic3r/GUI/app/ObjectEditCommands.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP：SlicingController.hpp→BackgroundSlicingProcess.hpp→PrintHost.hpp 引入 asio 前必须
#endif

#include "TypedChain.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;


TEST_CASE("ObjectEditCommands can_split_to_volumes: ptSLA gate short-circuits to false", "[ObjectEditCommands]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    tc.session.printer_technology = ptSLA;
    REQUIRE_FALSE(tc.objcmds.can_split_to_volumes());   // 短路：printer_technology != ptSLA 为假，不调 can_split/obj_list
}


TEST_CASE("ObjectEditCommands can_delete_all: mirrors model.objects emptiness", "[ObjectEditCommands]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    tc.model.clear_objects();   // TypedChain 构造期 PartPlateList init 可能注入默认对象——清空后测纯模型语义

    REQUIRE_FALSE(tc.objcmds.can_delete_all());   // 空模型
    tc.model.add_object();
    REQUIRE(tc.objcmds.can_delete_all());         // 有对象
}


TEST_CASE("ObjectEditCommands can_delete: empty selection is false", "[ObjectEditCommands]")
{
    FakeWorker worker;
    TypedChain tc(worker);

    REQUIRE_FALSE(tc.objcmds.can_delete());   // 空选中
}

// —— 4.2 can_fillcolor: constant true ——

TEST_CASE("ObjectEditCommands can_fillcolor: constant true", "[ObjectEditCommands]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    REQUIRE(tc.objcmds.can_fillcolor());
}
