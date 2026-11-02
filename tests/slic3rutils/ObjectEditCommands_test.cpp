// ObjectEditCommands 单测（阶段 F1，tasks 4.2）：谓词语义钉子——无 wx 应用态，TypedChain 直构服务。
// 无头约束（audit/tdd_green.md 执行期修正 2）：obj_list/view3d 等路径不可解引用（ObjectList/View3D 需 wx 应用态），
// 故只覆盖不触达这些路径的短路/纯模型路径；其余谓词由逐字 diff + 手测灰显矩阵兜底。
// H1 2.3 typed 化改造：ObjectEditCommandsHost 随 Host 消亡移除——TypedChain 无头闭包直构。
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

// —— 4.2 can_split_to_volumes：ptSLA 门控短路（原 priv 体 :4731-4734 语义；不触达 obj_list）——

TEST_CASE("ObjectEditCommands can_split_to_volumes: ptSLA gate short-circuits to false", "[ObjectEditCommands]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    tc.session.printer_technology = ptSLA;
    REQUIRE_FALSE(tc.objcmds.can_split_to_volumes());   // 短路：printer_technology != ptSLA 为假，不调 can_split/obj_list
}

// —— 4.2 can_delete_all：纯模型语义（原 priv 体 :4662-4665）——

TEST_CASE("ObjectEditCommands can_delete_all: mirrors model.objects emptiness", "[ObjectEditCommands]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    tc.model.clear_objects();   // TypedChain 构造期 PartPlateList init 可能注入默认对象——清空后测纯模型语义

    REQUIRE_FALSE(tc.objcmds.can_delete_all());   // 空模型
    tc.model.add_object();
    REQUIRE(tc.objcmds.can_delete_all());         // 有对象
}

// —— 4.2 can_delete 空选中路径（view3d_canvas 空 → SelectionQuery 静态空 Selection 防御）——

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
