#version 450

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

layout(set = 0, binding = 1) uniform sampler2D SceneColor;
layout(set = 0, binding = 2) uniform sampler2D EnvTexture;

const float PI = 3.14159265359;

// 与 cube.frag 保持同一映射，保证立方体反射与背景严格连续
vec2 EnvUV(vec3 dir)
{
	vec3 d = normalize(dir);
	float u = atan(d.x, -d.z) / (2.0 * PI) + 0.5;
	float v = 0.46 - 0.44 * asin(clamp(abs(d.y), 0.0, 1.0)) / (0.5 * PI);
	return vec2(u, v);
}

void main()
{
	vec2 uv = gl_FragCoord.xy / SceneUniforms.resolution.xy;
	vec2 ndc = uv * 2.0 - 1.0;
	float aspect = SceneUniforms.resolution.x / SceneUniforms.resolution.y;
	float tanHalf = SceneUniforms.resolution.z;

	// Vulkan NDC y 轴朝下：camUp 的分量取 -ndc.y
	vec3 ray = normalize(
		SceneUniforms.camRight.xyz * (ndc.x * aspect * tanHalf) +
		SceneUniforms.camUp.xyz * (-ndc.y * tanHalf) +
		SceneUniforms.camForward.xyz);
	vec3 background = texture(EnvTexture, EnvUV(ray)).rgb;

	// SceneColor 的 alpha 即立方体覆盖掩码：背景为 0，立方体为 1
	vec4 scene = texture(SceneColor, uv);
	fragColor = vec4(mix(background, scene.rgb, scene.a), 1.0);
}
