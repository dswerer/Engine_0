#version 450

// 纯色片元：无任何描述符绑定，供"只写附件不读资源"的 pass（如冒烟图的 GBuffer）建管线
layout(location = 0) out vec4 o;
void main()
{
	o = vec4(1.0, 0.0, 1.0, 1.0);
}
