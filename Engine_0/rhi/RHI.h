#pragma once
#include "RHIDesc.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// VMA 分配器句柄前置声明（VmaAllocator / VmaAllocation 实为 *_T*）；避免头文件引入 5k 行 vk_mem_alloc.h
struct VmaAllocator_T;
struct VmaAllocation_T;

// RHI 对象层：设备、资源、管线、描述符、命令列表、交换链。
namespace dEngine::rhi {

#pragma region Device // 设备创建

	struct DeviceDesc {
		void*    nativeWindowHandle = nullptr; // Win32 窗口句柄；为空则无 swapchain（离屏/测试）
		uint32_t width = 0, height = 0;
		bool     enableValidation = true;      // debug 下默认开
		uint32_t maxFramesInFlight = 2;        // 帧槽数量（创建期决定；内部钳制到 [1,8]）
	};

#pragma endregion

#pragma region Resource // RAII 资源对象（裸指针传给命令接口；生命周期由 Device 持有）

	// 视图：由 Image 持有（使用者经 Image::CreateView 显式注册并持引用），销毁由 RHI 统一负责
	class ImageView {
	public:
		ImageView(const ImageView&) = delete;
		ImageView& operator=(const ImageView&) = delete;
		~ImageView();

		VkImageView NativeHandle() const;
	private:
		friend class Image;
		ImageView(VkDevice device, VkImageView view);

		VkDevice    m_device = VK_NULL_HANDLE;
		VkImageView m_view = VK_NULL_HANDLE;
	};

	// 拥有 / 借用两种模式：借用的 Image 仅包装外部已存在的 VkImage（交换链 backbuffer、外部导入资源），
	// 析构时不销毁底层句柄；所有权语义通过 Image::Borrow 显式表达，构造函数不对外暴露。
	class Image {
	public:
		Image(const Image&) = delete;
		Image& operator=(const Image&) = delete;
		~Image();

		// 非拥有包装：由 Swapchain / 外部导入路径调用；format 为真实后端格式（含 sRGB 变体，避免视图格式错配）
		static std::unique_ptr<Image> Borrow(VkDevice device, VkImage image, VkFormat format,
			const ImageCreateInfo& desc);

		// 显式注册视图：按子资源去重，命中返回同一引用；视图由 Image 持有，随 Image 析构统一销毁
		const ImageView& CreateView(ImageSubresource sub) const;

		VkImage                NativeHandle() const;
		VkFormat               NativeFormat() const; // 真实后端格式（借用路径含 SRGB 变体）；barrier/clear 推 aspect 用
		const ImageCreateInfo& Desc() const;
		// 无状态查询接口：RHI 不跟踪 layout，由图的 state tracker 负责
	private:
		friend class Device;
		Image(VkDevice device, VkFormat format, VmaAllocator_T* allocator, VkImage image,
			VmaAllocation_T* allocation, const ImageCreateInfo& desc, bool borrowed);

		ImageCreateInfo  m_desc;
		VkDevice         m_device = VK_NULL_HANDLE;
		VkFormat         m_format = VK_FORMAT_UNDEFINED; // 真实后端格式（借用路径原样保留 SRGB 等）
		VkImage          m_image = VK_NULL_HANDLE;
		VmaAllocation_T* m_allocation = nullptr;
		VmaAllocator_T*  m_allocator = nullptr;
		bool             m_borrowed = false;

		// 已注册视图缓存（mutable：CreateView 为 const）；键为 (mipLevel << 32) | layer，
		// 用 node 容器保证返回的引用在后续注册时仍稳定
		mutable std::unordered_map<uint64_t, std::unique_ptr<ImageView>> m_views;
	};

	class Buffer {
	public:
		Buffer(const Buffer&) = delete;
		Buffer& operator=(const Buffer&) = delete;
		~Buffer();

		VkBuffer                NativeHandle() const;
		const BufferCreateInfo& Desc() const;
		void*                   Map();        // 仅 hostVisible 可用，持久映射，不提供 Unmap
		const void*             Map() const;  // 只读映射指针：写操作须用非 const 句柄

