#version 450

// 顶点流路径：位置从 binding 0 的 location 0 读（attribute 布局在 PipelineDesc 里声明）
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;

layout(location = 0) out vec3 vColor;

void main()
{
	gl_Position = vec4(aPos, 0.0, 1.0);
	vColor = aColor;
}
