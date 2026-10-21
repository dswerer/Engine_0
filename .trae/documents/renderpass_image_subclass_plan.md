# RenderPass 与 Image 子类实体化 — 实施计划

## Summary（目标）

让 `RenderGraph::ProcessRegister` 阶段真正按 `type` 构造出 **`RenderPass`**（Pass 子类）与 **`Image`**（Resource 子类），
用 `std::variant` 承载创建描述（Desc），并保留现有 `Pass::Type` / `Resource::Type` 枚举参数（**枚举 + variant 共存**）。
子类定义落在 `Engine_0/RenderGraphNodes.h`。
同时修复当前因 `Resource::Texture` → `Resource::Image` 改名遗留导致的编译失败。

## Current State Analysis（现状）

**关键文件**

- `Engine_0/RenderGraphAbstracted.h`：抽象层
  - `Resource`：`Type { None, Image, Buffer }`（已由 Texture 改名为 **Image**），成员 `id/name/readers_id/writer_id/crossFrame/external`；`AutoAllocator(Resource&&, Type, void*)`。
  - `Pass`：`Type { None, Render, Compute }`，成员 `id/name/to_read_id/to_write_id`；`AutoAllocator(Pass&&, Type, void*)`；`get_type()` 虚函数。
  - `RenderGraph`：两阶段注册（`RegisterPass`/`RegisterResource` 存 `reg_tmp_*` → `ProcessRegister` 解析为 id）；`reg_tmp_pass/reg_tmp_resource` 里仍是 `void* pInfo`；`GenerateSubmitSequence`（Kahn 拓扑，已修好）；`PrintGraph`。
- `Engine_0/RenderGraphNodes.h`：**已存在的草稿**，含以下错误：
  - `class RenderPass :Pass`、`class Image :Resource` —— **私有继承**，`unique_ptr<Pass>` 无法持有子类，必须改 `public`；
  - `Image() :Resource();` —— 非法语法；
  - `Image(Pass& base, void*)` —— 参数类型写错（应为 `Resource`）；
  - `enum Format { None, RGB, RGBA, Depth }` 嵌套裸 enum，`None` 会与继承来的 `Resource::Type::None` 冲突，需改为作用域枚举并上移。
- `Engine_0/Pass.cpp`、`Engine_0/Resource.cpp`：`AutoAllocator` 仍是 `make_unique<基类>`，丢弃 `type` 与 desc。
- `Engine_0/RenderGraph.cpp`：`ProcessRegister` 传 `void* pInfo`；`PrintGraph` 仍在用 `Resource::Texture`（**编译错误**）。
- `Engine_0/Engine_0.cpp`：示例图仍在用 `Resource::Texture`（**编译错误**）。
- `Engine_0/CMakeLists.txt`：`add_executable` 源文件列表。

**当前构建状态**：不通过（`Resource::Texture` 已不存在）。

## Proposed Changes（改动）

### 1. `Engine_0/RenderGraphAbstracted.h`

**Why**：Desc 与 variant 别名必须对 `RenderGraph` 可见；而 `RenderGraphNodes.h` 要 include 本文件，Desc 不能放 `RenderGraphNodes.h`（否则循环包含）。故 Desc 定义放这里。

- 增加 `#include <variant>`。
- 在 `namespace dEngine` 顶部新增 Desc 定义：
  ```cpp
  enum class ImageFormat { None, RGB, RGBA, Depth };

  struct ImageDesc {
      uint32_t    width  = 0;
      uint32_t    height = 0;
      uint32_t    depth  = 1;
      uint32_t    mipLevels = 1;
      uint32_t    samples   = 1;
      ImageFormat format = ImageFormat::None;
  };

  struct RenderPassDesc {
      // 本轮占位；后续放 load/clear op、viewport 等
  };

  using PassDesc     = std::variant<std::monostate, RenderPassDesc>;
  using ResourceDesc = std::variant<std::monostate, ImageDesc>;
  ```
