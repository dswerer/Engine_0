#pragma once
#include "Mesh.h"
#include "RenderGraphDesc.h"

namespace dEngine {

	class MeshGpu;

	// 由渲染技术声明，与具体着色器的输入 location / 类型保持一致。
	struct MeshShaderInput {
		VertexSemantic semantic = VertexSemantic::Position;
		uint32_t semanticIndex = 0;
		uint32_t location = 0;
		rhi::VertexFormat format = rhi::VertexFormat::Float3;
	};

	// CPU 侧的单次绘制准备结果，不持有 GPU 资源，也不选择材质或着色器。
	struct MeshDrawData {
		// 第 i 个 GPU binding 使用源 Mesh 中 binding == sourceBindings[i] 的字节流。
		// RenderGraph 按 vertexStreams 数组下标绑定缓冲，因此这里将源 binding 压紧为 0..N-1。
		std::vector<uint32_t> sourceBindings;
		std::vector<rhi::VertexInputBinding> vertexBindings;
		std::vector<rhi::VertexAttribute> vertexAttributes;
		rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
		uint32_t patchControlPoints = 3;
		// 仅当选中的子网格采用 IndexedRange 时有值。
		std::optional<rhi::IndexType> indexType;
		DrawSource draw;
	};

	// CPU 入口先校验 Mesh。GPU 入口复用 UploadMesh 已校验的布局与范围快照。
	// 失败时 output 保持原值，成功时清空 error。
	bool BuildMeshDrawData(const Mesh& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, MeshDrawData& output,
		std::string* error = nullptr);
	bool BuildMeshDrawData(const MeshGpu& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, MeshDrawData& output,
		std::string* error = nullptr);

	// 将几何输入写入渲染技术选定的管线，着色器和光栅化等状态由技术配置。
	void ApplyMeshPipeline(const MeshDrawData& geometry, PipelineDesc& pipeline);

	// 校验渲染层显式填写的 PipelineDesc 是否与 Mesh 几何兼容。
	// 检查显式布局的 binding/stride/input rate、location/format/offset、拓扑及 patch 大小。
	// 布局须与 BuildMeshDrawData 的映射完全对应（向量顺序可不同）；此入口不接受额外实例流。
	// 不修改 Mesh 或管线；不创建缓冲、注册 Pass 或反射 shader。inputs 的语义/location 契约由技术提供，
	// shader 阶段组合、描述符和设备能力仍由图层/RHI 检查。应在建图/创建管线前调用。
	bool ValidateMeshPipeline(const Mesh& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, const PipelineDesc& pipeline,
		std::string* error = nullptr);
}
