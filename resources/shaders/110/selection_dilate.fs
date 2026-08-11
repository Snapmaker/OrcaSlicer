#version 110

uniform sampler2D mask_texture;
uniform vec2 inverse_viewport_size;

varying vec2 tex_coord;

void main()
{
    const float coverageThreshold = 0.25;
    const int radius = 4;
    float expanded = 0.0;

    for (int x = -radius; x <= radius; ++x) {
        vec2 offset = vec2(float(x), 0.0) * inverse_viewport_size;
        float neighborCoverage = texture2D(mask_texture, tex_coord + offset).a;
        expanded = max(expanded, step(coverageThreshold, neighborCoverage));
    }

    gl_FragColor = vec4(expanded, 0.0, 0.0, 1.0);
}
