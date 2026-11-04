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
