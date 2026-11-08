#pragma once
#include "rhi/RHIDesc.h"
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <array>
#include <functional>
#include <cstdint>

// 渲染图的数据描述层：资源/Pass 的 Desc、绑定结构与相关枚举。
namespace dEngine {

	// Host 段的执行上下文（定义在 RenderGraphAbstracted.h）：回调在此类型上做 GPU 侧同步
	class HostContext;

	// 导出 RHI 公共词表
	using rhi::ImageFormat;
	using rhi::ImageViewType;
	using rhi::LoadOp;
	using rhi::StoreOp;
	using rhi::DepthStencilValue;
	using rhi::ClearValue;
	using rhi::ImageSubresource;
	using rhi::ImageSubresourceRange;
	using rhi::ImageViewDesc;
	using rhi::SamplerDesc;
	using rhi::kMaxColorAttachments;
	using rhi::CompareOp;
	using rhi::CullMode;
	using rhi::ColorAttachmentState;
	using rhi::DepthStencilState;
	using rhi::BufferUsage;
	using rhi::IndexType;
	using rhi::PrimitiveTopology;
	using rhi::VertexFormat;
	using rhi::VertexInputBinding;
	using rhi::VertexAttribute;
	using rhi::ShaderStageDesc;

#pragma region ImageDesc

	struct ImageDesc {
		uint32_t      width = 0;
		uint32_t      height = 0;
		uint32_t      depth = 1;       // 仅 Dim3D 有效
		uint32_t      arrayLayers = 1; // 数组/Cube 层数（与 3D depth 是两个维度）
		uint32_t      mipLevels = 1;
		uint32_t      samples = 1;
		ImageFormat   format = ImageFormat::None;
		ImageViewType viewType = ImageViewType::Dim2D;
		// 跨帧存活：帧首闭环上帧末次使用，仅帧内资源保持 false。
		bool          crossFrame = false;
	};

#pragma endregion

#pragma region BufferDesc

	// 图侧 Buffer：创建位（usage）由描述显式给出。
	struct BufferDesc {
		uint64_t    size = 0;
		BufferUsage usage = BufferUsage::None; // 显式创建位（Vertex/Index/Uniform/Storage/Transfer*）
		bool        hostVisible = false;      // 决定 VMA 内存类型
		// 跨帧存活：帧首补内存依赖（无布局语义）。
		bool        crossFrame = false;
	};

#pragma endregion

#pragma region Common

	// 逐次访问用途（非资源属性），与 RHI 创建能力位是正交两条轴。
	enum class Usage {
		Auto,
		ColorAttachment, DepthStencilAttachment, ResolveSrc, ResolveDst,
		SampledRead, StorageRead, StorageWrite,
		UniformRead,
		VertexInputRead, IndexRead,
		IndirectRead,                 // 间接参数缓冲读取（dispatch indirect；映射 DRAW_INDIRECT 阶段）
		TransferRead, TransferWrite,  // 拷贝通道：源读 / 目的写（Buffer 或 Image）
		HostWrite,                    // Host 节点的 CPU 写（映射 HOST 阶段，仅 Buffer）
		HostRead,                     // Host 节点的 CPU 读（映射 HOST 阶段 + HOST_READ，仅 Buffer）
		PresentSrc,                   // 呈现收尾（布局转 PresentSrc）
		MuxPrepare,                   // mux 候选准备：仅做布局转到 ShaderReadOnly（无内存读写）
		MuxSelect                     // mux 输出选择：虚拟写，不产生布局转换（状态继承准备布局）
	};

	// 依赖边的 use 记录：资源 × 用途，barrier compiler 的输入。
	struct ResourceUse {
		uint32_t resourceId = 0;
		Usage    usage = Usage::Auto;
		// 反射回填：该资源在这条边上的实际 shader 阶段并集（空 = 未知，barrier 取保守并集）
		rhi::ShaderStageMask stages = rhi::ShaderStageMask::None;
		// 可选：该边实际触达的子资源范围；未填时按节点 view / 操作语义推导
		std::optional<rhi::ImageSubresourceRange> range;
	};

	struct ViewportDesc {
		bool  autoFromColor = true; // true: 从首个颜色附件推导尺寸，忽略矩形
		float x = 0, y = 0, w = 0, h = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
	};

#pragma endregion

#pragma region PassDesc

	// 附件绑定。resourceName 声明期用户填；resourceId 由 ProcessRegister 独占回填
	struct AttachmentBinding {
		std::string             resourceName;
		std::optional<uint32_t> resourceId;
		Usage                   usage = Usage::Auto; // Auto: 按资源 format 推断（深度格式→DepthStencil，否则 Color）
		uint32_t                slot = 0;            // 仅 ColorAttachment 有语义；depth/resolve 忽略
		LoadOp                  load = LoadOp::DontCare;
		StoreOp                 store = StoreOp::Store;
		ClearValue              clear {};
		// 可选：附件实际使用的子资源范围；空 = 节点 view
		std::optional<rhi::ImageSubresourceRange> range;
	};

