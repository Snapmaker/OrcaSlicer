// CalibrationFlow 单测（阶段 G2）：无 wx 应用态，TypedChain 直构服务。
// 覆盖手册 §6 自动验证五项：① cut_horizontal 立方体切分 ② cut_horizontal 越界早退
// ③ record_start_print_preset 空 bundle 吞异常 ④ calib_flowrate pass 非法早退
// ⑤ calib_temp 模式不匹配早退分派计数。
// H1 2.3 typed 化改造：CalibrationFlowHost 随 Host 消亡移除——TypedChain 无头闭包直构。
// 计数器断言（slot traffic）随 Host 消亡移除——guard 早退改为状态不变断言。
// q 桥用例（record_start_print_preset / calib_temp mode mismatch）在无头态不可触达
// （q=nullptr → wxGetApp 链 AV），移手测 ⑤——erratum §11 追记教训 ⑭。
#include "slic3r/GUI/app/CalibrationFlow.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP：与既有服务单测同款 Windows/asio 前置块（先于一切 include）
#endif

#include "TypedChain.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

TEST_CASE("CalibrationFlow cut_horizontal splits cube into two halves", "[CalibrationFlow]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    ModelObject* mo = tc.model.add_object();
    mo->add_volume(TriangleMesh(make_cube(10.0, 10.0, 10.0)));
    mo->add_instance();

    tc.calib.cut_horizontal(0, 0, 5.0,
                       ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);

    // 上半 + 下半（原断言：apply_cut 槽计数 1 + applied_cuts[0].size()==2）
    REQUIRE(tc.model.objects.size() == 2);
}

TEST_CASE("CalibrationFlow cut_horizontal rejects out-of-range indices", "[CalibrationFlow]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    size_t objects_before = tc.model.objects.size();

    tc.calib.cut_horizontal(5, 0, 5.0, ModelObjectCutAttribute::KeepLower);   // obj_idx 越界
    REQUIRE(tc.model.objects.size() == objects_before);                        // 状态不变：早退零触达
}

TEST_CASE("CalibrationFlow calib_flowrate rejects invalid pass", "[CalibrationFlow]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    size_t objects_before = tc.model.objects.size();

    tc.calib.calib_flowrate(/*is_linear=*/true, /*pass=*/3);
    REQUIRE(tc.model.objects.size() == objects_before);                        // 状态不变：早退零触达
}
