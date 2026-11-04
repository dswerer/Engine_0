#pragma once
#include "RHIDesc.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;

// RHI 对象层：设备、资源、管线、描述符、命令列表、交换链。
namespace dEngine::rhi {

#pragma region Device

	struct DeviceDesc {
		void*    nativeWindowHandle = nullptr; // Win32 窗口句柄；为空则无 swapchain（离屏/测试）
		uint32_t width = 0, height = 0;
		bool     enableValidation = true;      // debug 下默认开
		uint32_t maxFramesInFlight = 2;        // 帧槽数量（创建期决定；内部钳制到 [1,8]）
	};

#pragma endregion

#pragma region Resource

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

	// 拥有/借用两种模式：借用仅包装外部 VkImage（如交换链），析构不销毁底层句柄。
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
		// 带范围/格式的视图：支持 mip/层区间与格式重解释（如 RGBA8 -> R32），range 的 count 为 0 时补齐到末尾
		const ImageView& CreateView(const ImageViewDesc& view) const;
		// 以指定格式重解释图像（默认整图）：等价于 CreateView + format 覆盖
		const ImageView& ReinterpretView(ImageFormat format,
			ImageSubresourceRange range = ImageSubresourceRange{ {}, 0, 0 }) const;

		VkImage                NativeHandle() const;
		VkFormat               NativeFormat() const; // 真实后端格式（含 SRGB 变体）
		const ImageCreateInfo& Desc() const;
	private:
		friend class Device;
		Image(VkDevice device, VkFormat format, VmaAllocator_T* allocator, VkImage image,
			VmaAllocation_T* allocation, const ImageCreateInfo& desc, bool borrowed);

		ImageCreateInfo  m_desc;
		VkDevice         m_device = VK_NULL_HANDLE;
		VkFormat         m_format = VK_FORMAT_UNDEFINED;
		VkImage          m_image = VK_NULL_HANDLE;
		VmaAllocation_T* m_allocation = nullptr;
		VmaAllocator_T*  m_allocator = nullptr;
		bool             m_borrowed = false;

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

		// 非一致内存显式可见性：主机写后须 Flush，GPU 写后主机读前须 Invalidate。
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

#pragma region Sampler

	// 不可变采样状态；同一描述可跨管线共享（由创建方持有）
	class Sampler {
	public:
		Sampler(const Sampler&) = delete;
		Sampler& operator=(const Sampler&) = delete;
		~Sampler();

		VkSampler NativeHandle() const;
	private:
		friend class Device;
		Sampler(VkDevice device, VkSampler sampler);

		VkDevice  m_device = VK_NULL_HANDLE;
		VkSampler m_sampler = VK_NULL_HANDLE;
	};

#pragma endregion

#pragma region Descriptor

	// 合并后的描述符槽位：绑定声明 + stageFlags 并集（后端侧类型，不进 Desc 层）。
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
		friend class Device;
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
		// 图像绑定：数组按 layer 展开；sampler 为空用默认，viewFormat 非 None 则重解释。
		// layout 为 Undefined 时按描述符类型推导（采样 = ShaderReadOnly，存储 = General）。
		void Write(uint32_t binding, const Image& image, ImageSubresource view, uint32_t count = 1,
			const Sampler* sampler = nullptr, ImageFormat viewFormat = ImageFormat::None,
			uint32_t firstElement = 0, ImageLayout layout = ImageLayout::Undefined);
		// 缓冲绑定（数组绑定按 range 步进：第 i 个元素 offset + i*range，count > 1 时 range 须非 0）
		void Write(uint32_t binding, const Buffer& buffer, uint64_t offset, uint64_t range, uint32_t count = 1);
	private:
		friend class Device;
		DescriptorSet(VkDevice device, VkDescriptorPool pool, VkSampler defaultSampler,
			VkDescriptorSet set, std::unordered_map<uint32_t, VkDescriptorType> bindingTypes);

		VkDevice         m_device = VK_NULL_HANDLE;
		VkDescriptorPool m_pool = VK_NULL_HANDLE;
		VkSampler        m_defaultSampler = VK_NULL_HANDLE;
		VkDescriptorSet  m_set = VK_NULL_HANDLE;
		std::unordered_map<uint32_t, VkDescriptorType> m_bindingTypes;
	};

