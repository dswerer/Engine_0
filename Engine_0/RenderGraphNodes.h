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
		// 实例化产物（Realize 填充；execute 阶段经此取后端对象，未实例化为 nullptr）
		const rhi::Pipeline* get_rhi() const { return m_rhiHandle.get(); }
		// 描述符集（RealizePass 按反射的 set 顺序分配并写入；下标即 set 索引）
		const rhi::DescriptorSet* get_set(uint32_t set) const {
			return set < m_sets.size() ? m_sets[set].get() : nullptr;
		}
	private:
		friend RenderGraph; // 基类友元不覆盖派生类新增成员：Realize 需写入句柄
		RenderPassDesc mDesc;
		// 生命周期随节点 RAII：须保证 graph 先于 device 消亡（作用域嵌套）
		std::unique_ptr<rhi::Pipeline> m_rhiHandle;
		std::vector<std::unique_ptr<rhi::DescriptorSet>> m_sets;
	};

	// 无描述的计算节点：读写边全部由拓扑承载（usage 暂为 Auto，compiler 阶段保守处理）
	class ComputePass : public Pass {
	public:
		explicit ComputePass(PassTopology&& topo)
			: Pass(std::move(topo)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Compute; }
	};

	// 图像资源节点：一个节点 = 一个「视图身份」（名称即身份，绑定与对账都按名称）。
	// 底层原生 image 有两种来源：
	//   Own    — 由 ImageDesc 在 Realize 时创建，usage 由图按全图访问并集推导；
	//   Borrow — 包装外部已创建/外部管理的 rhi::Image（交换链、外部导入，以及
	//            「同一原生 image 出多个视图」的别名节点），不创建、不拥有、不参与 usage 汇总
	class Image : public Resource {
	public:
		// Own：view 为本节点代表的子资源视图（默认首 mip 首层）
		Image(ResourceTopology&& topo, ImageDesc desc, ImageSubresource view)
			: Resource(std::move(topo)), mDesc(std::move(desc)), mView(view) {
		}
		// Borrow：不拥有底层 image，描述信息由外部 image 回填
		Image(ResourceTopology&& topo, const rhi::Image* external, ImageSubresource view);

		Resource::Type get_type() const override { return Resource::Type::Image; }
		const ImageDesc& get_desc() const { return mDesc; }
		// 本节点代表的子资源视图：绑定时作为起点，数组绑定按 layer 递增展开
		ImageSubresource get_view() const { return mView; }
		bool is_borrowed() const { return mExternal != nullptr; }
		// 实例化产物（Own 为自建句柄；Borrow 直接返回外部句柄；未实例化为 nullptr）
		const rhi::Image* get_rhi() const { return mExternal ? mExternal : m_rhiHandle.get(); }
	private:
		friend RenderGraph; // 基类友元不覆盖派生类新增成员：Realize 需写入句柄
		ImageDesc mDesc;
		ImageSubresource mView {};
		// 生命周期随节点 RAII：须保证 graph 先于 device 消亡（作用域嵌套）
		std::unique_ptr<rhi::Image> m_rhiHandle;  // Own 路径
		const rhi::Image*          mExternal = nullptr; // Borrow 路径（非拥有）
	};
}
