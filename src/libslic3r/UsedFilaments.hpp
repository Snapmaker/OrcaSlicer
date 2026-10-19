#pragma once

#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class ModelObject;

// Support nozzle diameter and material restriction of one support role (base or interface).
struct SupportFilamentRestriction
{
    double      nozzle_diameter = 0.; // 0: any nozzle
    std::string material;             // empty: any filament type
    bool        active() const { return nozzle_diameter > 0. || ! material.empty(); }
};

// May 1-based `filament_id` print support under `restriction`? Its nozzle is nozzle_diameter and
// its type filament_type at filament_id - 1 of `config`; a restriction whose key `config` lacks
// cannot be met. Always true for the "default" filament 0.
bool support_filament_passes(const ConfigBase &config, unsigned int filament_id, const SupportFilamentRestriction &restriction);

// 1-based filament a "default" (0) support role resolves to under `restriction`: the first
// non-soluble passing filament, else the first passing one; 0 when the restriction is inactive
// or nothing passes.
unsigned int resolve_restricted_support_filament(const ConfigBase &config, const SupportFilamentRestriction &restriction);

// Does `object` print support whose default filament a restriction resolves? Resolving it needs
// the nozzle diameters and filament types, which a print preset does not carry.
bool model_object_has_restricted_support(const ModelObject &object, const ConfigBase &print_config);

// Appends the 1-based filaments `object` prints, as Print collects them: volume, painted, layer range and
// role filaments (walls, infill, surfaces, support, interface), settings read over `print_config`;
// a default support filament under a restriction resolves against `filament_config`.
void append_model_object_filaments(const ModelObject &object, const ConfigBase &print_config, const ConfigBase &filament_config,
                                   std::vector<int> &filaments);

} // namespace Slic3r