#pragma endregion

#pragma region Pipeline

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
		std::vector<std::unique_ptr<DescriptorSetLayout>> m_setLayouts;
	};

#pragma endregion

#pragma region Sync

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

		// 仅 timeline 有效：Signal 推进 Host→GPU，Wait 等待提交推进到 value。
		bool            Signal(std::uint64_t value);
		bool            Wait(std::uint64_t value, std::uint64_t timeoutNs = ~0ull) const;
		std::uint64_t   Value() const;
	private:
		friend class Device;
		friend class Swapchain;
		Semaphore(VkDevice device, VkSemaphore semaphore, bool timeline = false);

		VkDevice    m_device = VK_NULL_HANDLE;
		VkSemaphore m_semaphore = VK_NULL_HANDLE;
		bool        m_timeline = false;
	};

#pragma endregion

#pragma region CommandList

	// 句柄类：包装 VkCommandBuffer，无所有权（一帧内有效）
	class CommandList {
	public:
		// 同步：barrier compiler 的直译出口，批量提交便于合并
		void Barrier(std::span<const ImageBarrier> images, std::span<const BufferBarrier> buffers);
		// 直接转换单张图像布局：内部构造 ImageBarrier，默认 AllCommands + MemoryRead/Write
		void TransitionImage(Image& image, ImageLayout oldLayout, ImageLayout newLayout,
			ImageSubresourceRange range = ImageSubresourceRange{ {}, 0, 0 },
			PipelineStage srcStage = PipelineStage::AllCommands,
			Access srcAccess = Access::MemoryWrite,
			PipelineStage dstStage = PipelineStage::AllCommands,
			Access dstAccess = Access::MemoryRead,
			uint32_t srcQueueFamily = kQueueFamilyIgnored,
			uint32_t dstQueueFamily = kQueueFamilyIgnored);

		// dynamic rendering
		void BeginRendering(const RenderingInfo& info);
		void EndRendering();

		// 状态绑定
		void BindPipeline(const Pipeline& pipeline);
		void BindDescriptorSet(uint32_t setIndex, const DescriptorSet& set); // 预编译路径
		void PushConstants(const void* data, uint32_t size);
		void SetViewport(const Viewport& viewport);

		// 顶点/索引与绘制：firstBinding 为多流顶点缓冲的起始绑定点。
		void BindVertexBuffer(const Buffer& buffer, uint64_t offset = 0, uint32_t firstBinding = 0);
		void BindIndexBuffer(const Buffer& buffer, uint64_t offset, IndexType type);
		void Draw(uint32_t vertexCount, uint32_t firstVertex = 0, uint32_t instanceCount = 1);
		void DrawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t vertexOffset = 0,
			uint32_t instanceCount = 1);
		// 间接绘制：参数从 buffer 读取，drawCount 为条目数（GPU 侧，CPU 不可见）。
		void DrawIndirect(const Buffer& args, uint64_t offset = 0, uint32_t drawCount = 1);
		void DrawIndexedIndirect(const Buffer& args, uint64_t offset = 0, uint32_t drawCount = 1);

		// 计算派发（工作组数非像素尺寸：通常为 ceil(宽/局部宽), ceil(高/局部高), 1）
		void Dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);
		// 间接派发：工作组数（VkDispatchIndirectCommand）从 buffer 读取，CPU 不可见
		void DispatchIndirect(const Buffer& args, uint64_t offset = 0);

		// 拷贝/清场：bufferOffset 为 buffer 侧起点（0 = 整段），拷贝区域固定为图像全层 mip0。
		void CopyBufferToImage(const Buffer& src, Image& dst, ImageLayout dstLayout, uint64_t bufferOffset = 0);
		void CopyBufferToImage(const Buffer& src, Image& dst, ImageLayout dstLayout,
			std::span<const BufferImageCopyRegion> regions);
		// 反向回读：图像全层 mip0 -> buffer 指定偏移；srcLayout 须为 TransferSrc（或 General）
		void CopyImageToBuffer(const Image& src, const Buffer& dst, ImageLayout srcLayout, uint64_t bufferOffset = 0);
		void CopyImageToBuffer(const Image& src, const Buffer& dst, ImageLayout srcLayout,
			std::span<const BufferImageCopyRegion> regions);
		// buffer -> buffer 整段/窗口拷贝；size == 0 时取两者较小值
		void CopyBuffer(const Buffer& src, const Buffer& dst, uint64_t srcOffset = 0,
			uint64_t dstOffset = 0, uint64_t size = 0);
		// image -> image 拷贝：同格式、同尺寸，整图 mip0 全层；两端布局通常为 TransferSrc/TransferDst
		void CopyImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout);
		void CopyImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout,
			std::span<const ImageCopyRegion> regions);
		// image→image blit：同格式可缩放；linearFilter 需格式支持，深度/整数格式用最近邻。
		void BlitImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout,
			bool linearFilter = true);
		void BlitImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout,
			std::span<const ImageBlitRegion> regions, bool linearFilter = true);
		void ClearColor(Image& image, ImageSubresource view, const float rgba[4], ImageLayout layout);
		// 按子资源范围清场；颜色/深度模板值由 value 变体决定
		void ClearImage(Image& image, ImageSubresourceRange range, ImageLayout layout, const ClearValue& value);

		// GPU 时间戳：批首/批尾打点，ResetTimestamps 必须位于同批 WriteTimestamp 之前。
		void ResetTimestamps(uint32_t first, uint32_t count);
		void WriteTimestamp(uint32_t index); // ALL_COMMANDS 阶段：本批此前命令全部完成的时刻

		VkCommandBuffer NativeHandle() const;
	private:
		friend class Device;
		VkCommandBuffer  m_commandBuffer = VK_NULL_HANDLE;
		VkQueryPool      m_timestampPool = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		uint32_t         m_pushConstantSize = 0;
		VkPipelineBindPoint m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		VkShaderStageFlags  m_stageFlags = 0;
	};

