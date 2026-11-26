#include "MeshDrawData.h"
#include "MeshGpu.h"

#include <algorithm>
#include <set>
#include <utility>

namespace dEngine {

	// layout 和 metadata 来自同一次通过校验的 Mesh。
	static bool BuildMeshDrawData(const VertexLayout& layout, const std::optional<MeshDrawMetadata>& metadata,
		const std::vector<MeshShaderInput>& inputs, MeshDrawData& output, std::string* error) {
		auto fail = [&](std::string message) {
			if (error) *error = "MeshDrawData: " + std::move(message);
			return false;
		};
		if (!metadata) return fail("子网格下标越界");
		if (inputs.empty()) return fail("渲染技术未声明顶点输入");

		MeshDrawData result;
		std::set<uint32_t> locations;
		std::set<std::pair<VertexSemantic, uint32_t>> semantics;
		for (const auto& input : inputs) {
			if (!locations.insert(input.location).second) return fail("重复的着色器 location");
			if (!semantics.emplace(input.semantic, input.semanticIndex).second)
				return fail("重复的着色器输入语义");
			const auto* attribute = layout.FindAttribute(input.semantic, input.semanticIndex);
			if (attribute == nullptr)
				return fail("Mesh 缺少 location " + std::to_string(input.location) + " 所需的属性");
			if (attribute->format != input.format)
				return fail("Mesh 属性格式与 location " + std::to_string(input.location) + " 不一致");

			auto source = std::find(result.sourceBindings.begin(), result.sourceBindings.end(), attribute->binding);
			const uint32_t binding = static_cast<uint32_t>(source - result.sourceBindings.begin());
			if (source == result.sourceBindings.end()) {
				const auto sourceLayout = std::find_if(layout.bindings.begin(), layout.bindings.end(),
					[&](const rhi::VertexInputBinding& b) { return b.binding == attribute->binding; });
				result.sourceBindings.push_back(attribute->binding);
				result.vertexBindings.push_back({binding, sourceLayout->stride, false});
			}
			result.vertexAttributes.push_back({input.location, binding, input.format, attribute->offset});
		}

		result.topology = metadata->topology;
		result.patchControlPoints = metadata->patchControlPoints;
		result.indexType = metadata->indexType;
		result.draw.count = metadata->count;
		result.draw.first = metadata->first;
		result.draw.vertexOffset = metadata->baseVertex;
		output = std::move(result);
		if (error) error->clear();
		return true;
	}

	bool BuildMeshDrawData(const Mesh& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, MeshDrawData& output, std::string* error) {
		std::vector<std::string> errors;
		if (!mesh.Validate(&errors)) {
			if (error) *error = "MeshDrawData: " + errors.front();
			return false;
		}
		return BuildMeshDrawData(mesh.layout, mesh.GetDrawMetadata(submeshIndex), inputs, output, error);
	}

	bool BuildMeshDrawData(const MeshGpu& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, MeshDrawData& output, std::string* error) {
		return BuildMeshDrawData(mesh.Layout(), mesh.GetDrawMetadata(submeshIndex), inputs, output, error);
	}

	void ApplyMeshPipeline(const MeshDrawData& geometry, PipelineDesc& pipeline) {
		pipeline.vertexBindings = geometry.vertexBindings;
		pipeline.vertexAttributes = geometry.vertexAttributes;
		pipeline.topology = geometry.topology;
		pipeline.tessPatchControlPoints = geometry.patchControlPoints;
	}

	bool ValidateMeshPipeline(const Mesh& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, const PipelineDesc& pipeline, std::string* error) {
		MeshDrawData expected;
		if (!BuildMeshDrawData(mesh, submeshIndex, inputs, expected, error)) return false;
		auto fail = [&](std::string message) {
			if (error) *error = "MeshPipeline: " + std::move(message);
			return false;
		};
		if (pipeline.topology != expected.topology)
			return fail("管线 topology 与子网格拓扑不一致");
		if (expected.topology == rhi::PrimitiveTopology::PatchList
			&& pipeline.tessPatchControlPoints != expected.patchControlPoints)
			return fail("管线 tessPatchControlPoints 与子网格控制点数量不一致");
		if (pipeline.vertexBindings.empty() || pipeline.vertexAttributes.empty())
			return fail("Mesh 管线必须显式声明 vertexBindings 和 vertexAttributes，不能依赖自动布局");
		if (pipeline.vertexBindings.size() != expected.vertexBindings.size())
			return fail("管线顶点 binding 数量与 Mesh 所需的流数量不一致");
		if (pipeline.vertexAttributes.size() != expected.vertexAttributes.size())
			return fail("管线顶点属性数量与渲染技术声明的 Mesh 输入不一致");

		std::set<uint32_t> bindings;
		for (const auto& actual : pipeline.vertexBindings) {
			if (!bindings.insert(actual.binding).second) return fail("管线存在重复的顶点 binding");
			const auto wanted = std::find_if(expected.vertexBindings.begin(), expected.vertexBindings.end(),
				[&](const rhi::VertexInputBinding& binding) { return binding.binding == actual.binding; });
			if (wanted == expected.vertexBindings.end())
				return fail("管线 binding 没有对应的 Mesh 流: " + std::to_string(actual.binding));
			if (actual.perInstance)
				return fail("此 Mesh 管线入口只接受逐顶点流，binding " + std::to_string(actual.binding));
			if (actual.stride != wanted->stride)
				return fail("binding " + std::to_string(actual.binding) + " 的 stride 不匹配，Mesh="
					+ std::to_string(wanted->stride) + ", pipeline=" + std::to_string(actual.stride));
		}

		std::set<uint32_t> locations;
		for (const auto& actual : pipeline.vertexAttributes) {
			if (!locations.insert(actual.location).second) return fail("管线存在重复的顶点 location");
			const auto wanted = std::find_if(expected.vertexAttributes.begin(), expected.vertexAttributes.end(),
				[&](const rhi::VertexAttribute& attribute) { return attribute.location == actual.location; });
			if (wanted == expected.vertexAttributes.end())
				return fail("管线 location 未由 Mesh 输入契约声明: " + std::to_string(actual.location));
			const std::string label = "location " + std::to_string(actual.location);
			if (actual.binding != wanted->binding) return fail(label + " 的流 binding 不匹配");
			if (actual.format != wanted->format) return fail(label + " 的属性 format 不匹配");
			if (actual.offset != wanted->offset) return fail(label + " 的属性 offset 不匹配");
		}
		if (error) error->clear();
		return true;
	}
}
