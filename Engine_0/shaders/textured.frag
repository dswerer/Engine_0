#version 450

// combined image sampler：对应 ResourceType::SampledImage。
// 变量名与图资源名一致（Blurred）——约定「shader 变量名 == 图资源名」，RealizePass 按名对账
layout(set = 0, binding = 0) uniform sampler2D Blurred;

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = texture(Blurred, vec2(0.5, 0.5));
}
