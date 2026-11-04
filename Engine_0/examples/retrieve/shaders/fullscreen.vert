#version 450

void main() {
    vec2 p = vec2(-1.0, -1.0);
    if (gl_VertexIndex == 1) p = vec2(3.0, -1.0);
    else if (gl_VertexIndex == 2) p = vec2(-1.0, 3.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
