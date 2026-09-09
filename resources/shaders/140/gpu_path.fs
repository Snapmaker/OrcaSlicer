#version 140

// Toolpath shading of the de-geometrized pipeline: the color is looked up
// per fragment from a 1D ramp texture driven by the per-move attribute
// table, following the same rules as the legacy CPU-side coloring.

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define INTENSITY_AMBIENT    0.3

// travel move colors by extruder delta sign (GCodeViewer::Travel_Colors)
const vec3 K_TRAVEL_MOVE    = vec3(0.219, 0.282, 0.609);
const vec3 K_TRAVEL_EXTRUDE = vec3(0.112, 0.422, 0.103);
const vec3 K_TRAVEL_RETRACT = vec3(0.505, 0.064, 0.028);

// move type values (EMoveType): Travel = 8, Wipe = 9, Extrude = 10
// view type values (GCodeViewer::EViewType): FeatureType = 0, Height = 1,
// Width = 2, Feedrate = 3, FanSpeed = 4, Temperature = 5, VolumetricRate = 6,
// Tool = 7, ColorPrint = 8, FilamentId = 9, LayerTime = 10, LayerTimeLog = 11

uniform mat3 normal_matrix;
uniform float emission_factor;

// x = isRangeView, y = isRangeValid, z = topLayerOnly, w = viewType
uniform vec4 u_view_config;
// min/max of the active value range (pre-logged for LayerTimeLog)
uniform vec2 u_range_bounds;
// nonzero for logarithmic ranges (LayerTimeLog)
uniform float u_range_is_log;
// 1.0 when this layer is the top layer of the visible window
uniform float u_is_top_layer;
// x = number of colors of the active palette (roles or tools)
uniform vec2 u_palette_config;

uniform sampler2D s_color_ramp;           // gradient (linear) or palette (nearest)
uniform samplerBuffer s_attribute_table;  // RGBA32F: moveType, viewValue, deltaExtruder/role, 0

in float moveGroup;
in vec3 fragNormal;
in vec3 fragPos;

out vec4 fragColor;

bool isTopLayer()
{
    return u_is_top_layer > 0.5;
}

vec4 extrusionColor(vec3 attr)
{
    float viewType = u_view_config.w;
    float value = attr.y;

    // palettes: FeatureType (roles) / Tool / ColorPrint (tool colors)
    if (viewType < 0.5 || (viewType > 6.5 && viewType < 8.5)) {
        // ColorPrint: custom gcode option ids beyond the tool count render gray
        if (viewType > 7.5 && int(value + 0.5) > int(u_palette_config.x - 0.5))
            return vec4(0.5, 0.5, 0.5, 1.0);
        return texture(s_color_ramp, vec2((value + 0.5) / u_palette_config.x, 0.5));
    }

    // FilamentId debug view
    if (viewType > 8.5 && viewType < 9.5)
        return vec4(value / 256.0, attr.z / 256.0, value / 256.0, 1.0);

    // value gradient (Height, Width, Feedrate, FanSpeed, Temperature,
    // VolumetricRate, LayerTime, LayerTimeLog); invalid range keeps the
    // first ramp color, like the legacy Range::get_color_at()
    if (u_view_config.y < 0.5)
        return texture(s_color_ramp, vec2(0.0, 0.5));

    float v = (u_range_is_log > 0.5) ? log(max(value, 1e-6)) : value;
    float u = clamp((v - u_range_bounds.x) / max(u_range_bounds.y - u_range_bounds.x, 1e-6), 0.0, 1.0);
    return texture(s_color_ramp, vec2(u, 0.5));
}

vec4 baseColor(vec3 attr)
{
    float moveType = attr.x;
    bool topLayerOnly = u_view_config.z > 0.5;

    if (moveType > 7.5 && moveType < 8.5) { // Travel
        if (!topLayerOnly || isTopLayer()) {
            float viewType = u_view_config.w;
            // travel follows the gradient in the Feedrate and Tool views
            if ((viewType > 2.5 && viewType < 3.5) || (viewType > 6.5 && viewType < 7.5))
                return extrusionColor(attr);
            if (attr.z < 0.0) return vec4(K_TRAVEL_RETRACT, 1.0);
            if (attr.z > 0.0) return vec4(K_TRAVEL_EXTRUDE, 1.0);
            return vec4(K_TRAVEL_MOVE, 1.0);
        }
        return vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color
    }

    if (moveType > 8.5 && moveType < 9.5)   // Wipe
        return vec4(1.0, 1.0, 0.0, 1.0);    // Wipe_Color

    if (moveType > 9.5 && moveType < 10.5) { // Extrude
        if (!topLayerOnly || isTopLayer())
            return extrusionColor(attr);
        return vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color
    }

    return vec4(0.0, 0.0, 0.0, 1.0);
}

void main()
{
    vec3 attr = texelFetch(s_attribute_table, int(moveGroup)).rgb;
    vec4 color = baseColor(attr);

    // same two-light shading as the legacy gouraud_light shader
    vec3 norm = normalize(normal_matrix * normalize(fragNormal));

    float NdotL = max(dot(norm, LIGHT_TOP_DIR), 0.0);
    float tainted = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    float specular = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(fragPos), reflect(-LIGHT_TOP_DIR, norm)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(norm, LIGHT_FRONT_DIR), 0.0);
    tainted += NdotL * LIGHT_FRONT_DIFFUSE;

    fragColor = vec4(vec3(specular) + color.rgb * (tainted + emission_factor), color.a);
}