		// 非一致（缺 HOST_COHERENT）主机可见内存的显式可见性操作；尺寸为 0 表示到缓冲末尾。
		// Flush：主机写完之后、GPU 使用之前调用（把主机缓存回写到设备可见内存）；
		// Invalidate：GPU 写完之后、主机读取之前调用（丢弃主机缓存，让设备写对主机可见）。
		// 未映射（hostVisible == false）时为空操作；一致内存上由驱动退化为 no-op
		void Flush(std::uint64_t offset = 0, std::uint64_t size = 0) const;
		void Invalidate(std::uint64_t offset = 0, std::uint64_t size = 0) const;
	private:
		friend class Device;
		Buffer(VmaAllocator_T* allocator, VkBuffer buffer, VmaAllocation_T* allocation,
			const BufferCreateInfo& desc, void* mapped);

		BufferCreateInfo m_desc;
		VkBuffer         m_buffer = VK_NULL_HANDLE;
		VmaAllocation_T* m_allocation = nullptr;
		VmaAllocator_T*  m_allocator = nullptr;
		void*            m_mapped = nullptr;
	};

#pragma endregion

#pragma region Descriptor // 描述符布局 / 集合（预编译路径）

	// 合并后的描述符槽位：声明本体 + 该槽位的 stageFlags 并集。
	// 反射/合并产物与布局构建之间用整条记录传递，不做平行数组（后端侧类型，不进入 Desc 层）
	struct MergedDescriptor {
		DescriptorBinding binding;
		VkShaderStageFlags stages = 0;
	};

	// 由 Pipeline 反射产出；持有 VkDescriptorSetLayout，生命周期随 Pipeline
	class DescriptorSetLayout {
	public:
		DescriptorSetLayout(const DescriptorSetLayout&) = delete;
		DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;
		~DescriptorSetLayout();

		VkDescriptorSetLayout NativeHandle() const;
		// 仅本 set 的绑定声明；分配 descriptor set 时用于回填各 binding 的 descriptor 类型
		const std::vector<DescriptorBinding>& Bindings() const;
	private:
		friend class Pipeline;
		friend class Device; // CreatePipeline 内构造各 set 的布局
		DescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
			std::vector<DescriptorBinding> bindings);

		VkDevice                       m_device = VK_NULL_HANDLE;
		VkDescriptorSetLayout          m_layout = VK_NULL_HANDLE;
		std::vector<DescriptorBinding> m_bindings;
	};

	// 由 Device 分配；写操作在录制前完成（预编译路径）
	class DescriptorSet {
	public:
		DescriptorSet(const DescriptorSet&) = delete;
		DescriptorSet& operator=(const DescriptorSet&) = delete;
		~DescriptorSet();

		VkDescriptorSet NativeHandle() const;
		// 图像绑定（数组绑定按 layer 递增逐层展开：第 i 个元素用 {view.mipLevel, view.layer + i}）。
		// 描述符类型取自分配时记录的 binding 表（SampledImage → COMBINED_IMAGE_SAMPLER，
		// StorageImage → STORAGE_IMAGE），据此选择采样器与 imageLayout
		void Write(uint32_t binding, const Image& image, ImageSubresource view, uint32_t count = 1);
		// 缓冲绑定（数组绑定按 range 步进：第 i 个元素 offset + i*range，count > 1 时 range 须非 0）
		void Write(uint32_t binding, const Buffer& buffer, uint64_t offset, uint64_t range, uint32_t count = 1);
	private:
		friend class Device;
		DescriptorSet(VkDevice device, VkDescriptorPool pool, VkSampler defaultSampler,
			VkDescriptorSet set, std::unordered_map<uint32_t, VkDescriptorType> bindingTypes);

		VkDevice         m_device = VK_NULL_HANDLE;
		VkDescriptorPool m_pool = VK_NULL_HANDLE;
		VkSampler        m_defaultSampler = VK_NULL_HANDLE; // RHI 内置默认采样器（combined image sampler 用）
		VkDescriptorSet  m_set = VK_NULL_HANDLE;
		// binding -> VkDescriptorType：写接口据此选择 uniform/storage 缓冲描述符类型
		std::unordered_map<uint32_t, VkDescriptorType> m_bindingTypes;
	};

#pragma endregion

