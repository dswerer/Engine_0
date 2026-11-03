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

// 渲染图抽象层：Resource / Pass 基类与 RenderGraph 本体。
namespace dEngine {
	class RenderGraph;
	// 图节点（定义见 RenderGraphNodes.h）：barrier 计划只保存节点身份，句柄在录制期解析
	class Image;
	class Buffer;

	// Host 段的执行上下文：Host 节点回调用它把业务代码夹在两条同步边界之间，
	// 并按图资源名取本段声明的缓冲句柄 / 图像指针（权限由句柄常性与集合归属双重约束）。
	// GPU→Host：WaitForGpu 等上游 timeline 值，随后按范围 Invalidate（设备写对主机可见）；
	// Host→GPU：SignalGpu 先 Flush（主机写对设备可见）再推进主机 timeline。
	// 图像不直接映射：图在段前/段后自动插入 staging 中转，这里返回的是中转 Buffer 的 CPU 指针
	// （mip0 全层紧打包）。可见性只覆盖图内声明的 Buffer 与中转：图外内存仍需自行 flush/invalidate
	class HostContext {
	public:
		// 上游同步目标（timeline 句柄 + 需要等到的值）；由提交线程在起回调前装配好
		struct Upstream {
			const rhi::Semaphore* timeline = nullptr;
			std::uint64_t         value = 0;
		};
		// 由提交线程装配的句柄表与可见性范围：read/write 按本段拓扑边分类
		// （同一资源可同时出现在两边）；invalidate = 读∪写，flush = 写
		struct Bindings {
			std::unordered_map<std::string, const rhi::Buffer*> read;
			std::unordered_map<std::string, rhi::Buffer*>       write;
			// 图像经隐式 staging 中转：键是逻辑图像名，值是该段的中转 Buffer（mip0 全层紧打包）
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
		// 图像读回指针（mip0 全层紧打包）：图在 Host 段之前自动插入 image -> staging 的拷贝，
		// WaitForGpu 已完成中转 Buffer 的 Invalidate。本段读集未声明该图像时返回 nullptr
		void* ReadImage(const std::string& name) {
			auto it = mBindings.readImages.find(name);
			if (it == mBindings.readImages.end() || it->second == nullptr)
				return nullptr;
			return const_cast<rhi::Buffer*>(it->second)->Map();
		}
		// 图像写回指针：Host 段之后图自动插入 staging -> image 的拷贝，SignalGpu 负责 Flush。
		// 本段写集未声明该图像时返回 nullptr；staging 与读回共用时即读改写语义
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
			// timeline 只给执行依赖：非一致内存须在读取前失效。
			// 写集也失效——避免部分写入时陈旧缓存行被回写、覆盖设备新写的数据
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
		friend RenderGraph; // 提交线程按批装配上下文
		HostContext(rhi::Semaphore* hostTimeline, std::uint64_t signalValue,
			std::vector<Upstream> upstream, Bindings bindings)
			: mHostTimeline(hostTimeline), mSignalValue(signalValue),
			  mUpstream(std::move(upstream)), mBindings(std::move(bindings)) {
		}

		rhi::Semaphore*       mHostTimeline = nullptr; // 空 = 设备无 timeline（退化为提交前同步执行）
		std::uint64_t         mSignalValue = 0;        // 本段在本帧主机 timeline 上的分配值
		std::vector<Upstream> mUpstream;
		Bindings              mBindings;
		bool                  mSigned = false;
	};

	// Pass 的图拓扑：节点身份 + 读写边（use = 资源 × usage）。
	// usage 为 ProcessRegister 从 Desc 回填的渲染语义，是 barrier compiler 的输入
	struct PassTopology {
		uint32_t id = static_cast<uint32_t>(-1);
		std::string name;
		std::vector<ResourceUse> reads;   // 读边：资源 id + 读用途
		std::vector<ResourceUse> writes;  // 写边：资源 id + 写用途
		rhi::QueueClass queue = rhi::QueueClass::Graphics; // 期望执行队列（Host 节点跟随其后首个 GPU pass）
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
		// ---- 手动分桶内存别名 ----
		// 宿主在注册期按自身寿命分桶并显式声明；图只校验、不做自动装箱。
		// 校验失败的组整组回退为非别名（aliasGroup=-1、memoryBlockId=自身 id），不终止进程。
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
			Transfer, // 拷贝通道（CopyBufferToImage）
			Present   // 呈现收尾：只做布局转换（PresentSrc），不录绘制命令
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
			TransferWriteSetMismatch,
			DispatchReadSetMismatch,
			PresentReadSetMismatch
		};
		RenderGraph() :pass_top_id(-1), resource_top_id(-1), m_graphicDevice(nullptr) {}
		RenderGraph(rhi::Device* device):pass_top_id(-1),resource_top_id(-1),m_graphicDevice(device) {}
		// type 必填（抽象基类无降级路径），desc 可缺省（如无描述的 Compute pass）；
		// queue 声明期望执行队列（缺省 Graphics），Host 节点无须声明（跟随其后的 GPU pass）
		void RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write,Pass::Type type,
			PassDesc desc={}, rhi::QueueClass queue = rhi::QueueClass::Graphics);
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
		// 手动分桶：把一组资源声明为同一内存块的别名候选（宿主自行按寿命分桶，图只做校验）。
		// 整组校验（同类型/描述一致/非借用/crossFrame 一致/批级寿命不重叠）失败时整组降级为非别名，
		// 打印原因后继续运行（不退出进程）。须在 RealizeAll（或 CompileBarriers）之前调用
		void AliasResources(std::vector<std::string> names);
		
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
		// 同一 passIndex 的图像/buffer barrier 合批；跨队列（不同批）额外拆出 release 与 acquire
		// 图侧 barrier 记录：只存节点身份与同步语义，不存后端句柄与子资源范围。
		// 录制期按节点解析（get_rhi / get_desc），借用图像换绑后计划依然有效，无需重编译
		struct GraphImageBarrier {
			const Image*         image = nullptr;
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

