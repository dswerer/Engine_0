# RHI 层说明文档

Engine_0 的后端抽象层（Render Hardware Interface），位于渲染图（RenderGraph）之下。当前后端为 **Vulkan 1.3**（启用 `dynamicRendering` + `synchronization2` 核心特性）。

## 1. 设计原则

- **显式优于隐式**：借用走显式工厂（`Image::Borrow`）、视图显式注册、fence 复位契约在使用者、无隐藏提交点——`Create*` 只构造资源，不做任何 GPU 操作。
- **资源无状态跟踪**：RHI 不记录资源的布局/归属状态，`Image::Desc()` 之外的查询接口一律无状态。布局追踪是渲染图 barrier compiler / state tracker 的职责（尚未实现）。
- **命令接口是直译出口**：`CommandList` 的方法与 Vulkan 命令一一对应（如 `Barrier` → `vkCmdPipelineBarrier2`），不引入中间语义。
- **描述层不依赖后端**：`RHIDesc.h` 只用标准库，格式/同步枚举等词表可映射到任意后端。

## 2. 文件结构

| 文件 | 内容 |
|---|---|
| `RHIDesc.h` | 后端无关描述层：格式词表、LoadOp/StoreOp、同步枚举（`PipelineStage`/`Access`，按位组合）、资源创建参数、`GraphicsPipelineDesc`、barrier/rendering 结构、`QueueClass` 词表 |
| `RHI.h` | 对象层声明：Device、资源、管线、描述符、同步原语、CommandList、Submit、FrameSlot、Swapchain |
| `RHI.cpp` | Vulkan 实现（约 2000 行，含词表映射与冒烟验证过的全部路径） |

## 3. 初始化与销毁

```cpp
rhi::DeviceDesc desc;
desc.nativeWindowHandle = glfwGetWin32Window(window); // 为空则离屏（无 swapchain）
desc.width = 1280; desc.height = 720;
desc.enableValidation = true;      // debug 下默认开
desc.maxFramesInFlight = 2;        // 帧槽数，创建期钳制到 [1,8]，运行期不可变更

auto device = rhi::Device::Create(desc);
```

- 队列族全自动选择：graphics 必选；present 优先同族；**专用 compute**（含 COMPUTE 不含 GRAPHICS 位）与**专用 transfer**（含 TRANSFER 不含 GRAPHICS/COMPUTE 位）尽力挑选，无专用族时兜底 graphics（规范保证 GRAPHICS 必带 COMPUTE/TRANSFER 位）。
- `~Device` 开头 `vkDeviceWaitIdle` 兜底，帧槽先于逻辑设备销毁。

## 4. 资源

```cpp
// Image：自有（RHI 负责销毁）与借用（外部句柄包装，析构不销毁）两种所有权
rhi::ImageCreateInfo info;
info.width = 1920; info.height = 1080;
info.format = rhi::ImageFormat::RGBA16F;
info.usage  = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::Sampled; // 创建能力位
auto image = device->CreateImage(info);

// 视图显式注册，按子资源去重，引用随 Image 存活
const rhi::ImageView& view = image->CreateView(rhi::ImageSubresource{ 0, 0 });

// Buffer：hostVisible 时 Map() 返回持久映射，无 Unmap
rhi::BufferCreateInfo binfo;
binfo.size = 4096; binfo.usage = rhi::BufferUsage::Uniform; binfo.hostVisible = true;
auto buffer = device->CreateBuffer(binfo);
*static_cast<uint32_t*>(buffer->Map()) = 42;
```

要点：
- `Image::NativeFormat()` 返回**真实后端格式**（借用路径含 sRGB 变体），barrier 推 aspect 用它。
- 交换链 backbuffer 通过 `Image::Borrow` 包装为非拥有 Image。
- 描述层无 sRGB 格式变体：`*_SRGB` 与对应 UNORM 归一到同一 `ImageFormat`（交换链优选 UNORM 规避签名错配）。

## 5. 管线与描述符（预编译路径）

