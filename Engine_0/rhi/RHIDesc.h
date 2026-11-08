#pragma once
#include <cstdint>
#include <string>
#include <array>
#include <variant>
#include <vector>

// RHI 描述层：后端无关的词表与参数结构，仅依赖标准库，不含 Vulkan 头。
namespace dEngine::rhi {

	class Image;
	class Buffer;

#pragma region Vocabulary

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

	// 采样状态（与图像解耦：同一图像可按不同寻址/过滤方式绑定）
	enum class Filter { Nearest, Linear };
	enum class SamplerMipmapMode { Nearest, Linear };
	enum class SamplerAddressMode { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };

	struct SamplerDesc {
		Filter              magFilter = Filter::Linear;
		Filter              minFilter = Filter::Linear;
		SamplerMipmapMode   mipmapMode = SamplerMipmapMode::Linear;
		SamplerAddressMode  addressU = SamplerAddressMode::Repeat;
		SamplerAddressMode  addressV = SamplerAddressMode::Repeat;
		SamplerAddressMode  addressW = SamplerAddressMode::Repeat;
		bool                anisotropyEnable = false;
		float               maxAnisotropy = 1.0f;
		float               mipLodBias = 0.0f;
		float               minLod = 0.0f;
		float               maxLod = 1000.0f; // 与 VK_LOD_CLAMP_NONE 同值

		// 常用预设：三轴向统一寻址（默认 ClampToEdge），mipmap 固定 Nearest；逐轴不同可先取预设再改字段
		static SamplerDesc Linear(SamplerAddressMode address = SamplerAddressMode::ClampToEdge) {
			SamplerDesc s;
			s.magFilter = s.minFilter = Filter::Linear;
			s.mipmapMode = SamplerMipmapMode::Nearest;
			s.addressU = s.addressV = s.addressW = address;
			return s;
		}
		static SamplerDesc Nearest(SamplerAddressMode address = SamplerAddressMode::ClampToEdge) {
			SamplerDesc s;
			s.magFilter = s.minFilter = Filter::Nearest;
			s.mipmapMode = SamplerMipmapMode::Nearest;
			s.addressU = s.addressV = s.addressW = address;
			return s;
		}
	};

	enum class LoadOp  { Load, Clear, DontCare };
	enum class StoreOp { Store, DontCare };

	struct DepthStencilValue { float depth = 1.0f; uint32_t stencil = 0; };
	struct ClearValue {
		std::variant<std::array<float, 4>, DepthStencilValue> value;
	};

	// 绑定处的子资源选择（单个 mip / 单层，供附件与数组元素起点使用）
	struct ImageSubresource {
		uint32_t mipLevel = 0;
		uint32_t layer = 0;
	};

	// 子资源范围（barrier 与视图创建用）；count 为 0 时由所在图像补齐到末尾
	struct ImageSubresourceRange {
		ImageSubresource base {};
		uint32_t mipLevelCount = 1;
		uint32_t layerCount = 1;
	};

	// 归一化范围：计数 0 补齐到末尾，越界返回 false。
	inline bool NormalizeImageSubresourceRange(ImageSubresourceRange& range,
		uint32_t mipLevels, uint32_t arrayLayers) {
		const uint32_t mips = mipLevels == 0 ? 1 : mipLevels;
		const uint32_t layers = arrayLayers == 0 ? 1 : arrayLayers;
		if (range.base.mipLevel >= mips || range.base.layer >= layers)
			return false;
		const uint32_t remainMips = mips - range.base.mipLevel;
		const uint32_t remainLayers = layers - range.base.layer;
		if (range.mipLevelCount == 0) range.mipLevelCount = remainMips;
		if (range.layerCount == 0) range.layerCount = remainLayers;
		return range.mipLevelCount <= remainMips && range.layerCount <= remainLayers;
	}

