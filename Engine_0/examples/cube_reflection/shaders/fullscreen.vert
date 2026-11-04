#version 450

// 全屏三角形：无顶点缓冲，靠内建 gl_VertexIndex 生成覆盖屏幕的三角形
void main()
{
	vec2 positions[3] = vec2[](
		vec2(-1.0, -1.0),
		vec2( 3.0, -1.0),
		vec2(-1.0,  3.0)
	);
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
