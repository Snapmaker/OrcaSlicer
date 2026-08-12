#version 140

uniform sampler2D mask_texture;
uniform sampler2D edge_texture;
uniform sampler2D glow_texture;
uniform vec3 outline_color;
uniform float fill_alpha;
uniform float outline_exclusion_low;
uniform float outline_exclusion_high;
uniform float edge_strength;
uniform float edge_glow;
uniform float render_fill;
uniform float render_glow;

in vec2 tex_coord;
out vec4 frag_color;

void main()
{
    float coverage = texture(mask_texture, tex_coord).a;
    if (render_fill > 0.5)
    {
        frag_color = vec4(vec3(fill_alpha), coverage * fill_alpha);
        return;
    }

    float edgeCoverage = texture(edge_texture, tex_coord).a;
    float glowCoverage = texture(glow_texture, tex_coord).a;
    float outlineExclusion = smoothstep(outline_exclusion_low, outline_exclusion_high, coverage);
    float outsideFill = 1.0 - outlineExclusion;
    float sourceCoverage = render_glow > 0.5 ? glowCoverage * edge_glow : edgeCoverage;
    float outlineAlpha = clamp(edge_strength * sourceCoverage * outsideFill, 0.0, 1.0);
    frag_color = vec4(outline_color, outlineAlpha);
}