	// outer 是否完整覆盖 inner（两者须已归一化）。
	inline bool ImageSubresourceRangeContains(const ImageSubresourceRange& outer,
		const ImageSubresourceRange& inner) {
		if (inner.mipLevelCount == 0 || inner.layerCount == 0)
			return true;
		if (inner.base.mipLevel < outer.base.mipLevel || inner.base.layer < outer.base.layer)
			return false;
		return inner.base.mipLevel + inner.mipLevelCount
				<= outer.base.mipLevel + outer.mipLevelCount
			&& inner.base.layer + inner.layerCount
				<= outer.base.layer + outer.layerCount;
	}

	// 视图创建参数：范围 + 可选格式重解释 + 可选视图类型覆盖
	struct ImageViewDesc {
		ImageSubresourceRange range {};
		ImageFormat    format = ImageFormat::None;    // None = 继承图像格式
		ImageViewType  viewType = ImageViewType::None; // None = 按图像与范围推导
	};

	// buffer <-> image 区域拷贝；subresource 的 mip 级数须为 1
	struct BufferImageCopyRegion {
		uint64_t              bufferOffset = 0;
		uint32_t              bufferRowLength = 0;    // 0 = 按 extent 紧打包
		uint32_t              bufferImageHeight = 0;  // 0 = 按 extent 紧打包
		ImageSubresourceRange subresource { {}, 1, 1 };
		int32_t               imageOffsetX = 0;
		int32_t               imageOffsetY = 0;
		int32_t               imageOffsetZ = 0;
		uint32_t              imageExtentWidth = 0;   // 0 = 该 mip 完整宽度
		uint32_t              imageExtentHeight = 0;  // 0 = 该 mip 完整高度
		uint32_t              imageExtentDepth = 0;   // 0 = 该 mip 完整深度
	};

	// image <-> image 区域拷贝；两端 mip 级数须为 1
	struct ImageCopyRegion {
		ImageSubresourceRange srcSubresource { {}, 1, 1 };
		ImageSubresourceRange dstSubresource { {}, 1, 1 };
		int32_t               srcOffsetX = 0;
		int32_t               srcOffsetY = 0;
		int32_t               srcOffsetZ = 0;
		int32_t               dstOffsetX = 0;
		int32_t               dstOffsetY = 0;
		int32_t               dstOffsetZ = 0;
		uint32_t              extentWidth = 0;        // 0 = 源 mip 完整宽度
		uint32_t              extentHeight = 0;
		uint32_t              extentDepth = 0;
	};

	// image blit 区域；两端 mip 级数须为 1
	struct ImageBlitRegion {
		ImageSubresourceRange srcSubresource { {}, 1, 1 };
		ImageSubresourceRange dstSubresource { {}, 1, 1 };
		int32_t               srcOffsetX0 = 0;
		int32_t               srcOffsetY0 = 0;
		int32_t               srcOffsetZ0 = 0;
		int32_t               srcOffsetX1 = 0;
		int32_t               srcOffsetY1 = 0;
		int32_t               srcOffsetZ1 = 0;
		int32_t               dstOffsetX0 = 0;
		int32_t               dstOffsetY0 = 0;
		int32_t               dstOffsetZ0 = 0;
		int32_t               dstOffsetX1 = 0;
		int32_t               dstOffsetY1 = 0;
		int32_t               dstOffsetZ1 = 0;
	};

