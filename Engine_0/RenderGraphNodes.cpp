#include "RenderGraphNodes.h"

#include <cstring>

using namespace dEngine;

void RenderPass::SetPushConstants(const void* data, uint32_t size) {
	mPushConstants.resize(size);
	if (size > 0 && data != nullptr)
		std::memcpy(mPushConstants.data(), data, size);
}

void ComputePass::SetPushConstants(const void* data, uint32_t size) {
	mPushConstants.resize(size);
	if (size > 0 && data != nullptr)
		std::memcpy(mPushConstants.data(), data, size);
}

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
	case Pass::Mux:
		if (auto* d = std::get_if<MuxPassDesc>(&desc))
			return std::make_unique<MuxPass>(std::move(topo), std::move(*d));
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

Image::Image(ResourceTopology&& topo, const rhi::Image* external, ImageSubresource view,
	rhi::ImageFormat viewFormat, ImageSubresourceRange barrierRange)
	: Resource(std::move(topo)), mView(view), mViewFormat(viewFormat),
	  mBarrierRange(barrierRange), mExternal(external), mBorrowed(true) {
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

Image::Image(ResourceTopology&& topo, ImageDesc desc, bool borrowedLater, ImageSubresource view,
	rhi::ImageFormat viewFormat, ImageSubresourceRange barrierRange)
	: Resource(std::move(topo)), mDesc(std::move(desc)), mView(view), mViewFormat(viewFormat),
	  mBarrierRange(barrierRange), mBorrowed(borrowedLater) {
}

Buffer::Buffer(ResourceTopology&& topo, const rhi::Buffer* external)
	: Resource(std::move(topo)), mExternal(external), mBorrowed(true) {
	const rhi::BufferCreateInfo& d = external->Desc();
	mDesc.size = d.size;
	mDesc.usage = d.usage;
	mDesc.hostVisible = d.hostVisible;
}

std::unique_ptr<Resource> Resource::AutoAllocator(ResourceTopology&& topo, Type type, ResourceDesc desc,
	ImageSubresource view, const rhi::Image* external, const rhi::Buffer* externalBuffer,
	rhi::ImageFormat viewFormat, ImageSubresourceRange barrierRange, bool borrowedLater) {
	switch (type) {
	case Resource::Image:
		if (external != nullptr)
			return std::make_unique<dEngine::Image>(std::move(topo), external, view, viewFormat, barrierRange);
		if (borrowedLater) {
			if (auto* d = std::get_if<ImageDesc>(&desc))
				return std::make_unique<dEngine::Image>(std::move(topo), *d, true, view, viewFormat, barrierRange);
			RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
			break;
		}
		if (auto* d = std::get_if<ImageDesc>(&desc))
			return std::make_unique<dEngine::Image>(std::move(topo), std::move(*d), view, viewFormat, barrierRange);
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	case Resource::Buffer:
		if (externalBuffer != nullptr)
			return std::make_unique<dEngine::Buffer>(std::move(topo), externalBuffer);
		if (auto* d = std::get_if<BufferDesc>(&desc)) {
			if (borrowedLater)
				return std::make_unique<dEngine::Buffer>(std::move(topo), std::move(*d), true);
			return std::make_unique<dEngine::Buffer>(std::move(topo), std::move(*d));
		}
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	default:
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	}
	return nullptr;
}
