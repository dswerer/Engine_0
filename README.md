# Engine_0

一个 C++20 + Vulkan 的学习型渲染引擎。核心目标是把「手写屏障、手动切队列、手动同步」替换为**声明式渲染图**：用户只描述资源与 Pass 的读写关系，引擎负责拓扑排序、子资源布局转换、屏障编译、多队列切批与跨队列信号量。

项目当前以冒烟测试程序作为主要运行入口，同时它也是两层 API 的完整使用样例。

CubeReflection 和 Retrieve 使用 `UploadMesh` 上传静态几何，通过 `BindMeshGeometry` 从网格填充管线的几何字段与绘制范围。[设计与渲染对接](docs/Mesh设计与渲染对接.md) · [CPU/GPU 测试与属性查看](tests/README.md)

## 目录

1. [项目简介](#1-项目简介)
2. [快速开始](#2-快速开始)
3. [核心概念](#3-核心概念)
4. [上手：一个最小渲染图](#4-上手一个最小渲染图)
5. [能力一览](#5-能力一览)
6. [示例](#6-示例)
7. [参考文档](#7-参考文档)
8. [工程备忘](#8-工程备忘)

---

## 1. 项目简介

引擎自上而下分为四层，每层都可以独立使用：

```text
资产层   ContentResolver / ShaderLibrary / AssetCatalog / AssetPack
         逻辑路径与逻辑名解析，屏蔽磁盘目录与内存整包的差异
            │
前端层   RenderGraphBuilder / RenderGraphModule
         逻辑声明、隐式版本化、模块编译与实例化、Lower
            │
渲染图   RenderGraph / RenderGraphNodes
         资源与 Pass 拓扑、子资源屏障编译、多队列切批、Host/Transfer/Mux/Present 节点
            │
RHI 层   dEngine::rhi（Vulkan 1.3，dynamic rendering + synchronization2）
         Device / 资源 / 管线 / 描述符 / CommandList / 同步原语 / 交换链
```

- 只用 RHI 时，它等价于一个轻量 Vulkan 封装。
- 只用渲染图时，不必手写任何 `vkCmdPipelineBarrier`。
- 字段级 API 参考不在本文件，按 [第 7 节](#7-参考文档) 的链接进入专题文档。

读者分流：想跑起来看 [第 2 节](#2-快速开始)；想理解整体设计看 [第 3 节](#3-核心概念)；想写代码看 [第 4 节](#4-上手一个最小渲染图)。

---

## 2. 快速开始

### 2.1 环境要求

- Windows 10/11
- Visual Studio 2022（C++ 桌面开发工作负载）
- CMake（当前工程使用 3.31 与 Ninja 生成器）
- Vulkan SDK（提供 `Vulkan::Vulkan`、`glslc` 与校验层）
- vcpkg（当前工作区使用 `x64-windows` 三元组安装 `glfw3`、`glm`）

第三方库来源：

| 依赖 | 来源 |
|---|---|
| Vulkan、glslc | Vulkan SDK |
| shaderc（可选，运行期编译） | vcpkg（`shaderc:x64-windows`，需与本机 MSVC 工具集匹配） |
| GLFW、GLM | vcpkg |
| assimp（retrieve 示例） | vcpkg（`assimp:x64-windows`） |
| VMA、SPIR-V Reflect、stb_image、RenderDoc API | 仓库内 `Engine_0/external/` |

### 2.2 配置与构建

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

构建前必须进入 VS 开发者环境，否则 MSVC 找不到标准库头文件（环境不跨进程保留，每次开新终端都要执行）：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
cmake --build out/build/x64-debug
```

### 2.3 运行

```powershell
.\out\build\x64-debug\Engine_0\Engine_0.exe
```

程序创建 1280×720 的 GLFW 窗口，顺序执行全部冒烟测试，最后提交 3 帧到交换链后退出；全部通过时 Vulkan 校验层零报错。

### 2.4 构建备注

- 警告 C4819：源文件含中文注释但以 UTF-8 无 BOM 保存，MSVC 按代码页 936 读取，属已知无害警告。
- 严禁 BOM：`CMakeLists.txt` 若被存成 UTF-8 with BOM，CMake 会报 `Parse error`；VS 保存编码须设为「UTF-8（无 BOM）」。
- 找不到 `glslc` 时 CMake 跳过着色器编译并警告，依赖着色器的冒烟项不会执行。
- `ShaderLibrary` 也可在 `Resolve` 时把 GLSL 源编译为 SPIR-V：默认启用，可用 `SetRuntimeCompilation(false)` 关闭。构建时找到 `shaderc` 则优先在进程内编译；否则回退到 `glslc` 子进程，按 `ENGINE0_GLSLC`、构建期记录的路径、`VULKAN_SDK`、`PATH` 查找，也可用 `SetGlslcPath` 指定。编译结果有内存缓存，`SetCompileCacheDirectory` 可再加磁盘缓存。

---

## 3. 核心概念

### 3.1 分层与文件

```text
Engine_0/
├── Engine_0.cpp               # 冒烟测试主程序 + API 使用样例
├── RenderGraphBuilder.h/.cpp  # 前端：逻辑声明、版本化 lowering、模块编译与实例化
├── RenderGraphAbstracted.h    # Resource/Pass 基类、RenderGraph、HostContext
├── RenderGraphDesc.h          # 图侧描述层：资源/Pass Desc、绑定结构、Usage
├── RenderGraphNodes.h/.cpp    # Render/Compute/Host/Transfer/Mux/Present/Image/Buffer 节点
├── RenderGraph.cpp            # 注册校验、拓扑、屏障编译、批划分、录制与提交
├── RenderGraphDocument.h/.cpp # 声明层序列化：Capture / Save / Load / Restore
├── ContentResolver.h/.cpp     # 逻辑路径 -> 内容源（磁盘/内存包、越界校验）
├── AssetCatalog / AssetLoader # 资产清单登记与宿主侧解码上传
├── AssetPack / ModulePack     # DENGPAK1 整包与模块自动打包
├── ShaderLibrary.h/.cpp       # 逻辑名 -> 阶段 / SPIR-V 或 GLSL 源 / 入口点
├── rhi/                       # RHI 层（详见 rhi/README.md）
├── shaders/                   # GLSL 源文件，构建期 glslc 编译
├── assets/                    # 纹理等运行时资源
└── examples/                  # retrieve 与 cube_reflection 两个独立目标
```

### 3.2 一帧的生命周期

1. **声明**：在 Builder 上声明资源、Pass 与拓扑；`Lower()` 做纯函数推导，分配隐式版本、补数据边、产出后端声明。
2. **构建**：`Build()` 把声明写入 `RenderGraph`，随后 `ProcessRegister()` 做名称对账与注册期校验。
3. **实化**：`RealizeAll()` 创建 RHI 资源、管线、描述符，并执行屏障编译；录制前必须完成，未实化资源被引用会直接抛异常。
4. **帧开始**：`BeginFrame` 取帧槽，`BeginFrame(slot)` 套用该槽的 mux 选择并刷新描述符集。
5. **提交**：`SubmitGraph` 按依赖感知策略切批，同队列屏障批内执行，跨队列依赖拆成批尾 release + 批头 acquire。
6. **收尾**：Host 回调在异步 worker 执行，`Present` 转 `PresentSrc` 布局，`WaitForFrame` 一次等待本帧全部队列提交。

### 3.3 关键设计取舍

- **声明式屏障**：barrier compiler 是纯函数，按资源使用链的相邻 prev/next 对推导，无可变状态机；屏障计划与运行期句柄解耦，交换链 backbuffer 每帧换绑不触发重编译。
- **借用资源宿主权威**：外部（external）资源图只建模不拥有，布局与跨帧策略由父级/宿主决定；端口与固定引用收敛时只校验类型与 `crossFrame` 单向约束，不要求 Desc 逐字段相等。
- **静态纹理 bindless**：仅静态只读采样纹理进入固定 set 0 的全局堆，一次绑定长期驻留；动态资源、附件、存储图像保持 per-pass 精确绑定，屏障推导能力不受损。
- **模块即快照**：`CompileModule` 产出只读声明快照，`Instantiate` 每次从快照按 tag 作用域拷贝展开，快照本身永不被实例修改；支持嵌套，父 Builder 统一 Lower 一次。
- **多写者版本化**：资源身份是 `(逻辑名, 版本)`，读未写资源为导入版本 v0，每次写生成下一版本；多写者必须被用户拓扑完全定序，否则编译期报错。

---

## 4. 上手：一个最小渲染图

以「Host 更新参数 → Compute 模糊 → Render 合成 → Present」为例的完整最短路径。

```cpp
// (1) 挂载资源目录并加载构建期生成的着色器清单
ContentResolver assets;
assets.Mount("shaders", ENGINE_SHADER_DIR);
assets.Mount("assets",  ENGINE_ASSET_DIR);

ShaderLibrary shaders(assets);
std::vector<std::string> warnings;
if (!shaders.LoadManifest("shaders/shaders.manifest.json", &warnings)) {
    /* 构建产物缺失：检查 glslc 是否执行 */
}

// (2) 设备与交换链
auto device    = rhi::Device::Create(deviceDesc);
auto swapchain = device->CreateSwapchain();

// (3) 前端声明：资源、Pass、拓扑
RenderGraphBuilder builder(device.get());
builder.DeclareImageResource("Color",      colorDesc);
builder.DeclareImageResource("Blurred",    blurredDesc);
builder.DeclareImageResource("Backbuffer", currentBackbuffer);  // 外部图像，图不拥有

builder.DeclarePass("Update",  {}, {"Params"}, Pass::Host,    HostPassDesc{ [](HostContext&) {} });
builder.DeclarePass("Blur",    Pass::Compute, blurDesc);       // 读写集从 PassDesc 推导
builder.DeclarePass("ToneMap", Pass::Render,  tonemapDesc);    // shaderRefs 用逻辑名
builder.DeclarePass("Present", {"Backbuffer"}, {}, Pass::Present, PresentPassDesc{});

builder.DeclareTopology(
    RenderGraphBuilder::Topology::Sequence({ "Update", "Blur", "ToneMap", "Present" }));

LowerResult lower = builder.Lower();   // 纯函数，不接触 Device
if (!lower.ok) { /* lower.diagnostics 给出原因 */ }

// (4) 构建并实化
RenderGraph graph(device.get(), &shaders);
builder.Build(graph);                  // 只写后端声明
graph.ProcessRegister();
graph.GenerateSubmitSequence();
graph.RealizeAll();                    // 创建资源/管线/描述符并编译屏障
```

帧循环：

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

要点：

- 也可以跳过 Builder，直接用 `RenderGraph::RegisterResource` / `RegisterPass` 显式列出读者与写者。
- 绘制数量（`DrawSource`）是录制期参数，不进入声明；`RealizeAll` 之后用 `RenderPass::SetDraw` 按帧设置，支持直绘与间接绘制。
- 交换链重建后只需 `graph.RetargetBorrowedImage("Backbuffer", frame.backbuffer)`，不触发屏障计划重编译。

运行期常用接口：

| 接口 | 作用 |
|---|---|
| `HostContext::WaitForGpu / SignalGpu` | Host 回调内等待上游、放行下游；自动 Invalidate/Flush，未 Signal 时引擎兜底推进防死锁 |
| `HostContext::ReadBuffer/WriteBuffer/ReadImage/WriteImage` | 按名取句柄或映射指针；不在该 Host 段读写集中的资源返回 `nullptr`，图像经隐式 staging 中转 |
| `SetPassActive(name, active)` | 执行层开关；有活跃读者时不允许关闭写者 |
| `SetMuxSelection(pass, slot, index)` | 按帧槽保存 mux 选择，`BeginFrame(slot)` 套用 |
| `RenderPass::SetPushConstants` / Desc 内 `pushConstantData` | 每帧覆盖或录制期自动推送，长度须与管线 `pushConstantSize` 匹配 |
| `GetSubmitPlan / GetSubmitSequence / PrintGraph` | 诊断批划分、同步点与图结构 |

保存 / 恢复声明（可选）：

```cpp
RenderGraphDocument::Capture(builder).Save("out/graph.json");
auto loaded = RenderGraphDocument::Load(resolver, "pack:docs/graph.json"); // 也支持磁盘路径
loaded->Restore(restored, externals);   // 外部句柄与 Host 回调按名注入
```

---

## 5. 能力一览

### 5.1 前端与模块

| 能力 | 说明 |
|---|---|
| 多写者版本化 | 隐式版本分配、用户拓扑定序、矛盾诊断；同 Pass 就地读写走 `inPlace` / `StorageReadWrite` |
| 纯函数 Lower | 不接触 Device；Build 失败不留半成品，目标图保持空状态 |
| 声明合并 | Pass 前缀重写、同名同描述资源去重、顺序边与别名组合并 |
| 模块 | 快照编译（封闭世界、无环、多写者定序）、作用域实例化、深度 2 嵌套、`ThenAll` 接线；允许零端口自包含模块 |
| 资源别名 | 手动分桶 + 三层校验；版本链失败硬报错，跨资源寿命重叠自动降级 |

### 5.2 渲染图与同步

| 能力 | 说明 |
|---|---|
| 拓扑与顺序 | 基于 submitSeq 排序，显式 `after`/`before` 只追加边，不覆盖数据依赖 |
| 屏障编译 | 纯函数按使用链推导；子资源（mip/层）分区独立维护布局 |
| 多队列 | Graphics / AsyncCompute / Transfer 自动回退；依赖感知切批，同队列跨批不建 semaphore |
| 跨队列同步 | 批内 barrier + 生产批尾 release + 消费批头 acquire 三分模型 |
| 跨帧资源 | 帧首闭环上帧末次使用，支持持久布局（`crossFrame`）与多帧槽在飞 |
| 回读 | 批尾 Host 可见性 barrier，GPU→Host 回读；Host 图像读写自动合成隐式 staging |
| mux | 零命令选择点，候选代理投影屏障，换绑不重编译；图像/Buffer、Sampled/Storage/Attachment 均支持 |
| 执行层 gate | `SetPassActive` 运行期裁剪，有活跃读者时拒绝关闭写者 |

### 5.3 资源与内存

| 能力 | 说明 |
|---|---|
| Own / Borrow | 自建资源按 Desc 创建；外部句柄图只包装不销毁，支持运行期 retarget |
| 静态纹理堆 | 固定 set 0，容量 16；三级回退 bindless / 固定数组+dummy / 非致命报错 |
| 多帧槽 | 每槽独立 mux 选择与描述符集，避免飞行中更新 set |
| 借用多视图 | 同一底层句柄按 backing store 归组，布局按子资源分区统一推导 |
| Buffer 同步 | 持久映射 + 按 offset/size 的 `Flush` / `Invalidate` |

### 5.4 Pass 与管线能力

| 类型 | 能力 |
|---|---|
| Render | 多颜色附件、深度、逐附件 blend/writeMask、MSAA resolve、push constant、自动顶点布局、直绘/间接绘制 |
| Compute | `Dispatch` 与 `DispatchIndirect` |
| Transfer | 四种拷贝方向、区域拷贝、行跨度、多 mip/多层、Blit 缩放、按范围清场 |
| Host | 异步 worker、按名读写句柄、自动 Flush/Invalidate、timeline 兜底防死锁 |
| 图形阶段 | Vertex / Tessellation / Geometry 完整组合，`PatchList` 与控制点校验 |
| Present | 唯一负责转 `PresentSrc` 的收尾节点 |

### 5.5 资产、文档与平台

| 能力 | 说明 |
|---|---|
| 内容寻址 | ContentResolver 前缀挂载、越界拒绝、磁盘与 `pack:` 内存包两种来源 |
| 着色器 | 构建期清单或运行期编译 GLSL（进程内 shaderc，回退 glslc），逻辑名解析、阶段推断、SPIR-V 魔数校验、编译缓存 |
| 资产清单 | AssetCatalog 纯描述登记；AssetLoader 宿主侧解码上传与句柄缓存 |
| 整包 | DENGPAK1 内存整包零拷贝读取；模块自动收集依赖打包 |
| 文档 | 图/模块声明层 JSON 序列化，round-trip 逐字节一致，支持从包内 Load |
| 平台工具 | 每队列 timeline semaphore、fence/命令池复用、GPU 时间戳、交换链自动重建 |

### 5.6 冒烟测试

```powershell
.\out\build\x64-debug\Engine_0\Engine_0.exe
```

`Engine_0.cpp` 按子系统顺序执行二十余项验证（渲染图基础、多队列、timeline、Tess/Geometry、描述符、Transfer、Host 回读、静态纹理堆、mux 四种输出、别名、就地读写、模块嵌套、文档 round-trip、资产打包、帧循环等），通过的代表性输出：

```text
Module compile ok: ports, entry/exit, 6 rejections
Module fixed ok: init entry, uninitialized rejection, identity convergence
Asset pack ok: pack: routing, in-memory SPIR-V, no extraction
Runtime mux ok: first(red)=ok, second(blue)=ok
Frame loop ok: 3 frames presented
```

---

## 6. 示例

### 6.1 retrieve（大气散射 + boids + 阴影复现）

位于 `Engine_0/examples/retrieve/`，独立构建目标，复现原 retrieve 工程实际生效的渲染路径：

- **大气散射**：Transmittance / MultipleScattering / SkyAmbient 三张 RGBA16F LUT 一次性预计算，全屏 back pass 合成天空与场景。
- **boids**：`boids_eval.comp` → `boids_update.comp` 两段计算，1000 条鱼，单 Pass 就地读写（`StorageReadWrite`）。
- **鱼**：assimp 读取 glTF，Vertex → TessControl → TessEval → Geometry → Fragment 五阶段管线，`PatchList` 3 控制点。
- **阴影**：4096×4096 D32F 阴影图 + 光空间 3×3 PCF。
- **模块**：大气预计算与鱼群更新封装为模块，运行时从 `modules/*.pak` 加载定义并实例化。

```powershell
# 交互：鼠标抬头/转向、滚轮前进、空格暂停太阳动画、ESC 退出
.\out\build\x64-debug\Engine_0\examples\retrieve\Retrieve.exe

# 离屏冒烟：渲染 60 帧并回读 out/retrieve_smoke.png
.\out\build\x64-debug\Engine_0\examples\retrieve\Retrieve.exe --smoke 60

# 重新生成模块 .pak（模块文档在内存中生成并直接嵌入包内）
cmake --build out\build\x64-debug --target retrieve_modules
```

通过时输出 `Boids update ok: active=1000/1000 ...` 与 `Retrieve smoke ok`。注意 Vulkan NDC 的 Y 翻转在相机投影与鼠标控制中已统一修正。

### 6.2 cube_reflection（最小渲染图 + 静态纹理堆）

位于 `Engine_0/examples/cube_reflection/`：`Device::UploadImage` 一次性上载全景环境贴图，`Cube` Pass 绘制带深度测试的旋转立方体，`Composite` 全屏采样合成。环境贴图经 `DeclareStaticTexture` 注册到 set 0 slot 0，着色器通过 `sampler2D staticTextures[16]` 采样；设备不支持静态堆时回退独立采样绑定。

```powershell
.\out\build\x64-debug\Engine_0\examples\cube_reflection\CubeReflection.exe
.\out\build\x64-debug\Engine_0\examples\cube_reflection\CubeReflection.exe --smoke 30
```

冒烟模式输出 `out/cube_reflection_smoke.png`，并打印 `Static texture heap ok: slot=0`。

---

## 7. 参考文档

| 文档 | 覆盖内容 |
|---|---|
| [Engine_0/RenderGraphFrontend.md](Engine_0/RenderGraphFrontend.md) | Builder 声明接口、拓扑与隐式版本规则、Lower/Build、前端合并、模块编译与实例化、资产与整包、图文档序列化、Pass 类型与 Tess/Geometry |
| [Engine_0/rhi/README.md](Engine_0/rhi/README.md) | RHI 设计原则、Device/资源/管线/描述符、CommandList、帧循环与同步模型、多队列、陷阱备忘 |
| [docs/词表.md](docs/词表.md) | 跨板块术语、结构体字段语义、易混词对照与缩写速查 |
| [.trae/documents/](.trae/documents/) | 各阶段设计方案与实现计划（RHI 接口、多队列、子资源模型、borrow 布局跟踪等历史文档） |

源码入口：前端 [RenderGraphBuilder.h](Engine_0/RenderGraphBuilder.h)、后端图 [RenderGraphAbstracted.h](Engine_0/RenderGraphAbstracted.h) 与 [RenderGraphDesc.h](Engine_0/RenderGraphDesc.h)、RHI [rhi/RHI.h](Engine_0/rhi/RHI.h) 与 [rhi/RHIDesc.h](Engine_0/rhi/RHIDesc.h)。

---

## 8. 工程备忘

### 8.1 已知限制

- 跨队列 release 的 `dstStage` 仍取 `AllCommands`、acquire 的 `srcAccess` 为空，尚未按 stage/access 细化。
- 交换链格式变化时不自动重建管线，依赖格式不变的前提。
- 静态纹理堆固定 set 0、容量 16，无分页与多堆回退；非一致索引仍依赖设备特性。
- 无自动 mip 链生成；SPIR-V 反射不支持运行时长度描述符数组（`T[]`）。
- `ImportDeclarations` 只合并同名同描述资源，不支持异名 unify；旧版未编译图不能导入 Builder。
- 模块内禁止 Present / Mux / 静态纹理；多写者须在 `CompileModule` 前定序；端口不能属于别名组。
- 同 Pass 读写同名资源未声明 `inPlace` 时按双缓冲处理，当前报 `DoubleBufferNotImplemented`；跨帧读旧写新需双逻辑资源 + mux 乒乓。
- 图文档只保存声明层：外部句柄与 Host 回调需 Restore 时注入；不保存 mux 运行期选择与 `DrawSource`。
- 运行期不做 GLSL 编译；`Revision` 只提供变更信号，不自动重建已缓存管线。
- Task / Mesh 着色器阶段尚未实现；MSAA 深度 resolve、Transfer 逐 mip 生成尚未实现。
- 主工程无内置模型导入器（retrieve 用 assimp）；`Engine_0.cpp` 是冒烟程序而非交互编辑器。
- retrieve 的大气 LUT 使用 RGBA16F（原工程 RGBA32F），存在轻微精度差异。

### 8.2 扩展指南

**新增一个 Render Pass**：着色器加入对应目标的 `*_SHADER_SOURCES`（清单自动生成）→ 构造 `RenderPassDesc` 并用 `shaderRefs` 引用逻辑名 → Builder 声明 Pass 与资源 → Lower/Build/Realize → 运行期 `SetDraw`，需要裁剪时用 `SetPassActive`。

**新增一种 Pass 类型**：`Pass::Type` 加枚举 → `RenderGraphNodes.h` 定义节点并接入 `AutoAllocator` → `RenderGraphDesc.h` 加 Desc 并纳入 variant → `RenderGraph.cpp` 的 Desc 解析、录制与屏障访问范围补分支 → `DefaultQueueForPass` 声明默认队列。

**新增 RHI 能力**：`RHIDesc.h` 加后端无关描述 → `RHI.h` 暴露接口与能力查询 → `RHI.cpp` 启用 `VkPhysicalDeviceFeatures` 并实现；若引入新阶段/访问类型，同步 `PipelineStage` / `Access` 与 Vulkan 映射 → 在 `Engine_0.cpp` 补冒烟用例，确保校验层零报错。
