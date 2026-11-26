#include "AssimpMesh.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
	struct FishVertex {
		glm::vec3 pos;
		glm::vec3 normal;
		glm::vec2 uv{0.0f};
	};
	static_assert(std::is_standard_layout_v<FishVertex> && std::is_trivially_copyable_v<FishVertex>);
}

bool LoadFishMesh(const std::string& path, dEngine::Mesh& out, std::string* error) {
	using namespace dEngine;
	auto fail = [&](std::string message) {
		if (error) *error = std::move(message);
		return false;
	};
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(path,
		aiProcess_Triangulate | aiProcess_GenSmoothNormals);
	if (scene == nullptr) return fail(importer.GetErrorString());
	if (!scene->HasMeshes()) return fail("网格为空");

	std::vector<FishVertex> vertices;
	std::vector<uint32_t> indices;
	constexpr float kScale = 0.05f;
	constexpr size_t kMaxCount = std::numeric_limits<uint32_t>::max();
	for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
		const aiMesh* mesh = scene->mMeshes[m];
		if (mesh->mNumVertices > kMaxCount - vertices.size()
			|| mesh->mNumFaces > (kMaxCount - indices.size()) / 3)
			return fail("网格超出 32 位顶点或索引数量范围");
		const uint32_t base = static_cast<uint32_t>(vertices.size());

		for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
			FishVertex v{};
			v.pos = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y,
				mesh->mVertices[i].z) * kScale;
			if (mesh->HasNormals()) {
				v.normal = glm::normalize(glm::vec3(mesh->mNormals[i].x,
					mesh->mNormals[i].y, mesh->mNormals[i].z));
			}
			else {
				v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
			}
			if (mesh->HasTextureCoords(0)) {
				v.uv = glm::vec2(mesh->mTextureCoords[0][i].x,
					mesh->mTextureCoords[0][i].y);
			}
			vertices.push_back(v);
		}

		for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
			const aiFace& face = mesh->mFaces[f];
			if (face.mNumIndices != 3) return fail("鱼网格包含非三角图元");
			for (unsigned int k = 0; k < face.mNumIndices; ++k) {
				if (face.mIndices[k] >= mesh->mNumVertices) return fail("鱼网格索引越界");
				indices.push_back(base + face.mIndices[k]);
			}
		}
	}

	if (vertices.empty() || indices.empty()) return fail("网格为空");

	Mesh result;
	result.name = "Fish";
	result.vertexCount = static_cast<uint32_t>(vertices.size());
	result.layout.bindings = {{0, sizeof(FishVertex), false}};
	result.layout.attributes = {
		{VertexSemantic::Position, 0, 0, rhi::VertexFormat::Float3, offsetof(FishVertex, pos)},
		{VertexSemantic::Normal, 0, 0, rhi::VertexFormat::Float3, offsetof(FishVertex, normal)},
		{VertexSemantic::TexCoord, 0, 0, rhi::VertexFormat::Float2, offsetof(FishVertex, uv)}
	};

	VertexStreamData stream{0, std::vector<std::byte>(vertices.size() * sizeof(FishVertex))};
	std::memcpy(stream.bytes.data(), vertices.data(), stream.bytes.size());
	result.vertexStreams.push_back(std::move(stream));
	result.indices = IndexData{rhi::IndexType::UInt32, std::vector<std::byte>(indices.size() * sizeof(uint32_t))};
	std::memcpy(result.indices->bytes.data(), indices.data(), result.indices->bytes.size());

	Submesh surface;
	surface.name = "FishSurface";
	surface.topology = rhi::PrimitiveTopology::PatchList;
	surface.patchControlPoints = 3;
	surface.range = IndexedRange{0, static_cast<uint32_t>(indices.size()), 0};
	result.submeshes.push_back(std::move(surface));
	if (!result.RecalculateBounds(error)) return false;
	out = std::move(result);
	return true;
}
