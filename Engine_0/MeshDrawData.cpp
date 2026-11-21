#include "MeshDrawData.h"

#include <algorithm>
#include <set>
#include <utility>

namespace dEngine {

	bool BuildMeshDrawData(const Mesh& mesh, size_t submeshIndex,
		const std::vector<MeshShaderInput>& inputs, MeshDrawData& output, std::string* error) {
		auto fail = [&](std::string message) {
			if (error) *error = "MeshDrawData: " + std::move(message);
			return false;
		};
		std::vector<std::string> errors;
		if (!mesh.Validate(&errors)) return fail(errors.front());
		if (submeshIndex >= mesh.submeshes.size()) return fail("子网格下标越界");
		if (inputs.empty()) return fail("渲染技术未声明顶点输入");

		MeshDrawData result;
		std::set<uint32_t> locations;
		std::set<std::pair<VertexSemantic, uint32_t>> semantics;
		for (const auto& input : inputs) {
			if (!locations.insert(input.location).second) return fail("重复的着色器 location");
			if (!semantics.emplace(input.semantic, input.semanticIndex).second)
				return fail("重复的着色器输入语义");
			const auto* attribute = mesh.FindAttribute(input.semantic, input.semanticIndex);
			if (attribute == nullptr)
				return fail("Mesh 缺少 location " + std::to_string(input.location) + " 所需的属性");
			if (attribute->format != input.format)
				return fail("Mesh 属性格式与 location " + std::to_string(input.location) + " 不一致");

			auto source = std::find(result.sourceBindings.begin(), result.sourceBindings.end(), attribute->binding);
			const uint32_t binding = static_cast<uint32_t>(source - result.sourceBindings.begin());
			if (source == result.sourceBindings.end()) {
				const auto layout = std::find_if(mesh.layout.bindings.begin(), mesh.layout.bindings.end(),
					[&](const rhi::VertexInputBinding& b) { return b.binding == attribute->binding; });
				result.sourceBindings.push_back(attribute->binding);
				result.vertexBindings.push_back({binding, layout->stride, false});
			}
			result.vertexAttributes.push_back({input.location, binding, input.format, attribute->offset});
		}

		const auto& submesh = mesh.submeshes[submeshIndex];
		result.topology = submesh.topology;
		result.patchControlPoints = submesh.patchControlPoints;
		if (const auto* indexed = std::get_if<IndexedRange>(&submesh.range)) {
			result.indexType = mesh.indices->type;
			result.draw.count = indexed->indexCount;
			result.draw.first = indexed->firstIndex;
			result.draw.vertexOffset = indexed->baseVertex;
		}
		else {
			const auto& direct = std::get<VertexRange>(submesh.range);
			result.draw.count = direct.vertexCount;
			result.draw.first = direct.firstVertex;
		}
		output = std::move(result);
		if (error) error->clear();
		return true;
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
