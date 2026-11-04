#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

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
#define fishCount BoidsParams.fishCount
#define data fishes.data

layout(location = 0) out VS_OUT {
    vec4 norm;
    vec2 uv;
    vec4 worldPos;
} vs_out;

layout(location = 3) flat out int instanceId;

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

mat4 front(vec3 dir) {
    vec3 z = normalize(dir);
    vec3 up = abs(dot(z, vec3(0., 1., 0.))) > 0.999 ? vec3(1., 0., 0.) : vec3(0., 1., 0.);
    vec3 x = normalize(cross(up, z));
    vec3 y = normalize(cross(z, x));
    return mat4(vec4(x, 0.), vec4(y, 0.), vec4(z, 0.), vec4(0., 0., 0., 1.));
}

mat4 translate(vec3 pos) {
    return mat4(1., 0., 0., 0.,
                0., 1., 0., 0.,
                0., 0., 1., 0.,
                pos.x, pos.y, pos.z, 1.);
}

void main() {
    mat4 fishM = translate(fpos(gl_InstanceIndex)) * front(fdir(gl_InstanceIndex));
    mat4 fishIt = inverse(fishM);

    instanceId = gl_InstanceIndex;

    vs_out.worldPos = vec4(aPos, 1.);
    vs_out.norm = vec4(aNormal, 0.);
    vs_out.uv = aUV;

    vs_out.worldPos = fishM * vs_out.worldPos;
    vs_out.norm = normalize(fishIt * vs_out.norm);

    gl_Position = PMat * vs_out.worldPos;
}
