#include "Flow.hpp"
#include "I18N.hpp"
#include "Print.hpp"
#include <cmath>
#include <assert.h>

#include <boost/algorithm/string/predicate.hpp>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

FlowErrorNegativeSpacing::FlowErrorNegativeSpacing() : 
	FlowError("Flow::spacing() produced negative spacing. Did you set some extrusion width too small?") {}

FlowErrorNegativeFlow::FlowErrorNegativeFlow() :
    FlowError("Flow::mm3_per_mm() produced negative flow. Did you set some extrusion width too small?") {}

// This static method returns a sane extrusion width default.
float Flow::auto_extrusion_width(FlowRole role, float nozzle_diameter)
{
    switch (role) {
    case frSupportMaterial:
    case frSupportMaterialInterface:
    case frSupportTransition:
    case frTopSolidInfill:
        return nozzle_diameter;
    default:
    case frExternalPerimeter:
    case frPerimeter:
    case frSolidInfill:
    case frInfill:
        return 1.125f * nozzle_diameter;
    }
}

// Used by the Flow::extrusion_width() funtion to provide hints to the user on default extrusion width values,
// and to provide reasonable values to the PlaceholderParser.
static inline FlowRole opt_key_to_flow_role(const std::string &opt_key)
{
 	if (opt_key == "inner_wall_line_width" || 
 		// or all the defaults:
 		opt_key == "line_width" || opt_key == "initial_layer_line_width")
        return frPerimeter;
    else if (opt_key == "outer_wall_line_width")
        return frExternalPerimeter;
    else if (opt_key == "sparse_infill_line_width")
        return frInfill;
    else if (opt_key == "internal_solid_infill_line_width")
        return frSolidInfill;
    else if (opt_key == "bridge_line_width")
        return frSolidInfill;
	else if (opt_key == "top_surface_line_width")
		return frTopSolidInfill;
	else if (opt_key == "support_line_width")
    	return frSupportMaterial;
    else 
    	throw Slic3r::RuntimeError("opt_key_to_flow_role: invalid argument");
};

static inline void throw_on_missing_variable(const std::string &opt_key, const char *dependent_opt_key) 
{
	throw FlowErrorMissingVariable((boost::format("Failed to calculate line width of %1%. Cannot get value of \u201c%2%\u201d.") % opt_key % dependent_opt_key).str());
}

ConfigOptionFloatOrPercent Flow::width_at(const ConfigOptionVector<FloatOrPercent> &widths, size_t column)
{
    if (widths.values.empty())
        return ConfigOptionFloatOrPercent(0., false);
    const FloatOrPercent &value = widths.get_at(column);
    if (std::isnan(value.value))
        return ConfigOptionFloatOrPercent(0., false);
    return ConfigOptionFloatOrPercent(value.value, value.percent);
}

namespace {

// A width key of `config` read at `column`: the element of a per tool head vector (a nil column
// reads 0), a scalar as it is. False when the key is not in the config.
bool config_width(const ConfigOptionResolver &config, const std::string &key, size_t column, FloatOrPercent &out)
{
    const ConfigOption *opt = config.option(key);
    if (opt == nullptr)
        return false;
    if (opt->type() == coFloatsOrPercents) {
        const ConfigOptionFloatOrPercent value = Flow::width_at(*static_cast<const ConfigOptionVector<FloatOrPercent> *>(opt), column);
        out = FloatOrPercent(value.value, value.percent);
        return true;
    }
    if (opt->type() == coFloatOrPercent) {
        const auto *value = static_cast<const ConfigOptionFloatOrPercent *>(opt);
        out = FloatOrPercent(value->value, value->percent);
        return true;
    }
    return false;
}

// The chain of extrusion_width(): the bridge width falls back to the internal solid width, a role
// width of 0 to the default line width, a default of 0 to the auto width of the role; every
// fallback is read at `column` (the tool head), the percent against the nozzle of `nozzle_extruder`.
double extrusion_width_of(const std::string &opt_key, FloatOrPercent width, const ConfigOptionResolver &config, const unsigned int nozzle_extruder, size_t column)
{
    auto opt_nozzle_diameters = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters == nullptr)
        throw_on_missing_variable(opt_key, "nozzle_diameter");
    const float nozzle_diameter = float(opt_nozzle_diameters->get_at(nozzle_extruder));

