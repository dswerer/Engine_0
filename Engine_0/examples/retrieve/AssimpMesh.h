#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

struct FishVertex {
	glm::vec3 pos;
	glm::vec3 normal;
	glm::vec2 uv;
};

struct FishMesh {
	std::vector<FishVertex> vertices;
	std::vector<uint32_t>   indices;
};

// 读取 lowpoly_fish.glb；位置乘 0.05，法线归一化，忽略材质贴图
bool LoadFishMesh(const std::string& path, FishMesh& out, std::string* error);
