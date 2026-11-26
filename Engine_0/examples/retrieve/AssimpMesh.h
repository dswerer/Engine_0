#pragma once

#include "Mesh.h"

#include <string>

// 读取示例模型，按三点 patch 保存鱼的表面。位置乘 0.05，法线归一化。
bool LoadFishMesh(const std::string& path, dEngine::Mesh& out, std::string* error);
