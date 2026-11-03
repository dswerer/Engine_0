#pragma once
#include "rhi/RHIDesc.h"   // 复用 RHI 公共词表（ImageFormat/ImageViewType/LoadOp/StoreOp/ClearValue/ImageSubresource/kMaxColorAttachments）
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
	using rhi::kMaxColorAttachments;
	using rhi::CompareOp;
	using rhi::CullMode;
	using rhi::BufferUsage;
	using rhi::IndexType;
	using rhi::PrimitiveTopology;
	using rhi::VertexFormat;
	using rhi::VertexInputBinding;
	using rhi::VertexAttribute;
	using rhi::ShaderStageDesc;

#pragma region ImageDesc // 图像描述

	struct ImageDesc {
		uint32_t      width = 0;
		uint32_t      height = 0;
		uint32_t      depth = 1;       // 仅 Dim3D 有效
		uint32_t      arrayLayers = 1; // 数组/Cube 层数（与 3D depth 是两个维度）
		uint32_t      mipLevels = 1;
		uint32_t      samples = 1;
		ImageFormat   format = ImageFormat::None;
		ImageViewType viewType = ImageViewType::Dim2D;
		// 跨帧存活：图把帧首同步闭环成「上帧末次使用 -> 本帧首次使用」，
		// 不再每帧从 initialLayout 重新起步（否则会丢弃上帧内容）；仅帧内资源保持 false
		bool          crossFrame = false;
	};

#pragma endregion

#pragma region BufferDesc // 缓冲描述

	// 图侧 Buffer 节点：创建位由描述显式给出。
	// 图像能从 shader 绑定派生 usage，是因为 Usage 里已有采样/存储语义；
	// 缓冲目前没有顶点/索引/统一缓冲的用法枚举，等 DrawBinding 落地后再改为按边派生
	struct BufferDesc {
		uint64_t    size = 0;
		BufferUsage usage = BufferUsage::None; // 显式创建位（Vertex/Index/Uniform/Storage/Transfer*）
		bool        hostVisible = false;      // 决定 VMA 内存类型
		// 跨帧存活：帧首按「上帧末次访问 -> 本帧首次访问」补内存依赖（无布局语义）
		bool        crossFrame = false;
	};

#pragma endregion

#pragma region Common // 公共内容

	// 逐次访问的用途（非资源属性）。Buffer 的 shader 通道用 UniformRead / StorageRead / StorageWrite；
	// 顶点/索引流是固定功能输入，也不进描述符布局，但其读语义仍需记录在拓扑读边里
	// （VertexInputRead / IndexRead），供 barrier compiler 映射 VERTEX_INPUT / INDEX_READ。
	// 注意与 RHI 的创建能力位 ImageUsage/BufferUsage 是正交的两根轴（后者在 rhi 命名空间）。
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
		PresentSrc                    // 呈现收尾（布局转 PresentSrc）
	};

	// 依赖边的 use 记录：资源 × 用途（barrier compiler 的输入粒度）。
	// usage 为渲染语义；Vulkan 侧 stage/access 由 compiler 按 usage 查表产出，不在此存储
	struct ResourceUse {
		uint32_t resourceId = 0;
		Usage    usage = Usage::Auto;
	};

	struct ViewportDesc {
		bool  autoFromColor = true; // true: 从首个颜色附件推导尺寸，忽略矩形
		float x = 0, y = 0, w = 0, h = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
	};

#pragma endregion