	// shader 资源绑定：resourceName 为 SPIR-V 变量名；graphResourceName 为图资源名（空则回落）
	struct ShaderBinding {
		// SPIR-V 变量名：反射对账用，模块实例化/作用域改写时保持不变
		std::string             resourceName;
		// 图资源名：参与图内对账；空表示回落 resourceName（模块实例化时写入）
		std::string             graphResourceName;
		std::optional<uint32_t> resourceId;
		Usage                   usage = Usage::Auto; // Auto: 图像推断 SampledRead，Buffer 推断 UniformRead
		// Buffer 专用：描述符的窗口（0 长度 = 整个 buffer）。图像绑定忽略这两个字段
		uint64_t                offset = 0;
		uint64_t                range = 0;
		// 图像专用：自定义采样状态（空 = 设备默认采样器）；采样器按描述去重共享
		std::optional<SamplerDesc> sampler;
		// 反射回填（RealizePass 对账成功后写入；注册期不填）——与 resourceId 的两阶段模式同构
		std::optional<uint32_t> set;
		std::optional<uint32_t> binding;
		// 反射回填：数组绑定的元素数（层展开的同步范围据此计算）
		std::optional<uint32_t> arrayCount;

		// 常用预设：一行完成绑定声明，其余字段（offset/range 等）可再改
		static ShaderBinding Uniform(std::string name) {
			ShaderBinding b;
			b.resourceName = std::move(name);
			b.usage = Usage::UniformRead;
			return b;
		}
		static ShaderBinding Sampled(std::string name,
			std::optional<SamplerDesc> sampler = std::nullopt) {
			ShaderBinding b;
			b.resourceName = std::move(name);
			b.usage = Usage::SampledRead;
			b.sampler = sampler;
			return b;
		}
		static ShaderBinding StorageRead(std::string name) {
			ShaderBinding b;
			b.resourceName = std::move(name);
			b.usage = Usage::StorageRead;
			return b;
		}
		static ShaderBinding StorageWrite(std::string name) {
			ShaderBinding b;
			b.resourceName = std::move(name);
			b.usage = Usage::StorageWrite;
			return b;
		}
	};

	// 管线状态：shader 与固定功能；附件格式与描述符布局由引擎推导。
	struct PipelineDesc {
		std::vector<ShaderStageDesc> shaders; // 各阶段 SPIR-V 路径（离线 glslc 产出；组合合法性由 RHI 校验）

		// 顶点输入：两向量留空 = 按反射派生单流交错布局。
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		// 细分管线：每个 patch 的控制点数；启用 Tess 阶段时 topology 须为 PatchList
		uint32_t tessPatchControlPoints = 3;
		std::vector<VertexInputBinding> vertexBindings;
		std::vector<VertexAttribute>    vertexAttributes;

		// 固定功能（实际深度开关还受有无深度附件约束，无深度时 RHI 自动关闭）
		CullMode  cullMode = CullMode::Back;
		// 逐附件颜色状态：空 = 全默认（blend 关、写满 RGBA）
		std::vector<ColorAttachmentState> colorStates;
		DepthStencilState depthStencil {};
		std::array<float, 4> blendConstants { 0.0f, 0.0f, 0.0f, 0.0f };
		uint32_t  pushConstantSize = 0;          // 0=无；全部 stage 共享、offset 固定 0
	};

	// 顶点流：声明资源与起始偏移，序号即管线的 binding index。
	struct VertexStreamBinding {
		std::string             resourceName; // 须是图中注册的 Buffer 节点
		std::optional<uint32_t> resourceId;   // ProcessRegister 回填
		uint64_t                offset = 0;
	};

	// 索引流：offset 与索引类型是录制参数（vkCmdBindIndexBuffer 的入参），不是管线状态
	struct IndexStreamBinding {
		std::string             resourceName;
		std::optional<uint32_t> resourceId;   // ProcessRegister 回填
		uint64_t                offset = 0;
		IndexType               type = IndexType::UInt32;
	};

	// 绘制数量来源：执行参数，Direct 走 CPU，Indirect 从 buffer 读。
	struct DrawSource {
		enum Kind { Direct, Indirect };
		Kind        kind = Direct;
		std::string indirectBuffer;    // Indirect：参数 Buffer 的图资源名
		uint64_t    indirectOffset = 0;
		uint32_t    count = 0;         // Direct：顶点数 / 索引数；Indirect：draw 条目数
		uint32_t    first = 0;         // Direct：firstVertex / firstIndex
		int32_t     vertexOffset = 0;  // Direct + 索引流
		uint32_t    instanceCount = 1;
	};

