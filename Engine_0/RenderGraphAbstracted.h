#pragma once
#include "RenderGraphDesc.h"
#include "rhi/RHI.h"
#include <vector>
#include <string>
#include <set>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <cstdint>
#include <tuple>

// 渲染图抽象层：Resource / Pass 基类与 RenderGraph 本体。
namespace dEngine {
	class RenderGraph;
	// 图节点（定义见 RenderGraphNodes.h）：barrier 计划只保存节点身份，句柄在录制期解析
	class Image;
	class Buffer;

	// Host 段执行上下文：Host 节点回调的同步边界，图像经自动 staging 中转。
	class HostContext {
	public:
		// 上游同步目标（timeline 句柄 + 需要等到的值）；由提交线程在起回调前装配好
		struct Upstream {
			const rhi::Semaphore* timeline = nullptr;
			std::uint64_t         value = 0;
		};
		// 本段句柄表与可见性范围：read/write 按拓扑边分类，同一资源可同时出现。
		struct Bindings {
			std::unordered_map<std::string, const rhi::Buffer*> read;
			std::unordered_map<std::string, rhi::Buffer*>       write;
			// 图像经隐式 staging 中转：键是逻辑图像名，值是该段的中转 Buffer（节点 view 对应子资源紧打包）
			std::unordered_map<std::string, const rhi::Buffer*> readImages;
			std::unordered_map<std::string, rhi::Buffer*>       writeImages;
			std::vector<const rhi::Buffer*> invalidate;
			std::vector<const rhi::Buffer*> flush;
		};
		// 只读句柄：仅本段读集，未声明或非 Buffer 返回 nullptr；const 指针挡住误写
		const rhi::Buffer* ReadBuffer(const std::string& name) const {
			auto it = mBindings.read.find(name);
			return it != mBindings.read.end() ? it->second : nullptr;
		}
		// 可写句柄：仅本段写集（HostWrite 边），未声明返回 nullptr；拿到的是非 const 指针与可写 Map
		rhi::Buffer* WriteBuffer(const std::string& name) {
			auto it = mBindings.write.find(name);
			return it != mBindings.write.end() ? it->second : nullptr;
		}
		// 图像读回指针（节点 view 对应子资源紧打包）：仅本段读集，否则返回 nullptr。
		void* ReadImage(const std::string& name) {
			auto it = mBindings.readImages.find(name);
			if (it == mBindings.readImages.end() || it->second == nullptr)
				return nullptr;
			return const_cast<rhi::Buffer*>(it->second)->Map();
		}
		// 图像写回指针：仅本段写集，SignalGpu 负责 Flush，否则返回 nullptr。
		void* WriteImage(const std::string& name) {
			auto it = mBindings.writeImages.find(name);
			if (it == mBindings.writeImages.end() || it->second == nullptr)
				return nullptr;
			return it->second->Map();
		}
		// 等本段依赖的上游 GPU 批（及上游 Host 段）全部完成，再让设备写对主机可见
		void WaitForGpu() const {
			for (const auto& u : mUpstream) {
				if (u.timeline != nullptr && u.value != 0)
					u.timeline->Wait(u.value);
			}
			for (const rhi::Buffer* b : mBindings.invalidate) {
				if (b != nullptr) b->Invalidate();
			}
		}
		// 先把主机写回写到设备可见内存，再推进主机 timeline 值（次序不能反）；幂等
		void SignalGpu() {
			if (mSigned) return;
			for (const rhi::Buffer* b : mBindings.flush) {
				if (b != nullptr) b->Flush();
			}
			if (mHostTimeline != nullptr && mSignalValue != 0)
				mHostTimeline->Signal(mSignalValue);
			mSigned = true;
		}
		bool Signaled() const { return mSigned; }
	private:
		friend RenderGraph;
		HostContext(rhi::Semaphore* hostTimeline, std::uint64_t signalValue,
			std::vector<Upstream> upstream, Bindings bindings)
			: mHostTimeline(hostTimeline), mSignalValue(signalValue),
			  mUpstream(std::move(upstream)), mBindings(std::move(bindings)) {
		}

