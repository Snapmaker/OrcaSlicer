#version 110

uniform bool ban_light;
uniform vec4 uniform_color;
uniform float emission_factor;
uniform vec2 z_range;

// x = tainted, y = specular;
varying vec2 intensity;
//varying float drop;
varying vec4 world_pos;

void main()
{
    if (world_pos.z < max(0.0, z_range.x) || world_pos.z > z_range.y)
        discard;
    if(ban_light){
       gl_FragColor = uniform_color;
    } else{
       gl_FragColor = vec4(vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor), uniform_color.a);
    }
}
