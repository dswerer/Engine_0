# RHI 三处硬编码参数化（多队列基础设施）计划

## Summary

将 RHI 层三处单队列硬编码参数化，为 RenderGraph 多执行域调度铺路：Device 选齐 compute/transfer 队列族（专用优先、graphics 兜底）、FrameSlot 改为每槽多池（每族一个）、barrier 与 Submit 增加队列族/队列类别字段。**默认行为完全不变**（全走 graphics、barrier 仍 IGNORED），单队列调用代码零改动；多队列调用可显式选用。冒烟新增"compute 队列经 semaphore 串到 graphics 队列"的无资源共享跨队列提交验证（不需 QFOT）。

## Current State Analysis

三处硬编码（均为本轮改造对象）：

1. **Device 队列维度**：
   - [RHI.h:409-412] 仅 `m_graphicsQueue/m_presentQueue/m_graphicsQueueFamily/m_presentQueueFamily`
   - [RHI.cpp:79-122] `PickPhysicalDevice` 仅挑 graphics+present 族
   - [RHI.cpp:438-452] `Device::Create` 的 `families` 向量只含 graphics（+present 异族时）
   - [RHI.cpp:480-482] 仅 `vkGetDeviceQueue` graphics/present 两个队列
   - [RHI.cpp:1891] `vkQueueSubmit2(m_graphicsQueue, ...)` 硬编码图形队列
2. **FrameSlot 单池单族**：
   - [RHI.h:283] `FrameSlot(VkDevice device, uint32_t queueFamily)`；成员单 `m_pool`
   - [RHI.cpp:546] 创建帧槽只传 `m_graphicsQueueFamily`
   - [RHI.cpp:1549-1574] `Allocate()` 只从唯一 `m_pool` 分配；`m_liveBuffers` 平铺向量
3. **Barrier 队列族写死 IGNORED**：
   - [RHI.cpp:1628-1629] image barrier、[RHI.cpp:1649-1650] buffer barrier：`src/dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED`
   - [RHIDesc.h:195-214] `ImageBarrier/BufferBarrier` 无队列族字段

约束：描述层头文件（RHIDesc.h）不得引入 Vulkan 头，`VK_QUEUE_FAMILY_IGNORED`(0xFFFFFFFF) 需自定义同值常量。Vulkan 规范保证 GRAPHICS 位的队列族必带 COMPUTE/TRANSFER 位，graphics 兜底恒有效。

## Proposed Changes

### 文件 1：`Engine_0/rhi/RHIDesc.h`

1. `#pragma region Sync` 末尾（ImageLayout 枚举后）新增队列类别词表：
   ```cpp
   // RHI 层队列类别（映射到 Device 选定的队列族；无专用队列时兜底 graphics）
   enum class QueueClass { Graphics, AsyncCompute, Transfer };
   // 与 VK_QUEUE_FAMILY_IGNORED 同值（描述层不引 Vulkan 头）
   inline constexpr uint32_t kQueueFamilyIgnored = 0xFFFFFFFFu;
   ```
2. `ImageBarrier`（dstAccess 字段后）新增：
   ```cpp
   uint32_t srcQueueFamily = kQueueFamilyIgnored; // 跨队列族所有权转移（QFOT）用；同族/单队列保持 IGNORED
   uint32_t dstQueueFamily = kQueueFamilyIgnored;
   ```
3. `BufferBarrier` 同样新增该两字段（语义同上）。

### 文件 2：`Engine_0/rhi/RHI.h`

1. **DeviceDesc**：不变（队列选择全自动，无需用户配置）。
2. **FrameSlot**（多池化）：
   - ctor 改 `FrameSlot(VkDevice device, std::vector<uint32_t> queueFamilies);`（去重后的族列表）
   - `Allocate()` 改 `VkCommandBuffer Allocate(uint32_t queueFamily);`（按族查池）
   - 成员改：
     ```cpp
     struct FamilyPool { uint32_t family = kQueueFamilyIgnored; VkCommandPool pool = VK_NULL_HANDLE; };
     std::vector<FamilyPool>              m_pools;       // 每族一池
     std::vector<std::vector<VkCommandBuffer>> m_liveBuffers; // 与 m_pools 平行：每池的在录 buffer
     ```
   - `InternalFence/MarkPending/DrainPending/Recycle` 签名不变（fence 与队列无关；Recycle 内部按池分组释放）
   - 类注释补充：帧内跨队列多次 Submit 仍经内部 fence 串行化（保守但正确）
