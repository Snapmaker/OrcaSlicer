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
