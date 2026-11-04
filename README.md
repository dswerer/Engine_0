# Engine_0

一个 C++20 + Vulkan 的学习型渲染引擎。核心目标是把「手写屏障、手动切队列、手动同步」替换为**声明式渲染图**：用户只描述资源与 Pass 的读写关系，引擎负责拓扑排序、子资源布局转换、屏障编译、多队列切批与跨队列信号量。

项目当前以冒烟测试程序作为主要运行入口，同时它也是两层 API 的完整使用样例。

---

## 目录

- [快速开始](#快速开始)
- [目录结构](#目录结构)
- [架构总览](#架构总览)
- [RHI 用户 API](#rhi-用户-api)
- [RenderGraph 用户 API](#rendergraph-用户-api)
- [Pass 类型](#pass-类型)
- [已实现功能](#已实现功能)
- [着色器与资源](#着色器与资源)
- [冒烟测试](#冒烟测试)
- [已知限制](#已知限制)
- [扩展指南](#扩展指南)

---

## 快速开始

### 环境要求

- Windows 10/11
- Visual Studio 2022（C++ 桌面开发工作负载）
- CMake（当前工程使用 3.31 与 Ninja 生成器）
- Vulkan SDK（提供 `Vulkan::Vulkan`、`glslc` 与校验层）
- vcpkg（当前工作区使用 `x64-windows` 三元组安装 `glfw3`、`glm`）

第三方库来源：

| 依赖 | 来源 |
|---|---|
| Vulkan、glslc | Vulkan SDK |
| GLFW、GLM | vcpkg |
| assimp（retrieve 示例） | vcpkg（`assimp:x64-windows`） |
| VMA | 仓库内 `Engine_0/external/vk_mem_alloc.h` |
| SPIR-V Reflect | 仓库内 `Engine_0/external/spv_reflect` |
| stb_image / stb_image_write | 仓库内 `Engine_0/external/stb_image` |
| RenderDoc API | 仓库内 `Engine_0/external/renderdoc` |

### 配置与构建

首次配置（路径按本机 vcpkg 实际位置调整）：

```powershell
cmake -S . -B out/build/x64-debug -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

retrieve 示例需要 assimp（首次配置前安装，已安装可跳过）：

```powershell
vcpkg install assimp:x64-windows
```

构建前必须进入 VS 开发者环境（否则 MSVC 找不到标准库头文件）：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
cmake --build out/build/x64-debug
```

### 运行

```powershell
.\out\build\x64-debug\Engine_0\Engine_0.exe
```

程序会创建 1280×720 的 GLFW 窗口，执行一串冒烟测试并输出结果，最后提交 3 帧到交换链后退出。

---

## 目录结构

```text
Engine_0/
├── CMakeLists.txt                 # 顶层工程，仅 add_subdirectory("Engine_0")
├── README.md
└── Engine_0/
    ├── CMakeLists.txt             # 可执行目标、依赖、着色器编译规则
    ├── Engine_0.h
    ├── Engine_0.cpp               # 冒烟测试主程序 + API 使用样例
    ├── RenderGraphDesc.h          # 图侧描述层：资源/Pass Desc、绑定结构、Usage
    ├── RenderGraphAbstracted.h    # Resource/Pass 基类、RenderGraph、HostContext
    ├── RenderGraphNodes.h         # RenderPass/ComputePass/HostPass/Transfer/Mux/Present/Image/Buffer
    ├── RenderGraphNodes.cpp
    ├── RenderGraph.cpp            # 注册校验、拓扑、屏障编译、批划分、录制与提交
    ├── rhi/
    │   ├── RHIDesc.h              # 后端无关描述词表（不含 Vulkan 头）
    │   ├── RHI.h                  # Device/资源/管线/描述符/命令/同步/交换链
    │   └── RHI.cpp                # Vulkan 实现
    ├── shaders/                   # GLSL 源文件（构建期由 glslc 编译）
    ├── assets/                    # 纹理等运行时资源
    ├── examples/retrieve/         # 大气散射 + boids + 阴影复现示例（独立目标）
    ├── external/                  # 第三方头文件与实现
    └── vmaExporter.cpp            # VMA 实现编译单元
```

---

## 架构总览

```text
宿主代码 / 冒烟测试
        │
        ▼
┌───────────────────────────────────────────┐
│ RenderGraph 层（dEngine）                  │
│ · Resource / Pass 拓扑                     │
│ · 拓扑排序与显式顺序约束                   │
│ · 子资源屏障编译                           │
│ · 多队列切批、跨队列信号量                 │
│ · Host / Transfer / Mux / Present 节点     │
└───────────────────────────────────────────┘
        │ 创建资源/管线、录制命令、提交
        ▼
┌───────────────────────────────────────────┐
│ RHI 层（dEngine::rhi）                     │
│ · Device / Image / Buffer / Sampler        │
│ · Pipeline / DescriptorSet                 │
│ · CommandList / Fence / Semaphore          │
│ · FrameSlot / Swapchain / 时间戳           │
└───────────────────────────────────────────┘
        │
        ▼
      Vulkan 1.3（dynamic rendering + synchronization2）
```

两层可以独立使用：只用 RHI 时等价于一个轻量 Vulkan 封装；只用 RenderGraph 时不必手写任何 `vkCmdPipelineBarrier`。

---

## RHI 用户 API

入口为 [rhi/RHI.h](Engine_0/rhi/RHI.h)，描述词表为 [rhi/RHIDesc.h](Engine_0/rhi/RHIDesc.h)。

### 设备与资源

```cpp
rhi::DeviceDesc desc;
desc.nativeWindowHandle = glfwGetWin32Window(window); // 为空则无交换链（离屏模式）
desc.width = 1280;
desc.height = 720;
desc.enableValidation = true;      // 默认开启校验层
desc.maxFramesInFlight = 2;        // 帧槽数量，钳制到 [1,8]

auto device = rhi::Device::Create(desc);
```

| 类别 | 接口 |
|---|---|
| 资源 | `CreateImage`、`CreateBuffer`、`CreateSampler` |
| 管线 | `CreatePipeline(GraphicsPipelineDesc)`、`CreateComputePipeline` |
| 描述符 | `AllocateDescriptorSet`、`CreateDescriptorSetLayout` |
| 同步 | `CreateFence`、`CreateSemaphore`、`CreateTimelineSemaphore` |
| 交换链 | `CreateSwapchain` |
| 帧 | `BeginFrame`、`BeginCommandList`、`Submit`、`WaitForFrame`、`WaitAllFrames`、`WaitIdle` |

能力查询：`SupportsBindlessTextures`、`SupportsIndependentBlend`、`SupportsDepthBounds`、`SupportsTessellation`、`SupportsGeometryShaders`、`SupportsVertexPipelineStoresAndAtomics`、`TimelineSemaphoresSupported`、`TimestampsSupported` / `TimestampsComparable` / `TimestampPeriodNs`。

### 资源细节

- `Image::CreateView(ImageViewDesc)` 支持 mip/层区间、格式重解释与视图类型覆盖。
- `Image::ReinterpretView(format, range)` 可直接以另一格式取得图像视图。
- `CommandList::TransitionImage(...)` 封装单图布局转换，免去手写 `ImageBarrier`。
- `Buffer::Map()` 为持久映射；非一致内存用 `Flush` / `Invalidate` 显式同步。
- `Image` 支持 Own 与 Borrow 两种模式；交换链图像通过 Borrow 包装，析构不销毁底层句柄。

### 命令录制

`rhi::CommandList` 覆盖：

- 屏障：`Barrier(images, buffers)`
- 动态渲染：`BeginRendering` / `EndRendering`
- 状态：`BindPipeline`、`BindDescriptorSet`、`PushConstants`、`SetViewport`
- 几何：`BindVertexBuffer`、`BindIndexBuffer`、`Draw` / `DrawIndexed` / `DrawIndirect` / `DrawIndexedIndirect`
- 计算：`Dispatch`、`DispatchIndirect`
- 拷贝：`CopyBufferToImage`、`CopyImageToBuffer`、`CopyBuffer`、`CopyImage`、`BlitImage`
- 清场：`ClearColor`、`ClearImage`
- 时间戳：`ResetTimestamps`、`WriteTimestamp`

### 提交与同步

- `QueueClass::Graphics / AsyncCompute / Transfer`；无专用队列时自动回退图形队列。
- 每队列一条 timeline semaphore，`Submit` 自动分配单调递增的值。
- `SubmitInfo` 的 `waits` / `signals` 支持 binary 与 timeline 两种语义。
- `FrameSlot` 内置命令池、fence 池与时间戳查询池，`WaitForFrame` 只回收当前帧槽的在飞提交。

---

## RenderGraph 用户 API

核心接口在 [RenderGraphAbstracted.h](Engine_0/RenderGraphAbstracted.h)，Desc 在 [RenderGraphDesc.h](Engine_0/RenderGraphDesc.h)。

### 1. 声明资源与 Pass

```cpp
RenderGraph graph(device.get());

// 自建资源：读者集合、写者、类型、Desc
graph.RegisterResource("Color",   {"Blur"}, "GBuffer", Resource::Image, colorDesc);
graph.RegisterResource("Blurred", {"ToneMap"}, "Blur", Resource::Image, blurredDesc);

// 借用外部资源：图不创建、不拥有
graph.RegisterResource("Backbuffer", {"Present"}, "Composite", frame.backbuffer,
                       {}, rhi::ImageLayout::Undefined);

// 注册 Pass：读集、写集、类型、Desc、可选顺序约束与目标队列
graph.RegisterPass("GBuffer", {},          {"Color"},   Pass::Render,  gbufferDesc);
graph.RegisterPass("Blur",    {"Color"},   {"Blurred"}, Pass::Compute, blurDesc);
graph.RegisterPass("Present", {"Backbuffer"}, {},       Pass::Present, PresentPassDesc{});
```

- `ShaderBinding.resourceName` 必须与着色器变量名一致，引擎按名对账。
- `AttachmentBinding` 的 `slot` 必须从 0 开始稠密排列；`load` / `store` / `clear` 显式声明。
- `PassOrder.after` / `.before` 只追加顺序边，不会覆盖真实数据依赖。
- 顶点/索引流通过 `VertexStreamBinding` / `IndexStreamBinding` 声明，不进入描述符绑定。

### 2. 编译

固定顺序：

```cpp
graph.ProcessRegister();        // 名称对账、自动 usage 推导、注册期校验
graph.GenerateSubmitSequence(); // 拓扑排序 + 显式顺序约束
graph.AliasResources({ ... });  // 可选：手动别名分桶
graph.RealizeAll();             // 创建 RHI 资源/管线/描述符，并自动 CompileBarriers
```

`RealizeAll` 内部会执行：别名解析、帧槽准备、资源实例化、Pass 实例化、`CompileBarriers()`。

### 3. 帧循环

```cpp
rhi::Swapchain::Frame frame = swapchain->AcquireNextImage();
if (!frame.acquired) { /* resize / 失焦，跳过本帧 */ }

uint32_t slot = device->BeginFrame();
graph.SetMuxSelection("SelectMux", slot, 0); // 可选
graph.BeginFrame(slot);                      // 套用该槽 mux 选择并刷新该槽描述符

graph.SubmitGraph(RenderGraph::FrameSync{ frame.ready, frame.presentReady });
graph.WaitHostPasses();
swapchain->Present();
device->WaitForFrame();
```

交换链重建后只需：

```cpp
graph.RetargetBorrowedImage("Backbuffer", frame.backbuffer);
```

不会触发屏障计划重编译。

### 4. 绘制数量

`RenderPass::SetDraw(DrawSource)` 每帧设置，支持 CPU 直绘与 GPU 间接参数：

```cpp
DrawSource draw;
draw.kind = DrawSource::Indirect;
draw.indirectBuffer = "DrawArgs";
draw.count = 1;                 // 间接条目数
renderPass->SetDraw(draw);
```

### Push Constants

`RenderPassDesc` / `ComputePassDesc` 的 `pushConstantData` 会在录制阶段自动推送；也可用 `RenderPass::SetPushConstants` / `ComputePass::SetPushConstants` 每帧覆盖。数据长度须与 `PipelineDesc.pushConstantSize` 匹配，超出时按管线声明大小截断。

### 5. HostContext

Host 节点的 CPU 回调运行在异步工作线程：

```cpp
hostPass->SetCallback([](HostContext& ctx) {
    ctx.WaitForGpu();                       // 等上游 GPU/Host 完成，并自动 Invalidate 读集
    if (void* p = ctx.WriteImage("Color")) // 图像经隐式 staging 中转
        /* 写像素 */;
    ctx.SignalGpu();                        // 自动 Flush 写集并推进主机 timeline
});
```

- `ReadBuffer` / `WriteBuffer` 按名取句柄，未声明则返回 `nullptr`。
- `ReadImage` / `WriteImage` 返回映射指针，无需手动管理 staging。
- 未调用 `SignalGpu` 时引擎会兜底推进，避免死锁；无 timeline 支持时自动降级为主线程同步执行。

### 6. 运行期控制

| 接口 | 作用 |
|---|---|
| `SetPassActive(name, active)` | 执行层开关；有活跃读者时不允许关闭写者 |
| `SetMuxSelection(pass, slot, index)` | 按帧槽保存 mux 选择 |
| `RegisterMuxOutput(name, readers, writer, desc)` | 注册 Borrow-later 的 mux 输出 |
| `GetSubmitPlan` / `GetSubmitSequence` | 诊断批划分与同步点 |
| `PrintGraph` | 打印资源、Pass、读写边与提交顺序 |

---

## Pass 类型

| 类型 | Desc | 关键字段 |
|---|---|---|
| `Pass::Render` | `RenderPassDesc` | 附件、`ShaderBinding`、顶点/索引流、viewport、`PipelineDesc` |
| `Pass::Compute` | `ComputePassDesc` | compute shader、工作组数、间接派发 buffer |
| `Pass::Host` | `HostPassDesc` | CPU 回调（可在运行期用 `SetCallback` 覆盖） |
| `Pass::Transfer` | `TransferPassDesc` | Buffer↔Image、Buffer↔Buffer、Image↔Image、ClearImage；支持区域拷贝与 Blit |
| `Pass::Mux` | `MuxPassDesc` | `alternatives` 候选名；零命令选择点 |
| `Pass::Present` | `PresentPassDesc` | 只把被读图像转到 `PresentSrc` |

资源类型：

- `Resource::Image`：Own（按 `ImageDesc` 创建）或 Borrow（包装外部 `rhi::Image`）。
- `Resource::Buffer`：Own（按 `BufferDesc` 创建）或 Borrow。
- `RegisterStaticTexture`：静态只读纹理进入固定 set 0 的 bindless 堆，一次绑定长期有效。

### 细分 / 几何着色器

`PipelineDesc` 支持完整图形阶段组合，`RenderPass` 会原样透传给 RHI：

```cpp
RenderPassDesc desc;
desc.pipeline.shaders = {
    { rhi::ShaderStage::Vertex,      shaderDir + "/tess.vert.spv" },
    { rhi::ShaderStage::TessControl, shaderDir + "/tess.tesc.spv" },
    { rhi::ShaderStage::TessEval,    shaderDir + "/tess.tese.spv" },
    { rhi::ShaderStage::Geometry,    shaderDir + "/tess.geom.spv" },
    { rhi::ShaderStage::Fragment,    shaderDir + "/tess.frag.spv" },
};
desc.pipeline.topology = PrimitiveTopology::PatchList; // 细分管线必须
desc.pipeline.tessPatchControlPoints = 3;              // 每个 patch 的控制点数
```

RHI 会在创建管线时校验：

- 设备是否支持 `tessellationShader` / `geometryShader`；
- `TessControl` 与 `TessEval` 必须成对；
- 细分管线必须使用 `PatchList`；
- patch 控制点数必须在 `[1, limits.maxTessellationPatchSize]` 内。

Task/Mesh 阶段仍为预留，尚未实现。

---

## 已实现功能

### 渲染图与同步

- 基于 submitSeq 的拓扑排序，支持显式 `after` / `before` 顺序约束。
- 纯函数式屏障编译器：按资源使用链推导 barrier，支持增量重算。
- 子资源分区：按 mip / 层范围独立维护布局与依赖。
- 跨帧资源：帧首闭环上帧末次使用，支持持久布局（`crossFrame`）。
- 跨队列依赖三分：批内 barrier、生产批尾 release、消费批头 acquire。
- 依赖感知多队列切批；同队列跨批不生成 semaphore。
- 批尾 Host 可见性 barrier，支持 GPU→Host 回读。
- `PipelineStage` / `Access` 覆盖顶点输入、间接读取、Host、Tess/Geometry 等阶段。

### 资源与内存

- 手动别名分桶：静态 Desc 兼容、跨帧/初始布局一致、批级寿命不重叠三层校验；失败自动降级为独立资源。
- 静态纹理 bindless 堆：固定 set 0，动态资源从 set 1 起，容量 `kStaticTextureHeapCapacity = 16`。
- 图像隐式 staging：Host 图像读写自动合成 staging buffer 与 Transfer Pass。
- 多帧槽：每槽独立 mux 选择与描述符集，避免 set 在飞行中更新。
- mux 候选代理：输出用途投影到所有候选的屏障链，换绑不重编译计划；支持图像与 Buffer。

### Pass 能力

- Render：多颜色附件、深度、逐附件 blend/writeMask、push constant、自动顶点布局、直绘与间接绘制。
- Compute：普通派发与 `DispatchIndirect`。
- Transfer：五种方向、区域拷贝、行跨度、多 mip/多层、Blit 缩放、按范围清场。
- Host：按名读写句柄、自动 Flush/Invalidate、异步执行与上游等待。
- Present：唯一负责转 `PresentSrc` 的收尾节点。

### 平台与工具

- 多队列：Graphics / AsyncCompute / Transfer，自动回退。
- 每队列独立 timeline semaphore；提交值单调递增。
- 帧槽 fence 池与命令池复用。
- GPU 时间戳：逐批打点、跨队列区间比较。
- 交换链：自动重建、acquire/present 信号量管理。

---

## 着色器与资源

- 着色器源文件位于 `Engine_0/shaders/`，构建期由 `glslc --target-env=vulkan1.3` 编译。
- 产物输出到 `${CMAKE_BINARY_DIR}/shaders`，命名规则为 `<文件名>.<扩展名>.spv`，例如 `tess.tesc.spv`。
- 编译期宏 `ENGINE_SHADER_DIR` 指向该目录；`ENGINE_ASSET_DIR` 指向 `Engine_0/assets`。
- 若未找到 `glslc`，CMake 会跳过着色器编译并给出警告，依赖着色器的冒烟项不会执行。

新增着色器只需把源文件加入 `Engine_0/CMakeLists.txt` 的 `ENGINE_SHADER_SOURCES` 列表。

---

## 冒烟测试

`Engine_0.cpp` 按顺序执行以下验证，全部通过时控制台无 Vulkan 校验错误：

| 项目 | 验证内容 |
|---|---|
| 渲染图基础 | 资源/Pass 注册、拓扑排序、Realize、管线创建 |
| 多队列 | compute → graphics 跨队列 semaphore |
| Timeline | 主机信号、GPU 等待、最终值推进 |
| Tess/Geometry | `PatchList` 管线创建、细分绘制、回读中心像素 |
| 描述符 | 采样图像、Uniform Buffer、反射布局 |
| Transfer | 区域拷贝、多 mip、按 mip 清场、层拷贝 |
| Host | staging 上传、图像读回、GPU→Host 回读 |
| 静态纹理堆 | bindless set 0 绑定与回读 |
| 逐附件状态 | 不同附件 blend 与 writeMask |
| 顺序约束 | 同队列无 semaphore、跨队列 1 个同步点 |
| mux | Sampled / Storage / Attachment / Buffer 四种输出的运行期切换 |
| 执行层开关 | 有活跃读者时拒绝关闭写者 |
| 多帧在飞 | 两个帧槽交替提交，无中间等待 |
| 别名 | 共享句柄成功与校验失败降级 |
| 跨帧持久 | 同一图连续两帧重提交 |
| 帧循环 | acquire → SubmitGraph → Present → WaitForFrame，连续 3 帧 |

预期输出片段：

```text
Device: NVIDIA GeForce RTX 5060 Laptop GPU (Vulkan 1.4)
Queues: graphics=f0 compute=f2(dedicated) transfer=f1(dedicated) timeline=yes
Tess/Geom pipeline ok: tess=yes, geom=yes, patchControlPoints=3
Runtime mux ok: first(red)=ok, second(blue)=ok
Frame loop ok: 3 frames presented
```

---

## 示例：retrieve 复现

`Engine_0/examples/retrieve/` 是独立构建目标，复现原 `retrieve` 工程实际生效的渲染路径：

- **大气散射**：Transmittance / MultipleScattering / SkyAmbient 三张 LUT 一次性预计算，再由全屏 back pass 读取 canvas + depth 合成天空与场景。
- **boids**：`boids_eval.comp` → `boids_update.comp` 两段计算，1000 条鱼，数据布局为 `pos[3N] + dir[3N] + vel[N]`。
- **鱼**：assimp 读取 `lowpoly_fish.glb`，使用 `Vertex → TessControl → TessEval → Geometry → Fragment` 五阶段管线，拓扑 `PatchList`，3 控制点。
- **阴影**：独立深度-only Shadow Pass 写入 4096×4096 D32F 阴影图，主 Pass 用光空间矩阵做 3×3 PCF。

运行：

```powershell
# 交互：鼠标上下抬头/低头、左右转向、滚轮前进、空格暂停太阳动画、ESC 退出
.\out\build\x64-debug\Engine_0\examples\retrieve\Retrieve.exe

# 离屏冒烟：渲染 60 帧并回读 out/retrieve_smoke.png
.\out\build\x64-debug\Engine_0\examples\retrieve\Retrieve.exe --smoke 60
```

预期输出：

```text
Fish mesh loaded: verts=206 indices=786
Atmosphere LUT precompute ok
Boids update ok: active=1000/1000 inactive=0 badDir=0 badPos=0 ...
Smoke image: min=0 max=238 mean=...
Retrieve smoke ok: wrote out/retrieve_smoke.png
```

实现约定：

- LUT 使用可线性过滤的 RGBA16F。
- Vulkan NDC 需要 Y 翻转，因此相机投影做了 `proj[1][1] *= -1`，鼠标 pitch 与滚轮方向也按翻转后的坐标修正。
- 默认相机为 `(0, 45, -90)`、俯角 8°，便于同时看到大气地平线与鱼群。
- boids 的 eval/update 公式、常量、`pos += vel * 0.001` 均与原工程一致；`BoidsParams.minSpeed` 默认 0（完全按原工程），可用 `--boids-min-speed 0.5` 打开演示用最低巡游速度。
- 修复过一个复现 bug：`outvel` 的 CPU 缓冲曾按 `sizeof(vec3) * N`（12N 字节）分配，但 std430 中 `vec3` 数组的步长是 16 字节，导致 id ≥ 768 的鱼越界写。表现正好是 232 条鱼（id 768..999）速度归零、位置和方向停在初始化值；把缓冲改为 `vec4[1024]`、按 16N 分配后，`minSpeed = 0` 下也是 `active=1000/1000`、`zeroVel=0`、`sameAsInitDir=0`。

---

## 已知限制

- MSAA Resolve 仅完成声明校验，执行逻辑尚未实现。
- 跨队列 barrier 未按 stage/access 细化，同步策略偏保守。
- 交换链格式变化时不会自动重建对应管线，当前依赖格式不变的前提。
- 静态纹理堆容量受 descriptor indexing 限制，暂未实现分页与多堆回退。
- Task / Mesh 着色器阶段尚未实现。
- 主工程没有内置模型导入器；`retrieve` 示例通过 assimp 读取 glTF，其余测试网格在代码中构造。
- 当前 `Engine_0.cpp` 是冒烟测试程序，不是持续交互的编辑器或演示器。
- retrieve 示例的大气 LUT 使用 RGBA16F（原工程为 RGBA32F），存在轻微精度差异；鱼的材质贴图未加载（原 fish fragment 实际不采样贴图）。

---

## 扩展指南

### 新增一个 Render Pass

1. 在 `shaders/` 添加 SPIR-V 源文件，并加入 `ENGINE_SHADER_SOURCES`。
2. 构造 `RenderPassDesc`：附件、`ShaderBinding`、顶点/索引流、`PipelineDesc`。
3. `RegisterPass` 声明读写集，`RegisterResource` 声明资源与读者/写者。
4. `ProcessRegister` → `GenerateSubmitSequence` → `RealizeAll`。
5. 运行期用 `RenderPass::SetDraw` 设置绘制数量；需要时用 `SetPassActive` 做执行层裁剪。

### 新增一种 Pass 类型

1. 在 `Pass::Type` 增加枚举值。
2. 在 `RenderGraphNodes.h` 定义节点类，并在 `Pass::AutoAllocator` 中接入。
3. 在 `RenderGraphDesc.h` 增加对应 Desc，并加入 `PassDesc` variant。
4. 在 `RenderGraph.cpp` 的 `Resolve*Desc`、`RecordPass`、屏障访问范围解析中补齐分支。
5. 默认队列映射在 `DefaultQueueForPass` 中声明。

### 新增 RHI 能力

1. 在 `RHIDesc.h` 增加后端无关的描述字段与枚举。
2. 在 `RHI.h` 暴露创建接口与能力查询。
3. 在 `RHI.cpp` 查询并启用对应 `VkPhysicalDeviceFeatures`，实现管线/资源创建。
4. 若新增阶段或访问类型，同步更新 `PipelineStage` / `Access` 与 `ToVkPipelineStage` 映射。
5. 在 `Engine_0.cpp` 增加冒烟用例，确保校验层零报错。
