#pragma once
#include <cstdint>
#include <string>
#include <array>
#include <variant>
#include <vector>

// RHI 描述层：后端无关的枚举与参数结构。承载 RHI 的公共词表（格式/加载存储/清除值/子资源）
// 与创建、录制参数；仅依赖标准库，不包含 Vulkan 头。
namespace dEngine::rhi {

	// 前置声明：描述结构中以指针引用运行期对象
	class Image;
	class Buffer;

#pragma region Vocabulary // 公共词表（渲染图侧复用）

	// 精简实用集，每个值均可映射到后端 format
	enum class ImageFormat {
		None,
		R8, RG8, RGBA8, BGRA8,
		R16F, RG16F, RGBA16F,
		R32F, RG32F, RGBA32F,
		R11G11B10F,
		D32F, D24S8, S8
	};

	enum class ImageViewType {
		None, Dim1D, Dim2D, Dim3D, Dim1DArray, Dim2DArray, Cube, CubeArray
	};

	enum class LoadOp  { Load, Clear, DontCare };
	enum class StoreOp { Store, DontCare };

	struct DepthStencilValue { float depth = 1.0f; uint32_t stencil = 0; };
	struct ClearValue {
		std::variant<std::array<float, 4>, DepthStencilValue> value;
	};

	// 绑定处的子资源选择（Image 的 view 参数；range 形式留待将来）
	struct ImageSubresource {
		uint32_t mipLevel = 0;
		uint32_t layer = 0;
	};

	inline constexpr uint32_t kMaxColorAttachments = 8;

#pragma endregion

#pragma region Sync // 同步词表（映射 VkPipelineStageFlags2 / VkAccessFlags2）

	// 按位组合使用（提供 operator|）
	enum class PipelineStage : uint64_t {
		None = 0,
		TopOfPipe          = 1ull << 0,
		VertexShader       = 1ull << 1,
		FragmentShader     = 1ull << 2,
		EarlyFragmentTests = 1ull << 3,
		LateFragmentTests  = 1ull << 4,
		ColorAttachmentOut = 1ull << 5,
		ComputeShader      = 1ull << 6,
		Transfer           = 1ull << 7,
		BottomOfPipe       = 1ull << 8,
		AllCommands        = 1ull << 9,
		Host               = 1ull << 10, // 宿主机写/读（Host 节点的 CPU 回调）
		VertexInput        = 1ull << 11  // 固定功能输入：顶点属性/索引读取
	};
	inline constexpr PipelineStage operator|(PipelineStage a, PipelineStage b) {
		return static_cast<PipelineStage>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
	}

	enum class Access : uint64_t {
		None                  = 0,
		ShaderRead            = 1ull << 0,
		ShaderWrite           = 1ull << 1,
		ColorAttachmentRead   = 1ull << 2,
		ColorAttachmentWrite  = 1ull << 3,
		DepthStencilRead      = 1ull << 4,
		DepthStencilWrite     = 1ull << 5,
		TransferRead          = 1ull << 6,
		TransferWrite         = 1ull << 7,
		MemoryRead            = 1ull << 8,
		MemoryWrite           = 1ull << 9,
		VertexAttributeRead   = 1ull << 10,
		IndexRead             = 1ull << 11,
		HostWrite             = 1ull << 12
	};
	inline constexpr Access operator|(Access a, Access b) {
		return static_cast<Access>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
	}

	// RHI 内部状态，不对上层暴露（图的 barrier compiler 负责决定目标布局）
	enum class ImageLayout {
		Undefined, General, ColorAttachment, DepthStencilAttachment,
		ShaderReadOnly, TransferSrc, TransferDst, PresentSrc
	};

	// RHI 层队列类别（映射到 Device 选定的队列族；无专用队列时兜底 graphics）
	enum class QueueClass { Graphics, AsyncCompute, Transfer };
	// 与 VK_QUEUE_FAMILY_IGNORED 同值（描述层不引 Vulkan 头）
	inline constexpr uint32_t kQueueFamilyIgnored = 0xFFFFFFFFu;

#pragma endregion

#pragma region ResourceCreate // 资源创建参数（能力位与图的逐次访问 Usage 正交）

