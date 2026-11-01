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
		// 图像首态：barrier compiler 的链首布局。借用资源可由外部显式声明，缺省 Undefined
		rhi::ImageLayout initialLayout = rhi::ImageLayout::Undefined;
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
		// 类型工厂：external/externalBuffer 非空走 Borrow（不创建、不拥有），否则按 desc 走 Own
		static std::unique_ptr<Resource> AutoAllocator(ResourceTopology&& topo, Type type, ResourceDesc desc,
			ImageSubresource view = {}, const rhi::Image* external = nullptr,
			const rhi::Buffer* externalBuffer = nullptr);
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
			Compute,
			Host,     // 纯 CPU 回调，不录命令
			Transfer  // 拷贝通道（CopyBufferToImage）
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
			DrawReadSetMismatch,
			DrawStreamCountMismatch,
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
			DescriptorSetAllocationFailed,
			TransferBindingTypeMismatch,
			TransferReadSetMismatch,
			TransferWriteSetMismatch
		};
		RenderGraph() :pass_top_id(-1), resource_top_id(-1), m_graphicDevice(nullptr) {}
		RenderGraph(rhi::Device* device):pass_top_id(-1),resource_top_id(-1),m_graphicDevice(device) {}
		// type 必填（抽象基类无降级路径），desc 可缺省（如无描述的 Compute pass）
		void RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write,Pass::Type type,PassDesc desc={});
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer, Resource::Type type,ResourceDesc desc={});
		// 借用外部已创建的原生 image 作为图资源节点（交换链、外部导入、同一 image 的多视图别名）：
		// 节点不创建、不拥有、不参与 usage 汇总，底层句柄与生命周期由外部负责；
		// initialLayout 声明图外传入时的既有布局（缺省 Undefined，compiler 据此推导链首 barrier）
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer,
			const rhi::Image* external, ImageSubresource view = {},
			rhi::ImageLayout initialLayout = rhi::ImageLayout::Undefined);
		// 借用外部已创建的原生 buffer（顶点/索引/UBO 等由上传层或渲染器自管）：同 Borrow 语义
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer,
			const rhi::Buffer* external);
		void ProcessRegister();
		void GenerateSubmitSequence();
		
		//日后可能会传递其它类型的device
		void SetDevice(rhi::Device* graphic);
		void RealizeResource(Resource* resource);//识别node类型并创建rhi实例（幂等：重复调用跳过）
		void RealizePass(Pass* pass);
		void RealizeAllResources();
		void RealizeAllPasses();
		void RealizeAll();

		// 录制分派：按节点类型把已实例化的 Pass 录进命令列表（与 RealizePass 同型，图侧 switch）。
		// barrier 由 CompileBarriers 预先推导，RecordGraph 在每个 Pass 之前注入
		void RecordPass(rhi::CommandList& cmd, Pass* pass);
		// 按 submitSeq 顺序遍历全部 Pass：先发该 Pass 的 barrier 批，再录命令
		void RecordGraph(rhi::CommandList& cmd);

		// ---- barrier compiler（纯函数推导，无状态机）----
		// 每个资源按 submitSeq 收集使用链 (passIndex, usage, isWrite)，相邻 prev/next 对产出 barrier：
		// 前驱写或后继写（或图像布局改变）即需要；链首用资源 initialLayout（缺省 Undefined）；
		// stage/access 由「Usage × Pass 类型」查表得出（每次使用都带，不存进拓扑）；
		// 同一 passIndex 的图像/buffer barrier 合批，跨域（跨队列）同步本轮不做
		struct BarrierBatch {
			std::vector<rhi::ImageBarrier> images;
			std::vector<rhi::BufferBarrier> buffers;
		};
		void CompileBarriers();

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
		// 解析 pass 的 ComputePassDesc：回填 shaderBindings 的 resourceId，再校验读/写集一致性
		void ResolveComputePassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			ComputePassDesc& desc);
		// 解析 pass 的 TransferPassDesc：回填两端 resourceId，校验资源类型与读/写集一致性
		void ResolveTransferPassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			TransferPassDesc& desc);
		// 反射对账 + 描述符集分配/写入（Render / Compute 共用）
		void RealizeShaderSets(rhi::Pipeline& pipeline, std::vector<ShaderBinding>& shaderBindings,
			std::vector<std::unique_ptr<rhi::DescriptorSet>>& sets);
		// 校验附件：usage 合法性、颜色 slot、深度唯一性、与 read/write 集一致性
		void ValidateAttachments(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const RenderPassDesc& desc);
		// 校验 shader 绑定：usage 合法性、与 read/write 集一致性、set/binding 去重
		void ValidateShaderBindings(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const std::vector<ShaderBinding>& bindings);
		// 校验顶点/索引流：资源须是 Buffer（ResolvePassDesc 已查）且出现在 read 集
		void ValidateDrawBindings(const std::set<uint32_t>& read, const RenderPassDesc& desc);
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
			const rhi::Image* externalImage = nullptr;   // 非空 = 借用外部 image 句柄
			const rhi::Buffer* externalBuffer = nullptr; // 非空 = 借用外部 buffer 句柄
			rhi::ImageLayout  initialLayout = rhi::ImageLayout::Undefined; // 图像首态（借用路径可声明）
		};
		uint32_t pass_top_id;
		uint32_t resource_top_id;
		std::vector<std::unique_ptr<Pass>> passPool;
		std::vector<std::unique_ptr<Resource>> resourcePool;

		std::vector<uint32_t> submitSeq;
		std::map<uint32_t, BarrierBatch> m_barriers; // passId -> 该 Pass 之前的 barrier 批

		// 全图扫描：该资源被各 pass 以何种 usage 触及，并集为创建位（图的职责，用户不必手填 usage）
		rhi::ImageUsage DeriveImageUsage(uint32_t resourceId) const;


		std::optional<std::map<std::string, reg_tmp_pass>> tmp_passes;
		std::optional<std::map<std::string, reg_tmp_resource>> tmp_resources;

		rhi::Device* m_graphicDevice;
	};
}