    if (opt_key == "bridge_line_width") {
        if (width.percent) {
            const double bridge_width = width.get_abs_value(nozzle_diameter);
            if (bridge_width > 0.)
                return bridge_width;
        } else if (width.value > 0.) {
            return width.value;
        }
        FloatOrPercent solid;
        if (!config_width(config, "internal_solid_infill_line_width", column, solid))
            throw_on_missing_variable(opt_key, "internal_solid_infill_line_width");
        return extrusion_width_of("internal_solid_infill_line_width", solid, config, nozzle_extruder, column);
    }

	if (width.value == 0.) {
		// The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
        if (!config_width(config, "line_width", column, width))
    		throw_on_missing_variable(opt_key, "line_width");
	}

    if (width.percent)
        return width.get_abs_value(nozzle_diameter);

	if (width.value == 0.) {
        // If user left option to 0, calculate a sane default width.
        return Flow::auto_extrusion_width(opt_key_to_flow_role(opt_key), nozzle_diameter);
    }

	return width.value;
}

} // namespace

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionFloatOrPercent* opt, const ConfigOptionResolver& config, const unsigned int first_printing_extruder)
{
	assert(opt != nullptr);
    if (opt == nullptr)
        throw_on_missing_variable(opt_key, opt_key.c_str());
    return extrusion_width_of(opt_key, FloatOrPercent(opt->value, opt->percent), config, first_printing_extruder, first_printing_extruder);
}

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionResolver &config, const unsigned int first_printing_extruder)
{
    // Snapmaker Orca: a per tool head width is read at the column of the extruder.
    return extrusion_width(opt_key, config, first_printing_extruder, size_t(first_printing_extruder));
}

double Flow::extrusion_width(const std::string &opt_key, const ConfigOptionResolver &config, const unsigned int nozzle_extruder, size_t column)
{
    FloatOrPercent width;
    if (!config_width(config, opt_key, column, width))
        throw_on_missing_variable(opt_key, opt_key.c_str());
    return extrusion_width_of(opt_key, width, config, nozzle_extruder, column);
}

// This constructor builds a Flow object from an extrusion width config setting
// and other context properties.
Flow Flow::new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent &width, float nozzle_diameter, float height)
{
    if (height <= 0)
        throw Slic3r::InvalidArgument("Invalid flow height supplied to new_from_config_width().");

    float w;
    if (!width.percent  && width.value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        w = auto_extrusion_width(role, nozzle_diameter);
    } else {
        // If user set a manual value, use it.
      w = float(width.get_abs_value(nozzle_diameter));
    }
    
    return Flow(w, height, rounded_rectangle_extrusion_spacing(w, height), nozzle_diameter, false);
}

// Adjust extrusion flow for new extrusion line spacing, maintaining the old spacing between extrusions.
Flow Flow::with_spacing(float new_spacing) const
{
    Flow out = *this;
    if (m_bridge) {
        // Diameter of the rounded extrusion.
        assert(m_width == m_height);
        float gap          = m_spacing - m_width;
        auto  new_diameter = new_spacing - gap;
        out.m_width        = out.m_height = new_diameter;
    } else {
        assert(m_width >= m_height);
        out.m_width += new_spacing - m_spacing;
        if (out.m_width < out.m_height)
            throw Slic3r::InvalidArgument("Invalid spacing supplied to Flow::with_spacing(), check your layer height and extrusion width.");
    }
    out.m_spacing = new_spacing;
    return out;
}

// Adjust the width / height of a rounded extrusion model to reach the prescribed cross section area while maintaining extrusion spacing.
Flow Flow::with_cross_section(float area_new) const
{
    assert(! m_bridge);
    assert(m_width >= m_height);

    // Adjust for bridge_flow, maintain the extrusion spacing.
    float area = this->mm3_per_mm();
    if (area_new > area + EPSILON) {
        // Increasing the flow rate.
        float new_full_spacing = area_new / m_height;
        if (new_full_spacing > m_spacing) {
            // Filling up the spacing without an air gap. Grow the extrusion in height.
            float height = area_new / m_spacing;
            return Flow(rounded_rectangle_extrusion_width_from_spacing(m_spacing, height), height, m_spacing, m_nozzle_diameter, false);
        } else {
            return this->with_width(rounded_rectangle_extrusion_width_from_spacing(area / m_height, m_height));
        }
    } else if (area_new < area - EPSILON) {
        // Decreasing the flow rate.
        float width_new = m_width - (area - area_new) / m_height;
        assert(width_new > 0);
        if (width_new > m_height) {
            // Shrink the extrusion width.
            return this->with_width(width_new);
        } else {
            // Create a rounded extrusion.
            auto dmr = float(sqrt(area_new / M_PI));
            return Flow(dmr, dmr, m_spacing, m_nozzle_diameter, false);
        }
    } else
        return *this;
}

