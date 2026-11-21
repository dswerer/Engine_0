#pragma once
#include "rhi/RHIDesc.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dEngine {

	// 局部坐标系包围盒。默认是空盒；平面、线或点形成的退化盒仍然有效。
	struct Aabb {
		glm::vec3 min{ std::numeric_limits<float>::infinity() };
		glm::vec3 max{ -std::numeric_limits<float>::infinity() };

		bool IsValid() const;
		bool Contains(const glm::vec3& point) const;
		bool Contains(const Aabb& other) const;
		// 无效盒从该点开始构建；非有限坐标返回 false，保持原值。
		bool Expand(const glm::vec3& point);
	};

	// 几何属性的含义；着色器 location 由渲染层按语义映射，不固化在 Mesh 中。
	enum class VertexSemantic { Position, Normal, Tangent, TexCoord, Color };

	struct MeshAttribute {
		VertexSemantic semantic = VertexSemantic::Position;
		uint32_t semanticIndex = 0; // TexCoord / Color 可有多个通道，其余语义只使用 0
		uint32_t binding = 0;
		rhi::VertexFormat format = rhi::VertexFormat::Float3;
		uint32_t offset = 0;
	};

	// Position0 必须存在且为 Float3；Normal0 可选，为 Float3。
	// Tangent0 可选，为 Float4，xyz 为切线，副切线 B = w * cross(N, T)。
	// TexCoord 使用 Float / Float2 / Float3；Color 使用 Float3 / Float4。
	struct VertexLayout {
		std::vector<rhi::VertexInputBinding> bindings;
		std::vector<MeshAttribute> attributes;
	};

	// binding 显式对应 VertexLayout，不依赖 vertexStreams 的排列顺序。
	// Mesh 只保存逐顶点流；逐实例属性由之后的实例数据通道提供。
	struct VertexStreamData {
		uint32_t binding = 0;
		// 所有流覆盖同一组 Mesh::vertexCount 个顶点，bytes.size() == vertexCount * stride。
		std::vector<std::byte> bytes;
	};

	struct IndexData {
		rhi::IndexType type = rhi::IndexType::UInt32;
		std::vector<std::byte> bytes;
		// 不完整的元素或非法索引类型返回 0；Validate 会给出具体错误。
		size_t IndexCount() const;
	};

	// firstIndex / indexCount 按索引元素计数，baseVertex 加到每个索引值上。
	struct IndexedRange {
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		int32_t baseVertex = 0;
	};

	// firstVertex / vertexCount 按顶点计数，均不是字节偏移。
	struct VertexRange {
		uint32_t firstVertex = 0;
		uint32_t vertexCount = 0;
	};

	// 同一局部坐标空间中可独立绘制的范围；独立节点变换由模型或场景保存。
	struct Submesh {
		std::string name;
		rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
		std::variant<IndexedRange, VertexRange> range;
		uint32_t materialSlot = 0; // Mesh 内的逻辑槽，由模型或实例映射到材质；不是 Assimp 场景材质索引
		uint32_t patchControlPoints = 3; // 仅 PatchList 使用，须与实际管线一致
		Aabb localBounds;
	};

	// 可共享的静态 CPU 几何资源，不持有对象变换、材质实例或 GPU 句柄。
	// 修改顶点数据、布局或绘制范围后，需重新计算包围盒。
	struct Mesh {
		std::string name;
		uint32_t vertexCount = 0;
		VertexLayout layout;
		std::vector<VertexStreamData> vertexStreams;
		std::optional<IndexData> indices;
		std::vector<Submesh> submeshes;
		Aabb localBounds;

		const VertexStreamData* FindVertexStream(uint32_t binding) const;
		const MeshAttribute* FindAttribute(VertexSemantic semantic, uint32_t semanticIndex = 0) const;

		// 创建或上传前校验：属性语义、统一流长度、拓扑、绘制范围及索引。
		// 所有 Position0 坐标必须有限；子网格包围盒须覆盖其引用的顶点，Mesh 盒须包含所有子盒。
		// 允许保守放大的包围盒；不要求包围盒覆盖未引用的顶点。
		// errors 非空时追加诊断；不会清空调用方已有内容。
		bool Validate(std::vector<std::string>* errors = nullptr) const;

		// 从 Position0 的 Float3 属性计算各子网格及 Mesh 的局部包围盒。
		// 只包含绘制范围实际引用的顶点；支持多流及带 baseVertex 的索引。
		// 所有位置必须是有限的笛卡尔坐标。失败时所有包围盒保持原值。
		bool RecalculateBounds(std::string* error = nullptr);
	};
}