	// 逐描述符绑定的 shader 阶段（反射产出；用于 barrier 精确到实际访问阶段）
	enum class ShaderStageMask : uint32_t {
		None        = 0,
		Vertex      = 1u << 0,
		TessControl = 1u << 1,
		TessEval    = 1u << 2,
		Geometry    = 1u << 3,
		Fragment    = 1u << 4,
		Task        = 1u << 5,
		Mesh        = 1u << 6,
		Compute     = 1u << 7
	};
	inline constexpr ShaderStageMask operator|(ShaderStageMask a, ShaderStageMask b) {
		return static_cast<ShaderStageMask>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	inline constexpr uint32_t kMaxColorAttachments = 8;

#pragma endregion

#pragma region Sync

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
		VertexInput        = 1ull << 11, // 固定功能输入：顶点属性/索引读取
		DrawIndirect       = 1ull << 12, // 间接参数读取（draw indirect / dispatch indirect）
		TessControlShader  = 1ull << 13, // 细分控制阶段
		TessEvalShader     = 1ull << 14, // 细分求值阶段
		GeometryShader     = 1ull << 15  // 几何着色阶段
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
		HostWrite             = 1ull << 12,
		HostRead              = 1ull << 13,
		IndirectCommandRead   = 1ull << 14
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

#pragma region ResourceCreate

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
	};

	struct BufferCreateInfo {
		uint64_t    size = 0;
		BufferUsage usage = BufferUsage::None;
		bool        hostVisible = false;  // 决定 VMA 内存类型
	};

#pragma endregion

#pragma region Pipeline

	enum class CompareOp { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
	enum class CullMode  { None, Front, Back };
	enum class IndexType { UInt16, UInt32 };

	// 逐附件颜色混合状态
	enum class BlendFactor : uint8_t {
		Zero, One,
		SrcColor, OneMinusSrcColor,
		DstColor, OneMinusDstColor,
		SrcAlpha, OneMinusSrcAlpha,
		DstAlpha, OneMinusDstAlpha,
		ConstantColor, OneMinusConstantColor,
		ConstantAlpha, OneMinusConstantAlpha,
		SrcAlphaSaturate
	};
	enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
	enum class ColorWriteMask : uint8_t { None = 0, R = 1, G = 2, B = 4, A = 8, All = 15 };

