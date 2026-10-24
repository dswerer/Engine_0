#pragma once
#include "RenderGraphDesc.h"
#include "rhi/RHI.h"
#include <vector>
#include <string>
#include <set>
#include <map>
#include <memory>
#include <optional>
#include <cstdint>

// 渲染图抽象层：Resource / Pass 基类与 RenderGraph 本体。
namespace dEngine {
	class RenderGraph;

	// Pass 的图拓扑：节点身份 + 读写边（use = 资源 × usage）。
	// usage 为 ProcessRegister 从 Desc 回填的渲染语义，是 barrier compiler 的输入
	struct PassTopology {
		uint32_t id = static_cast<uint32_t>(-1);
		std::string name;
		std::vector<ResourceUse> reads;   // 读边：资源 id + 读用途
		std::vector<ResourceUse> writes;  // 写边：资源 id + 写用途
	};

	// Resource 的图拓扑：节点身份 + 生产者/消费者 + 生命周期标志。
	// reader 侧 usage 在 reader 的 PassTopology 里，此处不冗余存储
	struct ResourceTopology {
		uint32_t id = static_cast<uint32_t>(-1);
		std::string name;
		std::set<uint32_t> readers;                   // 消费者 pass id 集
		uint32_t writer = static_cast<uint32_t>(-1);  // -1 表示无 writer
		bool crossFrame = false;                      // 是否跨帧存活（true=持久资源，false=仅帧内）
		bool external = false;                        // 该资源来源于外部，生命周期由外部管理
	};

	// 抽象基类：节点只承载拓扑，具体形态由子类定义；None 不再是合法节点类型（注册期 type 必填）
	class Resource {
	public:
		friend RenderGraph;
		enum Type {
			None,
			Image,
			Buffer
		};
		virtual ~Resource() = default;
		bool operator< (const Resource& another) const { return this->m_topo.id < another.m_topo.id; }
		virtual Type get_type() const = 0;
		uint32_t get_id() const { return m_topo.id; };
		const std::string& get_name() const { return m_topo.name; }
		const ResourceTopology& get_topology() const { return m_topo; }
		// 类型工厂：external 非空走 Borrow（不创建、不拥有），否则按 desc 走 Own
		static std::unique_ptr<Resource> AutoAllocator(ResourceTopology&& topo, Type type, ResourceDesc desc,
			ImageSubresource view = {}, const rhi::Image* external = nullptr);
	protected:
		explicit Resource(ResourceTopology&& topo) : m_topo(std::move(topo)) {}
		ResourceTopology m_topo;
	};
	class Pass {
	public:
		friend RenderGraph;
		enum Type {
			None,
			Render,
			Compute
		};
		virtual ~Pass() = default;
		bool operator<(const Pass& another) const { return this->m_topo.id < another.m_topo.id; }
		virtual Type get_type() const = 0;
		uint32_t get_id() const { return m_topo.id; };
		const std::string& get_name() const { return m_topo.name; }
		const PassTopology& get_topology() const { return m_topo; }
		static std::unique_ptr<Pass> AutoAllocator(PassTopology&& topo, Type newType, PassDesc desc);
	protected:
		explicit Pass(PassTopology&& topo) : m_topo(std::move(topo)) {}
		PassTopology m_topo;
	};
	class RenderGraph {
	public:
		enum class ErrorCode {
			None = 0,
			EmptyResourceName,
			ResourceNotRegistered,
			InvalidAttachmentUsage,
			InvalidShaderUsage,
			ColorSlotOutOfRange,
			DuplicateColorSlot,
			MultipleDepthAttachments,
			AttachmentWriteSetMismatch,
			ResolveSrcReadSetMismatch,
			ShaderReadSetMismatch,
			ShaderWriteSetMismatch,
			AttachmentShaderConflict,
			PassTypeUnspecified,
			ResourceTypeUnspecified,
			PassTypeDescMismatch,
			ResourceTypeDescMismatch,
			DeviceNotSet,
			ColorSlotNotDense,
			ShaderBindingTypeMismatch,
			ShaderBindingNameNotFound,
			ShaderVariableUnbound,
			PipelineCreationFailed,
			DescriptorSetAllocationFailed
		};
		RenderGraph() :pass_top_id(-1), resource_top_id(-1), m_graphicDevice(nullptr) {}
		RenderGraph(rhi::Device* device):pass_top_id(-1),resource_top_id(-1),m_graphicDevice(device) {}
		// type 必填（抽象基类无降级路径），desc 可缺省（如无描述的 Compute pass）
		void RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write,Pass::Type type,PassDesc desc={});
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer, Resource::Type type,ResourceDesc desc={});
		// 借用外部已创建的原生 image 作为图资源节点（交换链、外部导入、同一 image 的多视图别名）：
		// 节点不创建、不拥有、不参与 usage 汇总，底层句柄与生命周期由外部负责
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer,
			const rhi::Image* external, ImageSubresource view = {});
		void ProcessRegister();
		void GenerateSubmitSequence();
		
		//日后可能会传递其它类型的device
		void SetDevice(rhi::Device* graphic);
		void RealizeResource(Resource* resource);//识别node类型并创建rhi实例（幂等：重复调用跳过）
		void RealizePass(Pass* pass);
		void RealizeAllResources();
		void RealizeAllPasses();
		void RealizeAll();

		// 节点池访问（id 越界返回 nullptr；实例化产物由节点自身持有，Image::get_rhi / RenderPass::get_rhi 取用）
		Resource* GetResource(uint32_t resourceId) const;
		Pass*     GetPass(uint32_t passId) const;

		void PrintGraph();

		// 统一错误处理：打印到 cerr 并结束进程（public static：注册路径与节点类型工厂共用）
		[[noreturn]] static void HandleError(ErrorCode code);
	private:
		// 解析 pass 的 RenderPassDesc：回填 resourceId、Auto 推断 usage，再依次校验
		void ResolvePassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			RenderPassDesc& desc);
		// 校验附件：usage 合法性、颜色 slot、深度唯一性、与 read/write 集一致性
		void ValidateAttachments(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const RenderPassDesc& desc);
		// 校验 shader 绑定：usage 合法性、与 read/write 集一致性、set/binding 去重
		void ValidateShaderBindings(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const RenderPassDesc& desc);
		// 校验同一资源不同时作为附件与 shader 绑定
		void ValidateBindingConflict(const RenderPassDesc& desc);
		struct reg_tmp_pass {
			uint32_t id;
			//std::string name;
			std::set<std::string> to_read_name;
			std::set<std::string> to_write_name;
			Pass::Type type;
			PassDesc desc;
		};
		struct reg_tmp_resource {
			uint32_t id;
			//std::string name;
			std::set<std::string> readers_name;
			std::string writer_name;
			Resource::Type type;
			ResourceDesc desc;
			ImageSubresource  view {};                  // 节点代表的子资源视图
			const rhi::Image* externalImage = nullptr;   // 非空 = 借用外部句柄
		};
		uint32_t pass_top_id;
		uint32_t resource_top_id;
		std::vector<std::unique_ptr<Pass>> passPool;
		std::vector<std::unique_ptr<Resource>> resourcePool;

		std::vector<uint32_t> submitSeq;

		// 全图扫描：该资源被各 pass 以何种 usage 触及，并集为创建位（图的职责，用户不必手填 usage）
		rhi::ImageUsage DeriveImageUsage(uint32_t resourceId) const;


		std::optional<std::map<std::string, reg_tmp_pass>> tmp_passes;
		std::optional<std::map<std::string, reg_tmp_resource>> tmp_resources;

		rhi::Device* m_graphicDevice;
	};
}
