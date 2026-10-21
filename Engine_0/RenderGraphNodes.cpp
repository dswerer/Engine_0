#include "RenderGraphNodes.h"

using namespace dEngine;

// 类型判别工厂：本头已含派生类完整定义；无基类降级路径（抽象基类不可实例化），
// type 与 desc 不一致统一 HandleError
std::unique_ptr<Pass> Pass::AutoAllocator(PassTopology&& topo, Type newType, PassDesc desc) {
	switch (newType) {
	case Pass::Render:
		if (auto* d = std::get_if<RenderPassDesc>(&desc))
			return std::make_unique<RenderPass>(std::move(topo), std::move(*d));
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		break; // 不可达：HandleError 结束进程
	case Pass::Compute:
		if (!std::holds_alternative<std::monostate>(desc))
			RenderGraph::HandleError(RenderGraph::ErrorCode::PassTypeDescMismatch);
		return std::make_unique<ComputePass>(std::move(topo));
	default: // None 已在注册期拦截，此处为防御分支
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
		break; // 不可达
	default: // Buffer 等类型节点尚未实现；None 已在注册期拦截
		RenderGraph::HandleError(RenderGraph::ErrorCode::ResourceTypeDescMismatch);
		break;
	}
	return nullptr;
}
