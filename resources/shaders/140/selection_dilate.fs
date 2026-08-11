#version 140

uniform sampler2D mask_texture;
uniform vec2 inverse_viewport_size;

in vec2 tex_coord;
out vec4 frag_color;

void main()
{
    const float coverageThreshold = 0.25;
    const int radius = 4;
    float expanded = 0.0;

    for (int x = -radius; x <= radius; ++x) {
        vec2 offset = vec2(float(x), 0.0) * inverse_viewport_size;
        float neighborCoverage = texture(mask_texture, tex_coord + offset).a;
        expanded = max(expanded, step(coverageThreshold, neighborCoverage));
    }

    frag_color = vec4(expanded, 0.0, 0.0, 1.0);
}
