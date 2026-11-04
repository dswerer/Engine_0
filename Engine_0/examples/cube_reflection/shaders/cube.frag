#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;

layout(location = 0) out vec4 fragColor;

layout(std140, set = 0, binding = 0) uniform SceneUniformsBlock {
	mat4 viewProj;
	mat4 model;
	vec4 camPosition;
	vec4 camRight;
	vec4 camUp;
	vec4 camForward;
	vec4 resolution;
} SceneUniforms;

layout(set = 0, binding = 1) uniform sampler2D EnvTexture;

const float PI = 3.14159265359;

// 环境贴图为全景图，地面半区为纯黑：折叠仰角后只采样天空区域，避免俯视时整屏发黑
vec2 EnvUV(vec3 dir)
{
	vec3 d = normalize(dir);
	float u = atan(d.x, -d.z) / (2.0 * PI) + 0.5;
	float v = 0.46 - 0.44 * asin(clamp(abs(d.y), 0.0, 1.0)) / (0.5 * PI);
	return vec2(u, v);
}

void main()
{
	vec3 viewDir = normalize(vWorldPos - SceneUniforms.camPosition.xyz);
	vec3 reflectDir = reflect(viewDir, normalize(vNormal));
	vec3 env = texture(EnvTexture, EnvUV(reflectDir)).rgb;
	// 镜面反射：整体略压暗，避免与同色系背景完全融为一体
	fragColor = vec4(env * 0.88, 1.0);
}
