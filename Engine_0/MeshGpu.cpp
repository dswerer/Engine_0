#include "MeshGpu.h"
#include "rhi/RHI.h"

#include <utility>

namespace dEngine {

	MeshGpu::MeshGpu() = default;
	MeshGpu::~MeshGpu() = default;

	const rhi::Buffer* MeshGpu::FindVertexBuffer(uint32_t sourceBinding) const {
		const auto it = m_vertexBuffers.find(sourceBinding);
		return it == m_vertexBuffers.end() ? nullptr : it->second.get();
	}

	std::unique_ptr<MeshGpu> UploadMesh(rhi::Device& device, const Mesh& mesh, std::string* error) {
		auto fail = [&](std::string message) -> std::unique_ptr<MeshGpu> {
			if (error) *error = "UploadMesh (" + mesh.name + "): " + std::move(message);
			return nullptr;
		};
		std::vector<std::string> errors;
		if (!mesh.Validate(&errors)) return fail(errors.front());

		auto gpu = std::unique_ptr<MeshGpu>(new MeshGpu);
		gpu->m_layout = mesh.layout;
		gpu->m_submeshes = mesh.submeshes;
		for (const auto& stream : mesh.vertexStreams) {
			auto buffer = device.UploadBuffer(stream.bytes.data(), stream.bytes.size(), rhi::BufferUsage::Vertex,
				rhi::PipelineStage::VertexInput, rhi::Access::VertexAttributeRead);
			if (!buffer) return fail("顶点缓冲上传失败，源 binding " + std::to_string(stream.binding));
			gpu->m_vertexBuffers.emplace(stream.binding, std::move(buffer));
		}
		if (mesh.indices) {
			const auto& indices = *mesh.indices;
			gpu->m_indexBuffer = device.UploadBuffer(indices.bytes.data(), indices.bytes.size(),
				rhi::BufferUsage::Index, rhi::PipelineStage::VertexInput, rhi::Access::IndexRead);
			if (!gpu->m_indexBuffer) return fail("索引缓冲上传失败");
			gpu->m_indexType = indices.type;
		}
		if (error) error->clear();
		return gpu;
	}
}
