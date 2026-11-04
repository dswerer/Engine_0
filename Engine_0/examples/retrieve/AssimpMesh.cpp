#include "AssimpMesh.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/geometric.hpp>

bool LoadFishMesh(const std::string& path, FishMesh& out, std::string* error) {
	out.vertices.clear();
	out.indices.clear();

	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(path,
		aiProcess_Triangulate | aiProcess_GenSmoothNormals);
	if (scene == nullptr || !scene->HasMeshes()) {
		if (error != nullptr)
			*error = importer.GetErrorString();
		return false;
	}

	constexpr float kScale = 0.05f;
	for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
		const aiMesh* mesh = scene->mMeshes[m];
		const uint32_t base = static_cast<uint32_t>(out.vertices.size());

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
			out.vertices.push_back(v);
		}

		for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
			const aiFace& face = mesh->mFaces[f];
			for (unsigned int k = 0; k < face.mNumIndices; ++k)
				out.indices.push_back(base + face.mIndices[k]);
		}
	}

	if (out.vertices.empty() || out.indices.empty()) {
		if (error != nullptr)
			*error = "网格为空";
		return false;
	}
	return true;
}
