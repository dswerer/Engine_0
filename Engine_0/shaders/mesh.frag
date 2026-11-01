#version 450

// 顶点色直出：无描述符绑定，供图侧顶点/索引流冒烟
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 o;

void main()
{
	o = vec4(vColor, 1.0);
}
