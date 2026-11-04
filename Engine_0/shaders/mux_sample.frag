#version 450

// 运行期 mux 冒烟：变量名与图资源名 MuxOut 一致（按名对账）
layout(set = 0, binding = 0) uniform sampler2D MuxOut;
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = texture(MuxOut, vec2(0.5, 0.5));
}
