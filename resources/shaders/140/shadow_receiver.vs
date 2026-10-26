#version 140

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform float receiver_z;

in vec3 v_position;

out vec2 world_xy;

void main()
{
    world_xy = v_position.xy;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position.xy, receiver_z, 1.0);
}
