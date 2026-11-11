#version 140

// xy = min, zw = max of the mask rectangle in world XY
uniform vec4 mask_rect;

in vec3 v_position;

out vec2 world_xy;

void main()
{
    world_xy = v_position.xy;
    vec2 uv = (v_position.xy - mask_rect.xy) / (mask_rect.zw - mask_rect.xy);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