- 改工厂签名（`void*` → variant）：
  ```cpp
  static std::unique_ptr<Resource> AutoAllocator(Resource&& base, Type type, ResourceDesc desc);
  static std::unique_ptr<Pass>     AutoAllocator(Pass&&     father, Type type, PassDesc     desc);
  ```
- `reg_tmp_pass::pInfo` → `PassDesc desc`；`reg_tmp_resource::pInfo` → `ResourceDesc desc`。
- 注册接口（保留 `type`，`void*` 换 variant）：
  ```cpp
  void RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write,
                    Pass::Type type = Pass::Type::None, PassDesc desc = {});
  void RegisterResource(std::string name, std::set<std::string> readers, std::string writer,
                        Resource::Type type = Resource::Type::None, ResourceDesc desc = {});
  ```

### 2. `Engine_0/RenderGraphNodes.h`（重写）

**Why**：承载真实子类；修正草稿的继承/语法错误。

```cpp
#pragma once
#include "RenderGraphAbstracted.h"

namespace dEngine {
    class RenderPass : public Pass {
    public:
        RenderPass() = default;
        RenderPass(std::string name, uint32_t id,
                   std::set<uint32_t> read, std::set<uint32_t> write,
                   const RenderPassDesc& desc)
            : Pass(std::move(name), id, std::move(read), std::move(write)), mDesc(desc) {}
        Pass::Type get_type() override { return Pass::Type::Render; }
        const RenderPassDesc& get_desc() const { return mDesc; }
    private:
        RenderPassDesc mDesc;
    };

    class Image : public Resource {
    public:
        Image() = default;
        Image(std::string name, uint32_t id,
              std::set<uint32_t> readers, uint32_t writer,
              const ImageDesc& desc)
            : Resource(std::move(name), id, std::move(readers), writer), mDesc(desc) {}
        Resource::Type get_type() override { return Resource::Type::Image; }
        const ImageDesc& get_desc() const { return mDesc; }
    private:
        ImageDesc mDesc;
    };
}
```

说明：
- 头文件内联实现，**无需新增 .cpp**。
- 基类 `Pass`/`Resource` 已有 `virtual ~` 与 `get_type()`，子类 override 即可，`unique_ptr<基类>` 存储不会切片。

### 3. `Engine_0/Pass.cpp`

**Why**：按 `type` 分支构造子类；读 base 字段（`AutoAllocator` 是 `Pass` 的静态成员，可直接访问 `father` 的 protected/private 成员）。

```cpp
#include "RenderGraphAbstracted.h"
#include "RenderGraphNodes.h"
#include <memory>
#include <iostream>

using namespace dEngine;

std::unique_ptr<Pass> Pass::AutoAllocator(Pass&& father, Type type, PassDesc desc) {
    if (type == Type::Render) {
        if (auto* d = std::get_if<RenderPassDesc>(&desc)) {
            return std::make_unique<RenderPass>(
                std::move(father.name), father.id,
                std::move(father.to_read_id), std::move(father.to_write_id), *d);
        }
        std::cerr << "RenderGraph: type=Render 但 PassDesc 不匹配" << std::endl;
    }
    return std::make_unique<Pass>(std::move(father));
}
```

### 4. `Engine_0/Resource.cpp`

**Why**：同上，Resource 侧。

```cpp
#include "RenderGraphAbstracted.h"
#include "RenderGraphNodes.h"
#include <memory>
#include <iostream>

using namespace dEngine;

std::unique_ptr<Resource> Resource::AutoAllocator(Resource&& base, Type type, ResourceDesc desc) {
    if (type == Type::Image) {
        if (auto* d = std::get_if<ImageDesc>(&desc)) {
            return std::make_unique<Image>(
                std::move(base.name), base.id,
                std::move(base.readers_id), base.writer_id, *d);
        }
        std::cerr << "RenderGraph: type=Image 但 ResourceDesc 不匹配" << std::endl;
    }
    return std::make_unique<Resource>(std::move(base));
}
```

### 5. `Engine_0/RenderGraph.cpp`

