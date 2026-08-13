#version 110

uniform sampler2D mask_texture;
uniform sampler2D edge_texture;
uniform sampler2D glow_texture;
uniform vec3 outline_color;
uniform float fill_alpha;
uniform float outline_exclusion_low;
uniform float outline_exclusion_high;
uniform float edge_strength;
uniform float edge_glow;

varying vec2 tex_coord;

void main()
{
    float coverage = texture2D(mask_texture, tex_coord).a;
    float edgeCoverage = texture2D(edge_texture, tex_coord).a;
    float glowCoverage = texture2D(glow_texture, tex_coord).a;
    float outlineExclusion = smoothstep(outline_exclusion_low, outline_exclusion_high, coverage);
    float outsideFill = 1.0 - outlineExclusion;
    float fillAlpha = coverage * fill_alpha;
    float edgeAlpha = clamp(edge_strength * edgeCoverage * outsideFill, 0.0, 1.0);
    float glowAlpha = clamp(edge_strength * edge_glow * glowCoverage * outsideFill, 0.0, 1.0);

    // Precompose normal-alpha Fill and Edge followed by additive Glow.
    float compositeAlpha = fillAlpha + edgeAlpha * (1.0 - fillAlpha);
    vec3 compositeColor = vec3(fill_alpha) * fillAlpha * (1.0 - edgeAlpha) +
                          outline_color * (edgeAlpha + glowAlpha);
    gl_FragColor = vec4(compositeColor, compositeAlpha);
}
