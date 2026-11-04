#version 450

layout(triangles, fractional_even_spacing, ccw) in;

layout(location = 0) in TCS_OUT {
    vec4 norm;
    vec2 uv;
    vec4 worldPos;
} tcs_out[];

layout(location = 0) out TES_OUT {
    vec4 norm;
    vec2 uv;
    vec4 worldPos;
} tes_out;

layout(location = 3) flat in int instId[];
layout(location = 3) flat out int instanceId;

void main() {
    instanceId = instId[0];

    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position
                + gl_TessCoord.y * gl_in[1].gl_Position
                + gl_TessCoord.z * gl_in[2].gl_Position;

    tes_out.norm = gl_TessCoord.x * tcs_out[0].norm
                 + gl_TessCoord.y * tcs_out[1].norm
                 + gl_TessCoord.z * tcs_out[2].norm;
    tes_out.uv = gl_TessCoord.x * tcs_out[0].uv
               + gl_TessCoord.y * tcs_out[1].uv
               + gl_TessCoord.z * tcs_out[2].uv;
    tes_out.worldPos = gl_TessCoord.x * tcs_out[0].worldPos
                     + gl_TessCoord.y * tcs_out[1].worldPos
                     + gl_TessCoord.z * tcs_out[2].worldPos;
}