#pragma region Pipeline // 管线对象

	class Pipeline {
	public:
		Pipeline(const Pipeline&) = delete;
		Pipeline& operator=(const Pipeline&) = delete;
		~Pipeline();

		VkPipeline                 NativeHandle() const;
		VkPipelineLayout           NativeLayout() const; // 供 CommandList 绑定描述符 / push constant
		VkPipelineBindPoint        NativeBindPoint() const; // 图形 / 计算（决定 vkCmdBindPipeline 与描述符绑定点）
		VkShaderStageFlags         NativeStageFlags() const; // 管线实际阶段集合（push constant 用）
		uint32_t                   PushConstantSize() const; // 0=管线未声明 push constant
		const DescriptorSetLayout* SetLayout(uint32_t setIndex) const; // 反射产出，供分配 descriptor set
	private:
		friend class Device;
		Pipeline(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout, uint32_t pushConstantSize,
			VkShaderStageFlags stageFlags, VkPipelineBindPoint bindPoint,
			std::vector<std::unique_ptr<DescriptorSetLayout>> setLayouts);

		VkDevice                                          m_device = VK_NULL_HANDLE;
		VkPipeline                                        m_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout                                  m_layout = VK_NULL_HANDLE;
		uint32_t                                          m_pushConstantSize = 0;
		VkShaderStageFlags                                m_stageFlags = 0;
		VkPipelineBindPoint                               m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		std::vector<std::unique_ptr<DescriptorSetLayout>> m_setLayouts; // 按 set index 排布，保证 SetLayout 返回地址稳定
	};

#pragma endregion

#pragma region Sync // 同步原语外壳

	// CPU-GPU 同步：Wait 阻塞至信号；Reset 仅对已信号的 fence 合法（Submit 挂起期间 Reset 是使用错误）
	class Fence {
	public:
		Fence(const Fence&) = delete;
		Fence& operator=(const Fence&) = delete;
		~Fence();

		void Wait();   // 阻塞至信号
		void Reset();
	private:
		friend class Device;
		Fence(VkDevice device, VkFence fence);

		VkDevice m_device = VK_NULL_HANDLE;
		VkFence  m_fence = VK_NULL_HANDLE;
	};

	class Semaphore {
	public:
		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;
		~Semaphore();

		VkSemaphore NativeHandle() const;
		bool IsTimeline() const { return m_timeline; }

		// 仅 timeline 语义量有效（binary 调用一律返回 false / 0）：
		// Signal = 主机侧推进（Host→GPU：GPU 侧用 SubmitInfo 的 wait value 等它）
		// Wait   = 主机侧等待（GPU→Host：等某个提交把值推进到 value）
		bool            Signal(std::uint64_t value);
		bool            Wait(std::uint64_t value, std::uint64_t timeoutNs = ~0ull) const;
		std::uint64_t   Value() const;
	private:
		friend class Device;
		friend class Swapchain;
		// timeline 语义量须按 VK_SEMAPHORE_TYPE_TIMELINE 创建，创建方在 Device 内
		Semaphore(VkDevice device, VkSemaphore semaphore, bool timeline = false);

		VkDevice    m_device = VK_NULL_HANDLE;
		VkSemaphore m_semaphore = VK_NULL_HANDLE;
		bool        m_timeline = false;
	};

#pragma endregion