float Flow::rounded_rectangle_extrusion_spacing(float width, float height)
{
    auto out = width - height * float(1. - 0.25 * PI);
    if (out <= 0.f)
        throw FlowErrorNegativeSpacing();
    return out;
}

float Flow::rounded_rectangle_extrusion_width_from_spacing(float spacing, float height)
{
    return float(spacing + height * (1. - 0.25 * PI));
}

float Flow::bridge_extrusion_spacing(float dmr)
{
    return dmr + BRIDGE_EXTRA_SPACING;
}

// This method returns extrusion volume per head move unit.
double Flow::mm3_per_mm() const
{
    float res = m_bridge ?
        // Area of a circle with dmr of this->width.
        float((m_width * m_width) * 0.25 * PI) :
        // Rectangle with semicircles at the ends. ~ h (w - 0.215 h)
        float(m_height * (m_width - m_height * (1. - 0.25 * PI)));
    //assert(res > 0.);
	if (res <= 0.)
		throw FlowErrorNegativeFlow();
    return res;
}

// Nozzle diameter driving support / raft flows. A "default" (0) filament prints with the active extruder; the support_nozzle_diameter restriction defines which nozzle that may be.
float support_material_nozzle_diameter(const PrintObject *object, int configured_filament)
{
    if (configured_filament == 0 && object->config().support_nozzle_diameter.value > 0.)
        return float(object->config().support_nozzle_diameter.value);
    // for configured_filament == 0 (use the current extruder), get_at returns the 0th component.
    return float(object->print()->config().nozzle_diameter.get_at(configured_filament - 1));
}

size_t support_head(const PrintObject *object, int configured, bool interface_role, float *nozzle)
{
    const Print &print = *object->print();
    if (configured > 0) {
        // A configured filament prints the support: its head and its nozzle.
        const size_t head = print.width_slot((unsigned int)configured);
        if (nozzle != nullptr)
            *nozzle = support_material_nozzle_diameter(object, configured);
        return head;
    }
    // Default (0): the filament ToolOrdering pins (resolved_default_support_filament) gives head and
    // nozzle; otherwise head 1 with support_material_nozzle_diameter.
    if (const unsigned int resolved = object->resolved_default_support_filament(interface_role); resolved > 0) {
        if (nozzle != nullptr)
            *nozzle = float(print.config().nozzle_diameter.get_at(resolved - 1));
        return print.width_slot(resolved);
    }
    if (nozzle != nullptr)
        *nozzle = support_material_nozzle_diameter(object, 0);
    return 0;
}

// The width option of a support flow: support_line_width of the head, else its line_width.
static ConfigOptionFloatOrPercent support_width(const PrintObject *object, size_t head)
{
    const ConfigOptionFloatOrPercent width = Flow::width_at(object->config().support_line_width, head);
    return width.value > 0 ? width : Flow::width_at(object->config().line_width, head);
}

Flow support_material_flow(const PrintObject *object, float layer_height)
{
    float        nozzle = 0.f;
    const size_t head   = support_head(object, object->config().support_filament, false, &nozzle);
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        support_width(object, head),
        nozzle,
        (layer_height > 0.f) ? layer_height : float(object->config().layer_height.value));
}
//BBS
Flow support_transition_flow(const PrintObject* object)
{
    //BBS: support transition of tree support is bridge flow
    float dmr = support_material_nozzle_diameter(object, object->config().support_filament);
    return Flow::bridging_flow(dmr, dmr);
}

Flow support_material_1st_layer_flow(const PrintObject *object, float layer_height)
{
    const PrintConfig &print_config = object->print()->config();
    float              nozzle       = 0.f;
    const size_t       head         = support_head(object, object->config().support_filament, false, &nozzle);
    const ConfigOptionFloatOrPercent initial_layer = Flow::width_at(print_config.initial_layer_line_width, head);
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        (initial_layer.value > 0) ? initial_layer : support_width(object, head),
        nozzle,
        (layer_height > 0.f) ? layer_height : float(print_config.initial_layer_print_height.value));
}

Flow support_material_interface_flow(const PrintObject *object, float layer_height)
{
    float        nozzle = 0.f;
    const size_t head   = support_head(object, object->config().support_interface_filament, true, &nozzle);
    return Flow::new_from_config_width(
        frSupportMaterialInterface,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        support_width(object, head),
        nozzle,
        (layer_height > 0.f) ? layer_height : float(object->config().layer_height.value));
}

}
