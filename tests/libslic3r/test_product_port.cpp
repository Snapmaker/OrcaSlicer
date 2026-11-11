#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/FullSpectrumKSPairResidual.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <cmath>
#include <locale>
#include <regex>
#include <set>

using namespace Slic3r;
using Catch::Approx;

TEST_CASE("Product color library identifies the shipping bundle and its TD values", "[ProductPort][FullSpectrumKS]")
{
    struct RestoreResources
    {
        std::string path = resources_dir();
        ~RestoreResources()
        {
            set_resources_dir(path);
            FilamentColorLibrary::Instance().Reload();
        }
    } restore;
    set_resources_dir(TEST_RESOURCES_DIR);
    auto& library = FilamentColorLibrary::Instance();
    library.Reload();
    FilamentColorInfo bundle;
    REQUIRE(library.FindFilamentById("14170311270", bundle));
    for (const auto& [sku, td] :
         std::vector<std::pair<std::string, double>>{{"34267", 5.5}, {"34268", 5.5}, {"34269", 9.5}, {"34265", 6.5}}) {
        const auto found = std::find_if(bundle.colors.begin(), bundle.colors.end(), [&](const auto& item) { return item.sku == sku; });
        REQUIRE(found != bundle.colors.end());
        CHECK(found->tdValue == Approx(td));
        CHECK(found->fullSpectrumMaterialId == "snapmaker:" + sku);
        const std::string measured = sku == "34267" ? "#008BB3" : sku == "34268" ? "#AD4A76" : sku == "34269" ? "#EBBE00" : "#7B7F80";
        CHECK(found->colorData.PrimaryColor() == measured);
        CHECK_FALSE(found->legacyPrimaryColor.empty());
    }
}

