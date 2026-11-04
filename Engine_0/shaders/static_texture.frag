#version 450

// 静态纹理堆验证：set 0 / binding 0 的 sampler2D 数组（bindless，槽位 0/1 混合）
layout(set = 0, binding = 0) uniform sampler2D staticTextures[16];
layout(location = 0) out vec4 o;

void main()
{
	vec4 a = texture(staticTextures[0], vec2(0.5));
	vec4 b = texture(staticTextures[1], vec2(0.5));
	o = mix(a, b, 0.5);
}
