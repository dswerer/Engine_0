#version 450

// 逐附件管线状态冒烟：槽 0 参与混合（alpha 0.5），槽 1 输出常量色供 writeMask 验证
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;

void main()
{
	o0 = vec4(1.0, 0.0, 0.0, 0.5);
	o1 = vec4(1.0, 0.5, 0.25, 1.0);
}
