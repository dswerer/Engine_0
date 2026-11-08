#version 450

layout(location = 0) in vec2 g_uv;
layout(location = 1) in vec3 g_norm;
layout(location = 2) in vec4 g_worldPos;

layout(std140, set = 0, binding = 0) uniform FrameUniformsBlock {
    mat4 PMat;
    mat4 PMat_iv;
    mat4 lightSpaceMatrix;
    vec4 dlight;
    vec2 resolution;
    float time;
    vec3 lightCol;
    vec3 cameraPos;
    vec3 lightDir;
} FrameUniforms;

layout(std140, set = 0, binding = 2) uniform AtmosphereParamsBlock {
    float H_Rayleigh;
    float H_Mie;
    float g_Mie;
    float PlanetRadius;
    float AtmosphereRadius;
    float H_Ozone;
    float W_Ozone;
    vec3 SunLuminance;
    vec3 PlanetCenter;
} AtmosphereParams;

#define dlight FrameUniforms.dlight
#define lightSpaceMatrix FrameUniforms.lightSpaceMatrix
#define lightCol FrameUniforms.lightCol
#define PlanetCenter AtmosphereParams.PlanetCenter

layout(set = 0, binding = 7) uniform sampler2D shadowMap;

layout(location = 0) out vec4 fragColor;

float dirLight_diffuse(vec3 ld, vec3 norm) {
    return sqrt(max(dot(norm, ld), 0.1f));
}

float ShadowCalculation(vec4 worldPos, vec3 normal) {
    vec3 L = normalize(dlight.xyz);
    float NdotL = clamp(dot(normal, L), 0.0, 1.0);

    float minBias = 0.0005;
    float maxBias = 0.005;
    float bias = max(maxBias * (1.0 - NdotL), minBias);

    vec4 lightSpacePos = lightSpaceMatrix * vec4(worldPos.xyz, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    vec2 uv = projCoords.xy * 0.5 + 0.5;

    if (projCoords.z > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 0.0;

    float currentDepth = projCoords.z;
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, uv + vec2(x, y) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 0.5 : 0.0;
        }
    }
    shadow /= 6.0;

    //shadow *= smoothstep(0.07, 0.15, NdotL);
    return shadow;
}

void main() {
    vec3 norm = normalize(g_norm);
    float dense = dirLight_diffuse(normalize(dlight.xyz), norm) * 1.0;
    float shadow = ShadowCalculation(g_worldPos, norm);

    vec3 radialDir = normalize(g_worldPos.xyz - PlanetCenter);
    float ambientOcclusion = (1.0 + dot(norm, radialDir)) * 0.5;

    float lit = dense * (1.0 - shadow);
    fragColor = vec4(lit * lightCol.x, lit * lightCol.y, ambientOcclusion, 1.0);
}
