# RenderGraph 前端 API 说明

本文档描述 `RenderGraphBuilder` 多写者前端：逻辑声明、隐式版本化（SSA lowering）、前端合并与模块化封装。后端的注册、编译与执行接口不在本文范围。

涉及文件：`Engine_0/RenderGraphAbstracted.h`（类型与接口声明）、`Engine_0/RenderGraph.cpp`（实现）。

## 1. 整体链路

```
DeclareResource / DeclarePass / DeclareTopology / AliasLogicalResources
        │
        ├─ ImportDeclarations（前端合并，可选）
        ├─ CompileModule / Instantiate（模块化，可选）
        ▼
     Lower()          纯函数：逻辑声明 → 版本化 IR + 诊断，不接触 Device
        ▼
   Build(graph)       把 Lower 结果注册进 RenderGraph（不写 ProcessRegister）
        ▼
ProcessRegister / RealizeAll / 帧循环（后端既有流程）
```

原则：用户侧用**逻辑名**声明，允许多写者，但必须用拓扑显式定序；版本是 Lower 内部概念，后端只见到 `(name, version)` 已解析的单写者图。

## 2. 声明接口

### 2.1 资源

```cpp
// 选项聚合：默认即常用路径（锚点 mip0/layer0、全图 barrier、Undefined 初态、帧内）
struct ResourceOptions {
    ImageSubresource      view {};
    rhi::ImageFormat      viewFormat = rhi::ImageFormat::None;
    ImageSubresourceRange barrierRange { {}, 0, 0 };
    rhi::ImageLayout      initialLayout = rhi::ImageLayout::Undefined;
    bool                  crossFrame = false;
};

// 自有资源（owned）
void DeclareResource(std::string name, ResourceDesc desc, ResourceOptions options = {});
// 借用外部图像
void DeclareResource(std::string name, const rhi::Image* external, ResourceOptions options = {});
// 借用外部 Buffer
void DeclareResource(std::string name, const rhi::Buffer* external);
```

- 资源名即逻辑名；同一逻辑名的多次写入由 Lower 分配版本。
- C++20 指派初始化器按需覆盖：`{ .crossFrame = true }`、`{ .view = { 2, 0 }, .initialLayout = rhi::ImageLayout::TransferDst }`。
- `crossFrame = true` 表示跨帧存活（持久资源），别名策略上整链强制同块。

### 2.2 Pass

```cpp
// 读写集推导版：Render/Compute/Transfer 从 PassDesc 推导读写集
void DeclarePass(std::string name, Pass::Type type, PassDesc desc = {},
    std::optional<rhi::QueueClass> queue = std::nullopt);
// 显式版：Host/Present/Mux 无可推导绑定（Mux 输出不在 desc 内），必须用这个
void DeclarePass(std::string name,
    std::set<std::string> reads, std::set<std::string> writes,
    Pass::Type type, PassDesc desc = {},
    std::optional<rhi::QueueClass> queue = std::nullopt);
```

推导规则（与后端各 `*ReadSetMismatch`/`*WriteSetMismatch` 校验同一套分类）：

| 类型 | 读集 | 写集 |
|---|---|---|
| Render | ResolveSrc 附件、非 StorageWrite 的 shader 绑定、vertex/index 流 | 其余附件、StorageWrite 绑定 |
| Compute | 非 StorageWrite 的 shader 绑定、indirectBuffer | StorageWrite 绑定 |
| Transfer | sourceName（ClearImage 除外） | destName |

推导版传入 Host/Present/Mux 或 desc 与类型不匹配时触发 `ErrorCode::PassIoNotDerivable`。

- reads/writes 用逻辑名；Lower 推导每个 pass 触达的版本。
- **同一 pass 内同一逻辑名不允许既读又写**（`SamePassReadWrite`，error）。SPIR-V 变量只能绑一个版本，RMW 需拆成两个逻辑资源 + mux 乒乓。

### 2.3 ShaderBinding 双名分离