	// 静态纹理堆容量：固定 set 0 / binding 0 的 sampler2D 数组长度（引擎级常量）
	inline constexpr uint32_t kStaticTextureHeapCapacity = 16;

	struct RenderPassDesc {
		std::vector<AttachmentBinding> attachments;    // 顺序无语义，slot 显式
		std::vector<ShaderBinding>     shaderBindings; // 与 attachments 平级
		// 固定功能输入通道（非描述符绑定）：资源身份进拓扑读边，布局在 pipeline，数量在 DrawSource
		std::vector<VertexStreamBinding> vertexStreams;
		std::optional<IndexStreamBinding> indexStream;
		ViewportDesc                   viewport {};
		PipelineDesc                   pipeline {};    // 管线状态（shader/blend/depth/cull/vertex layout）
		// push constant 数据（字节）：非空时录制阶段按 pipeline.pushConstantSize 推送
		std::vector<uint8_t>           pushConstantData;
		// 绑定静态纹理堆到 set 0，动态资源从 set 1 起。
		bool                           bindStaticHeap = false;
	};

	// 计算 Pass：单 compute shader + 工作组数，绑定按名对账。
	struct ComputePassDesc {
		std::vector<ShaderBinding> shaderBindings;
		ShaderStageDesc            shader;
		uint32_t                   pushConstantSize = 0;
		uint32_t                   groupCountX = 1;
		uint32_t                   groupCountY = 1;
		uint32_t                   groupCountZ = 1;
		// 间接派发：Buffer 须在本 pass 读集，工作组数取自它。
		std::string                indirectBuffer;
		std::optional<uint32_t>    indirectBufferId; // ProcessRegister 回填
		uint64_t                   indirectOffset = 0;
		// push constant 数据（字节）：非空时录制阶段按 pushConstantSize 推送
		std::vector<uint8_t>       pushConstantData;
		// 绑定静态纹理堆到 set 0（与 RenderPassDesc 同语义）
		bool                       bindStaticHeap = false;
	};

	// Host Pass：纯 CPU 回调（不录命令），写边记为 HostWrite。
	struct HostPassDesc {
		std::function<void(HostContext&)> callback;
	};

	// 传输 Pass：单次拷贝，两端即拓扑读/写边，方向决定资源类型。
	enum class TransferKind {
		BufferToImage,
		ImageToBuffer,
		BufferToBuffer,
		ImageToImage,
		ClearImage
	};

	struct TransferPassDesc {
		TransferKind            kind = TransferKind::BufferToImage;
		std::string             sourceName;
		std::optional<uint32_t> sourceId; // ProcessRegister 回填
		std::string             destName;
		std::optional<uint32_t> destId;   // ProcessRegister 回填
		// Buffer 窗口：0 表示整段，byteSize 仅 BufferToBuffer 使用。
		uint64_t                srcOffset = 0;
		uint64_t                dstOffset = 0;
		uint64_t                byteSize = 0;
		// ImageToImage 专用：false = vkCmdCopyImage（尺寸须一致），true = vkCmdBlitImage（可缩放）
		bool                    blit = false;
		bool                    linearFilter = true; // blit 过滤：true=线性（需格式支持），false=最近邻
		// 区域拷贝：为空时按旧的整图 mip0 全层语义
		std::vector<rhi::BufferImageCopyRegion> bufferImageRegions;
		std::vector<rhi::ImageCopyRegion>        imageRegions;
		std::vector<rhi::ImageBlitRegion>        blitRegions;
		// ClearImage 专用：清场范围与值
		rhi::ImageSubresourceRange clearRange { {}, 0, 0 };
		rhi::ClearValue            clearValue {};
	};

	// Mux Pass：零命令选择点，输出为 Borrow-later，按帧槽 retarget。
	struct MuxPassDesc {
		std::vector<std::string> alternatives; // 顺序即选择索引
		// ProcessRegister 回填：候选解析到的资源 id（版本化后不再按名查表）
		std::vector<uint32_t> alternativeIds;
	};

	// Present Pass：只做布局转换到 PresentSrc，不录绘制命令。
	struct PresentPassDesc {
	};

#pragma endregion

#pragma region DescAlias
	using PassDesc = std::variant<std::monostate, RenderPassDesc, ComputePassDesc,
		HostPassDesc, TransferPassDesc, MuxPassDesc, PresentPassDesc>;
	using ResourceDesc = std::variant<std::monostate, ImageDesc, BufferDesc>;
#pragma endregion
}
