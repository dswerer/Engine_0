#include "Mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace dEngine {

	namespace {
		bool IsFinite(const glm::vec3& v) {
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		uint32_t FormatBytes(rhi::VertexFormat format) {
			switch (format) {
			case rhi::VertexFormat::Float:  return 4;
			case rhi::VertexFormat::Float2: return 8;
			case rhi::VertexFormat::Float3: return 12;
			case rhi::VertexFormat::Float4: return 16;
			}
			return 0;
		}

		const char* SemanticName(VertexSemantic semantic) {
			switch (semantic) {
			case VertexSemantic::Position: return "Position";
			case VertexSemantic::Normal:   return "Normal";
			case VertexSemantic::Tangent:  return "Tangent";
			case VertexSemantic::TexCoord: return "TexCoord";
			case VertexSemantic::Color:    return "Color";
			}
			return "Unknown";
		}

		bool ValidAttributeType(const MeshAttribute& attribute) {
			using Format = rhi::VertexFormat;
			switch (attribute.semantic) {
			case VertexSemantic::Position:
			case VertexSemantic::Normal:
				return attribute.semanticIndex == 0 && attribute.format == Format::Float3;
			case VertexSemantic::Tangent:
				return attribute.semanticIndex == 0 && attribute.format == Format::Float4;
			case VertexSemantic::TexCoord:
				return attribute.format == Format::Float || attribute.format == Format::Float2
					|| attribute.format == Format::Float3;
			case VertexSemantic::Color:
				return attribute.format == Format::Float3 || attribute.format == Format::Float4;
			}
			return false;
		}

		uint32_t IndexBytes(rhi::IndexType type) {
			switch (type) {
			case rhi::IndexType::UInt16: return 2;
			case rhi::IndexType::UInt32: return 4;
			}
			return 0;
		}

		// bytes 不保证整型对齐，通过 memcpy 读取，避免未对齐访问和别名问题。
		uint32_t ReadIndex(const IndexData& indices, size_t index) {
			if (indices.type == rhi::IndexType::UInt16) {
				uint16_t value;
				std::memcpy(&value, indices.bytes.data() + index * sizeof(value), sizeof(value));
				return value;
			}
			uint32_t value;
			std::memcpy(&value, indices.bytes.data() + index * sizeof(value), sizeof(value));
			return value;
		}

		bool ValidPrimitiveCount(const Submesh& submesh, uint32_t count) {
			switch (submesh.topology) {
			case rhi::PrimitiveTopology::PointList:     return count > 0;
			case rhi::PrimitiveTopology::LineList:      return count > 0 && count % 2 == 0;
			case rhi::PrimitiveTopology::LineStrip:     return count >= 2;
			case rhi::PrimitiveTopology::TriangleList:  return count > 0 && count % 3 == 0;
			case rhi::PrimitiveTopology::TriangleStrip: return count >= 3;
			case rhi::PrimitiveTopology::PatchList:
				return submesh.patchControlPoints > 0 && count > 0
					&& count % submesh.patchControlPoints == 0;
			}
			return false;
		}

		// 与包围盒分开校验，允许 RecalculateBounds 处理尚未生成包围盒的 Mesh。
		bool ValidateGeometry(const Mesh& mesh, std::vector<std::string>* errors) {
			bool ok = true;
			auto fail = [&](std::string message) {
				ok = false;
				if (errors) errors->push_back("Mesh: " + std::move(message));
			};
			if (mesh.layout.bindings.empty()) fail("顶点绑定为空");
			if (mesh.layout.attributes.empty()) fail("顶点属性为空");
			if (mesh.submeshes.empty()) fail("子网格为空");
			if (mesh.vertexCount == 0) fail("顶点数量为零");
			if (!mesh.FindAttribute(VertexSemantic::Position)) fail("缺少 Position0 属性");

			std::map<uint32_t, const rhi::VertexInputBinding*> bindings;
			for (const auto& binding : mesh.layout.bindings) {
				if (!bindings.emplace(binding.binding, &binding).second)
					fail("重复的顶点 binding " + std::to_string(binding.binding));
				if (binding.perInstance) fail("Mesh 不保存逐实例顶点流");
				if (binding.stride == 0 || binding.stride % 4 != 0)
					fail("当前浮点顶点布局要求 stride 非零且为 4 的倍数");
			}

			std::set<std::pair<VertexSemantic, uint32_t>> semantics;
			for (const auto& attribute : mesh.layout.attributes) {
				const std::string label = std::string(SemanticName(attribute.semantic))
					+ std::to_string(attribute.semanticIndex);
				if (!semantics.emplace(attribute.semantic, attribute.semanticIndex).second)
					fail("重复的顶点属性语义 " + label);
				if (!ValidAttributeType(attribute))
					fail("属性语义、通道或格式不匹配: " + label);
				const auto it = bindings.find(attribute.binding);
				if (it == bindings.end()) {
					fail("顶点属性引用未声明的 binding " + std::to_string(attribute.binding));
					continue;
				}
				const uint32_t bytes = FormatBytes(attribute.format);
				if (bytes == 0 || attribute.offset % 4 != 0
					|| uint64_t(attribute.offset) + bytes > it->second->stride)
					fail("顶点属性格式、偏移或范围无效: " + label);
			}

			std::set<uint32_t> streams;
			for (const auto& stream : mesh.vertexStreams) {
				if (!streams.insert(stream.binding).second)
					fail("重复的顶点流 binding " + std::to_string(stream.binding));
				const auto it = bindings.find(stream.binding);
				if (it == bindings.end()) {
					fail("顶点流引用未声明的 binding " + std::to_string(stream.binding));
					continue;
				}
				if (uint64_t(mesh.vertexCount) * it->second->stride != stream.bytes.size())
					fail("顶点流字节数须等于 Mesh::vertexCount * stride，binding "
						+ std::to_string(stream.binding));
			}
			for (const auto& binding : bindings)
				if (streams.count(binding.first) == 0)
					fail("缺少顶点流 binding " + std::to_string(binding.first));

			if (mesh.indices) {
				const uint32_t bytes = IndexBytes(mesh.indices->type);
				if (bytes == 0 || mesh.indices->bytes.empty()
					|| mesh.indices->bytes.size() % bytes != 0)
					fail("索引类型或索引字节数无效");
			}
			// 结构校验失败时不再读取字节数据。
			if (!ok) return false;

			for (size_t i = 0; i < mesh.submeshes.size(); ++i) {
				const auto& submesh = mesh.submeshes[i];
				const std::string label = "子网格 " + std::to_string(i) + ": ";
				if (const auto* range = std::get_if<IndexedRange>(&submesh.range)) {
					if (!ValidPrimitiveCount(submesh, range->indexCount))
						fail(label + "索引数量与拓扑不匹配（不支持 primitive restart）");
					if (!mesh.indices) {
						fail(label + "索引绘制缺少索引数据");
						continue;
					}
					if (uint64_t(range->firstIndex) + range->indexCount > mesh.indices->IndexCount()) {
						fail(label + "索引范围越界");
						continue;
					}
					for (uint32_t n = 0; n < range->indexCount; ++n) {
						const int64_t vertex = int64_t(ReadIndex(*mesh.indices,
							size_t(range->firstIndex) + n)) + range->baseVertex;
						if (vertex < 0 || uint64_t(vertex) >= mesh.vertexCount) {
							fail(label + "索引加 baseVertex 后超出顶点流范围");
							break;
						}
					}
				}
				else {
					const auto& vertexRange = std::get<VertexRange>(submesh.range);
					if (!ValidPrimitiveCount(submesh, vertexRange.vertexCount))
						fail(label + "顶点数量与拓扑不匹配");
					if (uint64_t(vertexRange.firstVertex) + vertexRange.vertexCount > mesh.vertexCount)
						fail(label + "顶点范围越界");
				}
			}
			return ok;
		}
		struct GeometryBounds {
			Aabb mesh;
			std::vector<Aabb> submeshes;
		};

		// 仅在 ValidateGeometry 成功后调用，布局、流长度及所有访问范围均已检查。
		bool CalculateBounds(const Mesh& mesh, GeometryBounds& result, std::string& error) {
			const auto& position = *mesh.FindAttribute(VertexSemantic::Position);
			const auto binding = std::find_if(mesh.layout.bindings.begin(), mesh.layout.bindings.end(),
				[&](const rhi::VertexInputBinding& b) { return b.binding == position.binding; });
			const auto& stream = *mesh.FindVertexStream(position.binding);
			auto readPosition = [&](size_t vertex) {
				float xyz[3];
				const size_t offset = vertex * binding->stride + position.offset;
				std::memcpy(xyz, stream.bytes.data() + offset, sizeof(xyz));
				return glm::vec3(xyz[0], xyz[1], xyz[2]);
			};

			// 未被引用的位置也属于此资源，不能隐藏 NaN / Inf。
			for (uint32_t vertex = 0; vertex < mesh.vertexCount; ++vertex) {
				if (!IsFinite(readPosition(vertex))) {
					error = "Position0 包含 NaN 或无穷大，顶点 " + std::to_string(vertex);
					return false;
				}
			}

			result.submeshes.resize(mesh.submeshes.size());
			for (size_t i = 0; i < mesh.submeshes.size(); ++i) {
				auto& bounds = result.submeshes[i];
				if (const auto* range = std::get_if<IndexedRange>(&mesh.submeshes[i].range)) {
					for (uint32_t n = 0; n < range->indexCount; ++n) {
						const int64_t vertex = int64_t(ReadIndex(*mesh.indices, size_t(range->firstIndex) + n))
							+ range->baseVertex;
						bounds.Expand(readPosition(static_cast<size_t>(vertex)));
					}
				}
				else {
					const auto& vertexRange = std::get<VertexRange>(mesh.submeshes[i].range);
					for (uint32_t n = 0; n < vertexRange.vertexCount; ++n)
						bounds.Expand(readPosition(size_t(vertexRange.firstVertex) + n));
				}
				result.mesh.Expand(bounds.min);
				result.mesh.Expand(bounds.max);
			}
			return true;
		}
	}

	bool Aabb::IsValid() const {
		return IsFinite(min) && IsFinite(max)
			&& min.x <= max.x && min.y <= max.y && min.z <= max.z;
	}

	bool Aabb::Contains(const glm::vec3& point) const {
		return IsValid() && IsFinite(point)
			&& point.x >= min.x && point.x <= max.x
			&& point.y >= min.y && point.y <= max.y
			&& point.z >= min.z && point.z <= max.z;
	}

	bool Aabb::Contains(const Aabb& other) const {
		return other.IsValid() && Contains(other.min) && Contains(other.max);
	}

	bool Aabb::Expand(const glm::vec3& point) {
		if (!IsFinite(point)) return false;
		if (!IsValid()) {
			min = max = point;
			return true;
		}
		min = glm::vec3(std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z));
		max = glm::vec3(std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z));
		return true;
	}

	size_t IndexData::IndexCount() const {
		const uint32_t size = IndexBytes(type);
		return size != 0 && bytes.size() % size == 0 ? bytes.size() / size : 0;
	}

	const VertexStreamData* Mesh::FindVertexStream(uint32_t binding) const {
		const auto it = std::find_if(vertexStreams.begin(), vertexStreams.end(),
			[binding](const VertexStreamData& stream) { return stream.binding == binding; });
		return it != vertexStreams.end() ? &*it : nullptr;
	}

	const MeshAttribute* VertexLayout::FindAttribute(VertexSemantic semantic, uint32_t semanticIndex) const {
		const auto it = std::find_if(attributes.begin(), attributes.end(),
			[&](const MeshAttribute& attribute) {
				return attribute.semantic == semantic && attribute.semanticIndex == semanticIndex;
			});
		return it != attributes.end() ? &*it : nullptr;
	}

	const MeshAttribute* Mesh::FindAttribute(VertexSemantic semantic, uint32_t semanticIndex) const {
		return layout.FindAttribute(semantic, semanticIndex);
	}

	std::optional<MeshDrawMetadata> detail::GetMeshDrawMetadata(const std::vector<Submesh>& submeshes,
		size_t submeshIndex, std::optional<rhi::IndexType> indexType) {
		if (submeshIndex >= submeshes.size()) return std::nullopt;
		const auto& submesh = submeshes[submeshIndex];

		MeshDrawMetadata metadata;
		metadata.topology = submesh.topology;
		metadata.patchControlPoints = submesh.patchControlPoints;
		metadata.materialSlot = submesh.materialSlot;
		metadata.localBounds = submesh.localBounds;
		if (const auto* indexed = std::get_if<IndexedRange>(&submesh.range)) {
			if (!indexType) return std::nullopt;
			metadata.indexType = indexType;
			metadata.count = indexed->indexCount;
			metadata.first = indexed->firstIndex;
			metadata.baseVertex = indexed->baseVertex;
		}
		else {
			const auto& vertices = std::get<VertexRange>(submesh.range);
			metadata.count = vertices.vertexCount;
			metadata.first = vertices.firstVertex;
		}
		return metadata;
	}

	std::optional<MeshDrawMetadata> Mesh::GetDrawMetadata(size_t submeshIndex) const {
		const auto metadata = detail::GetMeshDrawMetadata(submeshes, submeshIndex,
			indices ? std::optional{indices->type} : std::nullopt);
		if (!metadata || !ValidPrimitiveCount(submeshes[submeshIndex], metadata->count))
			return std::nullopt;
		const size_t elementCount = metadata->indexType ? indices->IndexCount() : vertexCount;
		if (uint64_t(metadata->first) + metadata->count > elementCount) return std::nullopt;
		return metadata;
	}

	bool Mesh::Validate(std::vector<std::string>* errors) const {
		const bool geometryValid = ValidateGeometry(*this, errors);
		bool ok = geometryValid;
		auto fail = [&](std::string message) {
			ok = false;
			if (errors) errors->push_back("Mesh: " + std::move(message));
		};
		if (!localBounds.IsValid()) fail("Mesh 包围盒无效");
		for (size_t i = 0; i < submeshes.size(); ++i) {
			if (!submeshes[i].localBounds.IsValid())
				fail("子网格 " + std::to_string(i) + " 包围盒无效");
			else if (localBounds.IsValid() && !localBounds.Contains(submeshes[i].localBounds))
				fail("Mesh 包围盒未包含子网格 " + std::to_string(i));
		}
		if (geometryValid) {
			GeometryBounds actual;
			std::string error;
			if (!CalculateBounds(*this, actual, error))
				fail(std::move(error));
			else {
				for (size_t i = 0; i < submeshes.size(); ++i)
					if (submeshes[i].localBounds.IsValid()
						&& !submeshes[i].localBounds.Contains(actual.submeshes[i]))
						fail("子网格包围盒未覆盖其引用的位置，子网格 " + std::to_string(i));
			}
		}
		return ok;
	}

	bool Mesh::RecalculateBounds(std::string* error) {
		auto fail = [&](std::string message) {
			if (error) *error = std::move(message);
			return false;
		};
		std::vector<std::string> errors;
		if (!ValidateGeometry(*this, &errors))
			return fail(errors.front());

		GeometryBounds bounds;
		std::string boundsError;
		if (!CalculateBounds(*this, bounds, boundsError))
			return fail("Mesh: " + boundsError);

		// 全部计算成功后提交，避免某个子网格失败时留下部分更新。
		localBounds = bounds.mesh;
		for (size_t i = 0; i < submeshes.size(); ++i)
			submeshes[i].localBounds = bounds.submeshes[i];
		if (error) error->clear();
		return true;
	}
}
