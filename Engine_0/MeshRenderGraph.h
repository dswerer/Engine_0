#pragma once
#include "MeshDrawData.h"

namespace dEngine {
	class RenderGraphBuilder;

	// 在 DeclarePass 前调用。用选定子网格替换 desc 的几何字段，返回直接绘制的范围。
	// 使用同一资源前缀为多个 Pass 绑定同一个 MeshGpu 时，共享已声明的缓冲。
	// Graph 借用 MeshGpu 的缓冲。Pass 的着色器、附件、描述符和实例数由渲染层配置。
	// 校验或资源合并失败时，builder 和 pass 保持原值。
	std::optional<DrawSource> BindMeshGeometry(RenderGraphBuilder& builder, RenderPassDesc& pass,
		const std::string& resourcePrefix, const MeshGpu& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, std::string* error = nullptr);
}