3. **Device 公共接口**：
   - `BeginCommandList()` 改 `CommandList BeginCommandList(QueueClass queue = QueueClass::Graphics);`（默认参数保持源兼容）
   - 新增查询：`uint32_t QueueFamily(QueueClass cls) const; // 某类队列的族索引（QFOT barrier 字段 / 诊断用）`
4. **SubmitInfo** 新增字段：
   ```cpp
   QueueClass queue = QueueClass::Graphics; // 本批命令目标队列；commandLists 须全部来自同族池
   ```
5. **Device 私有成员**（m_presentQueueFamily 后）新增：
   ```cpp
   VkQueue  m_computeQueue  = VK_NULL_HANDLE; // 无专用计算队列时 = graphics 队列
   VkQueue  m_transferQueue = VK_NULL_HANDLE; // 无专用传输队列时 = graphics 队列
   uint32_t m_computeQueueFamily  = 0;        // 选定期已兜底，恒有效
   uint32_t m_transferQueueFamily = 0;
   ```

### 文件 3：`Engine_0/rhi/RHI.cpp`

1. **PickPhysicalDevice**（~79-122）：签名追加 `uint32_t& outComputeFamily, uint32_t& outTransferFamily`；循环内每设备扫描：
   - 专用 compute = 首个含 `VK_QUEUE_COMPUTE_BIT` 且不含 `VK_QUEUE_GRAPHICS_BIT` 的族；
   - 专用 transfer = 首个含 `VK_QUEUE_TRANSFER_BIT` 且不含 GRAPHICS/COMPUTE 位的族；
   - 均未找到时记 `UINT32_MAX`，循环后兜底 `= outGraphicsFamily`（记录进 bestCompute/bestTransfer，随 bestScore 一起择优）。
2. **Device::Create**：
   - [425-426] 调用处传入两个新出参，回填 `m_computeQueueFamily/m_transferQueueFamily`；
   - [438-441] `families` 改为 `{graphics, present, compute, transfer}` 去重（`std::sort` + `unique` 或顺序查重）；
   - [480-482] 后追加 `vkGetDeviceQueue` compute/transfer（同族时取到同句柄，与 present 注释一致）；
   - [546] FrameSlot 构造传族列表：`{graphics, compute, transfer}` 去重（present 族无需命令池）。
3. **FrameSlot**（~1509-1600）：
   - ctor：遍历族列表逐个 `vkCreateCommandPool`（失败打印并 return，保持现有失败语义——Device::Create 侧已有 m_pool 判空检查，改为检查 m_pools 空或数量不符）；
   - `Allocate(family)`：线性查 `m_pools`（≤3 项）得 pool，分配逻辑不变，push 进对应池的 liveBuffers；
   - dtor / Recycle：按池分组 `vkFreeCommandBuffers`。
4. **Device::BeginCommandList**（~1800 区）：映射 `QueueClass → m_*QueueFamily`（Graphics→graphics、AsyncCompute→compute、Transfer→transfer，选定期已兜底无需判空），`slot.Allocate(family)`。
5. **Device::Submit**（~1826-1896）：
   - 映射 `info.queue → VkQueue`（三选一）；
   - [1891] `vkQueueSubmit2(queue, ...)`；
   - 其余逻辑（end/waits/signals/fence/MarkPending）不动。
6. **CommandList::Barrier**（~1613-1660）：image/barrier 的 `src/dstQueueFamilyIndex` 改填 `b.srcQueueFamily/b.dstQueueFamily`（描述层默认 kQueueFamilyIgnored，值与 VK_QUEUE_FAMILY_IGNORED 相同，单队列行为逐位不变）。

### 文件 4：`Engine_0/Engine_0.cpp`（冒烟追加，前段输出逐字不变）

