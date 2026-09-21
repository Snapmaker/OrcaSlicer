#version 140

// Option markers (seams, retractions, tool/color changes, pause prints,
// custom gcode) of the de-geometrized pipeline: one instance per marker;
// the unit diamond template from the CPU is scaled and placed here from the
// data tables (same sizing as the legacy instanced-marker shader:
// xy = 1.5 * extrusion width, z = 1.5 * height, dropped by half a height).

in vec3 v_position;
in vec3 v_normal;

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

uniform samplerBuffer s_node_table;         // RGBA32F: xyz + move group index
uniform samplerBuffer s_width_height_table; // RG32F: extrusion width + height, per group
uniform samplerBuffer s_marker_table;       // RGBA32F: (node, node, 0, 0)

// minimum world-space diameter; retraction moves can carry a zero width,
// which would collapse the diamond to nothing
uniform float u_min_marker_size;

out float moveGroup;
out vec3 fragNormal;
out vec3 fragPos;

void main()
{
    vec4 markerData = texelFetch(s_marker_table, gl_InstanceID);
    int node = int(markerData.y);

    vec4 nodeData = texelFetch(s_node_table, node);
    moveGroup = nodeData.w + 0.5;

    vec2 widthHeight = texelFetch(s_width_height_table, int(moveGroup)).rg;
    float width = max(1.5 * widthHeight.x, u_min_marker_size);
    float height = max(1.5 * widthHeight.y, u_min_marker_size);

    // column-major model matrix: scale then translate onto the node
    mat4 model = mat4(
        width, 0.0, 0.0, 0.0,
        0.0, width, 0.0, 0.0,
        0.0, 0.0, height, 0.0,
        nodeData.x, nodeData.y, nodeData.z - 0.5 * widthHeight.y, 1.0);

    vec4 viewPos = view_model_matrix * model * vec4(v_position, 1.0);
    fragPos = viewPos.xyz;
    fragNormal = normalize(v_normal);
    gl_Position = projection_matrix * viewPos;
}