		rhi::Semaphore*       mHostTimeline = nullptr;
		std::uint64_t         mSignalValue = 0;
		std::vector<Upstream> mUpstream;
		Bindings              mBindings;
		bool                  mSigned = false;
	};

	// Pass 拓扑：节点身份与读写边（use = 资源 × usage）。
	struct PassTopology {
		uint32_t id = static_cast<uint32_t>(-1);
		std::string name;
		std::vector<ResourceUse> reads;   // 读边：资源 id + 读用途
		std::vector<ResourceUse> writes;  // 写边：资源 id + 写用途
		rhi::QueueClass queue = rhi::QueueClass::Graphics; // 期望执行队列（Host 节点跟随其后首个 GPU pass）
	};

	// Resource 拓扑：节点身份、生产者/消费者与生命周期标志。
	struct ResourceTopology {
		uint32_t id = static_cast<uint32_t>(-1);
		std::string name;
		std::set<uint32_t> readers;                   // 消费者 pass id 集
		uint32_t writer = static_cast<uint32_t>(-1);  // -1 表示无 writer
		bool crossFrame = false;                      // 是否跨帧存活（true=持久资源，false=仅帧内）
		bool external = false;                        // 该资源来源于外部，生命周期由外部管理
		// 图像首态：barrier compiler 的链首布局。借用资源可由外部显式声明，缺省 Undefined
		rhi::ImageLayout initialLayout = rhi::ImageLayout::Undefined;
		// 手动分桶别名：宿主按寿命声明，图只校验，失败整组回退不终止进程。
		uint32_t aliasGroup = static_cast<uint32_t>(-1);    // 别名组下标（-1 = 未分桶）
		uint32_t memoryBlockId = static_cast<uint32_t>(-1); // 所属内存块的代表资源 id（非别名为自身 id）
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
			const rhi::Buffer* externalBuffer = nullptr,
			rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			ImageSubresourceRange barrierRange = ImageSubresourceRange{ {}, 0, 0 },
			bool borrowedLater = false);
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
			Transfer, // 拷贝通道（CopyBufferToImage）
			Mux,      // 零命令选择点：输出是 Borrow-later，BeginFrame 时 retarget 到选中候选
			Present   // 呈现收尾：只做布局转换（PresentSrc），不录绘制命令
		};
		virtual ~Pass() = default;
		bool operator<(const Pass& another) const { return this->m_topo.id < another.m_topo.id; }
		virtual Type get_type() const = 0;
		uint32_t get_id() const { return m_topo.id; };
		const std::string& get_name() const { return m_topo.name; }
		const PassTopology& get_topology() const { return m_topo; }
		// 执行层开关：false 时 RecordPass 直接跳过（计划骨架/同步点/barrier 不变）
		bool is_active() const { return mActive; }
		void set_active(bool active) { mActive = active; }
		static std::unique_ptr<Pass> AutoAllocator(PassTopology&& topo, Type newType, PassDesc desc);
	protected:
		explicit Pass(PassTopology&& topo) : m_topo(std::move(topo)) {}
		PassTopology m_topo;
		bool mActive = true;
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
			TransferWriteSetMismatch,
			DispatchReadSetMismatch,
			PresentReadSetMismatch,
			StaticHeapUnsupported,
			StaticHeapCapacityExceeded,
			PassOrderUnknownName,
			PassOrderSelfReference,
			PassOrderCycle,
			PassNotRegistered,
			MuxPassInvalid,
			MuxOutputNotBorrowed,
			MuxCandidateDescMismatch,
			InvalidImageSubresourceRange,
			ViewOutsideBarrierRange,
			AccessRangeOutsideBarrierRange,
			AttachmentLayerCountMismatch
		};
		RenderGraph() :pass_top_id(-1), resource_top_id(-1), m_graphicDevice(nullptr) {}
		RenderGraph(rhi::Device* device):pass_top_id(-1),resource_top_id(-1),m_graphicDevice(device) {}
		// 显式顺序约束：after 须在本 Pass 之前，before 须在其之后，只增边不覆盖依赖。
		struct PassOrder {
			std::set<std::string> after;
			std::set<std::string> before;
		};
		// 注册 Pass：type 必填，desc 可缺省，queue 缺省按类型映射。
		void RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write,
			Pass::Type type, PassDesc desc = {}, PassOrder order = {},
			std::optional<rhi::QueueClass> queue = std::nullopt);
		// view/viewFormat 声明节点视图（默认首 mip 首层、原格式）；barrierRange 为同步范围（count=0 = 整图）
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer, Resource::Type type,ResourceDesc desc={},
			ImageSubresource view = {}, rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			ImageSubresourceRange barrierRange = ImageSubresourceRange{ {}, 0, 0 });
		// 借用外部原生 image：节点不创建、不拥有、不参与 usage 汇总，生命周期由外部负责。
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer,
			const rhi::Image* external, ImageSubresource view = {},
			rhi::ImageLayout initialLayout = rhi::ImageLayout::Undefined,
			rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			ImageSubresourceRange barrierRange = ImageSubresourceRange{ {}, 0, 0 },
			bool crossFrame = false);
		// 借用外部已创建的原生 buffer（顶点/索引/UBO 等由上传层或渲染器自管）：同 Borrow 语义
		void RegisterResource(std::string name, std::set<std::string> readers, std::string writer,
			const rhi::Buffer* external);
		// 注册静态纹理：固定 set 0/binding 0，图像须已为 ShaderReadOnly，槽位只写一次。
		uint32_t RegisterStaticTexture(const std::string& name, const rhi::Image& image,
			ImageSubresource view = {}, rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			const std::optional<rhi::SamplerDesc>& sampler = std::nullopt);
		// 显式顺序约束通过 RegisterPass 的 PassOrder 参数声明
		void ProcessRegister();
		// 执行层开关：仅允许 Render/Compute，写资源不能有活跃读者，否则拒绝切换。
		void SetPassActive(const std::string& name, bool active);
		// 帧槽：BeginFrame 在录制当前帧命令之前调用，套用该槽的 mux 选择并刷新该槽描述符。
		void BeginFrame(uint32_t slot);
		uint32_t CurrentFrameSlot() const { return m_currentSlot; }
		uint32_t FrameSlotCount() const;
		// mux 输出资源：Borrow-later（图像或缓冲；desc 与候选严格一致）
		void RegisterMuxOutput(std::string name, std::set<std::string> readers, std::string writer,
			ResourceDesc desc);
		// 运行期 mux 选择：按帧槽保存，BeginFrame 时应用
		void SetMuxSelection(const std::string& muxPass, uint32_t slot, uint32_t index);
		void GenerateSubmitSequence();
		// 手动分桶声明：整组校验失败时降级为非别名，须在 RealizeAll 之前调用。
		void AliasResources(std::vector<std::string> names);
		
		void SetDevice(rhi::Device* graphic);
		void RealizeResource(Resource* resource);
		void RealizePass(Pass* pass);
		void RealizeAllResources();
		void RealizeAllPasses();
		void RealizeAll();

		// 录制单个 Pass：先注入其 barrier 批，再录命令。
		void RecordPass(rhi::CommandList& cmd, Pass* pass);
		// 按 submitSeq 顺序遍历全部 Pass：先发该 Pass 的 barrier 批，再录命令
		void RecordGraph(rhi::CommandList& cmd);

		// 图侧 barrier 记录：只存节点身份与同步语义，句柄在录制期解析。
		struct GraphImageBarrier {
			const Image*         image = nullptr;
			// 子资源分区路径显式填写；未设置时回落到节点 barrierRange
			std::optional<rhi::ImageSubresourceRange> range;
			rhi::ImageLayout     oldLayout = rhi::ImageLayout::Undefined;
			rhi::ImageLayout     newLayout = rhi::ImageLayout::Undefined;
			rhi::PipelineStage   srcStage = rhi::PipelineStage::None;
			rhi::PipelineStage   dstStage = rhi::PipelineStage::None;
			rhi::Access          srcAccess = rhi::Access::None;
			rhi::Access          dstAccess = rhi::Access::None;
			uint32_t             srcQueueFamily = rhi::kQueueFamilyIgnored;
			uint32_t             dstQueueFamily = rhi::kQueueFamilyIgnored;
		};
		struct GraphBufferBarrier {
			const Buffer*        buffer = nullptr;
			rhi::PipelineStage   srcStage = rhi::PipelineStage::None;
			rhi::PipelineStage   dstStage = rhi::PipelineStage::None;
			rhi::Access          srcAccess = rhi::Access::None;
			rhi::Access          dstAccess = rhi::Access::None;
			uint32_t             srcQueueFamily = rhi::kQueueFamilyIgnored;
			uint32_t             dstQueueFamily = rhi::kQueueFamilyIgnored;
		};
		struct BarrierBatch {
			std::vector<GraphImageBarrier> images;
			std::vector<GraphBufferBarrier> buffers;
		};
		void CompileBarriers();

		// 提交计划：按队列切批，跨队列依赖用 semaphore 表达。
		struct SyncPoint {
			uint32_t producerBatch = 0; // 批尾 signal
			uint32_t consumerBatch = 0; // 批首 wait
			// 消费侧相关阶段并集：供 semaphore wait 与 acquire 使用。
			rhi::PipelineStage consumerStage = rhi::PipelineStage::None;
		};
		struct QueueBatch {
			rhi::QueueClass queue = rhi::QueueClass::Graphics;
			uint32_t begin = 0;            // submitSeq 上的半开区间 [begin, end)
			uint32_t end = 0;
			BarrierBatch releases;         // 批尾跨队列 release（oldLayout==newLayout）
			std::vector<uint32_t> waits;   // syncPoints 下标（提交时按序 wait）
			std::vector<uint32_t> signals; // syncPoints 下标（批尾 signal）
		};
		struct SubmitPlan {
			std::vector<QueueBatch> batches;
			std::vector<SyncPoint> syncPoints;
		};
		const SubmitPlan& GetSubmitPlan() const { return m_plan; }
		// 当前拓扑排序结果（pass id 顺序）：用于诊断/测试顺序约束是否生效
		const std::vector<uint32_t>& GetSubmitSequence() const { return submitSeq; }
		// 录制单批：按 submitSeq 子序注入 Pass barrier + 录命令，末尾追加批尾 release
		void RecordBatch(rhi::CommandList& cmd, uint32_t batchIndex);

		// 提交整图：逐批提交，Host 段在独立线程执行，须先 WaitHostPasses 再等帧。
		struct FrameSync {
			const rhi::Semaphore* acquireWait = nullptr;  // AcquireNextImage 的 ready 信号量
			const rhi::Semaphore* presentReady = nullptr; // 渲染完成信号量（Swapchain::Frame::presentReady）
		};
		void SubmitGraph(const FrameSync& sync = FrameSync{});
		void WaitHostPasses();

		// 换绑借用图像：交换链重建后只更新节点句柄与描述，无需重编译计划。
		void RetargetBorrowedImage(const std::string& name, const rhi::Image* external);
		// 换绑借用缓冲：mux 选择或外部决策后更新句柄与描述。
		void RetargetBorrowedBuffer(const std::string& name, const rhi::Buffer* external);

		// 节点池访问（id 越界返回 nullptr；实例化产物由节点自身持有，Image::get_rhi / RenderPass::get_rhi 取用）
		Resource* GetResource(uint32_t resourceId) const;
		Pass*     GetPass(uint32_t passId) const;

		void PrintGraph();

		// 统一错误处理：打印到 cerr 并结束进程（public static：注册路径与节点类型工厂共用）
		[[noreturn]] static void HandleError(ErrorCode code);
	private:
		void ResolvePassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			RenderPassDesc& desc);
		void ResolveComputePassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			ComputePassDesc& desc);
		void ResolveTransferPassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			TransferPassDesc& desc);
		void ResolvePresentPassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const PresentPassDesc& desc);
		void RealizeShaderSets(rhi::Pipeline& pipeline, std::vector<ShaderBinding>& shaderBindings,
			std::vector<std::unique_ptr<rhi::DescriptorSet>>& sets, PassTopology& topo,
			uint32_t firstSetIndex = 0);
		rhi::Sampler* AcquireSampler(const rhi::SamplerDesc& desc);
		void ValidateAttachments(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const RenderPassDesc& desc);
		void ValidateShaderBindings(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const std::vector<ShaderBinding>& bindings);
		void ValidateDrawBindings(const std::set<uint32_t>& read, const RenderPassDesc& desc);
		void ValidateBindingConflict(const RenderPassDesc& desc);
		struct reg_tmp_pass {
			uint32_t id;
			std::set<std::string> to_read_name;
			std::set<std::string> to_write_name;
			Pass::Type type;
			rhi::QueueClass queue = rhi::QueueClass::Graphics;
			PassOrder order;
			PassDesc desc;
		};
		struct reg_tmp_resource {
			uint32_t id;
			std::set<std::string> readers_name;
			std::string writer_name;
			Resource::Type type;
			ResourceDesc desc;
			ImageSubresource  view {};
			rhi::ImageFormat  viewFormat = rhi::ImageFormat::None;
			ImageSubresourceRange barrierRange { {}, 0, 0 };
			const rhi::Image* externalImage = nullptr;
			const rhi::Buffer* externalBuffer = nullptr;
			rhi::ImageLayout  initialLayout = rhi::ImageLayout::Undefined;
			bool              borrowedLater = false;
			bool              crossFrame = false;
		};
		uint32_t pass_top_id;
		uint32_t resource_top_id;
		std::vector<std::unique_ptr<Pass>> passPool;
		std::vector<std::unique_ptr<Resource>> resourcePool;

		struct SamplerKey {
			rhi::Filter             magFilter, minFilter;
			rhi::SamplerMipmapMode  mipmapMode;
			rhi::SamplerAddressMode addressU, addressV, addressW;
			bool                    anisotropyEnable;
			float                   maxAnisotropy, mipLodBias, minLod, maxLod;
			bool operator<(const SamplerKey& o) const {
				return std::tie(magFilter, minFilter, mipmapMode, addressU, addressV, addressW,
					anisotropyEnable, maxAnisotropy, mipLodBias, minLod, maxLod)
					< std::tie(o.magFilter, o.minFilter, o.mipmapMode, o.addressU, o.addressV, o.addressW,
						o.anisotropyEnable, o.maxAnisotropy, o.mipLodBias, o.minLod, o.maxLod);
			}
		};
		std::map<SamplerKey, std::unique_ptr<rhi::Sampler>> m_samplers;

		std::unique_ptr<rhi::DescriptorSetLayout> m_staticHeapLayout;
		std::unique_ptr<rhi::DescriptorSet>       m_staticHeapSet;
		uint32_t                                  m_staticHeapNextSlot = 0;
		std::map<std::string, uint32_t>           m_staticTextureSlots;

		std::vector<uint32_t> submitSeq;
		std::map<uint32_t, BarrierBatch> m_barriers;
		std::map<uint32_t, BarrierBatch> m_firstFrameHeads;
		std::map<uint32_t, BarrierBatch> m_crossFrameHeads;
		bool m_firstFramePending = true;
		SubmitPlan m_plan;
		std::vector<uint32_t> m_passBatch;

		struct FrameSlotState {
			std::vector<uint32_t> muxSelections;
			std::vector<uint32_t> appliedSelections;
			std::vector<std::vector<std::unique_ptr<rhi::DescriptorSet>>> passSets;
			std::vector<std::unique_ptr<rhi::Semaphore>> crossQueueSemaphores;
			uint64_t frameSerial = 0;
		};
		std::vector<FrameSlotState> m_frameSlots;
		uint32_t m_currentSlot = 0;
		void EnsureFrameSlots(uint32_t count);
		struct MuxEntry {
			uint32_t passId = 0;
			uint32_t outputResId = 0;
			Resource::Type outputType = Resource::Image;
			std::vector<uint32_t> alternativeIds;
		};
		std::vector<MuxEntry> m_muxes;
		bool ApplyMuxSelection(uint32_t slot);
		void RefreshSlotDescriptorSets(uint32_t slot);
		// P2：编译完成后按屏障解析出的布局刷新所有帧槽的描述符
		void RefreshAllSlotDescriptorSets();
		// (passId, resourceId) -> 该 Pass 的目标布局
		std::map<std::pair<uint32_t, uint32_t>, rhi::ImageLayout> m_passImageLayouts;
		void BuildSubmitPlan();
		// 解析每个 Pass 对图像实际触达的子资源范围
		void ResolveAccessRanges();
		// 打印访问范围与节点 barrierRange 的差异
		void ReportAccessRangeMismatches() const;
		// P1：按子资源分区编译
		void CompileBarriersSubresource();

		struct ImageAccessUse {
			uint32_t passId = 0;
			uint32_t resourceId = 0;
			uint32_t blockId = 0;
			rhi::ImageSubresourceRange range {};
		};
		std::vector<ImageAccessUse> m_imageAccessRanges;

		struct HostPassSync {
			std::vector<uint32_t> gpuBatches;
			std::vector<uint32_t> hostPasses;
		};
		void SynthesizeHostImageStaging();
		std::vector<std::pair<uint32_t, uint32_t>> m_extraEdges;
		std::vector<std::pair<uint32_t, uint32_t>> m_orderEdges;
		struct HostImageStaging {
			std::string   imageName;
			uint32_t      stagingId = 0;
			bool          read = false;
			bool          write = false;
		};
		std::map<uint32_t, std::vector<HostImageStaging>> m_hostImageStaging;

		void BuildHostSync();
		HostContext MakeHostContext(uint32_t passId);
		void LaunchHostWorkers(uint32_t batchIndex);
		void RunHostPassesInline(uint32_t batchIndex);

		std::map<uint32_t, HostPassSync> m_hostSync;
		std::vector<std::vector<uint32_t>> m_batchHostPasses;
		std::vector<uint32_t> m_hostPassOrder;
		std::vector<bool> m_batchHasGpu;
		std::vector<rhi::PipelineStage> m_batchHostStage;
		std::vector<std::uint64_t> m_batchTimelineValue;
		std::vector<std::uint64_t> m_batchHostWait;

		std::unique_ptr<rhi::Semaphore> m_hostTimeline;
		std::uint64_t m_hostTimelineValue = 0;
		std::map<uint32_t, std::uint64_t> m_hostSignalValue;

		std::vector<std::thread> m_hostThreads;

		rhi::ImageUsage DeriveImageUsage(uint32_t memoryBlockId) const;
		void ResolveAliasGroups();

		std::vector<std::vector<std::string>> m_aliasRequests;
		bool m_aliasResolved = false;


		std::optional<std::map<std::string, reg_tmp_pass>> tmp_passes;
		std::optional<std::map<std::string, reg_tmp_resource>> tmp_resources;

		rhi::Device* m_graphicDevice;
	};
}
