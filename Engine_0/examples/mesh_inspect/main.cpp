#include "MeshDrawData.h"
#include "../cube_reflection/CubeMesh.h"

#include <iostream>

int main() {
	using namespace dEngine;
	Mesh mesh = examples::CreateCubeMesh();
	std::string error;
	if (!mesh.RecalculateBounds(&error)) {
		std::cerr << error << '\n';
		return 1;
	}
	MeshDrawData render;
	const std::vector<MeshShaderInput> inputs = {
		{VertexSemantic::Position, 0, 0, rhi::VertexFormat::Float3},
		{VertexSemantic::Normal, 0, 1, rhi::VertexFormat::Float3}
	};
	if (!BuildMeshDrawData(mesh, 0, inputs, render, &error)) {
		std::cerr << error << '\n';
		return 1;
	}
	PipelineDesc pipeline;
	pipeline.shaderRefs = {"cube.vert", "cube.frag"};
	pipeline.vertexBindings = render.vertexBindings;
	pipeline.vertexAttributes = render.vertexAttributes;
	pipeline.topology = render.topology;
	pipeline.tessPatchControlPoints = render.patchControlPoints;
	if (!ValidateMeshPipeline(mesh, 0, inputs, pipeline, &error)) {
		std::cerr << error << '\n';
		return 1;
	}
	std::cout << "Mesh: " << mesh.name << "\nvertices=" << mesh.vertexCount
		<< " indices=" << mesh.indices->IndexCount() << " submeshes=" << mesh.submeshes.size()
		<< "\nbounds.min=(" << mesh.localBounds.min.x << ',' << mesh.localBounds.min.y << ',' << mesh.localBounds.min.z
		<< ") bounds.max=(" << mesh.localBounds.max.x << ',' << mesh.localBounds.max.y << ',' << mesh.localBounds.max.z << ")\n";
	for (size_t i = 0; i < inputs.size(); ++i) {
		const auto& attribute = render.vertexAttributes[i];
		const auto* source = mesh.FindAttribute(inputs[i].semantic, inputs[i].semanticIndex);
		std::cout << (inputs[i].semantic == VertexSemantic::Position ? "Position0" : "Normal0")
			<< " sourceBinding=" << source->binding << " -> gpuBinding=" << attribute.binding
			<< " location=" << attribute.location << " offset=" << attribute.offset << '\n';
	}
	std::cout << "draw=DrawIndexed count=" << render.draw.count << " first=" << render.draw.first
		<< " baseVertex=" << render.draw.vertexOffset << " materialSlot=" << mesh.submeshes[0].materialSlot
		<< "\nValidate=PASS\nPipelineCompatible=PASS\n";
	return 0;
}
