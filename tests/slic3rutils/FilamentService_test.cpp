// FilamentService 单测（阶段 G1）：无 wx 应用态，TypedChain 直构服务。
// 覆盖手册 §6 自动验证：① 全局映射 set/get 往返 ② 下一颜色周期 ③ 批量删除守卫计数。
// H1 2.3 typed 化改造：FilamentServiceHost 随 Host 消亡移除——TypedChain 无头闭包直构。
// 计数器断言（queue_filament_usage_event 槽）随 Host 消亡移除——wxQueueEvent 空守卫后
// 无头 no-op，去抖语义移手测。
// q 桥用例（update_filament_colors_in_full_config——preset_bundle 经 q 桥）在无头态
// 不可触达（q=nullptr → wxGetApp 链 AV），移手测——erratum §11 追记教训 ⑭。
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/app/FilamentService.hpp"
#include "slic3r/GUI/app/NotificationHub.hpp"
#include "TypedChain.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

#include <wx/colour.h>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

namespace {

// FILAMENT_SYSTEM_COLORS_NUM = 16（Plater.hpp:38）；测试不引宿主头，按值钉住。
constexpr int kFilamentSystemColorsNum = 16;

} // namespace

TEST_CASE("FilamentService global filament map: set/get roundtrip", "[FilamentService]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    FilamentService svc(tc.hub, nullptr, &tc.config, tc.ppl, nullptr, tc.slicing, tc.fusync);

    svc.set_global_filament_map({0, 1, 2});
    REQUIRE(svc.get_global_filament_map() == std::vector<int>{0, 1, 2});

    svc.set_global_filament_map({2});
    REQUIRE(svc.get_global_filament_map() == std::vector<int>{2});
}

TEST_CASE("FilamentService next color: N+1 calls wrap to the first color", "[FilamentService]")
{
    std::vector<wxColour> colours;
    for (int i = 0; i < kFilamentSystemColorsNum + 1; ++i)
        colours.push_back(FilamentService::get_next_color_for_filament());

    // 周期 = FILAMENT_SYSTEM_COLORS_NUM：第 N+1 次与第 1 次同色（静态计数器相位不受单测影响）
    REQUIRE(colours[kFilamentSystemColorsNum] == colours[0]);
}

TEST_CASE("FilamentService batch physical deletion: inc/dec count", "[FilamentService]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    FilamentService svc(tc.hub, nullptr, &tc.config, tc.ppl, nullptr, tc.slicing, tc.fusync);

    REQUIRE(svc.batch_physical_deletion() == 0);
    svc.inc_batch_physical_deletion();
    REQUIRE(svc.batch_physical_deletion() == 1);
    svc.inc_batch_physical_deletion();
    REQUIRE(svc.batch_physical_deletion() == 2);
    svc.dec_batch_physical_deletion();
    REQUIRE(svc.batch_physical_deletion() == 1);
    svc.dec_batch_physical_deletion();
    REQUIRE(svc.batch_physical_deletion() == 0);
}
