# CommandList 与 Submit 实现计划

## Context

RHI 层已完成 Device/Image/Buffer/ImageView/Swapchain/Pipeline/DescriptorSet（Vulkan 1.3，dynamicRendering + synchronization2 已启用）。剩余缺口：CommandList 全部方法、`Device::BeginCommandList/Submit/WaitForFrame`、Fence 类均只有声明无实现，Present 因此从未被真正验证过。本轮落地命令录制与提交，并以完整渲染循环（Acquire → 录制 → Submit → Present → WaitForFrame）冒烟收尾。

## 已验证的关键设计（Plan agent 核验修正后）

- **帧槽数量**：`DeviceDesc` 加 `uint32_t maxFramesInFlight = 2`（创建期决定，运行期不变更；clamp 到 [1, 8]）。`Swapchain::Context` 联动加同名字段，由 Device 创建交换链时传入（替换现有硬编码 2），保证 acquire 信号量轮转数与帧槽数一致（两帧一轮回的复用安全论证依赖两者相等）。
- **FrameSlot 类**（新 RAII 类，`#pragma region FrameSlot` 置于 CommandList 与 Device 之间）：内聚一帧的录制上下文与回收状态机——
  ```cpp
  class FrameSlot {
      // dtor：free 在录 buffer → 销毁 pool 与内部 fence
      // 私有 Allocate()：从 pool 分配 primary buffer（记入 liveBuffers）→ begin(ONE_TIME_SUBMIT) → 返回 VkCommandBuffer
      // 私有 MarkPending(VkFence)：记录在飞提交的 fence（外部或内部）
      // 私有 Recycle()：pendingFence 非空 → vkWaitForFences（内部 fence 额外 reset）→ vkFreeCommandBuffers 全部 → 清 pending。
      //                    pending 为空绝不 wait —— "Begin 后不 Submit"、"漏调 WaitForFrame" 均只退化为串行、不死锁
      // 成员：VkDevice、VkCommandPool、VkFence m_fence(内部, unsignaled)、VkFence m_pendingFence、std::vector<VkCommandBuffer> m_liveBuffers
  };
  ```
  - **内部 fence 创建时 unsignaled**（signaled 会在首帧 Submit 触发"二次信号"VUID）。
  - **外部 fence 也记入 pendingFence**：否则下轮回收 `vkFreeCommandBuffers` 会在命令飞行中执行（VUID 00041）。外部 fence 由 RHI wait、由使用者 Reset（契约注释）。
  - **buffer 零累积**：回收时 free 全部在录 buffer（而非只 reset pool）。
  - Device 持 `std::vector<std::unique_ptr<FrameSlot>> m_frameSlots` + `uint32_t m_frameIndex`；`BeginCommandList/Submit/WaitForFrame` 薄转发，`vkQueueSubmit2` 留在 Device（队列是 Device 职责）。
  - `BeginCommandList`：`Recycle()` → `Allocate()`。
  - `Submit`：逐个 `vkEndCommandBuffer` → `VkSubmitInfo2`（sync2）→ fence = 外部 ?: 槽内 fence → `MarkPending(fence)` → `vkQueueSubmit2(m_graphicsQueue)`。
  - `WaitForFrame`：`Recycle()` → `m_frameIndex = (m_frameIndex+1) % slots.size()`。
- **`Swapchain::Frame` 加 `const Semaphore* ready`**：acquire 信号量必须被 Submit 等待（否则使用 presentable image 触发校验错误）。
- **纹理布局**：`DescriptorSet::Write` 声明 SHADER_READ_ONLY，纹理须先 Barrier（Undefined→ShaderReadOnly，dstStage=FragmentShader，dstAccess=ShaderRead），冒烟里仅首帧录一次。
- **signal stage**：renderDone 信号量用 AllCommands（用 ColorAttachmentOutput 可能早于 PresentSrc 转换完成）；`SemaphoreSignal::stage=None` 翻译为 AllCommands（VkSemaphoreSubmitInfo stageMask 禁 0）。
- **附带修复 swapchain 格式错配**：Build 现优选 B8G8R8A8_SRGB，而 `Format()` 归一化为 BGRA8 → CreatePipeline 得 UNORM，管线签名与实际附件不匹配（draw 时校验 ERROR）。改为优选 **B8G8R8A8_UNORM**（colorSpace 仍 SRGB_NONLINEAR，合法）。
- push constants：desc 加 `pushConstantSize`，range 固定 VERTEX|FRAGMENT、offset 0（最小集）。

## 文件改动

### 1. `Engine_0/rhi/RHIDesc.h`
- `GraphicsPipelineDesc` 加 `uint32_t pushConstantSize = 0;`（VS|FS 共享，offset 0；0=无）。