```cpp
struct ShaderBinding {
    std::string resourceName;       // SPIR-V 变量名：反射对账用，实例化时不变
    std::string graphResourceName;  // 图资源名：空则回落 resourceName
    ...
};
```

- `resourceName` 永远对着 shader 源码里的变量名，模块实例化时保持不变；
- `graphResourceName` 对着图里的逻辑资源名，实例化/合并时被重写。

## 3. 拓扑声明

```cpp
struct Topology {
    std::vector<BuilderEdge> edges;
    static Topology Sequence(std::vector<std::string> order); // 相邻成边
    Topology& Then(const std::string& from, const std::string& to);
    Topology& ThenAll(const std::vector<std::string>& from,
                      const std::vector<std::string>& to);    // 集合间全连接
};
void DeclareTopology(Topology topology);
```

- 拓扑边的职责：**给同一资源的多写者定序**。未被拓扑定序的多写者报 `WriterOrderUndetermined`（error）。
- 空名、自环、空集合的边被静默忽略（`ThenAll` 跳过）。

### 拓扑三情况（Lower 报告）

| 情况 | 含义 | 处理 |
|---|---|---|
| impliedAdded | 资源依赖新增了用户未声明的边 | 保留 + warning |
| conflicts | 用户拓扑成环 / 与资源依赖矛盾 | error |
| orderingOnly | 纯顺序边，不产生数据依赖 | 静默保留 |

## 4. 隐式版本规则

| 场景 | 规则 |
|---|---|
| 读 | 读当前版本 |
| 写 | 写下一版本（v(n) → v(n+1)） |
| 导入版本 | 只被读、无生产者的逻辑名创建 v0 导入版本 |
| ReadBeforeProducer | 读者排在生产者之前：仅当资源有可读初态（借用 / crossFrame / initialLayout 非 Undefined）才放行，否则 error |
| 同 pass 读写同名 | error（见 2.2） |

### 别名

```cpp
void AliasLogicalResources(std::vector<std::string> names);
```

- `LoadOp::Load` 的相邻版本自动同块；
- `crossFrame` 资源整链强制同块，字段一致，失败硬报错（`VersionAliasConflict`）；
- 用户别名组按版本链展开，校验同 pass 多触达与寿命重叠。

## 5. Lower 与 Build

```cpp
LowerResult Lower() const;                                  // 纯函数
bool Build(RenderGraph& out, LowerResult* result = nullptr) const;
```

- `Lower()` 不接触 Device/GPU，产出 `LowerResult`：
  - `ok` / `diagnostics`（`BuilderDiagnostic`：severity + kind + message + 可选 edge）
  - `topology`（`BuilderTopologyReport`：declared / impliedAdded / orderingOnly / conflicts）
  - `versions`（`BuilderVersionInfo`：资源 × 版本 × 生产者）
  - `graph`（`LoweredGraph`：版本化资源 / pass / 别名组 / 顺序边）
- `Build()` 把 Lower 结果注册进目标 `RenderGraph`：**只写声明，不调用 ProcessRegister**。失败时图保持为空。
- 诊断类别：`ImpliedEdgeAdded`（warning）、`Contradiction`、`WriterOrderUndetermined`、`ReadBeforeProducer`、`SamePassReadWrite`、`VersionAliasConflict`、`DuplicateName`、`UnknownName`、`EmptyName`、`MuxMultiWriter`。

## 6. 前端合并

```cpp
struct ImportOptions {
    std::string passPrefix;              // 追加图 Pass 名前缀
    bool dedupIdenticalResources = true; // 同名同描述资源自动合一
};
bool ImportDeclarations(const RenderGraphBuilder& other, const ImportOptions& options = {});
```

