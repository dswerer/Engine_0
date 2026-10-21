# RHI 接口声明（施工图纸）— 实施计划

## Summary

把此前评审通过的 **自研薄 Vulkan RHI 接口清单 v1.1** 落成真实代码，作为后续实现的施工图纸：

- 新增 `Engine_0/rhi/` 下的 3 个文件：`RHIDesc.h`（后端无关的纯数据描述）、`RHI.h`（对象与设备接口，含 Vulkan 类型）、`RHI.cpp`（空 TU，实现阶段的落脚点，当前仅 `#include "RHI.h"` 用于让构建方真编译校验头文件）。
- 仅**声明**，不写任何函数体；**不接线 RenderGraph**（`ImportImage` / `Execute` / Pass 录制回调留到下一阶段）；**不引入任何新第三方依赖**。
- 构建必须保持绿色，`Engine_0.exe` 输出与当前完全一致（现有图代码零改动）。

设计依据（已在前序讨论中确认）：RHI 为 Vulkan-only 薄封装（不做跨 API 抽象）；dynamic rendering + synchronization2 起步；句柄 = RAII 对象由 `Device` 以 `unique_ptr` 持有、命令接口传引用/裸指针；descriptor 以「编译期预建 + `BindDescriptorSet`」为主路径；shader 走 GLSL 源文件路径在线编译。

## Current State Analysis

**现有代码（只读调研结论）**

- `Engine_0/` 平铺布局：`RenderGraphDesc.h`（纯数据层，含 `ImageFormat`/`ImageViewType`/`Usage`/`LoadOp`/`StoreOp`/`ClearValue`/`ImageSubresource`/`kMaxColorAttachments`）、`RenderGraphAbstracted.h`（`Resource`/`Pass` 基类 + `RenderGraph`）、`RenderGraphNodes.h`（`RenderPass`/`Image` 子类）、`RenderGraph.cpp`、`Pass.cpp`、`Resource.cpp`、`Engine_0.cpp`、`vmaExporter.cpp`。
- **`Engine_0/rhi/` 目录已存在但为空** —— 用户已表明 RHI 文件应落在此处。
- `Engine_0/CMakeLists.txt`：`add_executable` 平铺源文件列表；已 `find_package(Vulkan REQUIRED)`、`find_package(glfw3 REQUIRED)`、`find_package(glm REQUIRED)` 并链接 `Vulkan::Vulkan` / `glfw` / `glm::glm`；已有 `target_include_directories(... ${CMAKE_SOURCE_DIR}/external)`（该路径实际指向 `Engine_0/external`，不存在 —— 现有代码靠相对包含 `"external/vk_mem_alloc.h"` 工作）；MSVC 已加 `/utf-8`。
- `Engine_0/external/vk_mem_alloc.h` + `vmaExporter.cpp`（`#define VMA_IMPLEMENTATION`）**已就位**，无需新增显存分配依赖。
- 命名与风格约定：`namespace dEngine`；头文件内联实现；`pragma region` 用英文标签 + 中文同行注释；源文件 UTF-8。

**环境调研结论（影响实现阶段，不影响本阶段声明）**

- Vulkan SDK = `C:\VulkanSDK\1.4.350.0`（`VULKAN_SDK` 环境变量已设），`Include\vulkan\vulkan.h` 存在 → **sync2 / dynamic rendering 可用**（Vulkan 1.3+ 核心）。
- SDK `Bin` 内提供可执行文件：`glslc.exe`、`glslangValidator.exe`、`dxc.exe`、`spirv-cross.exe`、`spirv-reflect.exe`、`spirv-val.exe`。
- **vcpkg 未安装任何 shaderc / glslang / spirv-reflect 库**（`installed/x64-windows/include` 下无相关目录）。→ 在线编译与 SPIR-V 反射的**具体机制**是实现阶段需要拍板的点，见「Assumptions & Decisions」第 6 条。本阶段声明的接口形状（GLSL 源路径、由反射产出 descriptor 布局）不受该机制选择影响。

**构建方式（重要）**

