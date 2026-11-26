#include "MeshRenderGraph.h"
#include "MeshGpu.h"
#include "RenderGraphBuilder.h"

#include <utility>

namespace dEngine {
	std::optional<DrawSource> BindMeshGeometry(RenderGraphBuilder& builder, RenderPassDesc& pass,
		const std::string& resourcePrefix, const MeshGpu& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, std::string* error) {
		if (resourcePrefix.empty()) {
			if (error) *error = "BindMeshGeometry: 图资源前缀为空";
			return std::nullopt;
		}
		MeshDrawData geometry;
		if (!BuildMeshDrawData(mesh, submeshIndex, inputs, geometry, error)) return std::nullopt;

		RenderGraphBuilder buffers;
		std::vector<VertexStreamBinding> vertexStreams;
		vertexStreams.reserve(geometry.sourceBindings.size());
		for (uint32_t sourceBinding : geometry.sourceBindings) {
			std::string name = resourcePrefix + "/vertex/" + std::to_string(sourceBinding);
			buffers.DeclareBufferResource(name, mesh.FindVertexBuffer(sourceBinding));
			vertexStreams.push_back({std::move(name), {}, 0});
		}
		std::optional<IndexStreamBinding> indexStream;
		if (geometry.indexType) {
			std::string name = resourcePrefix + "/index";
			buffers.DeclareBufferResource(name, mesh.IndexBuffer());
			indexStream = IndexStreamBinding{std::move(name), {}, 0, *geometry.indexType};
		}
		if (!builder.ImportDeclarations(buffers)) {
			if (error) *error = "BindMeshGeometry: 无法合并图资源 " + resourcePrefix;
			return std::nullopt;
		}

		ApplyMeshPipeline(geometry, pass.pipeline);
		pass.vertexStreams = std::move(vertexStreams);
		pass.indexStream = std::move(indexStream);
		return geometry.draw;
	}
}
