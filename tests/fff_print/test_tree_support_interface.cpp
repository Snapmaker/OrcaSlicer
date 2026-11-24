#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <cstdlib>

using namespace Slic3r;

namespace {

class ScopedSupportTestDirectory
{
public:
    ScopedSupportTestDirectory()
        : m_previous_temporary(temporary_dir())
        , m_previous_data(data_dir())
        , m_directory(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tree-interface-%%%%-%%%%"))
    {
        boost::filesystem::create_directories(m_directory);
        set_temporary_dir(m_directory.string());
        set_data_dir(m_directory.string());
    }

    ~ScopedSupportTestDirectory()
    {
        set_temporary_dir(m_previous_temporary);
        set_data_dir(m_previous_data);
        boost::system::error_code error;
        boost::filesystem::remove_all(m_directory, error);
    }

private:
    std::string             m_previous_temporary;
    std::string             m_previous_data;
    boost::filesystem::path m_directory;
};

void collect_role_footprint(const ExtrusionEntity& entity, ExtrusionRole role, Polygons& footprint)
{
    if (entity.is_collection()) {
        for (const ExtrusionEntity* child : static_cast<const ExtrusionEntityCollection&>(entity).entities)
            collect_role_footprint(*child, role, footprint);
    } else if (entity.role() == role) {
        entity.polygons_covered_by_width(footprint, 0.f);
    }
}

} // namespace

// The external curved-model project is deliberately opt-in; see the fixture's three
// contact bands below. A cropped foot or a planar ramp does not retain this failure.
// Fixture SHA256: 5114240fcf2ab53f192d2c9bcad89101c5577d19203b75a62041f1e94b4c3e12.
TEST_CASE("Tree support preserves interface paths at curved model contacts", "[.][SupportMaterial][TreeSupport][SlopedInterface]")
{
    const char* fixture = std::getenv("SLIC3R_SLOPED_INTERFACE_3MF");
    if (fixture == nullptr || *fixture == '\0')
        SKIP("Set SLIC3R_SLOPED_INTERFACE_3MF to the original curved-support defect project");

    ScopedSupportTestDirectory temporary;
    Model                      model;
    DynamicPrintConfig         config;
    ConfigSubstitutionContext  substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs              plates;
    std::vector<Preset*>       presets;
    bool                       is_bbl = false;
    Semver                     version;
    const bool loaded = load_bbs_3mf(fixture, &config, &substitutions, &model, &plates, &presets, &is_bbl, &version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances);
    release_PlateData_list(plates);
    for (Preset* preset : presets)
        delete preset;
    REQUIRE(loaded);
    REQUIRE(model.objects.size() == 1);

    bool interfaces_enabled = true;
    SECTION("Dedicated support interface filament") {}
    SECTION("Model and interface use the same filament") { config.set_key_value("support_interface_filament", new ConfigOptionInt(1)); }
    SECTION("Disabled top interfaces do not generate transition paths")
    {
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
        interfaces_enabled = false;
    }

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject& object = *print.objects().front();
    REQUIRE_FALSE(object.support_layers().empty());

    if (!interfaces_enabled) {
        for (const SupportLayer* support : object.support_layers()) {
            Polygons transition_paths;
            collect_role_footprint(support->support_fills, erSupportTransition, transition_paths);
            CHECK(transition_paths.empty());
        }
        return;
    }

    for (const double contact_z : {5.45, 6.65, 39.25}) {
        const SupportLayer* support = nullptr;
        const Layer*        current = nullptr;
        const Layer*        above   = nullptr;
        for (const SupportLayer* layer : object.support_layers())
            if (std::abs(layer->print_z - contact_z) < EPSILON)
                support = layer;
        for (const Layer* layer : object.layers()) {
            if (std::abs(layer->print_z - contact_z) < EPSILON)
                current = layer;
            if (std::abs(layer->bottom_z() - contact_z) < EPSILON)
                above = layer;
        }
        CAPTURE(contact_z);
        REQUIRE(support != nullptr);
        REQUIRE(current != nullptr);
        REQUIRE(above != nullptr);

        // Exclude the configured XY keep-out and a 0.05 mm edge margin, so the
        // assertion measures actual model contact rather than nearby deeper roofs.
        const ExPolygons contact = offset_ex(diff_ex(above->lslices,
                                                     offset_ex(current->lslices, scale_(object.config().support_object_xy_distance.value))),
                                             -scale_(0.05));
        REQUIRE(area(contact) * SCALING_FACTOR * SCALING_FACTOR > 0.01);
        Polygons interface_paths;
        Polygons transition_paths;
        collect_role_footprint(support->support_fills, erSupportMaterialInterface, interface_paths);
        collect_role_footprint(support->support_fills, erSupportTransition, transition_paths);
        const ExPolygons interface_contact        = intersection_ex(contact, interface_paths);
        const ExPolygons transition_contact       = intersection_ex(contact, transition_paths);
        const double     interface_fraction       = area(interface_contact) / area(contact);
        const double     transition_only_fraction = area(diff_ex(transition_contact, interface_contact)) / area(contact);
        CAPTURE(interface_fraction, transition_only_fraction);
        CHECK(interface_fraction > 0.9);
        CHECK(transition_only_fraction < 0.01);
    }
}
