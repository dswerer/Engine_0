#pragma once

#include <glm/glm.hpp>
#include <cstdint>

// 与 shader 的 std140 布局一一对应

struct FrameUniforms {
	glm::mat4 PMat;
	glm::mat4 PMat_iv;
	glm::mat4 lightSpaceMatrix;
	glm::vec4 dlight;
	glm::vec2 resolution;
	float     time;
	float     _pad;
	glm::vec3 lightCol;
	float     _pad2;
	glm::vec4 cameraPos;
	glm::vec4 lightDir;
};
static_assert(sizeof(FrameUniforms) == 272, "FrameUniforms 必须与 std140 布局一致");

struct AtmosphereParams {
	float H_Rayleigh;
	float H_Mie;
	float g_Mie;
	float PlanetRadius;
	float AtmosphereRadius;
	float H_Ozone;
	float W_Ozone;
	alignas(16) glm::vec3 SunLuminance;
	alignas(16) glm::vec3 PlanetCenter;
};
static_assert(sizeof(AtmosphereParams) == 64, "AtmosphereParams 必须与 std140 布局一致");

struct ShadowUniforms {
	glm::mat4 PMat;
	glm::vec4 params; // x = time
};
static_assert(sizeof(ShadowUniforms) == 80, "ShadowUniforms 必须与 std140 布局一致");

struct BoidsParams {
	int   fishCount;
	float time;
	float noiseStrength;
	float boundaryMargin;
	glm::vec4 boundsMin;
	glm::vec4 boundsMax;
	float boundaryStrength;
	float _pad[3];
};
static_assert(sizeof(BoidsParams) == 64, "BoidsParams 必须与 std140 布局一致");
