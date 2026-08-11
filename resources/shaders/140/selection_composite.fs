#version 140

uniform sampler2D mask_texture;
uniform sampler2D dilated_mask_texture;
uniform vec2 inverse_viewport_size;
uniform vec3 highlight_color;
uniform float fill_alpha;
uniform float outline_alpha;

in vec2 tex_coord;
out vec4 frag_color;

void main()
{
    float coverage = texture(mask_texture, tex_coord).a;
    const float coverageThreshold = 0.25;
    float mask = step(coverageThreshold, coverage);
    float expanded = 0.0;
    const int radius = 4;

    for (int y = -radius; y <= radius; ++y) {
        vec2 offset = vec2(0.0, float(y)) * inverse_viewport_size;
        expanded = max(expanded, texture(dilated_mask_texture, tex_coord + offset).r);
        if (expanded > 0.5)
            break;
    }

    float fillOpacity = coverage * fill_alpha;
    float outlineOpacity = expanded * (1.0 - mask) * outline_alpha;
    float outputAlpha = max(fillOpacity, outlineOpacity);
    vec3 outputColor = outlineOpacity > fillOpacity ? highlight_color : highlight_color * fill_alpha;
    frag_color = vec4(outputColor, outputAlpha);
}
