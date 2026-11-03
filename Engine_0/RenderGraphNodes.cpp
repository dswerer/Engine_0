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
		if (auto* d = std::get_if<ComputePassDesc>(&desc))
			return std::make_unique<ComputePass>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		break;
	case Pass::Host:
		if (auto* d = std::get_if<HostPassDesc>(&desc))
			return std::make_unique<HostPass>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		break;
	case Pass::Transfer:
		if (auto* d = std::get_if<TransferPassDesc>(&desc))
			return std::make_unique<TransferPass>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		break;
	case Pass::Present:
		if (auto* d = std::get_if<PresentPassDesc>(&desc))
			return std::make_unique<PresentPass>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		break;
	default:
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeUnspecified);
		break;
	}
	return nullptr;
}

// Borrow 路径：不创建底层 image，描述信息从外部句柄回填（格式/尺寸仍供管线格式签名与校验使用）
Image::Image(ResourceTopology&& topo, const rhi::Image* external, ImageSubresource view)
	: Resource(std::move(topo)), mView(view), mExternal(external) {
	const rhi::ImageCreateInfo& d = external->Desc();
	mDesc.width = d.width;
	mDesc.height = d.height;
	mDesc.depth = d.depth;
	mDesc.arrayLayers = d.arrayLayers;
	mDesc.mipLevels = d.mipLevels;
	mDesc.samples = d.samples;
	mDesc.format = d.format;
	mDesc.viewType = d.viewType;
}

// Borrow 路径：不创建底层 buffer，描述信息从外部句柄回填
Buffer::Buffer(ResourceTopology&& topo, const rhi::Buffer* external)
	: Resource(std::move(topo)), mExternal(external) {
	const rhi::BufferCreateInfo& d = external->Desc();
	mDesc.size = d.size;
	mDesc.usage = d.usage;
	mDesc.hostVisible = d.hostVisible;
}

std::unique_ptr<Resource> Resource::AutoAllocator(ResourceTopology&& topo, Type type, ResourceDesc desc,
	ImageSubresource view, const rhi::Image* external, const rhi::Buffer* externalBuffer) {
	switch (type) {
	case Resource::Image:
		// 借用优先：外部句柄存在时不创建、不拥有，desc 可为 monostate
		if (external != nullptr)
			return std::make_unique<dEngine::Image>(std::move(topo), external, view);
		if (auto* d = std::get_if<ImageDesc>(&desc))
			return std::make_unique<dEngine::Image>(std::move(topo), std::move(*d), view);
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	case Resource::Buffer:
		if (externalBuffer != nullptr)
			return std::make_unique<dEngine::Buffer>(std::move(topo), externalBuffer);
		if (auto* d = std::get_if<BufferDesc>(&desc))
			return std::make_unique<dEngine::Buffer>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	default:
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	}
	return nullptr;
}