```cpp
rhi::GraphicsPipelineDesc pso;
pso.vsSpirvPath = shaderDir + "/fullscreen.vert.spv"; // SPIR-V 离线编译（CMake glslc）
pso.psSpirvPath = shaderDir + "/textured.frag.spv";
pso.colorFormats[0] = swapchain->Format();  // 附件格式签名须与实际附件视图一致
pso.colorAttachmentCount = 1;
pso.depthFormat = rhi::ImageFormat::None;   // None = 关闭深度测试
pso.cullMode = rhi::CullMode::None;
pso.pushConstantSize = 64;                  // 0=无；VS/FS 共享、offset 0、4 对齐且 ≤128
pso.bindings = { rhi::DescriptorBinding{ 0, 0, rhi::ResourceType::SampledImage, 1 } }; // 反射落地前手工填

auto pipeline = device->CreatePipeline(pso);
auto set = device->AllocateDescriptorSet(*pipeline->SetLayout(0));
set->Write(0, *texture, rhi::ImageSubresource{ 0, 0 });   // combined image sampler（内置默认采样器）
set->Write(0, *uniformBuffer, 0, size);                   // uniform/storage 缓冲
```

- `DescriptorSetLayout` 由管线反射产物持有（当前绑定来自 `pso.bindings`，SPIR-V 反射落地后由反射填充同一字段）。
- 描述符写入在**录制前**完成（预编译路径），`Write` 声明采样图布局为 ShaderReadOnly。

## 6. 命令录制（CommandList）

`CommandList` 是**无所有权句柄**，一帧内有效：

```cpp
rhi::CommandList cmd = device->BeginCommandList(); // 自动 begin（PRIMARY + ONE_TIME_SUBMIT）
// ... 录制 ...
device->Submit(info);                              // 自动 vkEndCommandBuffer（无独立 End 接口）
```

方法要点：

| 方法 | 语义 |
|---|---|
| `Barrier(images, buffers)` | `vkCmdPipelineBarrier2` 批量直译；`size==0` 用 `VK_WHOLE_SIZE`；queueFamily 字段默认 IGNORED（QFOT 占位） |
| `BeginRendering/EndRendering` | dynamic rendering；深度格式含模板位时同时填 stencil 附件 |
| `BindPipeline` | 记录 layout 与 pushConstantSize 供后续绑定校验 |
| `SetViewport` | **同时设 viewport + scissor**（避免动态 scissor 未设触发 VUID 07832） |
| `ClearColor` | 仅 color aspect；`CopyBufferToImage` 单 region、全层、仅 mip0 |

## 7. 帧循环与同步模型

### 7.1 三种同步原语的生产消费关系

| 原语 | 方向 | 生产者 | 消费者 |
|---|---|---|---|
| FrameSlot 内部 fence | GPU → CPU | `vkQueueSubmit2` 完成 | `DrainPending`（wait + reset），槽复用前提 |
| 外部 Fence | GPU → CPU | Submit（`signalFence`） | RHI 回收时 wait；**Reset 交使用者** |
| acquire 信号量（每帧槽一个） | 呈现引擎 → GPU | `vkAcquireNextImageKHR` | Submit 的 `waits`（经 `Frame::ready`） |
| renderDone 信号量（**每 image 一个**） | GPU → 呈现引擎 | Submit 的 `signals`（经 `Frame::presentReady`） | `vkQueuePresentKHR` |
| Pipeline Barrier | 提交内部 | `CommandList::Barrier` | GPU 执行流的执行/内存依赖 |

renderDone 按 image 索引的原因：同一 image 被重新 acquire 前，其上次 present 必已完成，此刻复用才安全（规避单一信号量在 present 消费前被下一帧再次 signal 的 VUID 03868）。

### 7.2 FrameSlot 回收状态机

每槽持有：每队列族一个 command pool + 内部 unsignaled fence + 在飞记录（`pendingFence`）+ 在录 buffer 集合。

- **pending 为空绝不 wait**——"Begin 后不 Submit"、"漏调 WaitForFrame"均只退化为串行执行，不会死锁；
- 外部 fence 也记入 pending（否则回收时 free 在飞 buffer 违反 VUID 00041）；wait 后仅对内部 fence reset；
- 帧内多次 Submit（含跨队列）经内部 fence 串行化（保守但正确）；
- 回收 = `DrainPending` + 按池 free 在录 buffer（零累积）。

### 7.3 一帧的完整时序

