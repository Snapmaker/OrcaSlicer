#version 140

// Ground shadow mask to denoise: R = visibility, G = normalized blur step (from the penumbra width)
uniform sampler2D source_mask;
// (1, 0) for the horizontal pass, (0, 1) for the vertical pass
uniform ivec2 direction;
// Largest tap spacing in mask texels, reached where G = 1
uniform float max_step;

out vec4 frag_color;

// Normalized Gaussian weights for taps at 0, 1 and 2 steps
const float WEIGHT_0 = 0.4026;
const float WEIGHT_1 = 0.2442;
const float WEIGHT_2 = 0.0545;

float FetchStep(ivec2 c, ivec2 size)
{
    return texelFetch(source_mask, clamp(c, ivec2(0), size - ivec2(1)), 0).g;
}

void main()
{
    ivec2 size = textureSize(source_mask, 0);
    ivec2 c = ivec2(gl_FragCoord.xy);

    // The step is the largest one nearby: inside a wide penumbra some texels miss every blocker and
    // carry no step themselves, yet they must be smoothed with their neighbours. Contact edges have
    // only small steps around them and stay sharp.
    float g = max(FetchStep(c, size), max(FetchStep(c + direction, size), FetchStep(c - direction, size)));
    g = max(g, max(FetchStep(c + 2 * direction, size), FetchStep(c - 2 * direction, size)));
    float spacing = max(1.0, g * max_step);

    vec2 uv = (vec2(c) + 0.5) / vec2(size);
    vec2 offset = vec2(direction) * spacing / vec2(size);
    float v = texture(source_mask, uv).r * WEIGHT_0;
    v += (texture(source_mask, uv + offset).r + texture(source_mask, uv - offset).r) * WEIGHT_1;
    v += (texture(source_mask, uv + 2.0 * offset).r + texture(source_mask, uv - 2.0 * offset).r) * WEIGHT_2;

    // G carries the dilated step to the second pass
    frag_color = vec4(v, g, 0.0, 1.0);
}
