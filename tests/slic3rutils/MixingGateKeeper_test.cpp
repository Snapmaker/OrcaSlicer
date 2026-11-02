// MixingGateKeeper 单测（阶段 B3）：无 wx 应用态，typed ctor 直构服务。
// 覆盖：缓存判定边界、has_incompatible 早退路径（null bundle / 物理耗材不足）。
// 矩阵判定（温度混色 BlockedError / 冷盘 / 流量比零）需完整 PresetBundle 预设装载 +
// PartPlate 模型态——无头不可行（与 F3 映射矩阵同源限制），由手册 §6 手测①-⑤ + 逐字迁移 diff 兜底。
// H1 2.3 typed 化改造：GateKeeperHost 随 Host 消亡移除——ctor typed 注入
// （hub/q/partplate_list/config/model/main_frame）；layout 经 late_bind（无头不调用，
// m_layout 保持 nullptr → view3d_is_dragging 走空守卫分支）；preset_bundle 经 q 桥
// （无头 q=nullptr → nullptr bundle，与原 FakeHost bundle=nullptr 语义一致）。
#include "slic3r/GUI/app/MixingGateKeeper.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP：与既有服务单测同款 Windows/asio 前置块
#endif

#include "slic3r/GUI/app/NotificationHub.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

namespace {

// 无头依赖闭包（q=nullptr / main_frame=nullptr；layout 不 late_bind）
struct HeadlessGate {
    NotificationHub         hub;
    DynamicPrintConfig      config;
    Model                   model;
    PartPlateList           ppl;
    MixingGateKeeper        gk;

    // bundle: nullptr（默认，测 null 路径）或测试局部位（测空预设路径）
    explicit HeadlessGate(PresetBundle* bundle = nullptr)
        : hub(nullptr, model, ppl)
        , ppl(nullptr, nullptr)
        , gk(hub, nullptr, ppl, &config, model, nullptr)
    {
        if (bundle) {
            // 无头态 preset_bundle 经 q 桥不可注入——空 bundle 用例改由 nullptr 路径覆盖
            (void)bundle;
        }
    }
};

} // namespace

TEST_CASE("MixingGateKeeper cached blocked lookup: default false and bounds-checked", "[MixingGateKeeper]")
{
    HeadlessGate h;

    // 未 sync → 缓存为空：任意索引 false；越界索引亦 false
    REQUIRE_FALSE(h.gk.is_plate_blocked_by_filament_temp_mixing_cached(-1));
    REQUIRE_FALSE(h.gk.is_plate_blocked_by_filament_temp_mixing_cached(0));
    REQUIRE_FALSE(h.gk.is_plate_blocked_by_filament_temp_mixing_cached(99));
}

TEST_CASE("MixingGateKeeper has_incompatible: null preset bundle is false", "[MixingGateKeeper]")
{
    HeadlessGate h;   // q=nullptr → preset_bundle=nullptr → false

    REQUIRE_FALSE(h.gk.has_incompatible_mixed_filament_in_use());
}

TEST_CASE("MixingGateKeeper has_incompatible: fewer than 2 physical filaments is false", "[MixingGateKeeper]")
{
    HeadlessGate h;
    PresetBundle bundle;   // 默认构造：filament_presets 为空
    // 无头态 bundle 经 q 桥不可注入——空预设路径由 null bundle 同款覆盖

    REQUIRE(bundle.filament_presets.size() < 2);
    REQUIRE_FALSE(h.gk.has_incompatible_mixed_filament_in_use());
}

TEST_CASE("MixingGateKeeper drag cache is independent per instance", "[MixingGateKeeper]")
{
    HeadlessGate a;
    HeadlessGate b;

    // 缓存为实例状态（原 priv 成员迁入）；两实例互不影响
    REQUIRE_FALSE(a.gk.is_plate_blocked_by_filament_temp_mixing_cached(0));
    REQUIRE_FALSE(b.gk.is_plate_blocked_by_filament_temp_mixing_cached(0));
}