		// ---- 提交计划（依赖感知切分；批内 pass 顺序恒为图拓扑序的子序列）----
		// 切批规则：队列切换必切；此外在每个「有跨队列消费者」的生产 pass 之后切，让 signal 尽早发出。
		// 同一队列的相邻批靠提交顺序保证执行先后；跨队列依赖用 semaphore 点表达：生产批尾部 signal、
		// 消费批开头 wait。wait 掩码取消费侧 stage 并集（只挡真正读上游数据的阶段），空集才退回 AllCommands。
		// 跨批资源只在生产批尾发 release（old==new，不改布局），
		// 真正的布局转换与内存 acquire 放在消费 Pass 自己的 barrier 批里，避免多消费者重复转换。
		struct SyncPoint {
			uint32_t producerBatch = 0; // 批尾 signal
			uint32_t consumerBatch = 0; // 批首 wait
			// 消费侧使用上游资源的 stage 并集：semaphore wait 与 acquire srcStage 都用它，
			// 避免 AllCommands 把消费批里与上游无关的阶段也一起挡住
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
		// 录制单批：按 submitSeq 子序注入 Pass barrier + 录命令，末尾追加批尾 release
		void RecordBatch(rhi::CommandList& cmd, uint32_t batchIndex);

		// ---- 提交整图（Host 段异步执行）----
		// 按 SubmitPlan 逐批录制/提交；遇到批首的 Host 段则在独立线程执行回调（提交线程不阻塞），
		// 消费批在 GPU 侧 wait 该段的主机 timeline 值。返回时主机回调可能仍在跑：
		// 调用 WaitForFrame 之前先 WaitHostPasses（或下一帧 SubmitGraph 会自动收口）
		// 外部帧同步：acquireWait 挂到 Present 节点所在批的等待（binary，value=0），
		// presentReady 作为该批的提交完成信号（Present 无参时等它）
		struct FrameSync {
			const rhi::Semaphore* acquireWait = nullptr;  // AcquireNextImage 的 ready 信号量
			const rhi::Semaphore* presentReady = nullptr; // 渲染完成信号量（Swapchain::Frame::presentReady）
		};
		void SubmitGraph(const FrameSync& sync = FrameSync{});
		void WaitHostPasses();

		// 借用图像换绑：交换链重建后 backbuffer 指针会变，只更新节点句柄与描述。
		// barrier 计划只存节点身份，句柄在录制期解析，因此换绑不需要重编译计划（管线/描述符亦无需重建）
		void RetargetBorrowedImage(const std::string& name, const rhi::Image* external);

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
		// 解析 PresentPassDesc：只允许一条读边（presentable image），且不得出现写边
		void ResolvePresentPassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
			const PresentPassDesc& desc);
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
			rhi::QueueClass queue = rhi::QueueClass::Graphics;
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
		// 跨帧资源的帧首屏障（两套计划）：
		//   m_firstFrameHeads  首帧：资源初态（Own=Undefined / Borrow=声明的 initialLayout） -> 首次 GPU 使用
		//   m_crossFrameHeads  稳态：上帧末次使用 -> 本帧首次使用（闭环，兼做跨帧内存可见性）
		// 两套互斥，按 m_firstFramePending 选择；非跨帧资源的链首仍放在 m_barriers 里
		std::map<uint32_t, BarrierBatch> m_firstFrameHeads;
		std::map<uint32_t, BarrierBatch> m_crossFrameHeads;
		bool m_firstFramePending = true; // 尚未提交过任何一帧：首次录制用首帧计划
		SubmitPlan m_plan;                           // 批划分计划（RealizeAll 时编译）
		std::vector<uint32_t> m_passBatch;           // passId -> 所属批下标（跨批判定用）
		void BuildSubmitPlan();                      // submitSeq -> 批划分（依赖感知切分；Host 段置于批首）

