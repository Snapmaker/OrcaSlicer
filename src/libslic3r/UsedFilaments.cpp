#include "UsedFilaments.hpp"

#include "Config.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace Slic3r {

namespace {

// Reads a key from the first scope that sets it, else from the global config, else its default.
class ScopedLookup
{
public:
    ScopedLookup(std::initializer_list<const ConfigBase *> scopes, const ConfigBase &global) : m_scopes(scopes), m_global(global) {}

    const ConfigOption *option(const char *key) const
    {
        for (const ConfigBase *scope : m_scopes)
            if (scope != nullptr)
                if (const ConfigOption *opt = scope->option(key); opt != nullptr)
                    return opt;
        if (const ConfigOption *opt = m_global.option(key); opt != nullptr)
            return opt;
        const ConfigOptionDef *def = print_config_def.get(key);
        return def != nullptr ? def->default_value.get() : nullptr;
    }
    int get_int(const char *key) const
    {
        const ConfigOption *opt = this->option(key);
        return opt != nullptr ? opt->getInt() : 0;
    }
    double get_float(const char *key) const
    {
        const ConfigOption *opt = this->option(key);
        return opt != nullptr ? opt->getFloat() : 0.;
    }
    bool get_bool(const char *key) const
    {
        const ConfigOption *opt = this->option(key);
        return opt != nullptr && opt->getBool();
    }
    std::string get_string(const char *key) const
    {
        const auto *opt = dynamic_cast<const ConfigOptionString *>(this->option(key));
        return opt != nullptr ? opt->value : std::string();
    }

private:
    std::vector<const ConfigBase *> m_scopes;
    const ConfigBase               &m_global;
};

// Role filaments of a region with these settings, under the conditions of
// PrintRegion::collect_object_printing_extruders(). 0 follows the part's filament.
void append_role_filaments(const ScopedLookup &region, std::vector<int> &filaments)
{
    auto add = [&region, &filaments](const char *key) {
        if (const int id = region.get_int(key); id > 0)
            filaments.push_back(id);
    };
    const int  wall_loops = region.get_int("wall_loops");
    const int  top        = region.get_int("top_shell_layers");
    const int  bottom     = region.get_int("bottom_shell_layers");
    // Print switches infill this sparse off (region_config_from_model_volume).
    const bool sparse     = region.get_float("sparse_infill_density") >= 0.00011;
    if (wall_loops > 0 || region.get_int("brim_type") != int(btNoBrim))
        add("outer_wall_filament_id");
    if (wall_loops > 1)
        add("inner_wall_filament_id");
    if (sparse)
        add("sparse_infill_filament_id");
    if (sparse || top > 0 || bottom > 0)
        add("internal_solid_filament_id");
    if (top > 0)
        add("top_surface_filament_id");
    if (bottom > 0)
        add("bottom_surface_filament_id");
}

bool object_has_support(const ScopedLookup &object)
{
    return object.get_bool("enable_support") || object.get_int("enforce_support_layers") > 0 || object.get_int("raft_layers") > 0;
}

SupportFilamentRestriction support_restriction(const ScopedLookup &object, bool interface_role)
{
    return { object.get_float("support_nozzle_diameter"),
             object.get_string(interface_role ? "support_interface_material" : "support_base_material") };
}

const char *support_filament_key(bool interface_role)
{
    return interface_role ? "support_interface_filament" : "support_filament";
}

} // namespace

bool support_filament_passes(const ConfigBase &config, unsigned int filament_id, const SupportFilamentRestriction &restriction)
{
    if (filament_id == 0)
        return true;
    const size_t idx = filament_id - 1;
    if (restriction.nozzle_diameter > 0.) {
        const auto *nozzles = dynamic_cast<const ConfigOptionFloats *>(config.option("nozzle_diameter"));
        if (nozzles == nullptr || nozzles->values.empty() || std::abs(nozzles->get_at(idx) - restriction.nozzle_diameter) > EPSILON)
            return false;
    }
    if (! restriction.material.empty()) {
        const auto *types = dynamic_cast<const ConfigOptionStrings *>(config.option("filament_type"));
        if (types == nullptr || types->values.empty() || types->get_at(idx) != restriction.material)
            return false;
    }
    return true;
}

unsigned int resolve_restricted_support_filament(const ConfigBase &config, const SupportFilamentRestriction &restriction)
{
    if (! restriction.active())
        return 0;
    const auto  *nozzles   = dynamic_cast<const ConfigOptionFloats *>(config.option("nozzle_diameter"));
    const auto  *diameters = dynamic_cast<const ConfigOptionFloats *>(config.option("filament_diameter"));
    const auto  *soluble   = dynamic_cast<const ConfigOptionBools *>(config.option("filament_soluble"));
    const size_t num_filaments = std::max(nozzles != nullptr ? nozzles->values.size() : 0, diameters != nullptr ? diameters->values.size() : 0);
    unsigned int soluble_fallback = 0;
    for (size_t i = 0; i < num_filaments; ++ i)
        if (support_filament_passes(config, (unsigned int)(i + 1), restriction)) {
            if (soluble == nullptr || soluble->values.empty() || ! soluble->get_at(i))
                return (unsigned int)(i + 1);
            if (soluble_fallback == 0)
                soluble_fallback = (unsigned int)(i + 1);
        }
    return soluble_fallback;
}

bool model_object_has_restricted_support(const ModelObject &object, const ConfigBase &print_config)
{
    const ScopedLookup lookup({ &object.config.get() }, print_config);
    if (! object_has_support(lookup))
        return false;
    for (const bool interface_role : { false, true })
        if (lookup.get_int(support_filament_key(interface_role)) == 0 && support_restriction(lookup, interface_role).active())
            return true;
    return false;
}

void append_model_object_filaments(const ModelObject &object, const ConfigBase &print_config, const ConfigBase &filament_config,
                                   std::vector<int> &filaments)
{
    const DynamicPrintConfig &object_config = object.config.get();
    for (const ModelVolume *volume : object.volumes) {
        const std::vector<int> volume_filaments = volume->get_extruders();
        filaments.insert(filaments.end(), volume_filaments.begin(), volume_filaments.end());
        if (volume->is_model_part() || volume->is_modifier())
            append_role_filaments(ScopedLookup({ &volume->config.get(), &object_config }, print_config), filaments);
    }

    for (const auto &range : object.layer_config_ranges) {
        const ModelConfig &range_config = range.second;
        if (const ConfigOption *extruder = range_config.option("extruder"); extruder != nullptr && extruder->getInt() > 0)
            filaments.push_back(extruder->getInt());
        append_role_filaments(ScopedLookup({ &range_config.get(), &object_config }, print_config), filaments);
    }

    const ScopedLookup object_lookup({ &object_config }, print_config);
    if (object_has_support(object_lookup))
        for (const bool interface_role : { false, true }) {
            int id = object_lookup.get_int(support_filament_key(interface_role));
            if (id == 0)
                id = int(resolve_restricted_support_filament(filament_config, support_restriction(object_lookup, interface_role)));
            if (id > 0)
                filaments.push_back(id);
        }
}

} // namespace Slic3r
