#pragma once
#include "Mesh.h"

#include <map>
#include <memory>

namespace dEngine::rhi { class Device; }

namespace dEngine {

	// 静态几何的 GPU 资源。缓冲按源 binding 保存，布局与子网格在上传时取快照。
	// 引用这些缓冲的图及其在途 GPU 工作结束后，才能销毁 MeshGpu。Device 须存活到销毁之后。
	class MeshGpu {
	public:
		~MeshGpu();
		MeshGpu(const MeshGpu&) = delete;
		MeshGpu& operator=(const MeshGpu&) = delete;

		const VertexLayout& Layout() const { return m_layout; }
		const std::vector<Submesh>& Submeshes() const { return m_submeshes; }
		const rhi::Buffer* FindVertexBuffer(uint32_t sourceBinding) const;
		const rhi::Buffer* IndexBuffer() const { return m_indexBuffer.get(); }
		std::optional<rhi::IndexType> IndexType() const { return m_indexType; }

		// 查询上传时校验过的快照。子网格下标越界时返回空值。
		std::optional<MeshDrawMetadata> GetDrawMetadata(size_t submeshIndex) const {
			return detail::GetMeshDrawMetadata(m_submeshes, submeshIndex, m_indexType);
		}

	private:
		MeshGpu();
		friend std::unique_ptr<MeshGpu> UploadMesh(rhi::Device&, const Mesh&, std::string*);

		VertexLayout m_layout;
		std::vector<Submesh> m_submeshes;
		std::map<uint32_t, std::unique_ptr<rhi::Buffer>> m_vertexBuffers;
		std::unique_ptr<rhi::Buffer> m_indexBuffer;
		std::optional<rhi::IndexType> m_indexType;
	};

	// 在资源初始化时同步校验并上传，同一个返回值可供不同子网格和 Pass 使用。
	// 校验或上传失败时返回 nullptr，已创建的缓冲由临时对象释放。
	std::unique_ptr<MeshGpu> UploadMesh(rhi::Device& device, const Mesh& mesh,
		std::string* error = nullptr);
}