### 2. `Engine_0/rhi/RHI.h`
- `DeviceDesc` 加 `uint32_t maxFramesInFlight = 2;`（clamp [1,8]）。
- `Image` 加 `VkFormat NativeFormat() const;`（Barrier/ClearColor 推 aspectMask 用）。
- `Pipeline` 加 `uint32_t PushConstantSize() const;` + 私有成员 `m_pushConstantSize`（ctor 加参）。
- `Fence` 补成与 Semaphore 相同的 RAII 形态（dtor、delete copy、私有 ctor、friend Device；成员 VkDevice/VkFence）。
- `CommandList` 加私有成员：`VkCommandBuffer m_commandBuffer`、`VkPipelineLayout m_pipelineLayout`（BindPipeline 记录，BindDescriptorSet/PushConstants 用）、`uint32_t m_pushConstantSize`。公共接口不变，默认拷贝（句柄语义）。
- **新增 `#pragma region FrameSlot`**（CommandList 与 DeviceAPI 之间）：如上"关键设计"所述的 FrameSlot RAII 类，私有 ctor + friend Device。
- `Swapchain::Frame` 加 `const Semaphore* ready = nullptr;`；`Swapchain::Context` 加 `uint32_t maxFramesInFlight = 2;`（Device 创建交换链时传入）。
- `Device` 成员改为：`std::vector<std::unique_ptr<FrameSlot>> m_frameSlots` + `uint32_t m_frameIndex = 0`（不再持平行数组）。

### 3. `Engine_0/rhi/RHI.cpp`
- 新 helper：`ToVkImageLayout`、`ToVkPipelineStage`（TopOfPipe→NONE，sync2 废弃语义）、`ToVkAccess`、`ToVkIndexType`、`ToVkClearValue`（ClearValue variant→VkClearValue）、LoadOp/StoreOp 翻译。
- `Image::NativeFormat` 实现。
- Sync region：Fence 实现 + `Device::CreateFence`（Semaphore 旁）。
- 新 FrameSlot region：ctor/dtor/Allocate/MarkPending/Recycle 实现。
- 新 CommandList region：全部方法。要点：
  - `Barrier`：`vkCmdPipelineBarrier2`；src/dstQueueFamilyIndex=IGNORED；image 的 aspect 用 `NativeFormat()`+已有 `ToVkAspect`。
  - `BeginRendering`：`VkRenderingInfo` + `VkRenderingAttachmentInfo`（imageView 走 `image->CreateView(view)`；clear 值按 variant 分支，D24S8 用 depthStencil 分量；null image 附件跳过）。
  - `SetViewport`：`vkCmdSetViewport` + `vkCmdSetScissor`（同矩形，防 VUID 07832）。
  - `PushConstants`：size 与管线声明不符时 cerr。
  - `CopyBufferToImage`：无 region 参数 → 单个覆盖全资源 region（VK_REMAINING_*）。
- Device::Create 建 N 个 FrameSlot；析构时 vector 自然销毁（注意先于 vkDestroyDevice）。
- `Device::BeginCommandList/Submit/WaitForFrame`：薄转发（见关键设计）。
- `CreatePipeline`：pushConstantSize>0 时声明 range（校验 4 对齐、≤128），Pipeline 存值。
- `Swapchain::Build`：格式优选改 UNORM；`AcquireNextImage` 填 `frame.ready`；Context 的 maxFramesInFlight 用于 acquire 信号量数量与轮转。

### 4. `Engine_0/Engine_0.cpp`
前段输出逐字不变。在 `#ifdef ENGINE_SHADER_DIR` 块内、管线 B 冒烟后追加**完整渲染循环冒烟**（自建 swapchain/tex/pso/set，colorFormats 取 `swapchain->Format()`）：
```
成功渲染 3 帧（acquired=false 的帧跳过重试）：
  Acquire → BeginCommandList
  → (仅首帧) Barrier(tex: Undefined→ShaderReadOnly)
  → Barrier(backbuffer: Undefined→ColorAttachment, dstStage=ColorAttachmentOut, dstAccess=CO 读|写)
  → BeginRendering(backbuffer, Clear 深蓝) → BindPipeline → BindDescriptorSet → SetViewport → Draw(3) → EndRendering
  → Barrier(backbuffer: ColorAttachment→PresentSrc, dstStage=AllCommands, dstAccess=MemoryRead)
  → Submit(waits={frame.ready@ColorAttachmentOut}, signals={renderDone@AllCommands})
  → Present(renderDone) → WaitForFrame
输出：Frame loop ok: 3 frames presented
```

## 验证

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
cmake --build out/build/x64-debug
out\build\x64-debug\Engine_0\Engine_0.exe
```
- 退出码 0；前段输出逐字不变；新增 `Frame loop ok: 3 frames presented`。
- 校验层无 `[Vulkan WARN/ERROR]`（重点盯：fence 二次信号、pool 飞行中 reset、纹理布局、PresentSrc 转换、信号量复用）。
