# 实现 RHI 的 Pipeline 与 Descriptor

## Context

RHI 层已在 [RHI.h](file:///c:/Users/DSwer/source/repos/Engine_0/Engine_0/rhi/RHI.h#L78-L104) 声明 `DescriptorSetLayout` / `DescriptorSet` / `Pipeline`，描述结构也已在 [RHIDesc.h](file:///c:/Users/DSwer/source/repos/Engine_0/Engine_0/rhi/RHIDesc.h#L143-L175) 就位，但 **RHI.cpp 中没有任何定义**；RenderGraph 侧的 `ShaderBinding`（含 set/binding/view）也缺后端落地。本轮补齐这条链路：视图 → 描述符布局/集合 → 管线，并让 `Device` 具备 descriptor pool 与默认采样器。

已确认决策（用户）：① 手工填 `bindings` 字段（反射日后填同字段）；② RHI 内置**单个默认采样器**，`SampledImage` 映射为 combined image sampler；③ **`ImageView` 所有权归 `Image`**：使用者通过 `Image` 显式注册/获取 `ImageView` **引用**，销毁由 RHI（`Image` 析构）统一管理——因此 `DescriptorSet::Write` 沿用已声明签名 `(binding, image, subresource)` 即可，无需改 API；④ 测试 SPIR-V 由 CMake 调 SDK 的 `glslc.exe` 编译。

## 变更清单

### 1. [RHIDesc.h](file:///c:/Users/DSwer/source/repos/Engine_0/Engine_0/rhi/RHIDesc.h)
- `GraphicsPipelineDesc` 新增 `std::vector<DescriptorBinding> bindings {};`，并 `#include <vector>`。注释说明：本轮手工填写，反射落地后由反射填充同一字段。

### 2. [RHI.h](file:///c:/Users/DSwer/source/repos/Engine_0/Engine_0/rhi/RHI.h)
- **新增 `class ImageView`**（位于 Resource region）：RAII，**由 `Image` 持有**；私有 ctor + `friend class Image`；delete copy；成员 `VkDevice` + `VkImageView`；暴露 `VkImageView NativeHandle() const`。`rhi::ImageView` 与 `rhi::ImageViewType` 共存无冲突。
- **`Image`**：
  - 新增 `const ImageView& CreateView(ImageSubresource sub) const;` —— 显式注册：按 `sub` 在内部缓存中查找，命中则返回同一引用，未命中则创建并记入缓存后返回引用。
  - 新增私有成员 `mutable std::vector<std::unique_ptr<ImageView>> m_views;`（缓存用 mutable，使 `CreateView` 可 const）、`VkDevice m_device`、`VkFormat m_format`（保存**真实**后端格式，含 SRGB，避免 backbuffer 视图格式与真实图像不符）。
  - `Borrow` 改为 `Borrow(VkDevice device, VkImage image, VkFormat format, const ImageCreateInfo& desc)`。
  - 析构顺序：**先销毁 views（`m_views.clear()`），再销毁 VkImage**（借用图像同样如此，view 由 RHI 创建故由 RHI 销毁）。
- **`DescriptorSetLayout`**：RAII 化（dtor、delete copy、私有 ctor、`friend class Pipeline`），成员 `VkDevice` + `VkDescriptorSetLayout`。
- **`DescriptorSet`**：RAII 化（dtor → `vkFreeDescriptorSets`、delete copy、私有 ctor、`friend class Device`），成员 `VkDevice` + `VkDescriptorPool` + `VkSampler`（默认采样器）。
  - **保持已声明签名不变**：`void Write(uint32_t binding, const Image& image, ImageSubresource view);`（实现内部调 `image.CreateView(view)` 取持久 view）。
  - `void Write(uint32_t binding, const Buffer& buffer, uint64_t offset, uint64_t range);` 不变。
- **`Pipeline`**：RAII 化（dtor、delete copy、私有 ctor、`friend class Device`），成员 `VkDevice` + `VkPipeline` + `VkPipelineLayout` + `std::vector<std::unique_ptr<DescriptorSetLayout>>`（保证 `SetLayout` 返回的裸指针地址稳定）。
- **`Device`**：新增 `VkDescriptorPool m_descriptorPool`、`VkSampler m_defaultSampler`、`VkPipelineCache m_pipelineCache`。
- 范围外不动：`Fence`、`CommandList` 全量方法、`Submit`/`WaitForFrame` 仍留待后续。

### 3. [RHI.cpp](file:///c:/Users/DSwer/source/repos/Engine_0/Engine_0/rhi/RHI.cpp)（补 `#pragma region` 实现）
- **ImageView / Image**：`ImageView` ctor/dtor（`vkDestroyImageView`）；`Image` ctor/dtor 携带 `m_device`/`m_format`（owned 用 `ToVkFormat(desc.format)`）、析构先清 views；`CreateView` 用 `vkCreateImageView`（imageType 由 `viewType` 推、`format=m_format`、aspect 按 format 推断、mip/layer count=1、2D/3D 切换）并按 `ImageSubresource` 去重缓存；`Swapchain::Build` 的 `Image::Borrow` 调用点传入 `context.device` 与 `chosen.format`。
- **Descriptor region**：`DescriptorSetLayout` / `DescriptorSet` / `Pipeline` 的 ctor、dtor、`NativeHandle`、`SetLayout`、`Write` 实现。
- **Device::CreatePipeline(const GraphicsPipelineDesc&)**（dynamic rendering，无 VkRenderPass）：
  - `bindings` 按 `set` 分组、组内 `binding` 升序 → `vkCreateDescriptorSetLayout`（`stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS`，`descriptorType` 由 `ResourceType` 映射：SampledImage→COMBINED_IMAGE_SAMPLER、StorageImage→STORAGE_IMAGE、UniformBuffer→UNIFORM_BUFFER、StorageBuffer→STORAGE_BUFFER，`descriptorCount = arrayCount`）→ 包成 Pipeline 持有的 N 个 `DescriptorSetLayout`。
  - `vkCreatePipelineLayout`（`pSetLayouts`，`pushConstantRangeCount = 0`）。
  - 读 vs/ps SPIR-V 文件（校验 codeSize 为 4 倍数）→ `vkCreateShaderModule`（`pName = "main"`），用完即销毁。
  - 固定功能：空 `VkPipelineVertexInputStateCreateInfo`（无顶点缓冲，shader 走 `gl_VertexIndex`）；`TRIANGLE_LIST`；**动态态 VIEWPORT + SCISSOR**（`VkPipelineViewportStateCreateInfo` 的 count 仍须 ≥1）；raster（FILL、lineWidth=1、cullMode 映射、frontFace=CCW、depthClamp=false）；multisample samples=1。
  - **深度**：`depthFormat == None` 时**强制** `depthTestEnable=false`、`depthWriteEnable=false`；`stencilTestEnable=false`、`depthBoundsTestEnable=false`；否则按 desc 映射 `CompareOp`→`VkCompareOp`。
  - **color blend**：槽数须等于 `colorAttachmentCount`，每槽 `colorWriteMask=RGBA`，`blendEnable` 为 true 时用 srcAlpha/oneMinusSrcAlpha。
  - `VkPipelineRenderingCreateInfo` 挂 pNext：colorFormats 由 `ToVkFormat` 映射、`depthAttachmentFormat`；D24S8 时 stencil 同填 D24_UNORM_S8_UINT，D32F 时 stencil 留 UNDEFINED，无深度时两者皆 UNDEFINED。
  - `renderPass=VK_NULL_HANDLE`、`basePipelineHandle=VK_NULL_HANDLE`、`basePipelineIndex=-1`、`pipelineCache=m_pipelineCache`。
- **Device::AllocateDescriptorSet(const DescriptorSetLayout&)**：`vkAllocateDescriptorSets`（`m_descriptorPool`），返回持 `m_device` / `m_descriptorPool` / `m_defaultSampler` 的 `DescriptorSet`。
- **Write**：图像重载 → 取 `image.CreateView(view).NativeHandle()`，`VkDescriptorImageInfo{sampler=m_defaultSampler, imageView, imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}` + `VkWriteDescriptorSet{COMBINED_IMAGE_SAMPLER}`；缓冲区重载 → `VkDescriptorBufferInfo{offset, range}`（range=0 表示到末尾）。
- **Device::Create 末尾**：创建 descriptor pool（`VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`，四类 count 与 maxSets 留足）、默认采样器（LINEAR/LINEAR、REPEAT、`maxLod=VK_LOD_CLAMP_NONE`）、pipeline cache。`~Device` 在 `vkDestroyDevice` **之前**销毁这三者。

### 4. 新增测试 shader + [CMakeLists.txt](file:///c:/Users/DSwer/source/repos/Engine_0/Engine_0/CMakeLists.txt)
- 新增 `Engine_0/shaders/fullscreen.vert`（由 `gl_VertexIndex` 生成全屏三角形，无顶点缓冲）与 `Engine_0/shaders/textured.frag`（采样 set0.binding0）。
- CMakeLists：优先用 `find_package(Vulkan)` 已提供的 `Vulkan_GLSLC_EXECUTABLE`，兜底 `find_program(GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/Bin")`；`add_custom_command` 编译到 `${CMAKE_BINARY_DIR}/shaders`，加 `--target-env=vulkan1.3`；`add_custom_target(Engine_0Shaders ALL ...)` + `add_dependencies`；`target_compile_definitions(Engine_0 PRIVATE ENGINE_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")`（正斜杠绝对路径，避免 MSVC `\U` 转义与 CWD 依赖）。

### 5. [Engine_0.cpp](file:///c:/Users/DSwer/source/repos/Engine_0/Engine_0/Engine_0.cpp) 冒烟追加
在既有交换链冒烟之后追加（前段 RenderGraph 与 Device/Image/Buffer/Swapchain 输出**逐字不变**）：
- 构造 `GraphicsPipelineDesc`（`vsSpirvPath`/`psSpirvPath` 由 `ENGINE_SHADER_DIR` 拼出；`colorFormats[0]=BGRA8`；`depthFormat=None`；`bindings` = set0.binding0 `SampledImage`）→ `CreatePipeline` → 打印 `SetLayout(0)` 与管线句柄非空。
- `AllocateDescriptorSet(*pipeline->SetLayout(0))` → `CreateImage` → `const ImageView& view = image->CreateView({0,0});`（打印 view 句柄）→ `set->Write(0, *image, {0,0})`。
- 再建一条含 `UniformBuffer` 的描述符布局+集合，`Write(binding, buffer, offset, range)` 验证缓冲区路径。

## 验证

1. 构建（必须在同一命令内加载 DevShell）：
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null; cmake --build out/build/x64-debug
   ```
2. 确认 `out/build/x64-debug/shaders/*.spv` 已生成。
3. 运行 `out\build\x64-debug\Engine_0\Engine_0.exe`：期望 RenderGraph 输出与既有行不变，新增 `Pipeline ok` / `DescriptorSet ok` / `ImageView ok` / `Write ok` 行，**校验层无 `[Vulkan WARN/ERROR]`**。

## 已知边界（本轮不处理，仅记录）
- 管线 `colorFormats` 用 `BGRA8`(UNORM) 与交换链实际 `B8G8R8A8_SRGB` 不一致——本轮不实际绘制故安全；将来真渲染需补 sRGB 词表（已入"已知问题"）。
- `CommandList::SetViewport` 未来需同时 `vkCmdSetScissor`，否则开动态 scissor 后绘制会触发 07832。
- 描述符 `imageLayout` 固定 `SHADER_READ_ONLY_OPTIMAL`，实际绘制前需由 barrier 转到该布局。
- `~DescriptorSet` 调用 `vkFreeDescriptorSets` 前须确保引用它的命令缓冲已完成。
- 深度格式不适合做线性过滤的采样纹理，避免把深度资源当 SampledImage。