	inline ColorWriteMask operator|(ColorWriteMask a, ColorWriteMask b) {
		return static_cast<ColorWriteMask>(
			static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
	}
	inline ColorWriteMask& operator|=(ColorWriteMask& a, ColorWriteMask b) {
		a = a | b;
		return a;
	}

	enum class StencilOp : uint8_t {
		Keep, Zero, Replace, IncrClamp, DecrClamp, Invert, IncrWrap, DecrWrap
	};

	struct ColorAttachmentState {
		bool            enableBlend = false;
		BlendFactor     srcColor = BlendFactor::One;
		BlendFactor     dstColor = BlendFactor::Zero;
		BlendFactor     srcAlpha = BlendFactor::One;
		BlendFactor     dstAlpha = BlendFactor::Zero;
		BlendOp         colorOp = BlendOp::Add;
		BlendOp         alphaOp = BlendOp::Add;
		ColorWriteMask  writeMask = ColorWriteMask::All;
	};

	// 单面模板状态（front / back 独立）；reference 为静态值。
	struct StencilFaceState {
		CompareOp compareOp = CompareOp::Always;
		StencilOp failOp = StencilOp::Keep;
		StencilOp passOp = StencilOp::Keep;
		StencilOp depthFailOp = StencilOp::Keep;
		uint32_t  reference = 0;
		uint32_t  compareMask = 0xFF;
		uint32_t  writeMask = 0xFF;
	};

	struct DepthStencilState {
		bool      depthTestEnable = true;
		bool      depthWriteEnable = true;
		CompareOp depthCompareOp = CompareOp::Less;
		bool      depthBiasEnable = false;
		float     depthBiasConstant = 0.0f;
		float     depthBiasSlope = 0.0f;
		float     depthBiasClamp = 0.0f;
		bool      depthBoundsTestEnable = false;
		float     minDepthBounds = 0.0f;
		float     maxDepthBounds = 1.0f;
		bool      stencilTestEnable = false;
		StencilFaceState front {};
		StencilFaceState back {};
	};

	// 可编程着色器阶段；Tess/Geometry 已支持，Task/Mesh 仍为预留。
	enum class ShaderStage { Vertex, Fragment, TessControl, TessEval, Geometry, Task, Mesh, Compute };

	// 单个着色器阶段：SPIR-V 二进制路径（离线编译产出，如 glslc），RHI 直接读取
	struct ShaderStageDesc {
		ShaderStage stage;
		std::string spirvPath;
	};

	// 反射产出的资源类别（亦可手工声明时使用）
	enum class ResourceType { SampledImage, StorageImage, UniformBuffer, StorageBuffer };

	// 单条描述符声明：反射产出；图层按「变量名 == 资源名」对账。
	struct DescriptorBinding {
		std::string  name;
		uint32_t     set = 0;
		uint32_t     binding = 0;
		ResourceType type = ResourceType::SampledImage;
		uint32_t     arrayCount = 1;
		// 反射累积的 stage 并集：barrier 据此把资源同步到真正访问它的阶段
		ShaderStageMask stages = ShaderStageMask::None;
		// shader 是否对该 storage 资源声明了 readonly（NonWritable）
		bool nonWritable = false;
	};

	// 图元拓扑（PatchList 仅用于细分管线）
	enum class PrimitiveTopology { PointList, LineList, LineStrip, TriangleList, TriangleStrip, PatchList };

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
		uint32_t    samples = 1;                       // 采样数：须与 pass 的所有附件一致（MSAA 来自颜色附件）

		// 固定功能（第一版最小集）
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		// 细分管线专用：每个 patch 的控制点数；启用 Tess 阶段时 topology 须为 PatchList
		uint32_t tessPatchControlPoints = 3;
		// 顶点输入布局：两向量均空时由反射派生单流交错；任一非空则整条直译。
		std::vector<VertexInputBinding> vertexBindings;
		std::vector<VertexAttribute>    vertexAttributes;
		CullMode  cullMode = CullMode::Back;
		// 逐附件颜色状态：空则按默认；非空时数量必须等于 colorAttachmentCount。
		std::vector<ColorAttachmentState> colorStates;
		// 深度/模板完整状态（无深度附件时由 RHI 自动关闭）
		DepthStencilState depthStencil {};
		// blend 常量：仅 ConstantColor / ConstantAlpha 系列因子使用
		std::array<float, 4> blendConstants { 0.0f, 0.0f, 0.0f, 0.0f };
		// push constant 块大小（字节）：0=无；全部 stage 共享、offset 固定 0。
		uint32_t    pushConstantSize = 0;
	};

	struct Rect2D   { int32_t x = 0, y = 0; uint32_t width = 0, height = 0; };
	struct Viewport { float x = 0, y = 0, w = 0, h = 0, minDepth = 0.0f, maxDepth = 1.0f; };

	// 计算管线：Vulkan 规定每个计算管线恰有一个 Compute 阶段
	struct ComputePipelineDesc {
		ShaderStageDesc shader;
		uint32_t pushConstantSize = 0; // 0=无
	};

#pragma endregion

#pragma region Barrier

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

#pragma region Rendering

	struct RenderingAttachment {
		Image*         image = nullptr;
		ImageSubresourceRange range { {}, 1, 1 };
		ImageLayout    layout = ImageLayout::ColorAttachment; // barrier compiler 先行转换后填入
		LoadOp         load = LoadOp::DontCare;
		StoreOp        store = StoreOp::Store;
		ClearValue     clear {};
		// MSAA 解算目标（只对颜色附件有效；nullptr = 不解算）。源即 image 本身
		Image*         resolveImage = nullptr;
		ImageSubresourceRange resolveRange { {}, 1, 1 };
		ImageLayout    resolveLayout = ImageLayout::ColorAttachment;
	};

	struct RenderingInfo {
		const RenderingAttachment* colors = nullptr;   // 连续数组，长度 colorCount
		uint32_t                   colorCount = 0;
		const RenderingAttachment* depth = nullptr;     // 可空
		Rect2D                     renderArea {};
		uint32_t                   layerCount = 1;      // 分层渲染的层数（所有附件须一致）
	};

#pragma endregion
}