```cpp
rhi::Swapchain::Frame frame = swapchain->AcquireNextImage();
if (!frame.acquired) { /* 交换链重建窗口，跳过本帧重试 */ }

rhi::CommandList cmd = device->BeginCommandList();

// backbuffer：acquire 后按 Undefined 处理
cmd.Barrier(backbuffer: Undefined → ColorAttachment,
            dstStage=ColorAttachmentOut, dstAccess=CA 读|写);

cmd.BeginRendering({ &colorAttachment, 1, renderArea });   // Clear 深蓝
cmd.BindPipeline(*pipeline);
cmd.BindDescriptorSet(0, *set);
cmd.SetViewport({ 0, 0, w, h, 0, 1 });
cmd.Draw(3);
cmd.EndRendering();

cmd.Barrier(backbuffer: ColorAttachment → PresentSrc,
            srcStage=ColorAttachmentOut, srcAccess=CA 写,
            dstStage=AllCommands, dstAccess=MemoryRead);

rhi::SemaphoreWait  wait  { frame.ready,        rhi::PipelineStage::ColorAttachmentOut };
rhi::SemaphoreSignal signal{ frame.presentReady, rhi::PipelineStage::AllCommands };
rhi::SubmitInfo submit;
submit.commandLists = { &cmd, 1 };
submit.waits   = { &wait, 1 };    // 等 acquire（否则使用 presentable image 触发校验错误）
submit.signals = { &signal, 1 };  // 产出渲染完成信号量
device->Submit(submit);

swapchain->Present();             // 无参：内部等当帧 image 的 presentReady
device->WaitForFrame();           // 回收当期槽 + 轮转槽位
```

配套契约：
- `WaitForFrame` 只等 **Submit 侧**（fence）；present 对信号量的消费不在其覆盖内——销毁信号量/资源前须 `device->WaitIdle()`（`~Swapchain` 已内置兜底）。
- 交换链自动重建：每次获取前比对表面尺寸 + `VK_SUBOPTIMAL_KHR` 视为成功仅标记 + Acquire/Present 返回 `OUT_OF_DATE` 三类触发统一处理；重建以旧链为 `oldSwapchain`，先 `vkDeviceWaitIdle`。

## 8. 多队列

```cpp
// 队列类别（映射到 Device 选定的族；无专用族时已兜底 graphics）
rhi::QueueClass::Graphics / rhi::QueueClass::AsyncCompute / rhi::QueueClass::Transfer

rhi::CommandList cmd = device->BeginCommandList(rhi::QueueClass::AsyncCompute);
submit.queue = rhi::QueueClass::AsyncCompute;   // 本批命令目标队列；commandLists 须同族池
uint32_t family = device->QueueFamily(rhi::QueueClass::AsyncCompute); // QFOT barrier 字段 / 诊断用
```

- 命令缓冲只能提交到与其 pool 同族的队列——帧槽按族建池（present 族不建池）。
- 跨队列依赖经 `SubmitInfo::waits/signals`（semaphore）声明；barrier 的 `src/dstQueueFamily` 字段为 QFOT 占位（默认 IGNORED，执行路径待 barrier compiler）。

## 9. 已知限制与演进方向

| 项 | 状态 |
|---|---|
| barrier compiler / resource state tracker | 未实现；barrier 由调用方逐条手工声明 |
| QFOT 执行路径 | 字段占位；配对 release/acquire + semaphore 串联待 compiler（state tracker 应同时维护 layout 与 queueFamily） |
| SPIR-V 反射 | 未实现；`pso.bindings` 手工填写 |
| 上传路径（staging → CopyBufferToImage） | `CopyBufferToImage` 命令已具备，无配套 Upload API |
| sRGB 格式词表 | 缺失，`*_SRGB` 归一为 UNORM |
| Buffer 节点（渲染图侧） | 未实现 |

## 10. 陷阱备忘

- **Windows 头宏冲突**：`glfw3native.h` 引入的 `windows.h` 把 `CreateSemaphore` 展开为 `CreateSemaphoreA/W`，与 `rhi::Device::CreateSemaphore` 冲突——在 include 之后 `#undef CreateSemaphore`。
- **管线格式签名**：dynamic rendering 下 `pColorAttachmentFormats` 必须与附件视图格式一致，backbuffer 绘制用 `swapchain->Format()`。
- **信号量复用**：acquire/renderDone 的管理已内聚 Swapchain，上层只经 `Frame::ready/presentReady` 引用，勿自建单一信号量在 Present 路径复用。