#pragma endregion

#pragma region Submit

	// 跨队列同步端点：binary 语义量 value 必须为 0；timeline 按值匹配。
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

#pragma region FrameSlot

	// 帧槽：每族一命令池 + fence 池；仅 WaitForFrame 回收在飞提交，pending 为空不等待。
	class FrameSlot {
	public:
		// 每帧槽时间戳查询容量（每批 2 个：批首/批尾）
		static constexpr uint32_t kTimestampQueries = 32;

		FrameSlot(const FrameSlot&) = delete;
		FrameSlot& operator=(const FrameSlot&) = delete;
		~FrameSlot();

	private:
		friend class Device;
		FrameSlot(VkDevice device, std::vector<uint32_t> queueFamilies);

		VkCommandBuffer Allocate(uint32_t queueFamily);
		VkFence         AcquireFence();
		void            ReleaseFence(VkFence fence);
		void            MarkPending(VkFence fence, bool owned);
		void            DrainPending();
		void            Recycle();
		VkQueryPool     TimestampPool() const { return m_timestampPool; }

		struct FamilyPool {
			uint32_t      family = kQueueFamilyIgnored;
			VkCommandPool pool = VK_NULL_HANDLE;
		};

		struct PendingSubmit {
			VkFence fence = VK_NULL_HANDLE;
			bool    owned = false;
		};

		VkDevice                            m_device = VK_NULL_HANDLE;
		std::vector<FamilyPool>             m_pools;
		std::vector<std::vector<VkCommandBuffer>> m_liveBuffers;
		std::vector<VkFence>                m_fencePool;
		std::vector<PendingSubmit>          m_pending;
		VkQueryPool                         m_timestampPool = VK_NULL_HANDLE;
	};

#pragma endregion

