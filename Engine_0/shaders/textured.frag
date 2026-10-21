#version 450

// combined image sampler：对应 ResourceType::SampledImage（set 0, binding 0）
layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = texture(uTexture, vec2(0.5, 0.5));
}
