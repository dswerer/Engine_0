#include "MeshDrawData.h"
#include "examples/cube_reflection/CubeMesh.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace dEngine;

namespace {
	int checks = 0;
	void Check(bool condition, const char* message) {
		if (!condition) throw std::runtime_error(message);
		++checks;
	}
	const std::vector<MeshShaderInput> inputs = {
		{VertexSemantic::Position, 0, 0, rhi::VertexFormat::Float3},
		{VertexSemantic::Normal, 0, 1, rhi::VertexFormat::Float3}
	};
}

int main() {
	try {
		Mesh mesh = examples::CreateCubeMesh();
		std::string error;
		Check(mesh.RecalculateBounds(&error) && mesh.Validate(), "shared cube fixture is valid");
		Check(mesh.vertexCount == 24 && mesh.indices->IndexCount() == 36 && mesh.submeshes.size() == 1,
			"cube fixture has 24 face vertices and 36 indices");
		Check(mesh.localBounds.min.x == -1 && mesh.localBounds.min.y == -1 && mesh.localBounds.min.z == -1
			&& mesh.localBounds.max.x == 1 && mesh.localBounds.max.y == 1 && mesh.localBounds.max.z == 1,
			"cube fixture has expected local bounds");
		MeshDrawData data;
		Check(BuildMeshDrawData(mesh, 0, inputs, data, &error) && error.empty(), "cube shader input contract");
		Check(data.sourceBindings == std::vector<uint32_t>{7} && data.vertexBindings.size() == 1
			&& data.vertexBindings[0].binding == 0, "sparse source stream maps to dense GPU binding");
		Check(data.vertexAttributes[0].location == 0 && data.vertexAttributes[1].location == 1
			&& data.vertexAttributes[0].binding == 0 && data.vertexAttributes[1].binding == 0,
			"interleaved attributes share one GPU buffer");
		Check(data.indexType == rhi::IndexType::UInt32 && data.draw.kind == DrawSource::Direct
			&& data.draw.count == 36 && data.draw.first == 0 && data.draw.vertexOffset == 0
			&& data.draw.instanceCount == 1, "submesh translates into indexed draw parameters");

		Mesh separate = mesh;
		float normals[24][3] = {};
		for (auto& normal : normals) normal[1] = 1;
		VertexStreamData stream;
		stream.binding = 2;
		stream.bytes.resize(sizeof(normals));
		std::memcpy(stream.bytes.data(), normals, sizeof(normals));
		separate.vertexStreams.insert(separate.vertexStreams.begin(), std::move(stream));
		separate.layout.bindings.push_back({2, 12, false});
		separate.layout.attributes[1].binding = 2;
		separate.layout.attributes[1].offset = 0;
		auto reversed = inputs;
		std::swap(reversed[0], reversed[1]);
		Check(BuildMeshDrawData(separate, 0, reversed, data), "separate streams in arbitrary order");
		Check(data.sourceBindings == std::vector<uint32_t>({2, 7}) && data.vertexBindings[0].binding == 0
			&& data.vertexBindings[1].binding == 1 && data.vertexBindings[0].stride == 12,
			"upload order follows adapter output rather than source layout order");
		Check(data.vertexAttributes[0].location == 1 && data.vertexAttributes[0].binding == 0
			&& data.vertexAttributes[1].location == 0 && data.vertexAttributes[1].binding == 1,
			"shader locations remain stable while buffer bindings are remapped");
		Check(BuildMeshDrawData(separate, 0, {inputs[0]}, data) && data.sourceBindings == std::vector<uint32_t>{7},
			"depth-only technique can select only the position stream");

		auto Reject = [&](const Mesh& source, size_t submesh, const std::vector<MeshShaderInput>& contract,
			const char* message) {
			const auto previousBindings = data.sourceBindings;
			const auto previousCount = data.draw.count;
			error.clear();
			Check(!BuildMeshDrawData(source, submesh, contract, data, &error) && !error.empty()
				&& data.sourceBindings == previousBindings && data.draw.count == previousCount, message);
		};
		Reject(mesh, 1, inputs, "invalid submesh rejected without partial output");
		Reject(mesh, 0, {}, "missing shader contract rejected");
		auto invalidInputs = inputs;
		invalidInputs.push_back({VertexSemantic::TexCoord, 1, 2, rhi::VertexFormat::Float2});
		Reject(mesh, 0, invalidInputs, "missing UV channel is reported");
		invalidInputs = inputs;
		invalidInputs[0].format = rhi::VertexFormat::Float2;
		Reject(mesh, 0, invalidInputs, "shader format mismatch is reported");
		invalidInputs = inputs;
		invalidInputs[1].location = 0;
		Reject(mesh, 0, invalidInputs, "duplicate shader location rejected");
		invalidInputs = inputs;
		invalidInputs.push_back({VertexSemantic::Position, 0, 2, rhi::VertexFormat::Float3});
		Reject(mesh, 0, invalidInputs, "duplicate shader semantic rejected");
		Mesh invalid = mesh;
		invalid.vertexStreams[0].bytes.pop_back();
		Reject(invalid, 0, inputs, "broken source storage is rejected before adaptation");
		invalid = mesh;
		invalid.submeshes[0].localBounds.max.x = 0;
		Reject(invalid, 0, inputs, "stale geometry bounds rejected");

		Mesh mixed = mesh;
		Submesh direct;
		direct.range = VertexRange{3, 3};
		mixed.submeshes.push_back(direct);
		Check(mixed.RecalculateBounds() && BuildMeshDrawData(mixed, 1, inputs, data), "direct range in indexed mesh");
		Check(mixed.indices.has_value() && !data.indexType && data.draw.count == 3
			&& data.draw.first == 3 && data.draw.vertexOffset == 0,
			"non-indexed submesh must not attach an index stream");

		Mesh shifted = mesh;
		const uint16_t indices[] = {999, 1, 2, 3, 999};
		shifted.indices = IndexData{rhi::IndexType::UInt16, std::vector<std::byte>(sizeof(indices))};
		std::memcpy(shifted.indices->bytes.data(), indices, sizeof(indices));
		shifted.submeshes[0].range = IndexedRange{1, 3, -1};
		Check(shifted.RecalculateBounds() && BuildMeshDrawData(shifted, 0, inputs, data), "UInt16 range with negative base vertex");
		Check(data.indexType == rhi::IndexType::UInt16 && data.draw.first == 1
			&& data.draw.count == 3 && data.draw.vertexOffset == -1, "range units and signed offset are preserved");
		shifted.submeshes[0].topology = rhi::PrimitiveTopology::PatchList;
		Check(BuildMeshDrawData(shifted, 0, inputs, data) && data.topology == rhi::PrimitiveTopology::PatchList
			&& data.patchControlPoints == 3, "patch topology reaches pipeline preparation");
		error = "previous failure";
		Check(BuildMeshDrawData(mesh, 0, inputs, data, &error) && error.empty(), "success clears stale error");
		const int mappingChecks = checks;
		auto MakePipeline = [](const MeshDrawData& geometry) {
			PipelineDesc pipeline;
			pipeline.shaderRefs = {"cube.vert", "cube.frag"};
			pipeline.vertexBindings = geometry.vertexBindings;
			pipeline.vertexAttributes = geometry.vertexAttributes;
			pipeline.topology = geometry.topology;
			pipeline.tessPatchControlPoints = geometry.patchControlPoints;
			return pipeline;
		};
		const PipelineDesc pipeline = MakePipeline(data);
		error = "previous failure";
		Check(ValidateMeshPipeline(mesh, 0, inputs, pipeline, &error) && error.empty(),
			"actual pipeline matches mesh geometry and clears old error");
		auto RejectPipeline = [&](const PipelineDesc& candidate, const char* diagnostic, const char* message) {
			error.clear();
			Check(!ValidateMeshPipeline(mesh, 0, inputs, candidate, &error)
				&& error.find(diagnostic) != std::string::npos, message);
		};
		auto wrongPipeline = pipeline;
		wrongPipeline.vertexBindings[0].stride += 4;
		RejectPipeline(wrongPipeline, "stride", "reject wrong stride in the actual pipeline");
		wrongPipeline = pipeline;
		wrongPipeline.vertexBindings[0].stride = 0;
		RejectPipeline(wrongPipeline, "stride", "reject implicit packed stride");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes[0].offset += 4;
		RejectPipeline(wrongPipeline, "offset", "reject position byte offset mismatch");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes[0].format = rhi::VertexFormat::Float2;
		RejectPipeline(wrongPipeline, "format", "reject pipeline format mismatch despite correct shader contract");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes[0].binding = 7;
		RejectPipeline(wrongPipeline, "binding", "source binding is not the remapped GPU binding");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes[1].location = 9;
		RejectPipeline(wrongPipeline, "location", "reject location absent from input contract");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes[1].location = 0;
		RejectPipeline(wrongPipeline, "location", "reject duplicate pipeline locations");
		wrongPipeline = pipeline;
		wrongPipeline.vertexBindings[0].binding = 7;
		RejectPipeline(wrongPipeline, "binding", "reject unmapped pipeline buffer binding");
		wrongPipeline = pipeline;
		wrongPipeline.vertexBindings[0].perInstance = true;
		RejectPipeline(wrongPipeline, "binding", "do not silently treat vertex data as instance data");
		wrongPipeline = pipeline;
		wrongPipeline.topology = rhi::PrimitiveTopology::LineList;
		RejectPipeline(wrongPipeline, "topology", "reject topology incompatible with selected submesh");
		wrongPipeline = pipeline;
		wrongPipeline.vertexBindings.clear();
		wrongPipeline.vertexAttributes.clear();
		RejectPipeline(wrongPipeline, "vertexBindings", "require explicit layout rather than shader-derived packing");
		wrongPipeline = pipeline;
		wrongPipeline.vertexBindings.clear();
		RejectPipeline(wrongPipeline, "vertexBindings", "reject attribute-only layout");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes.clear();
		RejectPipeline(wrongPipeline, "vertexAttributes", "reject binding-only layout");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes.pop_back();
		RejectPipeline(wrongPipeline, "MeshPipeline", "pipeline must contain every contracted input");
		wrongPipeline = pipeline;
		wrongPipeline.vertexAttributes.push_back({2, 0, rhi::VertexFormat::Float3, 0});
		RejectPipeline(wrongPipeline, "MeshPipeline", "extra pipeline attribute must not bypass the contract");
		wrongPipeline = pipeline;
		wrongPipeline.vertexBindings.push_back({1, 16, true});
		RejectPipeline(wrongPipeline, "MeshPipeline", "external instance streams need a separate explicit interface");
		wrongPipeline = pipeline;
		wrongPipeline.cullMode = rhi::CullMode::Front;
		wrongPipeline.depthStencil.depthTestEnable = false;
		wrongPipeline.tessPatchControlPoints = 31;
		Check(ValidateMeshPipeline(mesh, 0, inputs, wrongPipeline),
			"appearance states and inactive patch size are not mesh geometry constraints");

		MeshDrawData multiple;
		Check(BuildMeshDrawData(separate, 0, reversed, multiple), "prepare multi-stream pipeline constraints");
		auto multiPipeline = MakePipeline(multiple);
		std::swap(multiPipeline.vertexBindings[0], multiPipeline.vertexBindings[1]);
		std::swap(multiPipeline.vertexAttributes[0], multiPipeline.vertexAttributes[1]);
		Check(ValidateMeshPipeline(separate, 0, reversed, multiPipeline),
			"pipeline vector order has no semantic meaning; IDs and locations do");
		multiPipeline.vertexBindings[1].binding = multiPipeline.vertexBindings[0].binding;
		Check(!ValidateMeshPipeline(separate, 0, reversed, multiPipeline, &error),
			"reject duplicate pipeline buffer binding IDs");
		Check(!ValidateMeshPipeline(mesh, 2, inputs, pipeline, &error), "revalidate selected mesh range");
		invalid = mesh;
		invalid.vertexStreams[0].bytes.pop_back();
		Check(!ValidateMeshPipeline(invalid, 0, inputs, pipeline, &error), "revalidate actual mesh storage");
		invalid = mesh;
		invalid.layout.attributes.pop_back();
		Check(!ValidateMeshPipeline(invalid, 0, inputs, pipeline, &error), "reject required semantic missing from source mesh");
		Check(BuildMeshDrawData(separate, 0, {inputs[0]}, multiple)
			&& ValidateMeshPipeline(separate, 0, {inputs[0]}, MakePipeline(multiple)),
			"a position-only technique can ignore unused mesh channels");
		Check(BuildMeshDrawData(shifted, 0, inputs, multiple)
			&& ValidateMeshPipeline(shifted, 0, inputs, MakePipeline(multiple)),
			"matching patch topology and control point count");
		auto patchPipeline = MakePipeline(multiple);
		patchPipeline.tessPatchControlPoints = 4;
		Check(!ValidateMeshPipeline(shifted, 0, inputs, patchPipeline, &error)
			&& error.find("tessPatchControlPoints") != std::string::npos,
			"reject pipeline patch control point mismatch");
		Check(BuildMeshDrawData(mixed, 1, inputs, multiple)
			&& ValidateMeshPipeline(mixed, 1, inputs, MakePipeline(multiple)),
			"pipeline geometry constraint also supports non-indexed submeshes");
		std::cout << "Mesh draw checks passed: " << mappingChecks << '\n';
		std::cout << "Mesh pipeline checks passed: " << checks - mappingChecks << '\n';
		return 0;
	}
	catch (const std::exception& error) {
		std::cerr << "Mesh draw check failed: " << error.what() << '\n';
		return 1;
	}
}