		// ---- Host 段：编译期固化依赖结构，提交期分配值并装配上下文 ----
		struct HostPassSync {
			std::vector<uint32_t> gpuBatches; // 上游最近一次使用所在的 GPU 批（等该批的队列 timeline 值）
			std::vector<uint32_t> hostPasses; // 上游 Host 段（等主机 timeline 上它的推进值）
		};
		// Host 段图像能力：Host 读写 Image 时由图自动合成「host-visible staging Buffer + Transfer Pass」，
		// HostContext 只暴露 CPU 指针（ReadImage/WriteImage）。注册期调用（ProcessRegister 内）
		void SynthesizeHostImageStaging();
		// 合成节点注入的额外依赖边（from -> to）：资源 writer/readers 是单写者模型，
		// staging 的中转关系（T_read -> Host、Host -> T_write）需要显式补边
		std::vector<std::pair<uint32_t, uint32_t>> m_extraEdges;
		// Host 段的图像中转记录：逻辑图像名 -> staging 资源（HostContext 据此装配 CPU 指针）
		struct HostImageStaging {
			std::string   imageName;
			uint32_t      stagingId = 0;
			bool          read = false;
			bool          write = false;
		};
		std::map<uint32_t, std::vector<HostImageStaging>> m_hostImageStaging; // hostPassId -> 记录

		void BuildHostSync();                          // 批划分后：Host 段的上游依赖 + 各批要等的主机值来源
		HostContext MakeHostContext(uint32_t passId);  // 提交期：把上游依赖固化为等待目标（之后与提交线程无关）
		void LaunchHostWorkers(uint32_t batchIndex);   // 批首 Host 段 -> 一个 worker 按序执行（不阻塞提交线程）
		void RunHostPassesInline(uint32_t batchIndex); // 设备无 timeline 时的退化路径：提交前同步执行

		std::map<uint32_t, HostPassSync> m_hostSync;         // passId -> 上游依赖（跨帧不变）
		std::vector<std::vector<uint32_t>> m_batchHostPasses; // 批 -> 该批 GPU 提交要等的主机值来源（Host 段 id）
		std::vector<uint32_t> m_hostPassOrder;               // Host 段按 submitSeq 的顺序（值分配顺序）
		std::vector<bool> m_batchHasGpu;                     // 批内是否有 GPU 命令（纯 Host 批只起回调）
		// 批 -> 与所等 Host 段数据相交的 GPU 阶段并集（GPU 侧 wait 主机值的掩码；空集退回 AllCommands）
		std::vector<rhi::PipelineStage> m_batchHostStage;
		std::vector<std::uint64_t> m_batchTimelineValue;     // 批 -> 本次提交在该队列 GPU timeline 上的值
		std::vector<std::uint64_t> m_batchHostWait;          // 本帧填充：该批 GPU 提交要 wait 的主机值（0=不 wait）

		// 主机 timeline：句柄归图，值跨帧单调递增、绝不复位（timeline 值不可回退）
		std::unique_ptr<rhi::Semaphore> m_hostTimeline;
		std::uint64_t m_hostTimelineValue = 0;
		std::map<uint32_t, std::uint64_t> m_hostSignalValue; // passId -> 本帧分配值

		// 跨队列 binary 信号量（每帧新建；上一帧在 WaitForFrame 后已无在飞使用）
		std::vector<std::unique_ptr<rhi::Semaphore>> m_frameSemaphores;
		std::vector<std::thread> m_hostThreads;              // Host 段 worker（WaitHostPasses 收口）

		// 全图扫描：该内存块被各 pass 以何种 usage 触及，并集为创建位（图的职责，用户不必手填 usage）。
		// 参数是内存块 id：别名组内所有成员的访问都算在同一个创建位上
		rhi::ImageUsage DeriveImageUsage(uint32_t memoryBlockId) const;
		// 分桶解析与校验：按名解析成员、做静态描述检查与批级寿命检查，
		// 失败整组降级（打印原因，不退出）；在 RealizeAll / CompileBarriers 首次调用时执行一次
		void ResolveAliasGroups();

		// 手动分桶声明（AliasResources 追加；ResolveAliasGroups 消费）
		std::vector<std::vector<std::string>> m_aliasRequests;
		bool m_aliasResolved = false;


		std::optional<std::map<std::string, reg_tmp_pass>> tmp_passes;
		std::optional<std::map<std::string, reg_tmp_resource>> tmp_resources;

		rhi::Device* m_graphicDevice;
	};
}
