# ImageDesc 补全 + AttachmentBinding / ShaderBinding 落地 — 实施计划

## Summary

本轮完成三件事（Buffer 明确跳过）：

1. **补全 `ImageDesc`**：新增 `viewType`（7 种视图类型）、`arrayLayers`（与 3D `depth` 拆分为两个维度）；`ImageFormat` 扩展为精简实用集（14 个可映射 VkFormat 的常用值）。
2. **落地 `AttachmentBinding` + `ShaderBinding`**：二者平级，都作为 `RenderPassDesc` 的成员（不放 Pass 公共层——未来可能有非 shader 节点）。`ShaderBinding` 显式携带 `set/binding`，shader 反射作为未来可选层叠加。
3. **资源引用用分开的内联字段 `resourceName` + `resourceId`**（不用 variant、也不引入 `ResourceRef` 包装结构）：直接作为 `AttachmentBinding` / `ShaderBinding` 的成员，用户只填 `resourceName`，`ProcessRegister` 独占回填 `resourceId`，回填后 name 保留作 debug 标签。

校验策略（用户已确认）：名字查不到、color slot 重复/越界、depth 多于一个、与读写集不一致、set/binding 重复、非附件 usage 出现在 attachments 等 → **throw std::runtime_error 终止**（与环检测一致，比现有的"名字解析失败仅 cerr"更严格——本结构的错误属于声明期错误，应当fail-fast）。

## Current State Analysis

- `RenderGraphAbstracted.h`：`RenderPassDesc` 是空占位；`ImageDesc` 只有 width/height/depth/mipLevels/samples/format；`ImageFormat` 仅 None/RGB/RGBA/Depth。
- `RenderPass`（RenderGraphNodes.h）存整份 `RenderPassDesc`，`get_desc()` 已有——**结构对新字段自动兼容，无需改动**。
- `Pass.cpp` / `Resource.cpp` 的 `AutoAllocator` 按 variant 分支透传 Desc——**无需改动**。
- `ProcessRegister`（RenderGraph.cpp）当前只解析 read/write 集合的名字→id，不触碰 desc。
- `PrintGraph` 已有 `FormatName` lambda（需为新枚举更新）和 Image desc 摘要打印（需扩展 viewType/arrayLayers）。
- 示例图（Engine_0.cpp）：GBuffer→Blur→ToneMap，Blur 是 Compute 无 desc。

## Proposed Changes

### 1. RenderGraphAbstracted.h（核心改动）

新增 `#include <array>`（ClearValue 需要）。

**替换 ImageFormat 枚举**（删除 RGB/RGBA/Depth，示例代码同步更新）：

```cpp
enum class ImageFormat {
    None,
    R8, RG8, RGBA8, BGRA8,
    R16F, RG16F, RGBA16F,
    R32F, RG32F, RGBA32F,
    R11G11B10F,
    D32F, D24S8, S8
};
```

**新增 ImageViewType + 修订 ImageDesc**：

```cpp
enum class ImageViewType { None, Dim1D, Dim2D, Dim3D, Dim1DArray, Dim2DArray, Cube, CubeArray };

struct ImageDesc {
    uint32_t    width = 0;
    uint32_t    height = 0;
    uint32_t    depth = 1;        // 仅 Dim3D 有效
    uint32_t    arrayLayers = 1;  // 数组/Cube 层数（与 3D depth 是两个维度）
    uint32_t    mipLevels = 1;
    uint32_t    samples = 1;
    ImageFormat format = ImageFormat::None;
    ImageViewType viewType = ImageViewType::Dim2D;
};
```

**新增共享类型**（放在 ImageDesc 之后、RenderPassDesc 之前）：