- `RegisterPass`：`rtp.desc = std::move(desc);`（替代 `rtp.pInfo = pInfo;`）。
- `RegisterResource`：`rtr.desc = std::move(desc);`。
- `ProcessRegister`：
  - `PassDesc desc = std::move(pass.second.desc);` → `passPool[id] = Pass::AutoAllocator(std::move(_base), type, std::move(desc));`
  - `ResourceDesc desc = std::move(resource.second.desc);` → `resourcePool[id] = Resource::AutoAllocator(std::move(_base), type, std::move(desc));`
- `PrintGraph`：
  - `ResourceType` lambda 里的 `Resource::Texture` → `Resource::Image`（**修复编译错误**）。
  - 增加 Desc 摘要打印（可选但用于验证）：`#include "RenderGraphNodes.h"`，对资源用 `dynamic_cast<Image*>(r.get())`，打印 `format/width×height`；对 Pass 用 `dynamic_cast<RenderPass*>` 打印占位信息。

### 6. `Engine_0/Engine_0.cpp`

- `#include "RenderGraphNodes.h"`。
- `Resource::Texture` → `Resource::Image`，并传入 `ImageDesc`，例如：
  ```cpp
  ImageDesc colorDesc; colorDesc.width = 1920; colorDesc.height = 1080; colorDesc.format = ImageFormat::RGBA;
  graph.RegisterResource("Color", {"Blur"}, "GBuffer", Resource::Image, colorDesc);
  // Backbuffer 可设 external 语义（本轮 Desc 不含 external，保留 base 默认）
  ```
- Pass 侧传 `RenderPassDesc{}`（`Pass::Render` 时）。

### 7. `Engine_0/CMakeLists.txt`

- 在 `add_executable` 源文件列表加入 `"RenderGraphNodes.h"`（头文件，便于 IDE 索引；无新 .cpp）。

## Assumptions & Decisions（假设与决策）

1. **Desc 定义位置**：放在 `RenderGraphAbstracted.h`（而非 `RenderGraphNodes.h`）。原因：`RenderGraph` 需要 `PassDesc`，而 `RenderGraphNodes.h` 需要 include `RenderGraphAbstracted.h`，放进子类头会形成循环包含。若坚持独立，可后续抽出 `RenderGraphDesc.h` 供两者共同包含。
2. **枚举 + variant 共存**：保留 `type` 参数决定子类；`type == None` 时忽略 desc 返回基类；`type` 与 desc 不匹配时报错并回落基类（不抛异常，保持与现有 `cerr` 风格一致）。
3. **本轮范围仅 `RenderPass` + `Image`**；`ComputePass` / `Buffer` 后续按同样模式补（届时扩展 `PassDesc`/`ResourceDesc` 的 variant 备选）。
4. `Image::Format` 提升为 `enum class ImageFormat` 并移入 Desc（避免与 `Resource::Type::None` 命名冲突）。
5. 子类构造采用「显式字段」参数（由 `AutoAllocator` 传入），而非草稿里的 `Pass& base`——因为派生类访问基类 protected 成员只能经由派生类型对象，传 `Pass&` 无法读其成员。
6. 暂不引入 usage/stage/access（留给 barrier 编译器那一轮）；`ImageDesc` 的 `format` 作为将来推断 usage 的基础。
7. 不改变单写者模型、两阶段注册、`GenerateSubmitSequence` 逻辑。

## Verification（验证）

1. 加载 VS DevShell 后全量重建：
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
   cmake --build out/build/x64-debug --clean-first
   ```
   期望：无 error（`Resource::Texture` 已消除）。
2. 运行 `out/build/x64-debug/Engine_0/Engine_0.exe`：
   - `PrintGraph` 中资源类型显示 `Image`、Pass 类型显示 `Render/Compute`（不再是 `None`）；
   - `Image` 的 Desc 摘要（format / 尺寸）正确打印；
   - `SubmitSequence` 仍为 `GBuffer -> Blur -> ToneMap`。
3. 边界验证：注册一个 `type=None` 的资源/Pass，确认回落到基类且不崩；注册 `type=Image` 但不传 `ImageDesc`，确认打印错误且回落到基类。