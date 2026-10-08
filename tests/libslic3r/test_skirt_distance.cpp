#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// Orca #12999: Arrange reserves get_real_skirt_dist() around the bed; the skirt loops themselves
// have a width, so the reserve is the distance plus the loops' own width, not the distance alone.
namespace {

DynamicPrintConfig skirt_config(int loops, double distance, double first_layer_width)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_key_value("skirt_height", new ConfigOptionInt(1));
    cfg.set_key_value("skirt_loops", new ConfigOptionInt(loops));
    cfg.set_key_value("skirt_distance", new ConfigOptionFloat(distance));
    cfg.set_key_value("initial_layer_line_width", new ConfigOptionFloat(first_layer_width));
    cfg.set_key_value("draft_shield", new ConfigOptionEnum<DraftShield>(dsDisabled));
    return cfg;
}

} // namespace

TEST_CASE("Real skirt distance includes the width of the skirt loops", "[Arrange][Skirt]")
{
    CHECK(get_real_skirt_dist(skirt_config(1, 2., 0.5)) == Approx(2.5));
    CHECK(get_real_skirt_dist(skirt_config(3, 2., 0.5)) == Approx(3.5));
    CHECK(get_real_skirt_dist(skirt_config(3, 0., 0.42)) == Approx(1.26));
}

TEST_CASE("Real skirt distance is zero without a skirt", "[Arrange][Skirt]")
{
    CHECK(get_real_skirt_dist(skirt_config(0, 2., 0.5)) == Approx(0.));

    DynamicPrintConfig no_height = skirt_config(3, 2., 0.5);
    no_height.set_key_value("skirt_height", new ConfigOptionInt(0));
    CHECK(get_real_skirt_dist(no_height) == Approx(0.));
}

TEST_CASE("A draft shield counts as one skirt loop", "[Arrange][Skirt]")
{
    DynamicPrintConfig cfg = skirt_config(0, 2., 0.5);
    cfg.set_key_value("draft_shield", new ConfigOptionEnum<DraftShield>(dsEnabled));
    CHECK(get_real_skirt_dist(cfg) == Approx(2.5));
}
