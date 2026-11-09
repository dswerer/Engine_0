# Engine_0

一个 C++20 + Vulkan 的学习型渲染引擎。核心目标是把「手写屏障、手动切队列、手动同步」替换为**声明式渲染图**：用户只描述资源与 Pass 的读写关系，引擎负责拓扑排序、子资源布局转换、屏障编译、多队列切批与跨队列信号量。

项目当前以冒烟测试程序作为主要运行入口，同时它也是两层 API 的完整使用样例。

---

## 目录

- [快速开始](#快速开始)
- [使用步骤](#使用步骤)
- [目录结构](#目录结构)
- [架构总览](#架构总览)
- [RHI 用户 API](#rhi-用户-api)
- [RenderGraph 用户 API](#rendergraph-用户-api)
- [RenderGraph 前端（Builder）](#rendergraph-前端builder)
- [资源定位与着色器登记](#资源定位与着色器登记)
- [渲染图文档（统一格式）](#渲染图文档统一格式)
- [Pass 类型](#pass-类型)
- [已实现功能](#已实现功能)
- [着色器与资源](#着色器与资源)
- [冒烟测试](#冒烟测试)
- [示例](#示例)
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

## 使用步骤

以「Host 更新参数 → Compute 模糊 → Render 合成」为例的最短路径。

### 1. 挂载资源目录

```cpp
AssetResolver assets;
assets.Mount("shaders", ENGINE_SHADER_DIR);
assets.Mount("assets",  ENGINE_ASSET_DIR);
```

### 2. 加载着色器清单

```cpp
ShaderLibrary shaders(assets);
std::vector<std::string> warnings;
if (!shaders.LoadManifest("shaders/shaders.manifest.json", &warnings)) {
    /* 构建产物缺失：检查 glslc 是否执行 */
}
```

新增着色器只需把源文件加入对应目标的 `*_SHADER_SOURCES`，清单与 C++ 引用都无需写路径。

### 3. 创建设备与交换链

```cpp
auto device = rhi::Device::Create(deviceDesc);
auto swapchain = device->CreateSwapchain();
```

### 4. 声明渲染图

```cpp
RenderGraphBuilder builder(device.get());
builder.DeclareResource("Color",      colorDesc);
builder.DeclareResource("Blurred",    blurredDesc);
builder.DeclareResource("Backbuffer", currentBackbuffer);  // 外部图像，图不拥有

builder.DeclarePass("Update", {}, {"Params"}, Pass::Host,    hostDesc);
builder.DeclarePass("Blur",   Pass::Compute, blurDesc);     // shaderRef = "blur.comp"
builder.DeclarePass("ToneMap", Pass::Render, tonemapDesc);  // shaderRefs = { ... }

builder.DeclareTopology(
    RenderGraphBuilder::Topology::Sequence({ "Update", "Blur", "ToneMap" }));

LowerResult lower = builder.Lower();
if (!lower.ok) { /* lower.diagnostics 给出原因 */ }
```

也可以跳过 Builder，直接用 `RenderGraph::RegisterResource` / `RegisterPass` 显式声明读写集。

### 5. 构建并实化

```cpp
RenderGraph graph(device.get(), &shaders);
builder.Build(graph);
graph.ProcessRegister();
graph.GenerateSubmitSequence();
graph.RealizeAll();          // 创建资源/管线/描述符并编译屏障
```

### 6. 每帧提交

```cpp
auto frame = swapchain->AcquireNextImage();
if (!frame.acquired) { /* 交换链重建，跳过本帧 */ }

const uint32_t slot = device->BeginFrame();
graph.BeginFrame(slot);
graph.SubmitGraph(RenderGraph::FrameSync{ frame.ready, frame.presentReady });
graph.WaitHostPasses();
swapchain->Present();
device->WaitForFrame();
```

### 7. 保存 / 恢复图（可选）

```cpp
RenderGraphDocument::Capture(builder).Save("out/graph.json");

auto loaded = RenderGraphDocument::Load("out/graph.json");
RenderGraphBuilder restored;
loaded->Restore(restored, externals);   // 外部资源与 Host 回调按名注入
```

文档只保存声明层；外部图像/缓冲与 Host 回调需在 `Restore` 时提供。

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
    ├── RenderGraphDocument.h/.cpp # 声明层序列化：Capture / Save / Load / Restore
    ├── AssetResolver.h/.cpp       # 逻辑路径 -> 绝对路径（挂载前缀、越界校验）
    ├── ShaderLibrary.h/.cpp       # 逻辑名 -> 阶段 / SPIR-V 路径 / 入口点
    ├── rhi/
    │   ├── RHIDesc.h              # 后端无关描述词表（不含 Vulkan 头）
    │   ├── RHI.h                  # Device/资源/管线/描述符/命令/同步/交换链
    │   └── RHI.cpp                # Vulkan 实现
    ├── shaders/                   # GLSL 源文件（构建期由 glslc 编译）
    ├── assets/                    # 纹理等运行时资源
    ├── examples/retrieve/         # 大气散射 + boids + 阴影复现示例（独立目标）
    ├── examples/cube_reflection/  # 环境贴图反射示例（独立目标）
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
│ RenderGraphBuilder 前端（可选）            │
│ · 逻辑声明、隐式版本化、Lower / Build      │
│ · 声明合并、模块编译与实例化               │
└───────────────────────────────────────────┘
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

横向辅助模块：

- `AssetResolver`：逻辑路径 → 绝对路径，隔离开发期构建目录与发布期资源包。
- `ShaderLibrary`：逻辑名 → 阶段 / SPIR-V 路径 / 入口点，由构建期清单驱动。
- `RenderGraphDocument`：把 Builder 的声明层序列化成稳定 JSON，支持跨机器保存与恢复。

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
ShaderLibrary shaders(assets);                         // 见「资源定位与着色器登记」
RenderGraph graph(device.get(), &shaders);             // 可选：启用 shaderRefs 解析

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
- 着色器用逻辑名声明：`desc.pipeline.shaderRefs = { "fullscreen.vert", "solid.frag" }`、
  `computeDesc.shaderRef = "blur.comp"`，由构造时传入的 `ShaderLibrary` 在 `RealizePass` 解析；
  `desc.pipeline.shaders` 仍保留直接路径作为低层入口（例如原始 RHI 冒烟）。

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

## RenderGraph 前端（Builder）

`RenderGraph` 后端要求每个资源显式列出读者/写者，便于精确对账；需要多写者、跨帧复用或模块化组合时，可先用 `RenderGraphBuilder` 做逻辑声明，再 Lower 成后端图。入口同样在 [RenderGraphAbstracted.h](Engine_0/RenderGraphAbstracted.h)。

```cpp
RenderGraphBuilder builder(device.get());

builder.DeclareResource("Color", colorDesc);
builder.DeclareResource("Blurred", blurredDesc);
builder.DeclareResource("Backbuffer", frame.backbuffer,
                        ResourceOptions{ .initialLayout = rhi::ImageLayout::Undefined });

// Render / Compute / Transfer 的读写集可从 PassDesc 推导
builder.DeclarePass("GBuffer", Pass::Render, gbufferDesc);
builder.DeclarePass("Blur",    Pass::Compute, blurDesc);
builder.DeclarePass("ToneMap", Pass::Render, tonemapDesc);
// Host / Mux / Present 无法推导，需显式给出读写集
builder.DeclarePass("Update", std::set<std::string>{}, {"Params"}, Pass::Host,
                    HostPassDesc{ [](HostContext&) {} });

builder.DeclareTopology(RenderGraphBuilder::Topology::Sequence({ "Update", "GBuffer", "Blur", "ToneMap" }));

LowerResult lower = builder.Lower();
if (!lower.ok) { /* lower.diagnostics 给出原因 */ }
builder.Build(graph);      // 只写后端声明，不自动 ProcessRegister
graph.ProcessRegister();
graph.RealizeAll();
```

要点：

- 资源身份是 `(逻辑名, 版本)`：读未写资源为 `v0`（导入版本），每次写生成下一版本；版本对用户隐式。
- 同一 Pass 内读写同名、多写者未被用户拓扑定序、读者排在所有写者之前且无可读初态，都会产生 `BuilderDiagnostic`。
- `Lower()` 是纯函数，不接触 `Device`；`Build()` 只注册声明，`ProcessRegister` / `RealizeAll` 由调用方完成。
- 别名策略：附件 `LoadOp::Load` 的相邻版本强制同块，`crossFrame` 整条版本链强制同块；也可用 `AliasLogicalResources` 手动指定，失败会硬报错。
- `ImportDeclarations` 在 Lower 之前合并另一份声明，可为导入 Pass 加前缀，并对同名同描述资源去重。
- 绘制数量不进入声明；`RenderPass::SetDraw` 在 `RealizeAll` 之后按帧设置。

### 模块

`CompileModule` 把当前声明快照编译为只读模块，`Instantiate` 在父 Builder 中按 tag 作用域展开实例：

```cpp
RenderGraphModule post;
builder.CompileModule("Post",
    { {"Scene", ModulePortKind::Input}, {"Out", ModulePortKind::Output} }, &post);

auto inst = builder.Instantiate(post, "postA",
    { {"Scene", "SceneColor"}, {"Out", "Backbuffer"} });
builder.DeclareTopology(RenderGraphBuilder::Topology{}.ThenAll(inst->Entries, inst->Exits));
```

- 端口必须在模块内已声明，且形状（类型 / Desc / 视图 / crossFrame）与父资源一致。
- 内部资源与 Pass 名按 tag 前缀拼接；允许嵌套模块，内部名可含 `/`。
- 模块内禁止 Present / Mux；多写者必须在 `CompileModule` 阶段已经定序；模块不单独 Lower。

---

## 资源定位与着色器登记

`AssetResolver` 与 `ShaderLibrary` 位于 RHI 与渲染图之间，负责把逻辑路径 / 逻辑名解析成磁盘文件，避免在 C++ 中散落构建机绝对路径。

### AssetResolver

```cpp
AssetResolver assets;
assets.Mount("shaders", ENGINE_SHADER_DIR);   // 前缀 -> 根目录
assets.Mount("assets",  ENGINE_ASSET_DIR);
assets.SetFallbackRoot("packed");             // 可选：发布期资源包根目录

auto file = assets.Resolve("shaders/blur.comp.spv");
// file->absolutePath / file->logicalPath / file->size / file->mtime
```

- 逻辑路径第一段是挂载前缀，按最长前缀匹配；未匹配时退到兜底根。
- `Normalize` 拒绝绝对路径、盘符与 `..` 越界，并统一分隔符；失败返回空串。
- 构造后只读，可多线程共享；错误经 `std::string*` 返回，不抛异常。

### ShaderLibrary

构建期为每个目标生成 `shaders.manifest.json`，与 `glslc` 共用同一份源列表：

```json
{
  "version": 1,
  "shaders": {
    "fullscreen.vert": { "stage": "Vertex", "path": "shaders/fullscreen.vert.spv", "entry": "main" },
    "blur.comp": { "stage": "Compute", "path": "shaders/blur.comp.spv", "entry": "main" }
  }
}
```

运行时：

```cpp
ShaderLibrary shaders(assets);
std::vector<std::string> warnings;
shaders.LoadManifest("shaders/shaders.manifest.json", &warnings);

auto stage = shaders.Resolve("blur.comp");    // 阶段 + 绝对路径 + entryPoint
```

- 阶段可由清单显式给出，也可由 `.vert/.frag/.comp/.tesc/.tese/.geom` 推断；两者冲突时登记失败。
- `Resolve` 校验文件存在与 SPIR-V 魔数（`0x07230203`），避免把 GLSL 源误配为 `.spv`。
- `ResolveStages` 批量解析并汇总全部失败原因；`Revision(name)` 返回 mtime/size 供变更检测。
- `RHI` 侧 `ShaderStageDesc::entryPoint` 默认 `"main"`，不再硬编码入口点。

---

## 渲染图文档（统一格式）

`RenderGraphDocument` 保存 **声明层**：资源、Pass、拓扑边与别名组。注册期回填字段（`resourceId`、`set`/`binding`、`alternativeIds` 等）与外部句柄不落盘，因此同一份文档可在不同机器、不同构建目录之间共享。

```cpp
RenderGraphDocument doc = RenderGraphDocument::Capture(builder);
doc.Save("out/graph.json");

auto loaded = RenderGraphDocument::Load("out/graph.json");
RenderGraphBuilder restored;
RenderGraphDocument::ExternalBindings externals;
externals.image = [&](const std::string& name) -> const rhi::Image* { /* ... */ };
externals.buffer = [&](const std::string& name) -> const rhi::Buffer* { /* ... */ };
externals.hostCallbacks["update.params"] = [](HostContext&) {};
loaded->Restore(restored, externals);

LowerResult lower = restored.Lower();   // 校验仍由 Lower 负责
restored.Build(graph);
```

文档结构：

- `resources`：名称、类型、`ImageDesc` / `BufferDesc`、view / viewFormat / barrierRange / initialLayout / crossFrame；外部资源只记 `external: "image" | "buffer"`。
- `passes`：名称、类型、队列、读写集与完整 Desc；着色器只存 `shaderRefs` / `shaderRef` 逻辑名。
- `edges` / `aliases`：顺序边与别名组，按声明顺序保存。

约束：

- 外部图像 / 缓冲由宿主在 `Restore` 时按资源名注入，缺失返回错误而非静默降级。
- Host 回调以 `HostPassDesc::callbackKey` 关联，加载时从 `ExternalBindings::hostCallbacks` 查表。
- 模块实例按展开后的扁平声明保存，模块定义本身不单独持久化。
- 输出为稳定格式 JSON（字段顺序固定），可直接用于 diff 与 round-trip 比对。

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
desc.pipeline.shaderRefs = { "tess.vert", "tess.tesc", "tess.tese", "tess.geom", "tess.frag" };
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

### 前端与模块

- 多写者版本化：隐式版本分配、用户拓扑定序、补边警告与矛盾诊断。
- `Lower` 纯函数产出后端声明；`Build` 不做实化，失败不留半成品。
- 前端声明合并：Pass 前缀重写、同名同描述资源去重、顺序边与别名组合并。
- 模块：`CompileModule` 快照校验（封闭世界、无环、多写者定序、端口规则），`Instantiate` 作用域展开，支持嵌套与 `ThenAll` 接线。

### 资源与文档

- `AssetResolver`：前缀挂载、路径规范化与越界拒绝、开发/打包两种根目录，构造后只读。
- `ShaderLibrary`：构建期清单、逻辑名解析、阶段推断与冲突检查、SPIR-V 魔数校验、`Revision` 变更检测。
- 着色器引用：`PipelineDesc::shaderRefs` / `ComputePassDesc::shaderRef`，`RealizePass` 时按 `ShaderLibrary` 解析。
- `RenderGraphDocument`：声明层 `Capture` / `Save` / `Load` / `Restore`，稳定 JSON 输出，round-trip 逐字节一致。

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

### 着色器清单

`Engine_0/CMakeLists.txt` 中的 `engine0_write_shader_manifest` 在配置期生成 `<binary>/shaders/shaders.manifest.json`，条目与 `glslc` 共用同一份 `ENGINE_SHADER_SOURCES`：

```json
{
  "version": 1,
  "shaders": {
    "fullscreen.vert": { "stage": "Vertex", "path": "shaders/fullscreen.vert.spv", "entry": "main" }
  }
}
```

清单键是 `<文件名>.<GLSL 扩展名>`（如 `blur.comp`），`stage` 由扩展名映射。运行时 `ShaderLibrary::LoadManifest` 读取该文件，Pass 只引用逻辑名。

### 运行期装配

```cpp
AssetResolver assets;
assets.Mount("shaders", ENGINE_SHADER_DIR);
assets.Mount("assets",  ENGINE_ASSET_DIR);

ShaderLibrary shaders(assets);
if (!shaders.LoadManifest("shaders/shaders.manifest.json", &warnings))
    /* 构建产物缺失 */;

RenderGraph graph(device.get(), &shaders);
```

- 主程序、`retrieve`、`cube_reflection` 三个目标各自生成并加载自己的清单。
- 原始 RHI 管线（`CreatePipeline` / `CreateComputePipeline`）仍可直接传 `ShaderStageDesc` 路径，属于低层逃生口。
- 新增着色器只需加入对应目标的 `*_SHADER_SOURCES`，清单与 C++ 引用都无需手写路径。

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
| MSAA | 4x 颜色附件 + 1x resolve，回读中心像素 |
| 纹理链路 | stb_image 解码、Host/Transfer 上传、采样绘制 |
| 前端 Lower / Build | 版本链、补边警告、Load 别名、跨帧别名、失败回滚 |
| 模块 | CompileModule 校验、作用域实例化、ThenAll 接线、深度 2 嵌套 |
| 图文档 | Capture → Save → Load → Restore → Capture，JSON 逐字节一致；非法 format 被拒绝 |

预期输出片段：

```text
Device: NVIDIA GeForce RTX 5060 Laptop GPU (Vulkan 1.4)
Queues: graphics=f0 compute=f2(dedicated) transfer=f1(dedicated) timeline=yes
Tess/Geom pipeline ok: tess=yes, geom=yes, patchControlPoints=3
Runtime mux ok: first(red)=ok, second(blue)=ok
Graph document round-trip ok: 5 resources, 6 passes, 5 edges
Frame loop ok: 3 frames presented
```

---

## 示例

### retrieve 复现

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

### cube_reflection

`Engine_0/examples/cube_reflection/` 是最小渲染图用法：`Device::UploadImage` 一次性上载全景环境贴图，`Cube` Pass 绘制带深度测试的旋转立方体，`Composite` Pass 全屏采样环境贴图与场景色，最后交给 `Present`（冒烟模式则走 `Capture` 回读）。

```powershell
.\out\build\x64-debug\Engine_0\examples\cube_reflection\CubeReflection.exe
.\out\build\x64-debug\Engine_0\examples\cube_reflection\CubeReflection.exe --smoke 30
```

冒烟模式输出 `out/cube_reflection_smoke.png`。

---

## 已知限制

- 跨队列 release 的 `dstStage` 仍取 `AllCommands`，acquire 的 `srcAccess` 为空；执行顺序由 semaphore 保证，尚未做更细的 stage/access 划分。
- 交换链格式变化时不会自动重建对应管线，当前依赖格式不变的前提。
- 静态纹理堆固定占用 set 0，容量 `kStaticTextureHeapCapacity = 16`；未实现分页与多堆回退。
- 无自动 mip 链生成；`TransferPass` 只做显式的区域拷贝与 Blit。
- SPIR-V 反射不支持运行时长度描述符数组（`T[]`），固定长度数组可用。
- `RenderGraphBuilder::ImportDeclarations` 只合并同名同描述资源，不支持异名资源统一。
- 模块内禁止 Present / Mux / 静态纹理；模块内多写者须在 `CompileModule` 前定序，端口不能属于别名组。
- 图文档只保存声明层：外部图像 / 缓冲需宿主在 `Restore` 时按名注入，Host 回调需 `callbackKey` 绑定；模块实例按展开后的名称保存，模块定义不单独持久化。
- 图文档不保存 mux 运行期选择与 `DrawSource`（绘制数量属于每帧参数）。
- 着色器清单由构建期生成，运行期不做 GLSL 编译；`ShaderLibrary::Revision` 只提供变更信号，不会自动重建已缓存管线与描述符集。
- Task / Mesh 着色器阶段尚未实现。
- 主工程没有内置模型导入器；`retrieve` 示例通过 assimp 读取 glTF，其余测试网格在代码中构造。
- 当前 `Engine_0.cpp` 是冒烟测试程序，不是持续交互的编辑器或演示器。
- retrieve 示例的大气 LUT 使用 RGBA16F（原工程为 RGBA32F），存在轻微精度差异；鱼的材质贴图未加载（原 fish fragment 实际不采样贴图）。

---

## 扩展指南

### 新增一个 Render Pass

1. 在 `shaders/` 添加 SPIR-V 源文件，并加入对应目标的 `*_SHADER_SOURCES`（清单自动生成）。
2. 构造 `RenderPassDesc`：附件、`ShaderBinding`、顶点/索引流、`PipelineDesc`；着色器用 `shaderRefs` 引用逻辑名。
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