#pragma region CommandList // 命令录制（Pass 录制回调拿到的对象）

	// 句柄类：包装 VkCommandBuffer，无所有权（一帧内有效）
	class CommandList {
	public:
		// 同步：barrier compiler 的直译出口，批量提交便于合并
		void Barrier(std::span<const ImageBarrier> images, std::span<const BufferBarrier> buffers);

		// dynamic rendering
		void BeginRendering(const RenderingInfo& info);
		void EndRendering();

		// 状态绑定
		void BindPipeline(const Pipeline& pipeline);
		void BindDescriptorSet(uint32_t setIndex, const DescriptorSet& set); // 预编译路径
		void PushConstants(const void* data, uint32_t size);
		void SetViewport(const Viewport& viewport);

		// 顶点/索引与绘制
		// firstBinding：多流顶点缓冲的起始绑定点（与 Pipeline 的 VertexInputBinding.binding 对应）
		void BindVertexBuffer(const Buffer& buffer, uint64_t offset = 0, uint32_t firstBinding = 0);
		void BindIndexBuffer(const Buffer& buffer, uint64_t offset, IndexType type);
		void Draw(uint32_t vertexCount, uint32_t firstVertex = 0, uint32_t instanceCount = 1);
		void DrawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t vertexOffset = 0,
			uint32_t instanceCount = 1);
		// 间接绘制：参数（VkDrawIndirectCommand / VkDrawIndexedIndirectCommand）从 buffer 读取，
		// drawCount 为条目数（参数在 GPU 侧，CPU 不可见）
		void DrawIndirect(const Buffer& args, uint64_t offset = 0, uint32_t drawCount = 1);
		void DrawIndexedIndirect(const Buffer& args, uint64_t offset = 0, uint32_t drawCount = 1);

		// 计算派发（工作组数非像素尺寸：通常为 ceil(宽/局部宽), ceil(高/局部高), 1）
		void Dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);
		// 间接派发：工作组数（VkDispatchIndirectCommand）从 buffer 读取，CPU 不可见
		void DispatchIndirect(const Buffer& args, uint64_t offset = 0);

		// 拷贝/清场（图外工具与上传/回读路径用）
		// bufferOffset 为 buffer 侧起点（0 = 整段）；拷贝区域固定为图像全层 mip0（接口未暴露逐 region）
		void CopyBufferToImage(const Buffer& src, Image& dst, ImageLayout dstLayout, uint64_t bufferOffset = 0);
		// 反向回读：图像全层 mip0 -> buffer 指定偏移；srcLayout 须为 TransferSrc（或 General）
		void CopyImageToBuffer(const Image& src, const Buffer& dst, ImageLayout srcLayout, uint64_t bufferOffset = 0);
		// buffer -> buffer 整段/窗口拷贝；size == 0 时取两者较小值
		void CopyBuffer(const Buffer& src, const Buffer& dst, uint64_t srcOffset = 0,
			uint64_t dstOffset = 0, uint64_t size = 0);
		// image -> image 拷贝：同格式、同尺寸，整图 mip0 全层；两端布局通常为 TransferSrc/TransferDst
		void CopyImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout);
		// image -> image blit：同格式，可缩放（源/目的 extent 各按自身尺寸）；
		// linearFilter=true 需格式支持 SAMPLED_IMAGE_FILTER_LINEAR，深度/整数格式请用最近邻
		void BlitImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout,
			bool linearFilter = true);
		void ClearColor(Image& image, ImageSubresource view, const float rgba[4], ImageLayout layout);

		// GPU 时间戳（性能诊断）：在批首/批尾各打一个点，配合 Device::ReadFrameTimestamps 回读。
		// ResetTimestamps 必须出现在同一批的 WriteTimestamp 之前；跨帧复用同一索引是安全的——
		// 帧槽只有在该槽全部在飞提交完成后才回收，届时旧查询已不再被引用
		void ResetTimestamps(uint32_t first, uint32_t count);
		void WriteTimestamp(uint32_t index); // ALL_COMMANDS 阶段：本批此前命令全部完成的时刻

		VkCommandBuffer NativeHandle() const;
	private:
		friend class Device; // BeginCommandList 构造并填句柄
		VkCommandBuffer  m_commandBuffer = VK_NULL_HANDLE;
		VkQueryPool      m_timestampPool = VK_NULL_HANDLE; // 所属帧槽的时间戳查询池（队列族不支持时为空）
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE; // BindPipeline 记录，供描述符 / push constant 绑定用
		uint32_t         m_pushConstantSize = 0;            // 当前管线声明的 push constant 大小（0=无）
		VkPipelineBindPoint m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; // 当前管线绑定点（描述符绑定沿用）
		VkShaderStageFlags  m_stageFlags = 0;                              // 当前管线的阶段集合（push constant 用）
	};

#pragma endregion

#pragma region Submit // 提交与帧间同步

	// 跨队列同步的两端点。binary 语义量 value 必须为 0；timeline 语义量按值匹配
	// （signal 的 value 须大于当前值，wait 的 value 由提交在该处之后推进）
	struct SemaphoreWait   { const Semaphore* semaphore = nullptr; PipelineStage stage = PipelineStage::None; std::uint64_t value = 0; };
	struct SemaphoreSignal { const Semaphore* semaphore = nullptr; PipelineStage stage = PipelineStage::None; std::uint64_t value = 0; };

	struct SubmitInfo {
		std::span<const CommandList>     commandLists {};
		std::span<const SemaphoreWait>   waits {};    // 跨队列 / acquire 用
		std::span<const SemaphoreSignal> signals {};
		Fence* signalFence = nullptr;                 // CPU 等待用，可空；经 RHI 回收等待，由使用者负责 Reset 复用
		QueueClass queue = QueueClass::Graphics;      // 本批命令目标队列；commandLists 须全部来自同族池
	};

#pragma endregion