#pragma region PassDesc // Pass 描述

	// 附件绑定。resourceName 声明期用户填；resourceId 由 ProcessRegister 独占回填
	struct AttachmentBinding {
		std::string             resourceName;
		std::optional<uint32_t> resourceId;
		Usage                   usage = Usage::Auto; // Auto: 按资源 format 推断（深度格式→DepthStencil，否则 Color）
		uint32_t                slot = 0;            // 仅 ColorAttachment 有语义；depth/resolve 忽略
		LoadOp                  load = LoadOp::DontCare;
		StoreOp                 store = StoreOp::Store;
		ClearValue              clear {};
	};

	// shader 资源绑定，与 AttachmentBinding 平级。
	// 匹配约定：shader 变量名 == 图资源名——RHI 反射产出管线布局后，RealizePass 按名对账
	struct ShaderBinding {
		std::string             resourceName;
		std::optional<uint32_t> resourceId;
		Usage                   usage = Usage::Auto; // Auto: 图像推断 SampledRead，Buffer 推断 UniformRead
		// Buffer 专用：描述符的窗口（0 长度 = 整个 buffer）。图像绑定忽略这两个字段
		uint64_t                offset = 0;
		uint64_t                range = 0;
		// 反射回填（RealizePass 对账成功后写入；注册期不填）——与 resourceId 的两阶段模式同构
		std::optional<uint32_t> set;
		std::optional<uint32_t> binding;
	};

	// 管线状态描述：shader 与固定功能状态，与附件布局（attachments/viewport）分离。
	// 两类信息不在此处由用户填写：
	//   附件格式签名（colorFormats/depthFormat）—— Realize 时由图从附件按 slot 推导；
	//   描述符布局 —— 由 RHI 对 SPIR-V 反射得出，RealizePass 按「变量名 == 资源名」对账回填 set/binding
	struct PipelineDesc {
		std::vector<ShaderStageDesc> shaders; // 各阶段 SPIR-V 路径（离线 glslc 产出；组合合法性由 RHI 校验）

		// 顶点输入布局（管线状态）：绑定/属性直接复用 RHI 的无后端词表版结构。
		// 两向量均留空 = 缺省：按 Vertex 阶段反射派生单流交错布局（字段顺序须与 location 顺序一致）；
		// shader 无用户输入时为空布局（gl_VertexIndex 路径）
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		std::vector<VertexInputBinding> vertexBindings;
		std::vector<VertexAttribute>    vertexAttributes;

		// 固定功能（与 RHI 最小集对齐；实际深度开关还受有无深度附件约束，无深度时 RHI 自动关闭）
		bool      depthTestEnable  = true;
		bool      depthWriteEnable = true;
		bool      blendEnable      = false;      // 单附件统一，逐附件后置
		CompareOp depthCompareOp   = CompareOp::Less;
		CullMode  cullMode         = CullMode::Back;
		uint32_t  pushConstantSize = 0;          // 0=无；全部 stage 共享、offset 固定 0
	};

	// 顶点流：声明「用哪个 Buffer 资源、从哪个偏移开始」，序号即管线的 binding index。
	// stride/属性格式属于布局，在 PipelineDesc 的 vertexBindings/vertexAttributes 里
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

	// 绘制数量来源（执行参数，非声明）：注册期不写死，宿主每帧经 RenderPass::SetDraw 提供。
	// Direct = 数值走 CPU；Indirect = 参数在 buffer 里（有索引流则走 indexed indirect）
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

	struct RenderPassDesc {
		std::vector<AttachmentBinding> attachments;    // 顺序无语义，slot 显式
		std::vector<ShaderBinding>     shaderBindings; // 与 attachments 平级
		// 固定功能输入通道（非描述符绑定）：资源身份进拓扑读边，布局在 pipeline，数量在 DrawSource
		std::vector<VertexStreamBinding> vertexStreams;
		std::optional<IndexStreamBinding> indexStream;
		ViewportDesc                   viewport {};
		PipelineDesc                   pipeline {};    // 管线状态（shader/blend/depth/cull/vertex layout）
	};

	// 计算 Pass：单 compute shader + 工作组数；描述符布局同样由 SPIR-V 反射产出，
	// shaderBindings 按「变量名 == 资源名」对账（与 RenderPassDesc 同约定）
	struct ComputePassDesc {
		std::vector<ShaderBinding> shaderBindings;
		ShaderStageDesc            shader;
		uint32_t                   pushConstantSize = 0;
		uint32_t                   groupCountX = 1;
		uint32_t                   groupCountY = 1;
		uint32_t                   groupCountZ = 1;
		// 间接派发：indirectBuffer 非空时工作组数从该 Buffer 读取（忽略上面三个数值）。
		// 参数 Buffer 须登记在本 pass 的读集（拓扑边 usage=IndirectRead），barrier 才会把它排到正确位置
		std::string                indirectBuffer;
		std::optional<uint32_t>    indirectBufferId; // ProcessRegister 回填
		uint64_t                   indirectOffset = 0;
	};

	// Host Pass：纯 CPU 回调（解码/上传等宿主机工作），不录任何命令。
	// 它经 RegisterPass 的 write 集声明「往哪个 Buffer 写」，拓扑边记为 HostWrite；
	// 回调是执行参数，可在 Realize 后经 HostPass::SetCallback 覆盖 desc 中的声明。
	// 回调参数是同步上下文：业务代码前 WaitForGpu()、业务代码后 SignalGpu()
	struct HostPassDesc {
		std::function<void(HostContext&)> callback;
	};

	// 传输 Pass：单次拷贝。方向决定两端资源类型：
	//   BufferToImage：Buffer -> Image（上传；源读 TransferRead、目的写 TransferWrite）
	//   ImageToBuffer：Image -> Buffer（回读；目的 Buffer 须 hostVisible + TransferDst）
	//   BufferToBuffer：Buffer -> Buffer（中转/压实）
	//   ImageToImage：Image -> Image（同格式；blit=false 为等尺寸拷贝，true 为可缩放 blit）
	// 两端即拓扑边：source 进读边、dest 进写边；图像侧布局由 compiler 按 TransferRead/Write 推导
	enum class TransferKind {
		BufferToImage,
		ImageToBuffer,
		BufferToBuffer,
		ImageToImage
	};

	struct TransferPassDesc {
		TransferKind            kind = TransferKind::BufferToImage;
		std::string             sourceName;
		std::optional<uint32_t> sourceId; // ProcessRegister 回填
		std::string             destName;
		std::optional<uint32_t> destId;   // ProcessRegister 回填
		// Buffer 侧窗口：0 表示整段。BufferToImage 用 srcOffset；ImageToBuffer 用 dstOffset；
		// BufferToBuffer 两者都用；byteSize 仅 BufferToBuffer 使用（0 = 取两者较小值）
		uint64_t                srcOffset = 0;
		uint64_t                dstOffset = 0;
		uint64_t                byteSize = 0;
		// ImageToImage 专用：false = vkCmdCopyImage（尺寸须一致），true = vkCmdBlitImage（可缩放）
		bool                    blit = false;
		bool                    linearFilter = true; // blit 过滤：true=线性（需格式支持），false=最近邻
	};

	// Present Pass：呈现收尾。读边声明 presentable image（映射为 Usage::PresentSrc），
	// barrier compiler 据此产出 ColorAttachment -> PresentSrc 的布局转换；不录任何绘制命令。
	// 交换链的 acquire 等待与渲染完成信号由 SubmitGraph 的 FrameSync 挂到本节点所在的批
	struct PresentPassDesc {
	};

#pragma endregion

#pragma region DescAlias // 注册接口别名（Desc 变体）
	using PassDesc = std::variant<std::monostate, RenderPassDesc, ComputePassDesc,
		HostPassDesc, TransferPassDesc, PresentPassDesc>;
	using ResourceDesc = std::variant<std::monostate, ImageDesc, BufferDesc>;
#pragma endregion
}