	// 位标志：按位组合使用
	enum class ImageUsage : uint32_t {
		None            = 0,
		Sampled         = 1u << 0,
		Storage         = 1u << 1,
		ColorAttachment = 1u << 2,
		DepthStencil    = 1u << 3,
		TransferSrc     = 1u << 4,
		TransferDst     = 1u << 5
	};
	inline constexpr ImageUsage operator|(ImageUsage a, ImageUsage b) {
		return static_cast<ImageUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	enum class BufferUsage : uint32_t {
		None         = 0,
		Vertex       = 1u << 0,
		Index        = 1u << 1,
		Uniform      = 1u << 2,
		Storage      = 1u << 3,
		TransferSrc  = 1u << 4,
		TransferDst  = 1u << 5,
		Indirect     = 1u << 6   // 间接绘制参数（Vulkan 要求 INDIRECT_BUFFER 用法位）
	};
	inline constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
		return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	struct ImageCreateInfo {
		uint32_t     width = 0, height = 0;
		uint32_t     depth = 1;        // 仅 3D 有效
		uint32_t     arrayLayers = 1;
		uint32_t     mipLevels = 1;
		uint32_t     samples = 1;
		ImageFormat  format = ImageFormat::None;
		ImageViewType viewType = ImageViewType::Dim2D;
		ImageUsage   usage = ImageUsage::None; // 显式；将来由图按全图访问并集回填
		// 无 layout / tiling / aspect：由 RHI 内部决定
	};

	struct BufferCreateInfo {
		uint64_t    size = 0;
		BufferUsage usage = BufferUsage::None;
		bool        hostVisible = false;  // 决定 VMA 内存类型
	};

#pragma endregion

#pragma region Pipeline // 管线描述（描述符布局由反射得出，无手工 resources 字段）

	enum class CompareOp { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
	enum class CullMode  { None, Front, Back };
	enum class IndexType { UInt16, UInt32 };

	// 可编程着色器阶段。Tess/Task/Mesh 为预留：枚举与组合校验先行，
	// 管线状态创建（tessellation state / mesh 特性）待后续版本
	enum class ShaderStage { Vertex, Fragment, TessControl, TessEval, Geometry, Task, Mesh, Compute };

	// 单个着色器阶段：SPIR-V 二进制路径（离线编译产出，如 glslc），RHI 直接读取
	struct ShaderStageDesc {
		ShaderStage stage;
		std::string spirvPath;
	};

	// 反射产出的资源类别（亦可手工声明时使用）
	enum class ResourceType { SampledImage, StorageImage, UniformBuffer, StorageBuffer };

	// 单条描述符声明：由 SPIR-V 反射产出（name 为 shader 顶层变量名，
	// 图层按「变量名 == 资源名」约定对账）
	struct DescriptorBinding {
		std::string  name;
		uint32_t     set = 0;
		uint32_t     binding = 0;
		ResourceType type = ResourceType::SampledImage;
		uint32_t     arrayCount = 1;
	};

	// 图元拓扑（第一版最小集；其余拓扑随用随加）
	enum class PrimitiveTopology { PointList, LineList, LineStrip, TriangleList, TriangleStrip };

	// 顶点属性格式（第一版最小集：按 float 分量数；整型/归一化格式后续补）
	enum class VertexFormat { Float, Float2, Float3, Float4 };

	// 顶点缓冲绑定（VkVertexInputBindingDescription 的无后端词表版）：binding 序号由 pass 侧的流序号对应
	struct VertexInputBinding {
		uint32_t binding = 0;
		uint32_t stride = 0;
		bool     perInstance = false;
	};

	// 顶点属性（VkVertexInputAttributeDescription 的无后端词表版）
	struct VertexAttribute {
		uint32_t     location = 0;
		uint32_t     binding = 0;
		VertexFormat format = VertexFormat::Float3;
		uint32_t     offset = 0;
	};

	struct GraphicsPipelineDesc {
		// 着色器阶段集合（数组驱动，合法组合由 RHI 校验：Vertex 与 Task/Mesh 互斥、Tess 成对等）
		std::vector<ShaderStageDesc> shaders;

		// 附件格式签名：dynamic rendering 需要，与图的附件绑定对应
		std::array<ImageFormat, kMaxColorAttachments> colorFormats {};
		uint32_t    colorAttachmentCount = 0;
		ImageFormat depthFormat = ImageFormat::None;   // None = 无深度

		// 固定功能（第一版最小集）
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		// 顶点输入布局。两向量均为空 = 缺省：由 RHI 反射 Vertex 阶段输入派生
		// 「单流（binding 0）、交错排布」；shader 无用户输入时即无顶点缓冲（gl_VertexIndex 路径）。
		// 任一非空 = 整条按显式声明直译（多流/per-instance/非 float 格式等）
		std::vector<VertexInputBinding> vertexBindings;
		std::vector<VertexAttribute>    vertexAttributes;
		bool      depthTestEnable = true;
		bool      depthWriteEnable = true;
		CompareOp depthCompareOp = CompareOp::Less;
		CullMode  cullMode = CullMode::Back;
		bool      blendEnable = false;   // 单附件统一，逐附件后置
		// 描述符布局：不在此声明——由 RHI 对各 stage SPIR-V 反射得出（见 Pipeline::SetLayout），
		// 图层按「shader 变量名 == 图资源名」对账后回填 ShaderBinding 的 set/binding
		// push constant 块大小（字节）：0=无；全部 stage 共享、offset 固定 0（第一版最小集）
		uint32_t    pushConstantSize = 0;
	};

	struct Rect2D   { int32_t x = 0, y = 0; uint32_t width = 0, height = 0; };
	struct Viewport { float x = 0, y = 0, w = 0, h = 0, minDepth = 0.0f, maxDepth = 1.0f; };

	// 计算管线：Vulkan 规定每个计算管线恰有一个 Compute 阶段
	struct ComputePipelineDesc {
		ShaderStageDesc shader;
		// 描述符布局同样由该 SPIR-V 反射得出（无手工 bindings 字段）
		uint32_t pushConstantSize = 0; // 0=无
	};

#pragma endregion

#pragma region Barrier // 依赖边的直译出口

	struct ImageSubresourceRange {
		ImageSubresource base {};
		uint32_t mipLevelCount = 1;
		uint32_t layerCount = 1;
	};

	struct ImageBarrier {
		Image*               image = nullptr;
		ImageSubresourceRange range {};
		ImageLayout          oldLayout = ImageLayout::Undefined;
		ImageLayout          newLayout = ImageLayout::General;
		PipelineStage        srcStage = PipelineStage::None;
		PipelineStage        dstStage = PipelineStage::None;
		Access               srcAccess = Access::None;
		Access               dstAccess = Access::None;
		uint32_t             srcQueueFamily = kQueueFamilyIgnored; // 跨队列族所有权转移（QFOT）用；同族/单队列保持 IGNORED
		uint32_t             dstQueueFamily = kQueueFamilyIgnored;
	};

	struct BufferBarrier {
		Buffer*        buffer = nullptr;
		uint64_t       offset = 0;
		uint64_t       size = 0;   // 0 = 到末尾
		PipelineStage  srcStage = PipelineStage::None;
		PipelineStage  dstStage = PipelineStage::None;
		Access         srcAccess = Access::None;
		Access         dstAccess = Access::None;
		uint32_t       srcQueueFamily = kQueueFamilyIgnored; // 语义同 ImageBarrier
		uint32_t       dstQueueFamily = kQueueFamilyIgnored;
	};

#pragma endregion

#pragma region Rendering // dynamic rendering 的附件结构

	struct RenderingAttachment {
		Image*         image = nullptr;
		ImageSubresource view {};
		ImageLayout    layout = ImageLayout::ColorAttachment; // barrier compiler 先行转换后填入
		LoadOp         load = LoadOp::DontCare;
		StoreOp        store = StoreOp::Store;
		ClearValue     clear {};
	};

	struct RenderingInfo {
		const RenderingAttachment* colors = nullptr;   // 连续数组，长度 colorCount
		uint32_t                   colorCount = 0;
		const RenderingAttachment* depth = nullptr;     // 可空
		Rect2D                     renderArea {};
	};

#pragma endregion
}