插入点：现有 swapchain 冒烟块（Acquire #3 之后、`#ifdef ENGINE_SHADER_DIR` 之前）：

```cpp
// 多队列冒烟：报告各族索引 + 空 cmd 经 semaphore 从 compute 队列串到 graphics 队列（无资源共享，不需 QFOT）
{
    const uint32_t gf = device->QueueFamily(rhi::QueueClass::Graphics);
    const uint32_t cf = device->QueueFamily(rhi::QueueClass::AsyncCompute);
    const uint32_t tf = device->QueueFamily(rhi::QueueClass::Transfer);
    cout << "Queues: graphics=f" << gf << " compute=f" << cf
         << (cf != gf ? "(dedicated)" : "(fallback)")
         << " transfer=f" << tf << (tf != gf ? "(dedicated)" : "(fallback)") << "\n";

    if (auto sem = device->CreateSemaphore()) {
        rhi::CommandList computeList = device->BeginCommandList(rhi::QueueClass::AsyncCompute);
        rhi::SemaphoreSignal signal{ sem.get(), rhi::PipelineStage::AllCommands };
        rhi::SubmitInfo computeSubmit;
        computeSubmit.queue = rhi::QueueClass::AsyncCompute;
        computeSubmit.commandLists = { &computeList, 1 };
        computeSubmit.signals = { &signal, 1 };
        device->Submit(computeSubmit);

        rhi::CommandList gfxList = device->BeginCommandList();
        rhi::SemaphoreWait wait{ sem.get(), rhi::PipelineStage::AllCommands };
        rhi::SubmitInfo gfxSubmit;
        gfxSubmit.commandLists = { &gfxList, 1 };
        gfxSubmit.waits = { &wait, 1 };
        device->Submit(gfxSubmit);
        device->WaitForFrame();
        cout << "Multi-queue ok: compute(f" << cf << ") -> graphics(f" << gf << ") via semaphore\n";
    }
}
```

注：QFOT 配对 barrier 不做执行冒烟——单侧 release 会使所有权悬置触发校验错误，字段管道由编译期验证，真实 QFOT 等 barrier compiler/state tracker 落地后再验。

## Assumptions & Decisions

- **范围**：选队列+暴露路由+默认仍 graphics（用户已确认）。不做完整 QFOT 执行路径（无 barrier compiler，跨队列资源共享尚无调用方）。
- **队列选择策略**：专用族优先（compute 排除 GRAPHICS 位、transfer 排除 GRAPHICS/COMPUTE 位），无专用则 graphics 兜底；兜底在 PickPhysicalDevice 内一次解析完，Submit 侧映射零分支。
- **QueueClass 而非裸族索引**进 SubmitInfo/BeginCommandList：与记忆中执行域枚举（Graphics/AsyncCompute/Transfer/CPU/CUDA）的 GPU 侧子集对齐，未来图编译期查表直通。
- **FrameSlot 每槽多池**而非按族拆槽：保持"槽=帧节流单位"语义不变，fence/回收状态机零改动；跨队列同帧多次 Submit 经内部 fence 串行化（保守正确，注释说明）。
- **present 族不建池**：present 队列从不录制命令。
- 提交到非 graphics 队列时 fence 节流、WaitForFrame 回收逻辑不变（fence 与队列族无关）。

## Verification

1. DevShell 构建（同一命令内加载，环境不跨进程）：
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation | Out-Null
   cmake --build out/build/x64-debug
   ```
2. 运行 `out\build\x64-debug\Engine_0\Engine_0.exe`，检查：
   - 退出码 0；此前全部输出逐字不变；
   - 新增 `Queues: graphics=fN compute=fM(...) transfer=fK(...)` 行（本机 RTX 独显预期 compute/transfer 均为 dedicated 族）；
   - 新增 `Multi-queue ok: compute(fM) -> graphics(fN) via semaphore` 行；
   - 校验层无 `[Vulkan WARN/ERROR]`（重点：命令池族与目标队列族一致、semaphore 跨队列 signal→wait 链）。
3. 完成后更新 project_memory.md：记录多队列基础设施落地状态与"QFOT 执行路径待 barrier compiler"的遗留项。
