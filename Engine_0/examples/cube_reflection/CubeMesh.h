#pragma once
#include "Mesh.h"

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace dEngine::examples {

	// 窗口示例、CPU 检查程序和测试共同使用同一份几何生成代码。
	inline Mesh CreateCubeMesh() {
		struct Vertex { glm::vec3 position; glm::vec3 normal; };
		static_assert(std::is_standard_layout_v<Vertex> && std::is_trivially_copyable_v<Vertex>);
		struct Face { glm::vec3 normal; glm::vec3 u; glm::vec3 v; };
		const Face faces[] = {
			{{ 1, 0, 0}, { 0, 0,-1}, {0,1,0}},
			{{-1, 0, 0}, { 0, 0, 1}, {0,1,0}},
			{{ 0, 1, 0}, { 1, 0, 0}, {0,0,-1}},
			{{ 0,-1, 0}, { 1, 0, 0}, {0,0,1}},
			{{ 0, 0, 1}, { 1, 0, 0}, {0,1,0}},
			{{ 0, 0,-1}, {-1, 0, 0}, {0,1,0}}
		};
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		for (const auto& face : faces) {
			const glm::vec3 corners[] = {face.normal - face.u - face.v, face.normal + face.u - face.v,
				face.normal + face.u + face.v, face.normal - face.u + face.v};
			const uint32_t first = static_cast<uint32_t>(vertices.size());
			for (const auto& corner : corners) vertices.push_back({corner, face.normal});
			for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(first + index);
		}

		Mesh mesh;
		mesh.name = "Cube";
		mesh.vertexCount = static_cast<uint32_t>(vertices.size());
		// 故意使用非连续源 binding，验证渲染适配会将它映射到 GPU binding 0。
		mesh.layout.bindings = {{7, static_cast<uint32_t>(sizeof(Vertex)), false}};
		mesh.layout.attributes = {
			{VertexSemantic::Position, 0, 7, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(Vertex, position))},
			{VertexSemantic::Normal, 0, 7, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(Vertex, normal))}
		};
		VertexStreamData stream;
		stream.binding = 7;
		stream.bytes.resize(vertices.size() * sizeof(Vertex));
		std::memcpy(stream.bytes.data(), vertices.data(), stream.bytes.size());
		mesh.vertexStreams.push_back(std::move(stream));
		mesh.indices = IndexData{rhi::IndexType::UInt32, std::vector<std::byte>(indices.size() * sizeof(uint32_t))};
		std::memcpy(mesh.indices->bytes.data(), indices.data(), mesh.indices->bytes.size());
		Submesh submesh;
		submesh.name = "CubeSurface";
		submesh.range = IndexedRange{0, static_cast<uint32_t>(indices.size()), 0};
		mesh.submeshes.push_back(submesh);
		// 包围盒在调用方显式生成并检查错误，避免静默忽略失败。
		return mesh;
	}
}