#pragma region FrameSlot // 帧槽（命令录制与回收上下文）

	// 单个帧槽：每队列族一个 command pool + 内部 fence 池 + 在录 buffer 集合。
	// 回收约定：帧内可任意多次 BeginCommandList / Submit（含跨队列），每次 Submit 取一个
	// 独立 fence 记入在飞集合；回收只在 WaitForFrame（Recycle）时发生 —— 一次性等本槽全部
	// 在飞提交，不做逐次 CPU 收账，因此多队列提交可真正并行。
	// pending 为空绝不 wait："Begin 后不 Submit"、漏调 WaitForFrame 只退化为串行，不会死锁。
	class FrameSlot {
	public:
		// 每帧槽时间戳查询容量（每批 2 个：批首/批尾）
		static constexpr uint32_t kTimestampQueries = 32;

		FrameSlot(const FrameSlot&) = delete;
		FrameSlot& operator=(const FrameSlot&) = delete;
		~FrameSlot();

	private:
		friend class Device;
		FrameSlot(VkDevice device, std::vector<uint32_t> queueFamilies); // 去重后的族列表（present 族不建池）

		VkCommandBuffer Allocate(uint32_t queueFamily); // 按族查池，分配并 begin（ONE_TIME_SUBMIT），记入在录集合
		VkFence         AcquireFence();                // 取一个空闲内部 fence（池耗尽则新建，均为 unsignaled）
		void            ReleaseFence(VkFence fence);   // 未提交成功时原样归还池（仍为 unsignaled）
		void            MarkPending(VkFence fence, bool owned); // 记录一次在飞提交（外部或内部 fence）
		void            DrainPending();                // 等全部在飞提交完成并清账（不动在录 buffer）
		void            Recycle();                     // DrainPending + 按池释放在录 buffer
		VkQueryPool     TimestampPool() const { return m_timestampPool; } // 时间戳回读（Device 用）

		// 每队列族一个命令池（命令缓冲只能提交到与其 pool 同族的队列）
		struct FamilyPool {
			uint32_t      family = kQueueFamilyIgnored;
			VkCommandPool pool = VK_NULL_HANDLE;
		};

		// 一次提交的在飞记录：owned=true 表示内部 fence（等待后由本槽复位归还）
		struct PendingSubmit {
			VkFence fence = VK_NULL_HANDLE;
			bool    owned = false;
		};

		VkDevice                            m_device = VK_NULL_HANDLE;
		std::vector<FamilyPool>             m_pools;      // 每族一池（数量 = 构造传入族数）
		std::vector<std::vector<VkCommandBuffer>> m_liveBuffers; // 与 m_pools 平行：每池的在录 buffer
		std::vector<VkFence>                m_fencePool;  // 空闲内部 fence（等待并复位后归还）
		std::vector<PendingSubmit>          m_pending;    // 本帧在飞提交（每 Submit 一条）
		VkQueryPool                         m_timestampPool = VK_NULL_HANDLE; // 时间戳查询（每槽一池）
	};

#pragma endregion