```cpp
// 逐次访问的用途（非资源属性）。Buffer 相关 usage 等 Buffer 落地时再补；
// 无 ReadWrite 组合值：图不支持自环，同 pass 读写同一资源由前端翻译成双缓冲/版本化
enum class Usage {
    Auto,
    ColorAttachment, DepthStencilAttachment, ResolveSrc, ResolveDst,
    SampledRead, StorageRead, StorageWrite
};

enum class LoadOp  { Load, Clear, DontCare };
enum class StoreOp { Store, DontCare };

struct DepthStencilValue { float depth = 1.0f; uint32_t stencil = 0; };
struct ClearValue {
    std::variant<std::array<float,4>, DepthStencilValue> value;
};

// 绑定处的子资源选择（Image 的 view 参数；range 形式留待将来）
struct ImageSubresource {
    uint32_t mipLevel = 0;
    uint32_t layer = 0;
};

struct ViewportDesc {
    bool  autoFromColor = true;  // true: 从首个颜色附件推导尺寸，忽略矩形
    float x = 0, y = 0, w = 0, h = 0;
    float minDepth = 0.0f, maxDepth = 1.0f;
};

struct AttachmentBinding {
    std::string resourceName;    // 声明期用户填
    std::optional<uint32_t> resourceId;  // ProcessRegister 回填（用户不填）
    Usage usage = Usage::Auto;   // Auto: 按 target 资源的 format 推断（深度格式→DepthStencil，否则 Color）
    uint32_t slot = 0;           // 仅 ColorAttachment 有语义；depth/resolve 填 0 被忽略
    ImageSubresource view {};
    LoadOp  load  = LoadOp::DontCare;
    StoreOp store = StoreOp::Store;
    ClearValue clear {};
};

struct ShaderBinding {
    std::string resourceName;
    std::optional<uint32_t> resourceId;
    Usage usage = Usage::Auto;   // Auto: 推断为 SampledRead
    uint32_t set = 0;
    uint32_t binding = 0;
    ImageSubresource view {};
};

inline constexpr uint32_t kMaxColorAttachments = 8;

struct RenderPassDesc {
    std::vector<AttachmentBinding> attachments;   // 顺序无语义，slot 显式
    std::vector<ShaderBinding> shaderBindings;   // 与 attachments 平级
    ViewportDesc viewport {};
};
```

### 2. RenderGraph.cpp

**ProcessRegister**：在 read/write 集合解析完成之后、`AutoAllocator` 之前，对持有 `RenderPassDesc` 的 pass 执行**解析 + 校验**（在局部 desc 副本上原地修改，回填结果随 AutoAllocator 存入 RenderPass，无需改 RenderPass/友元）：

1. **Auto 推断**（附件）：查 `tmp_resources` 中 `resourceName` 对应资源的 `ImageDesc::format`，`D32F/D24S8/S8` → `DepthStencilAttachment`，否则 `ColorAttachment`。ShaderBinding 的 Auto → `SampledRead`。
2. **名字解析回填**：`resourceName` 查 `tmp_resources` 得 id 回填 `resourceId`；name 为空或查不到 → throw（消息含 pass 名与资源名）。id 无条件覆盖（编译器独占）。
3. **usage 合法性**：attachments 只接受 ColorAttachment/DepthStencilAttachment/ResolveSrc/ResolveDst；shaderBindings 只接受 SampledRead/StorageRead/StorageWrite → 否则 throw。
4. **slot 校验**：ColorAttachment 的 slot 重复或 ≥ kMaxColorAttachments → throw；DepthStencilAttachment（含推断后）多于一个 → throw。
5. **读写集一致性**：ColorAttachment/DepthStencilAttachment/ResolveDst 的 `resourceId` 必须在该 pass 的 write 集；ResolveSrc 与 SampledRead/StorageRead 必须在 read 集；StorageWrite 必须在 write 集 → 否则 throw（消息指明是哪个绑定、期望读还是写）。
6. **set/binding 去重**：同一 pass 内 shaderBindings 的 (set, binding) 重复 → throw。
7. **附件/sampler 冲突**：同一资源同时出现在本 pass 的 attachments 与 shaderBindings（= 同 pass 又读又写 = 自环）→ throw（提前于依赖边生成阶段的自环检测报错）。

throw 统一用 `std::runtime_error`（`<stdexcept>` 已包含），消息格式：`"RenderGraph: <pass 名> <具体错误>"`。

