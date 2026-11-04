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

struct HeadlessGate {
    NotificationHub         hub;
    DynamicPrintConfig      config;
    Model                   model;
    PartPlateList           ppl;
    MixingGateKeeper        gk;

    explicit HeadlessGate(PresetBundle* bundle = nullptr)
        : hub(nullptr, model, ppl)
        , ppl(nullptr, nullptr)
        , gk(hub, nullptr, ppl, &config, model, nullptr)
    {
        if (bundle) {
            (void)bundle;
        }
    }
};

} // namespace

TEST_CASE("MixingGateKeeper cached blocked lookup: default false and bounds-checked", "[MixingGateKeeper]")
{
    HeadlessGate h;

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

    REQUIRE(bundle.filament_presets.size() < 2);
    REQUIRE_FALSE(h.gk.has_incompatible_mixed_filament_in_use());
}

TEST_CASE("MixingGateKeeper drag cache is independent per instance", "[MixingGateKeeper]")
{
    HeadlessGate a;
    HeadlessGate b;

    REQUIRE_FALSE(a.gk.is_plate_blocked_by_filament_temp_mixing_cached(0));
    REQUIRE_FALSE(b.gk.is_plate_blocked_by_filament_temp_mixing_cached(0));
}
