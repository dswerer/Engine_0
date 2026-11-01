#version 450

// 采样上传后的贴图（Host -> Transfer 管线产物）。
// 变量名与图资源名一致（Texture）——约定「shader 变量名 == 图资源名」，RealizePass 按名对账
layout(set = 0, binding = 0) uniform sampler2D Texture;

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = texture(Texture, vec2(0.5, 0.5));
}