**PrintGraph**：

- `FormatName` lambda 扩展到全部新枚举值。
- Image desc 摘要增加 viewType、arrayLayers。
- RenderPass 新增 desc 打印（经 `dynamic_cast<RenderPass*>` 后取 `get_desc()`）：

```
      att  =[#0 Color(slot0,ColorAttachment,Clear/Store) #1 Depth(slot0,DepthStencilAttachment,Clear/Store)]
      bind=[set0.binding0 Blurred(SampledRead)]
```

附一个 `UsageName` lambda。

### 3. Engine_0.cpp（示例图更新）

- Color/Blurred：`RGBA16F`；Depth：`D32F`；Backbuffer：`RGBA8`（其余字段用默认值，viewType 默认 Dim2D）。
- GBuffer 的 RenderPassDesc：Color 附件（slot 0，load=Clear）+ Depth 附件（usage 显式 DepthStencilAttachment，load=Clear，clear.depth=1.0）。
- ToneMap 的 RenderPassDesc：Backbuffer 附件（slot 0，load=Clear）+ ShaderBinding（Blurred，SampledRead，set 0 / binding 0）。
- Blur 保持 Compute 无 desc 不变。

### 4. 无需改动

`RenderGraphNodes.h`、`Pass.cpp`、`Resource.cpp`、`CMakeLists.txt`。

## Assumptions & Decisions

- 资源引用用 name + optional\<id\> 直接内联为绑定结构的成员（用户指定，否决 variant 与 `ResourceRef` 包装两种方案）。
- ShaderBinding 放 RenderPassDesc 内与 AttachmentBinding 平级（用户指定，否决 Pass 公共层——未来可能有 CUDA/CPU 节点）；将来 ComputePassDesc 可复用 `ShaderBinding` 类型本身。
- usage 是"逐次访问"属性，挂在绑定上而非资源上；资源创建 usage 标志 = 编译器对全图访问取并集，留到 barrier compiler 轮。
- **不采用"由绑定推导 read/write 集"的长期路线**：不能假定所有 pass 的资源访问都有丰富 usage 信息——例如将来的 CPU pass 在主机端计算，其内存没有 attachment/sampled/storage 这类类别，只有读与写。故 read/write 集必须保持为图结构的一等输入（由 `RegisterPass` 显式声明），usage 仅是绑定的附加子信息。校验规则 5 因此长期保留。
- 无 `StorageReadWrite`：图不支持自环（见校验规则 7）。同 pass 读写同一资源的 compute 场景，日后由前端（用户输入→图信息）翻译为双缓冲/版本化，而非在图内用 RMW usage 表达。
- TransferSrc/TransferDst 本轮不定义：无消费者（无 TransferPass/绑定结构），等 TransferPass 落地时以纯增量方式加入枚举。
- Auto 推断结果**写回** desc（回填后的 usage/随 desc 存入 RenderPass，PrintGraph 显示的是解析后的值）。
- aspect 选择（D24S8 的 depth-only/stencil-only 视图）、mip/layer range 形式、反射自动分配 set/binding：本轮不做，留作将来。
- 校验失败统一 throw（用户确认）；原有"read/write 名字查不到仅 cerr"的行为保持不变，不在本轮收紧。

## Verification

1. 构建需先加载 VS DevShell（环境不跨进程保留）：
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
   cmake --build out/build/x64-debug
   ```
2. 运行 `out\build\x64-debug\Engine_0\Engine_0.exe`，确认：
   - 资源行显示新格式（RGBA16F/D32F/RGBA8）、viewType、arrayLayers；
   - GBuffer/ToneMap 显示 att/bind 行，id 已回填、usage 为解析后的具体值；
   - SubmitSequence 仍为 GBuffer→Blur→ToneMap。
3. 异常路径抽测（临时改示例后还原）：把 ToneMap 的 Backbuffer 附件 slot 改成与另一 color 附件相同、或把 Blurred 移出 read 集 → 程序应抛 runtime_error 且消息可定位。