#pragma region Swapchain

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
		// presentWait 为空则用按 image 索引的 presentReady，避免跨帧复用同一信号量。
		void        Present(const Semaphore* presentWait = nullptr);
		ImageFormat Format() const;
		uint32_t    Width() const;
		uint32_t    Height() const;
	private:
		friend class Device;

		struct Context {
			VkDevice         device = VK_NULL_HANDLE;
			VkQueue          presentQueue = VK_NULL_HANDLE;
			VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
			VkSurfaceKHR     surface = VK_NULL_HANDLE;
			uint32_t         graphicsFamily = 0;
			uint32_t         presentFamily = 0;
			uint32_t         windowWidth = 0;
			uint32_t         windowHeight = 0;
			uint32_t         maxFramesInFlight = 2;
		};

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

		bool Recreate();
		bool NeedsRecreate() const;

		Context            m_context;
		VkSwapchainKHR     m_swapchain = VK_NULL_HANDLE;
		VkSurfaceFormatKHR m_surfaceFormat {};
		VkExtent2D         m_extent {};
		std::vector<std::unique_ptr<Image>>     m_images;
		std::vector<std::unique_ptr<Semaphore>> m_acquireSemaphores;
		std::vector<std::unique_ptr<Semaphore>> m_renderDone;
		uint32_t           maxFrameInFlight = 2;

		uint32_t m_frameIndex = 0;
		bool     m_recreatePending = false;
		Frame    m_lastFrame {};
	};

#pragma endregion

