#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;

layout(std140, set = 0, binding = 0) uniform SceneUniformsBlock {
	mat4 viewProj;
	mat4 model;
	vec4 camPosition;
	vec4 camRight;
	vec4 camUp;
	vec4 camForward;
	vec4 resolution;
} SceneUniforms;

void main()
{
	vec4 worldPos = SceneUniforms.model * vec4(aPos, 1.0);
	vWorldPos = worldPos.xyz;
	vNormal = mat3(SceneUniforms.model) * aNormal;
	gl_Position = SceneUniforms.viewProj * worldPos;
}
