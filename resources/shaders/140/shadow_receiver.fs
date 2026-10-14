#version 140

uniform sampler2D shadow_mask;
// xy = min, zw = max of the mask rectangle in world XY
uniform vec4 mask_rect;
uniform float shadow_strength;

in vec2 world_xy;

out vec4 frag_color;

void main()
{
    vec2 uv = (world_xy - mask_rect.xy) / (mask_rect.zw - mask_rect.xy);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        discard;

    float shadow = (1.0 - texture(shadow_mask, uv).r) * shadow_strength;
    if (shadow < 0.004)
        discard;

    frag_color = vec4(0.0, 0.0, 0.0, shadow);
}
