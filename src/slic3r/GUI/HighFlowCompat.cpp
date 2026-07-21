#include "HighFlowCompat.hpp"

#include "GUI.hpp"
#include "I18N.hpp"

#include <boost/algorithm/string/case_conv.hpp>

namespace Slic3r { namespace GUI { namespace HighFlowCompat {

std::optional<std::string> incompat_reason(const std::string &filament_type, const std::string &preset_name)
{
    const std::string type = boost::algorithm::to_lower_copy(filament_type);
    const std::string name = boost::algorithm::to_lower_copy(preset_name);
    auto contains = [&type, &name](const char *needle) {
        return type.find(needle) != std::string::npos || name.find(needle) != std::string::npos;
    };

    if (contains("cf") || contains("gf"))
        return into_u8(_L("CF or GF based materials"));
    if (contains("wood"))
        // Match the requirement copy ("PLA-wood"): reuse the type when it already
        // mentions wood, otherwise append the "-wood" suffix to it.
        return type.find("wood") != std::string::npos ? filament_type : filament_type + "-wood";
    return std::nullopt;
}

}}} // namespace Slic3r::GUI::HighFlowCompat