- 合并发生在 **Lower 之前**，只搬逻辑声明，不涉及版本。
- 资源：同名且描述完全一致自动合一；冲突则失败。
- Pass：追加侧加 `passPrefix` 避免同名。
- 拓扑边、别名组一并并入。
- **失败原子**：返回 false 时本 Builder 保持原状。
- 注意：后端的 `RenderGraph::ApplyMergeTask` 只接受 v0 注册式旧图，Builder 产物（含非零版本）会被守卫拒绝——合并请统一走 `ImportDeclarations`。

## 7. 模块

模块 = 只读声明快照 + 端口（内部已声明资源的对外视图）。实例化类似内联函数展开：端口显式绑定父资源，内部名加 `tag/` 作用域前缀。

### 7.1 编译模块

```cpp
enum class ModulePortKind { Input, Output, Inout };
struct ModulePort { std::string name; ModulePortKind kind; }; // name 是模块内部逻辑名

bool CompileModule(std::string name, std::vector<ModulePort> ports,
    RenderGraphModule* out, std::vector<std::string>* errors = nullptr) const;
```

校验（任一失败即拒）：

- 端口必须先 `DeclareResource` 为 **owned** 资源（外部借用不可做端口），且被至少一个 pass 引用；
- 端口不得出现在别名组中；
- 封闭世界：模块内 pass 引用的资源都在模块内声明；
- 禁 Present / Mux 节点；
- 顺序边端点存在、无自环；声明边 + 读写推导的数据边合成内部图做成环检测；
- 模块内多写者同样必须被内部拓扑定序。

编译产物 `RenderGraphModule`：`Name()` / `Ports()`（含端口资源形状快照 `ModulePortInfo`）/ `Entries()` / `Exits()`（内部图中入度/出度为 0 的 pass，内部名）。

### 7.2 实例化

```cpp
struct ModuleBinding { std::string port; std::string parentResource; };
struct ModuleInstance {
    std::string tag;
    std::vector<std::string> Entries;  // 已作用域化（tag/name）
    std::vector<std::string> Exits;
};

std::optional<ModuleInstance> Instantiate(const RenderGraphModule& module,
    std::string tag, const std::vector<ModuleBinding>& bindings,
    std::vector<std::string>* errors = nullptr);
```

- `tag` 非空、不含 `'/'`、在本 Builder 内唯一；
- `bindings` 必须**恰好覆盖**全部端口；
- 端口与父资源形状对账：`type / desc / view / viewFormat / barrierRange` 相等，`crossFrame` 单向要求（端口要求则父必须满足）；`initialLayout` 与 borrowed 不比较；
- 名称屏蔽：端口 → 父资源名（不加前缀），内部资源/Pass → `tag/name`；作用域名不得与已有声明冲突；
- 失败原子回滚；成功后返回作用域化的 `Entries/Exits`，供父拓扑接线：

```cpp
builder.DeclareTopology(RenderGraphBuilder::Topology{}
    .ThenAll(prevExits, inst.Entries)
    .ThenAll(inst.Exits, nextEntries));
```

- 多实例：同模块可多次实例化（不同 tag）。限制在**固定形状**——端口形状快照不变，不同分辨率/格式需按形状各编译一份模块。
- 嵌套模块：内部名允许含 `'/'`，因此可先实例化内层（tag `in`）再整体 `CompileModule` 外层、再以 `tag out` 实例化，名称逐层叠加为 `out/in/Temp`，端口逐层重声明。

## 8. 最小示例

```cpp
dEngine::RenderGraphBuilder b(device);
b.DeclareResource("Hdr", ImageDesc{ ... });
b.DeclareResource("Ldr", ImageDesc{ ... });
b.DeclarePass("Lighting", {}, {"Hdr"}, Pass::Render, renderDesc);
b.DeclarePass("Tonemap", {"Hdr"}, {"Ldr"}, Pass::Render, tonemapDesc);
// 多写者场景下用拓扑定序：
b.DeclareTopology(RenderGraphBuilder::Topology::Sequence({"Lighting", "Tonemap"}));

dEngine::RenderGraph graph;
if (!b.Build(graph)) { /* 查 LowerResult::diagnostics */ }
graph.ProcessRegister();
graph.RealizeAll();
```

