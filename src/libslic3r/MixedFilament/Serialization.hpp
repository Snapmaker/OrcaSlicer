#ifndef slic3r_MixedFilament_Serialization_hpp_
#define slic3r_MixedFilament_Serialization_hpp_

#include "../MixedFilament.hpp"

namespace Slic3r {

inline constexpr const char* MIXED_FILAMENTS_3MF_PATH = "Metadata/mixed_filaments.json";

// JSON operates on the existing typed definitions. Invalid or unsupported data
// throws std::runtime_error; callers must not fall back to stale legacy rows.
std::string serialize_mixed_filaments_json(const std::vector<MixedFilamentDefinition>& definitions, size_t physical_count);
std::vector<MixedFilamentDefinition> deserialize_mixed_filaments_json(const std::string& json, size_t physical_count);

// The project configuration still transports compact rows internally. These
// adapters do not regenerate auto recipes or change the saved recipe order.
std::vector<MixedFilamentDefinition> mixed_filaments_from_project_config(const DynamicPrintConfig& config, size_t physical_count);
std::string                          mixed_filaments_to_legacy(const std::vector<MixedFilamentDefinition>& definitions);

} // namespace Slic3r

#endif
