#include "RenderGraphNodes.h"

using namespace dEngine;

std::unique_ptr<Pass> Pass::AutoAllocator(PassTopology&& topo, Type newType, PassDesc desc) {
	switch (newType) {
	case Pass::Render:
		if (auto* d = std::get_if<RenderPassDesc>(&desc))
			return std::make_unique<RenderPass>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		break;
	case Pass::Compute:
		if (!std::holds_alternative<std::monostate>(desc))
			RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		return std::make_unique<ComputePass>(std::move(topo));
	default:
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeUnspecified);
		break;
	}
	return nullptr;
}

std::unique_ptr<Resource> Resource::AutoAllocator(ResourceTopology&& topo, Type type, ResourceDesc desc) {
	switch (type) {
	case Resource::Image:
		if (auto* d = std::get_if<ImageDesc>(&desc))
			return std::make_unique<dEngine::Image>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	default:
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	}
	return nullptr;
}
