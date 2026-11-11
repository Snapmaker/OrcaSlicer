#version 140

// Light depth map, or the intermediate level of the reduction (R channel)
uniform sampler2D shadow_depth;
// Source texels per output texel along each axis
uniform int block_size;

out vec4 frag_color;

// Each output texel keeps the depth closest to the light inside its block, i.e. the local top of the casters.
// The blocker search derives its radius from it instead of from the top of the whole scene.
void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * block_size;
    float top = 1.0;
    for (int y = 0; y < block_size; ++y) {
        for (int x = 0; x < block_size; ++x)
            top = min(top, texelFetch(shadow_depth, base + ivec2(x, y), 0).r);
    }
    frag_color = vec4(top, 0.0, 0.0, 1.0);
}
