#version 450

// uniform 缓冲：对应 ResourceType::UniformBuffer（set 0, binding 0）
layout(set = 0, binding = 0) uniform Ubo {
	vec4 tint;
} ubo;

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = ubo.tint;
}
