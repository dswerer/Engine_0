#version 450

// 静态纹理堆版本：set 0 / binding 0 为 bindless 堆，环境贴图取 slot 0；
// 其余动态绑定从 set 1 开始（引擎会跳过 set 0 的反射槽位并绑定堆布局）。
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D staticTextures[16];

layout(std140, set = 1, binding = 0) uniform SceneUniformsBlock {
	mat4 viewProj;
	mat4 model;
	vec4 camPosition;
	vec4 camRight;
	vec4 camUp;
	vec4 camForward;
	vec4 resolution;
} SceneUniforms;

layout(set = 1, binding = 1) uniform sampler2D SceneColor;

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
	vec3 background = texture(staticTextures[0], EnvUV(ray)).rgb;

	// SceneColor 的 alpha 即立方体覆盖掩码：背景为 0，立方体为 1
	vec4 scene = texture(SceneColor, uv);
	fragColor = vec4(mix(background, scene.rgb, scene.a), 1.0);
}