## 9. 当前边界

- `Build` 不自动调 `ProcessRegister` / `RealizeAll`，由调用方完成；
- 静态纹理不走 Builder；
- 模块端口形状固定（形状多态模块为未来项）；
- 模块内禁 Present / Mux；
- 同 pass RMW 不可表达（需双逻辑资源 + mux 乒乓）；
- 旧图（未编译的 RenderGraph 声明）暂无导入 Builder 的入口。

## 10. 附录：用户需填写的 Desc

定义见 `Engine_0/RenderGraphDesc.h`。带默认值的字段均可省略，这里只列用户侧关注点。

### 10.1 资源 Desc（`DeclareResource` 的第 2 参）

`ResourceDesc = std::variant<std::monostate, ImageDesc, BufferDesc>`。

**ImageDesc**

| 字段 | 默认 | 说明 |
|---|---|---|
| width / height | 0 | 像素尺寸 |
| depth | 1 | 仅 Dim3D 有效 |
| arrayLayers | 1 | 数组/Cube 层数 |
| mipLevels | 1 | mip 链长度 |
| samples | 1 | MSAA 采样数 |
| format | None | 像素格式（必填） |
| viewType | Dim2D | 视图维度 |
| crossFrame | false | 跨帧存活（也可用 `ResourceOptions.crossFrame` 覆盖） |

**BufferDesc**

| 字段 | 默认 | 说明 |
|---|---|---|
| size | 0 | 字节数（必填） |
| usage | None | 显式创建位：Vertex/Index/Uniform/Storage/Transfer* |
| hostVisible | false | 决定 VMA 内存类型（Host 读写需 true） |
| crossFrame | false | 跨帧存活 |

### 10.2 Pass Desc（`DeclarePass` 的 desc 参）

`PassDesc = std::variant<std::monostate, RenderPassDesc, ComputePassDesc, HostPassDesc, TransferPassDesc, MuxPassDesc, PresentPassDesc>`。

**RenderPassDesc**

| 字段 | 说明 |
|---|---|
| attachments | 颜色/深度附件绑定列表（见 AttachmentBinding） |
| shaderBindings | 描述符绑定（见 ShaderBinding） |
| vertexStreams / indexStream | 顶点/索引流（Buffer 资源名 + offset；indexStream 另含 IndexType） |
| viewport | 默认从首个颜色附件推导尺寸 |
| pipeline | 见 PipelineDesc |
| pushConstantData | 非空时录制期按 pushConstantSize 推送 |
| bindStaticHeap | 绑定静态纹理堆到 set 0（动态资源从 set 1 起） |

**AttachmentBinding**：`resourceName`（图资源名）、`usage`（默认 Auto：深度格式→DepthStencil，否则 Color）、`slot`（颜色/resolve 用 slot 配对）、`load/store/clear`、`range`（空 = 节点 view）。

**ShaderBinding**：`resourceName`（SPIR-V 变量名，必填）、`graphResourceName`（图资源名，空 = 回落 resourceName）、`usage`（默认 Auto：图像→SampledRead，Buffer→UniformRead）、`offset/range`（Buffer 描述符窗口，0 = 整个）、`sampler`（空 = 设备默认）。预设工厂一行声明：

```cpp
ShaderBinding::Uniform("Params")          // UniformRead
ShaderBinding::Sampled("Tex", sampler)    // SampledRead，采样器可选
ShaderBinding::StorageRead("Src")
ShaderBinding::StorageWrite("Dst")
```

**PipelineDesc**：`shaders`（各阶段 SPIR-V 路径，必填）、`topology`、`tessPatchControlPoints`、`vertexBindings/vertexAttributes`（留空 = 按反射派生单流交错布局）、`cullMode`、`colorStates`（空 = 全默认）、`depthStencil`、`blendConstants`、`pushConstantSize`。

**ComputePassDesc**

