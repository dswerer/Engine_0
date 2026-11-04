#version 450

layout(triangles, equal_spacing, ccw) in;

layout(location = 0) in vec3 tcColor[];
layout(location = 0) out vec3 teColor;

void main() {
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position
                + gl_TessCoord.y * gl_in[1].gl_Position
                + gl_TessCoord.z * gl_in[2].gl_Position;
    teColor = gl_TessCoord.x * tcColor[0]
            + gl_TessCoord.y * tcColor[1]
            + gl_TessCoord.z * tcColor[2];
}