普通 PowerShell 直接 `cmake --build` 会报 `fatal error C1034`，必须在同一命令内先加载 VS DevShell：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
cmake --build out/build/x64-debug
```

## Proposed Changes

### 1. 新增 `Engine_0/rhi/RHIDesc.h`（后端无关的纯数据描述）

**Why**：集中 RHI 的枚举与创建/录制参数结构；只依赖标准库与 `RenderGraphDesc.h`，**不 include Vulkan 头**，从而保持「描述层」可独立理解、可被图/上层引用。

**How**：完整内容如下（`Image`/`Buffer` 前置声明供 barrier / rendering 结构引用）：

```cpp
#pragma once
#include "../RenderGraphDesc.h"   // 复用 ImageFormat / ImageViewType / LoadOp / StoreOp / ClearValue / ImageSubresource / kMaxColorAttachments
#include <cstdint>
#include <string>
#include <array>

// RHI 描述层：后端无关的枚举与参数结构。仅依赖标准库与渲染图描述层。
namespace dEngine::rhi {

	// 前置声明：描述结构中以指针引用运行期对象
	class Image;
	class Buffer;

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
		AllCommands        = 1ull << 9
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
		MemoryWrite           = 1ull << 9
	};
	inline constexpr Access operator|(Access a, Access b) {
		return static_cast<Access>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
	}

	// RHI 内部状态，不对上层暴露（图的 barrier compiler 负责决定目标布局）
	enum class ImageLayout {
		Undefined, General, ColorAttachment, DepthStencilAttachment,
		ShaderReadOnly, TransferSrc, TransferDst, PresentSrc
	};

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
		TransferDst  = 1u << 5
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

	// 反射产出的资源类别（亦可手工声明时使用）
	enum class ResourceType { SampledImage, StorageImage, UniformBuffer, StorageBuffer };

	// 单条描述符声明：反射输出或手工填写
	struct DescriptorBinding {
		uint32_t     set = 0;
		uint32_t     binding = 0;
		ResourceType type = ResourceType::SampledImage;
		uint32_t     arrayCount = 1;
	};

	struct GraphicsPipelineDesc {
		std::string vsPath, psPath;   // GLSL 源文件路径，RHI 内部在线编译为 SPIR-V

		// 附件格式签名：dynamic rendering 需要，与图的附件绑定对应
		std::array<ImageFormat, kMaxColorAttachments> colorFormats {};
		uint32_t    colorAttachmentCount = 0;
		ImageFormat depthFormat = ImageFormat::None;   // None = 无深度

		// 固定功能（第一版最小集）
		bool      depthTestEnable = true;
		bool      depthWriteEnable = true;
		CompareOp depthCompareOp = CompareOp::Less;
		CullMode  cullMode = CullMode::Back;
		bool      blendEnable = false;   // 单附件统一，逐附件后置
		// resources 字段已删除：set/binding/type 由 SPIR-V 反射产出
	};

	struct Rect2D   { int32_t x = 0, y = 0; uint32_t width = 0, height = 0; };
	struct Viewport { float x = 0, y = 0, w = 0, h = 0, minDepth = 0.0f, maxDepth = 1.0f; };

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
	};

	struct BufferBarrier {
		Buffer*        buffer = nullptr;
		uint64_t       offset = 0;
		uint64_t       size = 0;   // 0 = 到末尾
		PipelineStage   srcStage = PipelineStage::None;
		PipelineStage   dstStage = PipelineStage::None;
		Access         srcAccess = Access::None;
		Access         dstAccess = Access::None;
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
```

### 2. 新增 `Engine_0/rhi/RHI.h`（对象与设备接口）

**Why**：承载对上层可见的运行期对象与设备/命令接口。含 Vulkan 类型（`NativeHandle()`），是「Vulkan 薄封装」的边界所在。

**How**：完整内容如下（保持与会话中评审通过的接口清单 v1.1 一致）：

```cpp
#pragma once
#include "RHIDesc.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <span>

// RHI 对象层：设备、资源、管线、描述符、命令列表、交换链。
namespace dEngine::rhi {

#pragma region Device // 设备创建

	struct DeviceDesc {
		void*    nativeWindowHandle = nullptr; // Win32 窗口句柄；为空则无 swapchain（离屏/测试）
		uint32_t width = 0, height = 0;
		bool     enableValidation = true;      // debug 下默认开
	};

#pragma endregion

#pragma region Resource // RAII 资源对象（裸指针传给命令接口；生命周期由 Device 持有）

	class Image {
	public:
		VkImage                NativeHandle() const;
		const ImageCreateInfo& Desc() const;
		// 无状态查询接口：RHI 不跟踪 layout，由图的 state tracker 负责
	};

	class Buffer {
	public:
		VkBuffer                NativeHandle() const;
		const BufferCreateInfo& Desc() const;
		void*                   Map();   // 仅 hostVisible 可用，持久映射，不提供 Unmap
	};

#pragma endregion

#pragma region Descriptor // 描述符布局 / 集合（预编译路径）

	// 由 Pipeline 反射产出；持有 VkDescriptorSetLayout
	class DescriptorSetLayout {
	public:
		VkDescriptorSetLayout NativeHandle() const;
	};

	// 由 Device 分配；写操作在录制前完成（预编译路径）
	class DescriptorSet {
	public:
		VkDescriptorSet NativeHandle() const;
		void Write(uint32_t binding, const Image& image, ImageSubresource view);
		void Write(uint32_t binding, const Buffer& buffer, uint64_t offset, uint64_t range);
	};

#pragma endregion

#pragma region Pipeline // 管线对象

	class Pipeline {
	public:
		VkPipeline                        NativeHandle() const;
		const DescriptorSetLayout*        SetLayout(uint32_t setIndex) const; // 反射产出，供分配 descriptor set
	};

#pragma endregion

#pragma region Sync // 同步原语外壳

	class Fence {
	public:
		void Wait();   // 阻塞至信号
		void Reset();
	};

	class Semaphore {
	public:
		VkSemaphore NativeHandle() const;
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
		void BindVertexBuffer(const Buffer& buffer, uint64_t offset = 0);
		void BindIndexBuffer(const Buffer& buffer, uint64_t offset, IndexType type);
		void Draw(uint32_t vertexCount, uint32_t firstVertex = 0);
		void DrawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t vertexOffset = 0);

		// 拷贝/清场（图外工具与上传路径用）
		void CopyBufferToImage(const Buffer& src, Image& dst, ImageLayout dstLayout);
		void ClearColor(Image& image, ImageSubresource view, const float rgba[4], ImageLayout layout);

		VkCommandBuffer NativeHandle() const;
	};

#pragma endregion

#pragma region Submit // 提交与帧间同步

	struct SemaphoreWait   { const Semaphore* semaphore = nullptr; PipelineStage stage = PipelineStage::None; };
	struct SemaphoreSignal { const Semaphore* semaphore = nullptr; PipelineStage stage = PipelineStage::None; };

	struct SubmitInfo {
		std::span<const CommandList>   commandLists {};
		std::span<const SemaphoreWait>   waits {};    // 跨队列 / acquire 用
		std::span<const SemaphoreSignal> signals {};
		Fence* signalFence = nullptr;                 // CPU 等待用，可空
	};

#pragma endregion

#pragma region Swapchain // 交换链

	class Swapchain {
	public:
		struct Frame {
			uint32_t imageIndex = 0;
			Image*   backbuffer = nullptr;  // 指向当帧交换链图像的非拥有句柄；"外部资源"导入直接吃它
			bool     acquired = false;      // resize / 失焦时为 false，上层跳过渲染
		};

		Frame      AcquireNextImage();  // 内部管理 acquire semaphore
		void       Present();           // 内部管理 present semaphore 等待
		ImageFormat Format() const;
		uint32_t   Width() const;
		uint32_t   Height() const;
	};

#pragma endregion

#pragma region DeviceAPI // 设备主接口

	class Device {
	public:
		static std::unique_ptr<Device> Create(const DeviceDesc& desc);

		// 资源与对象创建
		std::unique_ptr<Image>       CreateImage(const ImageCreateInfo& info);
		std::unique_ptr<Buffer>      CreateBuffer(const BufferCreateInfo& info);
		std::unique_ptr<Pipeline>    CreatePipeline(const GraphicsPipelineDesc& desc);
		std::unique_ptr<DescriptorSet> AllocateDescriptorSet(const DescriptorSetLayout& layout);
		std::unique_ptr<Fence>       CreateFence(bool signaled = false);
		std::unique_ptr<Semaphore>   CreateSemaphore();

		// 帧上下文
		std::unique_ptr<Swapchain> CreateSwapchain();
		CommandList                BeginCommandList();  // 自当期帧 pool 分配，一帧内有效

		// 提交与帧回收（frames-in-flight 上限，默认 2）
		void Submit(const SubmitInfo& info);
		void WaitForFrame();
	};

#pragma endregion
}
```

**说明**：本阶段**不写任何函数体**。类不声明私有成员（实施阶段在 RHI.cpp 落地时按需补 native 句柄/VMA 分配等字段）；`RHI.h` 因此可以独立编译通过（无未定义符号被引用）。

### 3. 新增 `Engine_0/rhi/RHI.cpp`（空 TU，实现落脚点 + 编译校验）

**Why**：给构建方一个真实 TU 去语法校验 `RHI.h`（既有的头文件列表在 Ninja 下并不会被单独编译）；同时它是后续实现的天然落脚点，与项目「模块头 + cpp」的既有模式（`Pass.cpp`/`Resource.cpp`）一致。当前**只包含头文件，无任何定义**。

```cpp
#include "RHI.h"

