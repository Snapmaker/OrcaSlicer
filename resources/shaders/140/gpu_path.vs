#version 140

// GPU-generated toolpath geometry (de-geometrized pipeline): each instance
// is one path step; the 10 prism vertices (start/end diamonds + miter fill)
// are reconstructed here from data tables instead of a vertex buffer.
// Cross-section: diamond with height along `up` (layer height) and width
// along `right` (extrusion width); the top face lies on the path line,
// matching the legacy CPU-generated prisms.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

uniform samplerBuffer s_node_table;         // RGBA32F: xyz + move group index
uniform samplerBuffer s_width_height_table; // RG32F: extrusion width + height, per group
uniform samplerBuffer s_step_table;         // RGBA32F: startNode, endNode, hasPrev, prevStartNode
uniform samplerBuffer s_attribute_table;    // RGBA32F: moveType, viewValue, ...

// minimum world-space thickness for line-like moves (travel, wipe), derived
// from the camera distance so they keep a near-constant screen width like
// the legacy GL lines; 0 disables the floor
uniform float u_min_line_size;

out float moveGroup;
out vec3 fragNormal;
out vec3 fragPos;

void main()
{
    vec4 stepData = texelFetch(s_step_table, gl_InstanceID);
    int startNode = int(stepData.x);
    int endNode = int(stepData.y);
    bool hasPrev = stepData.z > 0.5;
    int prevStartNode = int(stepData.w);

    vec4 startNodeData = texelFetch(s_node_table, startNode);
    vec4 endNodeData = texelFetch(s_node_table, endNode);

    vec3 line = endNodeData.xyz - startNodeData.xyz;
    vec3 lineDir = vec3(1.0, 0.0, 0.0);
    float lineLen = length(line);
    lineDir = line / max(lineLen, 1e-6);

    // local basis: right is horizontal (xy plane), up completes the frame
    vec3 rightDir = vec3(lineDir.y, -lineDir.x, 0.0);
    vec3 up = vec3(0.0, 0.0, 1.0);
    if (length(rightDir) < 1e-4) {
        // vertical move: pick a stable fallback frame
        up = vec3(1.0, 0.0, 0.0);
        rightDir = cross(lineDir, up);
    } else {
        rightDir = normalize(rightDir);
        lineDir = normalize(lineDir);
        up = cross(rightDir, lineDir);
    }

    vec3 basePos = gl_VertexID < 4 ? startNodeData.xyz : endNodeData.xyz;
    moveGroup = endNodeData.w + 0.5;

    vec2 widthHeight = texelFetch(s_width_height_table, int(moveGroup)).rg;
    float halfWidth = 0.5 * widthHeight.x;
    float halfHeight = 0.5 * widthHeight.y;

    // travel and wipe carry no meaningful width/height: clamp them to the
    // screen-constant minimum so they stay visible at any zoom (the fetch is
    // inside the uniform branch so the disabled path costs nothing)
    if (u_min_line_size > 0.0) {
        float moveType = texelFetch(s_attribute_table, int(moveGroup)).r;
        if (moveType > 7.5 && moveType < 9.5) {
            halfWidth = max(halfWidth, 0.5 * u_min_line_size);
            halfHeight = max(halfHeight, 0.5 * u_min_line_size);
        }
    }

    vec3 dUp = halfHeight * up;
    vec3 dRight = halfWidth * rightDir;

    // the prism hangs below the path line (top face on the line)
    vec3 position = basePos - halfHeight * up;
    // defined default for every vertex: the miter vertices 8/9 only get a
    // real normal inside the hasPrev branch below, and reading an
    // uninitialized varying at the normalize() further down is GLSL UB
    fragNormal = up;
    if (0 == gl_VertexID || 4 == gl_VertexID) {
        position = position + dUp;
        fragNormal = up;
    } else if (1 == gl_VertexID || 5 == gl_VertexID) {
        position = position + dRight;
        fragNormal = rightDir;
    } else if (2 == gl_VertexID || 6 == gl_VertexID) {
        position = position - dUp;
        fragNormal = -up;
    } else if (3 == gl_VertexID || 7 == gl_VertexID) {
        position = position - dRight;
        fragNormal = -rightDir;
    }

    // miter fill: close the wedge gap between this step and the connected
    // previous one at sharp corners (degenerate, invisible when !hasPrev)
    if (gl_VertexID > 7 && hasPrev) {
        vec4 prevNodeData = texelFetch(s_node_table, prevStartNode);
        vec3 prevDir = startNodeData.xyz - prevNodeData.xyz;
        prevDir = prevDir / max(length(prevDir), 1e-6);
        vec3 prevRightDir = vec3(prevDir.y, -prevDir.x, 0.0);
        prevRightDir = normalize(prevRightDir);
        vec3 prevUp = cross(prevRightDir, prevDir);

        vec3 prevAnchor = startNodeData.xyz - halfHeight * prevUp;
        if (8 == gl_VertexID) {
            position = prevAnchor - halfWidth * prevRightDir;
            fragNormal = -prevRightDir;
        } else if (9 == gl_VertexID) {
            position = prevAnchor + halfWidth * prevRightDir;
            fragNormal = prevRightDir;
        }
    }

    vec4 viewPos = view_model_matrix * vec4(position, 1.0);
    fragPos = viewPos.xyz;
    fragNormal = normalize(fragNormal);
    gl_Position = projection_matrix * viewPos;
}
