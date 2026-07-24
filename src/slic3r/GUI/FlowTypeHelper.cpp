#include "FlowTypeHelper.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace FlowType {

static size_t nozzle_count()
{
    const auto *diameters = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    return diameters != nullptr && !diameters->values.empty() ? diameters->values.size() : 1;
}

static void notify_plater()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    plater->update_project_dirty_from_presets();
    plater->on_config_change(wxGetApp().preset_bundle->full_config());
}

bool printer_supports_high_flow()
{
    const auto *support = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionStrings>("printer_flow_support");
    return support != nullptr &&
           std::find(support->values.begin(), support->values.end(), FLOW_MODE_HIGH_FLOW) != support->values.end();
}

std::vector<std::string> nozzle_volume_types()
{
    std::vector<std::string> types;
    if (const auto *opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")) {
        types.reserve(opt->values.size());
        for (int v : opt->values)
            types.push_back(to_string(FilamentVolumeType(v)));
    }
    types.resize(nozzle_count(), FLOW_MODE_STANDARD);
    const bool supported = printer_supports_high_flow();
    for (std::string &t : types)
        if (!supported || t != FLOW_MODE_HIGH_FLOW)
            t = FLOW_MODE_STANDARD;
    return types;
}

void set_nozzle_volume_type(size_t nozzle_idx, const std::string &volume_type)
{
    std::vector<std::string> types = nozzle_volume_types();
    if (nozzle_idx >= types.size())
        return;
    types[nozzle_idx] = volume_type == FLOW_MODE_HIGH_FLOW ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD;
    auto *opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
    opt->values.clear();
    for (const std::string &t : types)
        opt->values.push_back(filament_volume_type_from_string(t));
    notify_plater();
}

void set_nozzle_volume_types(const std::vector<std::string> &volume_types)
{
    const size_t count = nozzle_count();
    if (volume_types.size() != count)
        return;

    auto *opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
    opt->values.clear();
    opt->values.reserve(volume_types.size());
    for (const std::string &type : volume_types)
        opt->values.push_back(type == FLOW_MODE_HIGH_FLOW ? fvtHighFlow : fvtStandard);
    notify_plater();
}

std::string grouping_mode()
{
    const auto *opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("filament_grouping_mode");
    return opt != nullptr && opt->value == FILAMENT_GROUPING_CUSTOM ? FILAMENT_GROUPING_CUSTOM : FILAMENT_GROUPING_STANDARD;
}

void set_grouping_mode(const std::string &mode)
{
    wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("filament_grouping_mode", true)->value =
        mode == FILAMENT_GROUPING_CUSTOM ? FILAMENT_GROUPING_CUSTOM : FILAMENT_GROUPING_STANDARD;
    notify_plater();
}

void apply_custom_mapping(const std::vector<FilamentVolumeType> &mapping)
{
    wxGetApp().preset_bundle->set_filament_volume_types(mapping);
    notify_plater();
}

}}} // namespace Slic3r::GUI::FlowType
