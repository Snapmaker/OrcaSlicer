#ifndef slic3r_GUI_GatewayMachineSnapshot_hpp_
#define slic3r_GUI_GatewayMachineSnapshot_hpp_

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace Slic3r {

class PresetBundle;

namespace GUI {

class GatewayMachineSnapshot
{
public:
    using PresetBundleAccessor = std::function<PresetBundle*()>;
    using NativeUiRefresh      = std::function<void()>;

    GatewayMachineSnapshot() = default;

    void set_dependencies(PresetBundleAccessor preset_bundle_accessor, NativeUiRefresh refresh_native_ui);
    void reset();
    void apply(const nlohmann::json& snapshot);
    void clear(const std::string& serial_number = {});

private:
    PresetBundleAccessor preset_bundle_accessor_;
    NativeUiRefresh      refresh_native_ui_;

    std::string                         serial_number_;
    std::map<std::string, std::int64_t> revisions_;
    bool                                initialized_{false};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_GatewayMachineSnapshot_hpp_
