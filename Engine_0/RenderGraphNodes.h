#pragma once
#include "RenderGraphAbstracted.h"

namespace dEngine {
	class RenderPass : public Pass {
	public:
		RenderPass(PassTopology&& topo, RenderPassDesc desc)
			: Pass(std::move(topo)), mDesc(std::move(desc)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Render; }
		const RenderPassDesc& get_desc() const { return mDesc; }
	private:
		RenderPassDesc mDesc;
	};

	// 无描述的计算节点：读写边全部由拓扑承载（usage 暂为 Auto，compiler 阶段保守处理）
	class ComputePass : public Pass {
	public:
		explicit ComputePass(PassTopology&& topo)
			: Pass(std::move(topo)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Compute; }
	};

	class Image : public Resource {
	public:
		Image(ResourceTopology&& topo, ImageDesc desc)
			: Resource(std::move(topo)), mDesc(std::move(desc)) {
		}
		Resource::Type get_type() const override { return Resource::Type::Image; }
		const ImageDesc& get_desc() const { return mDesc; }
	private:
		ImageDesc mDesc;
	};
}