#pragma region Swapchain // 交换链

	class Swapchain {
	public:
		struct Frame {
			uint32_t imageIndex = 0;
			Image*   backbuffer = nullptr;  // 指向当帧交换链图像的非拥有句柄；"外部资源"导入直接吃它
			bool     acquired = false;      // resize / 失焦时为 false，上层跳过渲染
			const Semaphore* ready = nullptr; // acquire 信号量；Submit 的 waits 须等它，否则使用 presentable image 触发校验错误
			const Semaphore* presentReady = nullptr; // 渲染完成信号量（按 image 索引）；Submit 的 signals 产出它，Present 无参时等它
		};

		Swapchain(const Swapchain&) = delete;
		Swapchain& operator=(const Swapchain&) = delete;
		~Swapchain();

		Frame       AcquireNextImage();  // 内部管理 acquire semaphore；OUT_OF_DATE 时自动重建
		// presentWait：外部渲染完成信号量（上层 Submit 产出）；为空则用 Frame::presentReady——
		// 按 image 索引的内部信号量，规避单一信号量在 present 消费完成前被下一帧再次 signal 的复用风险
		void        Present(const Semaphore* presentWait = nullptr);
		ImageFormat Format() const;
		uint32_t    Width() const;
		uint32_t    Height() const;
	private:
		friend class Device;

		// 自动重建所需的环境信息（Device 创建时填入；Swapchain 自持，不反向引用 Device）
		struct Context {
			VkDevice         device = VK_NULL_HANDLE;
			VkQueue          presentQueue = VK_NULL_HANDLE;
			VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
			VkSurfaceKHR     surface = VK_NULL_HANDLE;
			uint32_t         graphicsFamily = 0;
			uint32_t         presentFamily = 0;
			uint32_t         windowWidth = 0;
			uint32_t         windowHeight = 0;
			uint32_t         maxFramesInFlight = 2; // 与 Device 帧槽数一致（acquire 信号量按槽数建与轮转）
		};

		// 新建交换链本体并取回 backbuffer（首次与重建共用；oldSwapchain 非空时复用以降峰值）
		struct BuildResult {
			VkSwapchainKHR     swapchain = VK_NULL_HANDLE;
			VkSurfaceFormatKHR surfaceFormat {};
			VkExtent2D         extent {};
			std::vector<std::unique_ptr<Image>> images;
		};
		static bool Build(const Context& context, VkSwapchainKHR oldSwapchain, BuildResult& result);
		static std::vector<std::unique_ptr<Semaphore>> CreateAcquireSemaphores(VkDevice device, uint32_t count);

		Swapchain(const Context& context, BuildResult&& built,
			std::vector<std::unique_ptr<Semaphore>> acquireSemaphores, uint32_t maxFramesInFlight);

		// 自动重建：等设备空闲后重取格式/尺寸，重建交换链与借用图像，并重造 acquire 信号量
		bool Recreate();
		// 表面当前尺寸是否已与交换链不符（部分驱动 resize 后仍返回 VK_SUCCESS，故需主动比对）
		bool NeedsRecreate() const;

		Context            m_context;
		VkSwapchainKHR     m_swapchain = VK_NULL_HANDLE;
		VkSurfaceFormatKHR m_surfaceFormat {};
		VkExtent2D         m_extent {};
		std::vector<std::unique_ptr<Image>>     m_images;            // 借用：每张 backbuffer 一个包装
		std::vector<std::unique_ptr<Semaphore>> m_acquireSemaphores; // 每帧槽一个，由 acquire 置位
		std::vector<std::unique_ptr<Semaphore>> m_renderDone;       // 每 image 一个，Submit 置位、present 消费
		uint32_t           maxFrameInFlight = 2;

		uint32_t m_frameIndex = 0;          // 帧槽（acquire 信号量索引）；帧回收落地前在获取成功时轮转
		bool     m_recreatePending = false; // suboptimal 等场景：下次获取前先重建
		Frame    m_lastFrame {};            // 上一帧获取结果，供 Present 使用
	};

#pragma endregion

