#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) in TES_OUT {
    vec4 norm;
    vec2 uv;
    vec4 worldPos;
} gs_in[];

layout(location = 3) flat in int instanceId[];

layout(location = 0) out vec2 g_uv;
layout(location = 1) out vec3 g_norm;
layout(location = 2) out vec4 g_worldPos;

layout(std140, set = 0, binding = 0) uniform ShadowUniformsBlock {
    mat4 PMat;
    vec4 params;
} ShadowUniforms;

layout(std140, set = 0, binding = 1) uniform BoidsParamsBlock {
    int fishCount;
    float time2;
    float noiseStrength;
    float boundaryMargin;
    vec4 boundsMin;
    vec4 boundsMax;
    float boundaryStrength;
} BoidsParams;

layout(std430, set = 0, binding = 9) readonly buffer fishesBlock {
    float data[7168];
} fishes;

#define PMat ShadowUniforms.PMat
#define params ShadowUniforms.params
#define fishCount BoidsParams.fishCount
#define data fishes.data

vec3 fpos(int i) {
    vec3 p = vec3(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
    if (any(isnan(p)) || length(p) < 1e-6) p = vec3(0.0, 20.0, 10.0);
    return p;
}

vec3 fdir(int i) {
    int offset = fishCount * 3;
    vec3 d = vec3(data[i * 3 + offset], data[i * 3 + 1 + offset], data[i * 3 + 2 + offset]);
    if (any(isnan(d)) || length(d) < 1e-6) d = vec3(0.0, 0.0, 1.0);
    return d;
}

void main() {
    vec3 dp[3];

    vec3 dir = fdir(instanceId[0]);
    vec3 dir_x = normalize(cross(dir, vec3(0., 1., 0.)));

    for (int i = 0; i < 3; ++i) {
        vec3 displace = gs_in[i].worldPos.xyz - fpos(instanceId[0]);
        dp[i] = gs_in[i].worldPos.xyz;
        dp[i] += 0.2 * sin(params.x * 7 + dot(displace, dir) + instanceId[0]) * dir_x;
    }

    vec3 avgNorm = normalize(gs_in[0].norm.xyz + gs_in[1].norm.xyz + gs_in[2].norm.xyz);

    for (int i = 0; i < 3; ++i) {
        gl_Position = PMat * vec4(dp[i], 1.0);
        g_norm = avgNorm;
        g_uv = gs_in[i].uv;
        g_worldPos = vec4(dp[i], 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
