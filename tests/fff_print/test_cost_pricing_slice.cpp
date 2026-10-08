// Costs > Project: fees and markup are display only. Changing them never invalidates a slice and
// never reaches the G-code.

#include <catch2/catch.hpp>

#include <fstream>
#include <iterator>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

#include "libslic3r/CostPricing.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

ProjectPricing everything_set()
{
    PricingSettings v;
    v.markup_type         = MarkupType::Percent;
    v.markup_basis        = MarkupBasis::Overall;
    v.markup_value        = 42.;
    v.assembly_hours      = 1.25;
    v.assembly_rate_per_h = 33.;
    v.fee_per_object      = 1.11;
    v.fee_per_part        = 2.22;
    v.fee_per_plate       = 3.33;
    v.packaging_per_plate = 4.44;
    ProjectPricing p;
    for (size_t i = 0; i < PRICING_FIELDS; ++i)
        p.set_field(PricingField(i), v);
    return p;
}

} // namespace

TEST_CASE("Changing fees or markup does not invalidate the slice, and none of it is in the G-code", "[CostPricing][GCode]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"brim_type", "no_brim"}, {"skirt_loops", "0"}, {"sparse_infill_density", "10%"}});
    config.option<ConfigOptionFloats>("filament_cost")->values = {25.};
    config.option<ConfigOptionFloat>("time_cost")->value       = 1.5;

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(print.is_step_done(psSkirtBrim));

    // The project's pricing changes (and is cleared again): Print::apply sees nothing to redo.
    model.pricing = everything_set();
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_UNCHANGED);
    CHECK(print.is_step_done(psSkirtBrim));
    for (const PrintObject *object : print.objects())
        CHECK(object->is_step_done(posSlice));
    model.pricing.clear();
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_UNCHANGED);

    // Export with the pricing set: no fee, markup or selling price anywhere in the G-code.
    model.pricing = everything_set();
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_UNCHANGED);
    const boost::filesystem::path path = scratch_path();
    print.export_gcode(path.string(), nullptr, nullptr);
    std::ifstream     in(path.string(), std::ios::binary);
    const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    boost::nowide::remove(path.string().c_str());
    REQUIRE_FALSE(gcode.empty());
    for (const char *needle : {"edgeslicer_pricing", "markup", "selling", "assembly_rate", "fee_per_object", "fee_per_part",
                               "fee_per_plate", "packaging_per_plate"}) {
        INFO(needle);
        CHECK(gcode.find(needle) == std::string::npos);
    }
}