| 字段 | 说明 |
|---|---|
| shader | 计算 shader（必填） |
| shaderBindings | 同 Render |
| groupCountX/Y/Z | 工作组数（Direct 派发） |
| indirectBuffer / indirectOffset | 间接派发：参数 Buffer 图资源名，推导版自动进读集 |
| pushConstantSize / pushConstantData | 同 Render |
| bindStaticHeap | 同 Render |

**TransferPassDesc**

| 字段 | 说明 |
|---|---|
| kind | BufferToImage / ImageToBuffer / BufferToBuffer / ImageToImage / ClearImage |
| sourceName / destName | 两端图资源名（推导版自动进读/写集；ClearImage 无 source） |
| srcOffset / dstOffset / byteSize | Buffer 窗口；byteSize 仅 BufferToBuffer，0 = 整段 |
| blit / linearFilter | ImageToImage：true 走 Blit（可缩放），false 走 Copy（尺寸须一致） |
| bufferImageRegions / imageRegions / blitRegions | 区域拷贝；空 = 整图 mip0 全层旧语义 |
| clearRange / clearValue | ClearImage 专用：清场范围（count 0 = 补齐到末尾）与值 |

**HostPassDesc**：只有 `callback`（`std::function<void(HostContext&)>`）。读写集不可推导，用显式 `DeclarePass` 重载声明——回调内经 `HostContext::ReadBuffer/WriteBuffer/ReadImage/WriteImage` 拿到的句柄按该读写集分类。

**MuxPassDesc**：只有 `alternatives`（候选资源名，顺序即选择索引）。**输出不在 desc 内**，因此 Mux 不能用推导版 `DeclarePass`——用显式重载：读集填 alternatives，写集填输出资源名；输出按 owned desc 声明，Build 检测到写者为 Mux 时自动走 `RegisterMuxOutput`（图像/Buffer 均可，运行时经 `SetMuxSelection` 选择候选）。

**PresentPassDesc**：空结构。读集（恰好一个图像）须用显式重载声明。

### 10.3 执行参数（DrawSource）

渲染绘制数量来源不在 PassDesc 里：`DrawSource { kind = Direct|Indirect, indirectBuffer, count, first, vertexOffset, instanceCount }` 是录制参数，由后端绘制接口传入，不参与声明。

### 10.4 MSAA 解算

颜色附件为多采样图（`ImageDesc.samples > 1`），另在同一 slot 声明一个单采样 `Usage::ResolveDst` 附件即启用解算：

```cpp
ImageDesc msaaDesc;  msaaDesc.width = 64; msaaDesc.height = 64; msaaDesc.samples = 4; ...
ImageDesc resolveDesc = msaaDesc; resolveDesc.samples = 1;

AttachmentBinding color;  color.resourceName  = "MsaaColor"; color.slot  = 0; color.load = LoadOp::Clear;
AttachmentBinding resolve; resolve.resourceName = "Resolved"; resolve.slot = 0;
resolve.usage = Usage::ResolveDst;
```

- 解算在渲染结束时由 dynamic rendering 完成（Vulkan 1.4 的逐附件 `resolveImageView` / `resolveMode`），源就是同 slot 的颜色附件，不需要单独的解算 pass。
- 管线采样数由附件推导，用户无需填写。
- 校验（声明期）：颜色/深度/解算源同 pass 采样数必须一致；解算目标必须 `samples == 1`；源与目标 format/宽高/层数必须一致；`ResolveDst` 必须能配到同 slot 的颜色附件。
- `Usage::ResolveSrc` 可选：若声明，必须与同 slot 的颜色附件同源，且该 slot 配备 `ResolveDst`。Builder 前端不必声明它（同一个逻辑名不能在一个 pass 里既读又写，`ColorAttachment` 已在写集里）。
- 深度附件可以是 MSAA，但当前不做深度解算。
- 多颜色附件可只解算其中一部分（解算是逐附件的，不存在"全有或全无"限制）。