// RHI 实现落脚点。当前阶段仅声明接口（施工图纸），实现将在后续阶段填入。
```

### 4. `Engine_0/CMakeLists.txt`

在 `add_executable` 源文件列表追加 3 个新文件（保持平铺风格与既有写法）：

```cmake
add_executable (Engine_0 "Engine_0.cpp" "Engine_0.h" "RenderGraphDesc.h" "RenderGraphAbstracted.h" "RenderGraph.cpp" "Pass.cpp" "Resource.cpp" "RenderGraphNodes.h" "vmaExporter.cpp" "rhi/RHIDesc.h" "rhi/RHI.h" "rhi/RHI.cpp")
```

**不做**的事：不新增 `find_package`、不改 `target_link_libraries`、不改 include 目录（`RHIDesc.h` 用相对包含 `"../RenderGraphDesc.h"`、`RHI.cpp` 用同目录 `"RHI.h"`，沿用项目既有相对包含约定；Vulkan 头由现有 `Vulkan::Vulkan` 目标提供）。

### 5. 无需改动

`Engine_0.cpp`、`RenderGraphDesc.h`、`RenderGraphAbstracted.h`、`RenderGraphNodes.h`、`RenderGraph.cpp`、`Pass.cpp`、`Resource.cpp`、`vmaExporter.cpp`、根 `CMakeLists.txt`、`CMakePresets.json` —— 全部零改动（保证 `Engine_0.exe` 输出不变）。

## Assumptions & Decisions

1. **命名空间 `dEngine::rhi`**（嵌在项目命名空间内），不与图层的 `dEngine::Image` 冲突（`RenderGraphNodes.h` 已定义 `dEngine::Image`；`rhi::Image` 限定后无歧义）。
2. **文件目录与拆分**：落在已存在的空目录 `Engine_0/rhi/`；拆为 `RHIDesc.h`（后端无关纯数据）+ `RHI.h`（对象/设备，含 Vulkan 类型）两层，对齐图层「Desc / 本体」的既有分层习惯。不放单一 `RHI.h`。
3. **词表复用**：`ImageFormat`/`ImageViewType`/`LoadOp`/`StoreOp`/`ClearValue`/`ImageSubresource`/`kMaxColorAttachments` 直接复用 `RenderGraphDesc.h`，不另立一套再做映射。
4. **能力位 vs 逐次访问 usage**：图的 `Usage` 是「逐次访问」语义枚举，不能表达「资源创建能力」，故 RHI 另设位标志 `ImageUsage`/`BufferUsage`（创建参数用）；两者正交，由 barrier compiler 在后续阶段负责翻译。**这是对前序「RHI 与图共用 usage 词表」的精确化**：共用的是 format/load/store/clear/subresource，capacity flags 为 RHI 局部定义。
5. **状态跟踪不在 RHI**：RHI 不提供资源状态查询、不自动插 barrier；`ImageLayout` 只在 RHI 内部/barrier 结构中出现，图的 state tracker 决定目标布局。`ImageDesc` 不加 layout/tiling/aspect/usageFlags（图层保持现状）。
6. **shader 编译与反射机制（实现阶段待拍板，本阶段不阻塞）**：`GraphicsPipelineDesc` 只声明 GLSL 源**路径**，编译机制（`shaderc` 库 / 调用 SDK `glslc.exe` 子进程 / SDK `glslangValidator.exe`）在实现阶段选择，均不改变接口形状。同理，`Pipeline::SetLayout()` 依赖的 SPIR-V 反射**当前无可用库**（vcpkg 未装 shaderc/glslang/spirv-reflect），实现阶段需三选一：(a) `vcpkg install spirv-reflect` 引入库；(b) 调 SDK `spirv-reflect.exe` 子进程取布局；(c) 临时恢复「手工填 `DescriptorBinding` 列表」的旧路径作为过渡。**声明接口在任一选择下均不变**。
7. **descriptor 主路径 = 预编译**：写操作放在 `DescriptorSet::Write`（录制前完成），绑定只走 `CommandList::BindDescriptorSet`；无「每次绑定即写 descriptor」的 `BindResource` 接口（该语义已在评审中降级/取消）。指向外部导入资源（交换链 backbuffer）的 descriptor 由上层按 frames-in-flight 复制多份并帧级刷新——属上层（图的执行后端）职责，不进 RHI 接口。
8. **同步原语**：`Barrier` 采用批量签名（`span<ImageBarrier>` + `span<BufferBarrier>`），便于 barrier compiler 合并；`Semaphore`/`Fence` 为薄 RAII 外壳，跨队列/CUDA 等更复杂的同步机制留待图的 `SyncEdge` 边分类器阶段（本轮不涉及）。
9. **无 compute pipeline、无多队列暴露**：`Submit` 固定图形队列；`ExecutionDomain` 尚未进图。按「第一版最少集」原则。
10. **交换链 backbuffer 包装**：`Swapchain::Frame::backbuffer` 为非拥有 `rhi::Image*`；未来 `RenderGraph::ImportImage(name, rhi::Image*)` 直接接收它——接口形状已为其预留。
11. **本阶段范围内明确不做**：任何函数体实现、`RenderGraph` 侧接线（`ImportImage`/`Execute`/Pass 录制回调）、`Pass` 基类加 `RecordCommands`、Vulkan 实例/设备/交换链创建、新第三方依赖引入、`ImageDesc` 扩展。这些属「下一步施工」。
12. **`RHIDesc.h` 中 `depth` 注释**：沿用与图 `ImageDesc` 一致的语义（3D 的 depth 与数组层数分离）。

## Verification

1. **构建**（必须加载 VS DevShell，环境不跨进程保留）：
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
   cmake --build out/build/x64-debug
   ```
   期望：编译 0 error；`RHI.cpp` 使 `RHI.h` + `RHIDesc.h` 被真实语法校验（证明 `vulkan.h` 可达、`RenderGraphDesc.h` 相对包含可解析、位或运算符可用）。允许既有无害警告（C4101/C4267）保持不变。
2. **运行** `out\build\x64-debug\Engine_0\Engine_0.exe`：输出与改动前**逐字一致**（`[Resources]`/`[Passes]`/`att`/`bind`/`SubmitSequence` 均为 `GBuffer -> Blur -> ToneMap`），确认图侧零回归。
3. **接口自查**（交付复审用，逐项对照本计划第 1、2 节的代码块）：确认 `Device`/`Image`/`Buffer`/`Pipeline`/`DescriptorSet(Layout)`/`Fence`/`Semaphore`/`CommandList`/`Swapchain`/`SubmitInfo` 均已声明；确认 `GraphicsPipelineDesc` **不含** `resources` 字段；确认 `CommandList` **不含** `BindResource`；确认 `RHIDesc.h` **不** include Vulkan 头。
```
