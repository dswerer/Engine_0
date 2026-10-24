#pragma once
#include "rhi/RHIDesc.h"   // 复用 RHI 公共词表（ImageFormat/ImageViewType/LoadOp/StoreOp/ClearValue/ImageSubresource/kMaxColorAttachments）
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <array>
#include <cstdint>

// 渲染图的数据描述层：资源/Pass 的 Desc、绑定结构与相关枚举。
namespace dEngine {

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
	};

#pragma endregion

#pragma region Common // 公共内容

	// 逐次访问的用途（非资源属性）。Buffer 相关 usage 等 Buffer 落地时再补；
	// 注意与 RHI 的创建能力位 ImageUsage/BufferUsage 是正交的两根轴（后者在 rhi 命名空间）。
	enum class Usage {
		Auto,
		ColorAttachment, DepthStencilAttachment, ResolveSrc, ResolveDst,
		SampledRead, StorageRead, StorageWrite
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
		Usage                   usage = Usage::Auto; // Auto: 推断为 SampledRead
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

		// 固定功能（与 RHI 最小集对齐；实际深度开关还受有无深度附件约束，无深度时 RHI 自动关闭）
		bool      depthTestEnable  = true;
		bool      depthWriteEnable = true;
		bool      blendEnable      = false;      // 单附件统一，逐附件后置
		CompareOp depthCompareOp   = CompareOp::Less;
		CullMode  cullMode         = CullMode::Back;
		uint32_t  pushConstantSize = 0;          // 0=无；全部 stage 共享、offset 固定 0
	};

	struct RenderPassDesc {
		std::vector<AttachmentBinding> attachments;    // 顺序无语义，slot 显式
		std::vector<ShaderBinding>     shaderBindings; // 与 attachments 平级
		ViewportDesc                   viewport {};
		PipelineDesc                   pipeline {};    // 管线状态（shader/blend/depth/cull）
	};

#pragma endregion

#pragma region DescAlias // 注册接口别名（Desc 变体）
	using PassDesc = std::variant<std::monostate, RenderPassDesc>;
	using ResourceDesc = std::variant<std::monostate, ImageDesc>;
#pragma endregion
}