#pragma region DeviceAPI // 设备主接口

	class Device {
	public:
		Device() = default;
		~Device();

		static std::unique_ptr<Device> Create(const DeviceDesc& desc);

		// 资源与对象创建
		std::unique_ptr<Image>         CreateImage(const ImageCreateInfo& info);
		std::unique_ptr<Buffer>        CreateBuffer(const BufferCreateInfo& info);
		std::unique_ptr<Pipeline>      CreatePipeline(const GraphicsPipelineDesc& desc);
		std::unique_ptr<Pipeline>      CreateComputePipeline(const ComputePipelineDesc& desc);
		std::unique_ptr<DescriptorSet> AllocateDescriptorSet(const DescriptorSetLayout& layout);
		std::unique_ptr<Fence>         CreateFence(bool signaled = false);
		std::unique_ptr<Semaphore>     CreateSemaphore(); // binary（acquire/present 链、单次跨队列）
		// timeline：可被主机 Signal（Host→GPU 唯一的非阻塞手段）；initialValue 由使用者给定
		std::unique_ptr<Semaphore>     CreateTimelineSemaphore(std::uint64_t initialValue = 0);
		bool                           TimelineSemaphoresSupported() const { return m_timelineSupported; }

		// 帧上下文
		std::unique_ptr<Swapchain> CreateSwapchain();
		CommandList                BeginCommandList(QueueClass queue = QueueClass::Graphics); // 自当期帧对应族 pool 分配

		// 提交与帧回收（frames-in-flight 上限，默认 2）
		// 提交到 info.queue。返回本次提交在该队列 GPU timeline 上的 signal 值（0 = 设备无 timeline）：
		// 主机可 QueueTimeline(queue)->Wait(value) 等这一批完成（GPU→Host 的非阻塞等待）
		std::uint64_t Submit(const SubmitInfo& info);

		// 每队列一条 GPU timeline：值随该队列的提交顺序单调推进（队列内有序，不会与并行队列撞值），
		// 由 Submit 自动分配并作为信号追加，无需使用者在 signals 里手填
		Semaphore*    QueueTimeline(QueueClass cls) const;
		std::uint64_t LastQueueTimelineValue(QueueClass cls) const;
		void WaitForFrame();
		void WaitIdle();   // 等待设备完全空闲（呈现仍在途时，销毁信号量/资源前调用）

		// 某类队列的族索引（QFOT barrier 字段 / 诊断用；无专用队列时已兜底 graphics）
		uint32_t QueueFamily(QueueClass cls) const;

		// GPU 时间戳（性能诊断）：须在 WaitForFrame 之后立即调用——回读刚结束那一帧的查询区间，
		// 输出为设备原始 tick（毫秒换算用 TimestampPeriodNs）。查询索引约定为批 b 的 [2b, 2b+1]
		bool ReadFrameTimestamps(uint32_t first, uint32_t count, std::uint64_t* out) const;
		bool TimestampsSupported() const { return m_timestampsSupported; }   // 各队列族 timestampValidBits > 0
		bool TimestampsComparable() const { return m_timestampComparable; } // 跨队列同一时间基
		double TimestampPeriodNs() const { return m_timestampPeriod; }      // ns / tick

		// 诊断信息（日志 / 冒烟测试）
		const char* DeviceName() const;
		uint32_t    ApiVersion() const;

	private:
		// 合并槽位表 -> 各 set 布局 + pipeline layout（图形 / 计算共用）。
		// 每条记录自带 stageFlags；失败时已建对象就地清理
		bool BuildSetLayouts(std::vector<MergedDescriptor> slots, uint32_t pushConstantSize,
			VkShaderStageFlags pushStages,
			std::vector<std::unique_ptr<DescriptorSetLayout>>& outLayouts, VkPipelineLayout& outLayout);

		VkInstance               m_instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
		VkSurfaceKHR             m_surface = VK_NULL_HANDLE;   // 有窗口句柄时创建，否则为空（离屏路径）
		VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
		VkDevice                 m_device = VK_NULL_HANDLE;
		VkQueue                  m_graphicsQueue = VK_NULL_HANDLE;
		VkQueue                  m_presentQueue = VK_NULL_HANDLE;
		uint32_t                 m_graphicsQueueFamily = 0;
		uint32_t                 m_presentQueueFamily = 0;
		VkQueue                  m_computeQueue = VK_NULL_HANDLE;  // 无专用计算队列时 = graphics 队列
		VkQueue                  m_transferQueue = VK_NULL_HANDLE; // 无专用传输队列时 = graphics 队列
		uint32_t                 m_computeQueueFamily = 0;         // 选定期已兜底，恒有效
		uint32_t                 m_transferQueueFamily = 0;
		VmaAllocator_T*          m_allocator = nullptr;
		VkDescriptorPool         m_descriptorPool = VK_NULL_HANDLE;
		VkSampler                m_defaultSampler = VK_NULL_HANDLE;  // 供 combined image sampler 复用
		VkPipelineCache          m_pipelineCache = VK_NULL_HANDLE;

		std::vector<std::unique_ptr<FrameSlot>> m_frameSlots; // 帧槽（数量 = DeviceDesc::maxFramesInFlight）
		uint32_t m_frameIndex = 0;                             // 当期帧槽索引

		bool   m_timestampsSupported = false; // 三类队列族都支持 timestampValidBits > 0
		bool   m_timestampComparable = false; // limits.timestampComputeAndGraphics（跨队列时间基可比）
		double m_timestampPeriod = 0.0;       // limits.timestampPeriod（ns / tick）
		bool   m_timelineSupported = false;   // timelineSemaphore 特性已开启（Create 时查询并启用）
		// 每队列一条 GPU timeline（下标 = QueueClass），值为该队列已分配的最大 signal 值
		std::unique_ptr<Semaphore> m_queueTimelines[3];
		std::uint64_t              m_queueTimelineValue[3] = { 0, 0, 0 };

		uint32_t m_windowWidth = 0, m_windowHeight = 0; // 无 currentExtent 时的回退尺寸（来自 DeviceDesc）

		std::string m_deviceName;
		uint32_t    m_apiVersion = 0;
	};

#pragma endregion
}
