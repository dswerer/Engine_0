#version 450

layout(vertices = 3) out;

layout(location = 0) in VS_OUT {
    vec4 norm;
    vec2 uv;
    vec4 worldPos;
} vs_out[];

layout(location = 0) out TCS_OUT {
    vec4 norm;
    vec2 uv;
    vec4 worldPos;
} tcs_out[3];

layout(location = 3) flat in int instanceId[];
layout(location = 3) flat out int instId[];

void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    tcs_out[gl_InvocationID].norm = vs_out[gl_InvocationID].norm;
    tcs_out[gl_InvocationID].uv = vs_out[gl_InvocationID].uv;
    tcs_out[gl_InvocationID].worldPos = vs_out[gl_InvocationID].worldPos;

    instId[gl_InvocationID] = instanceId[gl_InvocationID];

    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = 1.0;
        gl_TessLevelOuter[1] = 1.0;
        gl_TessLevelOuter[2] = 1.0;
        gl_TessLevelInner[0] = 1.0;
    }
}