#pragma region DeviceAPI

	class Device {
	public:
		Device() = default;
		~Device();

		static std::unique_ptr<Device> Create(const DeviceDesc& desc);

		// 资源与对象创建
		std::unique_ptr<Image>         CreateImage(const ImageCreateInfo& info);
		std::unique_ptr<Buffer>        CreateBuffer(const BufferCreateInfo& info);
		std::unique_ptr<Sampler>       CreateSampler(const SamplerDesc& desc);
		std::unique_ptr<Pipeline>      CreatePipeline(const GraphicsPipelineDesc& desc,
			const DescriptorSetLayout* externalSet0 = nullptr);
		std::unique_ptr<Pipeline>      CreateComputePipeline(const ComputePipelineDesc& desc,
			const DescriptorSetLayout* externalSet0 = nullptr);
		std::unique_ptr<DescriptorSet> AllocateDescriptorSet(const DescriptorSetLayout& layout);
		// 手工描述符布局（非反射）：partiallyBound=true 启用 PARTIALLY_BOUND。
		std::unique_ptr<DescriptorSetLayout> CreateDescriptorSetLayout(
			const std::vector<DescriptorBinding>& bindings, bool partiallyBound = false);
		// descriptor indexing / 采样图像数组动态索引是否可用（静态纹理堆的前提）
		bool                           SupportsBindlessTextures() const { return m_bindlessSupported; }
		// 逐附件独立混合状态是否可用（不同附件不同 blend/writeMask 的前提）
		bool                           SupportsIndependentBlend() const { return m_independentBlendSupported; }
		// depth bounds test 是否可用（不可用时 RHI 自动关闭该测试）
		bool                           SupportsDepthBounds() const { return m_depthBoundsSupported; }
		// 细分着色器（TessControl/TessEval）是否可用
		bool                           SupportsTessellation() const { return m_tessellationSupported; }
		// 几何着色器是否可用
		bool                           SupportsGeometryShaders() const { return m_geometrySupported; }
		// 顶点/细分/几何阶段是否可写 storage 资源；不可写时这些阶段必须声明 readonly
		bool                           SupportsVertexPipelineStoresAndAtomics() const {
			return m_vertexPipelineStoresAndAtomics;
		}
		std::unique_ptr<Fence>         CreateFence(bool signaled = false);
		std::unique_ptr<Semaphore>     CreateSemaphore(); // binary（acquire/present 链、单次跨队列）
		// timeline：可被主机 Signal；initialValue 由使用者给定。
		std::unique_ptr<Semaphore>     CreateTimelineSemaphore(std::uint64_t initialValue = 0);
		bool                           TimelineSemaphoresSupported() const { return m_timelineSupported; }

		// 帧上下文
		std::unique_ptr<Swapchain> CreateSwapchain();
		CommandList                BeginCommandList(QueueClass queue = QueueClass::Graphics); // 自当期帧对应族 pool 分配

		// 提交到 info.queue，返回该队列 GPU timeline 的 signal 值（0 = 无 timeline）。
		std::uint64_t Submit(const SubmitInfo& info);

		// 每队列一条 GPU timeline，由 Submit 自动分配并作为信号追加。
		Semaphore*    QueueTimeline(QueueClass cls) const;
		std::uint64_t LastQueueTimelineValue(QueueClass cls) const;
		void WaitForFrame();
		void WaitIdle();   // 等待设备完全空闲（呈现仍在途时，销毁信号量/资源前调用）

		// 某类队列的族索引（QFOT barrier 字段 / 诊断用；无专用队列时已兜底 graphics）
		uint32_t QueueFamily(QueueClass cls) const;

		// GPU 时间戳：须在 WaitForFrame 后立即调用，输出设备原始 tick。
		bool ReadFrameTimestamps(uint32_t first, uint32_t count, std::uint64_t* out) const;
		bool TimestampsSupported() const { return m_timestampsSupported; }   // 各队列族 timestampValidBits > 0
		bool TimestampsComparable() const { return m_timestampComparable; } // 跨队列同一时间基
		double TimestampPeriodNs() const { return m_timestampPeriod; }      // ns / tick

		// 诊断信息（日志 / 冒烟测试）
		const char* DeviceName() const;
		uint32_t    ApiVersion() const;
		// 帧槽数固定于创建期；WaitForFrame 只等待当前槽，不要每帧调用。
		uint32_t FramesInFlight() const { return static_cast<uint32_t>(m_frameSlots.size()); }
		uint32_t CurrentFrameSlot() const { return m_currentFrameSlot; }
		uint32_t BeginFrame();     // 开始一帧：回收复用槽并轮转，返回本帧槽索引
		void     WaitAllFrames();  // 等待所有槽的在飞提交（收尾/回读前用）

	private:
		bool BuildSetLayouts(std::vector<MergedDescriptor> slots, uint32_t pushConstantSize,
			VkShaderStageFlags pushStages, const DescriptorSetLayout* externalSet0,
			std::vector<std::unique_ptr<DescriptorSetLayout>>& outLayouts, VkPipelineLayout& outLayout);

		VkInstance               m_instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
		VkSurfaceKHR             m_surface = VK_NULL_HANDLE;
		VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
		VkDevice                 m_device = VK_NULL_HANDLE;
		VkQueue                  m_graphicsQueue = VK_NULL_HANDLE;
		VkQueue                  m_presentQueue = VK_NULL_HANDLE;
		uint32_t                 m_graphicsQueueFamily = 0;
		uint32_t                 m_presentQueueFamily = 0;
		VkQueue                  m_computeQueue = VK_NULL_HANDLE;
		VkQueue                  m_transferQueue = VK_NULL_HANDLE;
		uint32_t                 m_computeQueueFamily = 0;
		uint32_t                 m_transferQueueFamily = 0;
		VmaAllocator_T*          m_allocator = nullptr;
		VkDescriptorPool         m_descriptorPool = VK_NULL_HANDLE;
		VkSampler                m_defaultSampler = VK_NULL_HANDLE;
		VkPipelineCache          m_pipelineCache = VK_NULL_HANDLE;

		std::vector<std::unique_ptr<FrameSlot>> m_frameSlots;
		uint32_t m_frameIndex = 0;
		uint32_t m_currentFrameSlot = 0;
		uint32_t m_framesBegun = 0;

		bool   m_timestampsSupported = false;
		float  m_maxSamplerAnisotropy = 1.0f;
		bool   m_bindlessSupported = false;
		bool   m_depthBoundsSupported = false;
		bool   m_independentBlendSupported = false;
		bool   m_tessellationSupported = false;
		bool   m_geometrySupported = false;
		bool   m_vertexPipelineStoresAndAtomics = false;
		uint32_t m_maxTessellationPatchSize = 1;
		bool   m_timestampComparable = false;
		double m_timestampPeriod = 0.0;
		bool   m_timelineSupported = false;
		std::unique_ptr<Semaphore> m_queueTimelines[3];
		std::uint64_t              m_queueTimelineValue[3] = { 0, 0, 0 };

		uint32_t m_windowWidth = 0, m_windowHeight = 0;

		std::string m_deviceName;
		uint32_t    m_apiVersion = 0;
	};

#pragma endregion
}
