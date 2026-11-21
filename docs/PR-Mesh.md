# feat: 定义 CPU Mesh，接入渲染绘制适配并提供可复现测试

## 问题与结果

上游示例分别生成原始顶点/索引数组，并在建图和执行阶段手填布局与绘制数量，没有可共享的 CPU Mesh 定义。这个改动加入几何资源、语义到着色器输入的适配，并让 CubeReflection 从同一份 Mesh 数据生成 GPU 输入和 DrawSource。独立的属性输出程序与正式 CTest 入口使数据约束和渲染结果可检查。

## 对照基准

对照仓库：[dswerer/Engine_0](https://github.com/dswerer/Engine_0)。本次固定到 main 提交 [5490f4ed09ce3f2ac3b93e133cf63da60e186354](https://github.com/dswerer/Engine_0/commit/5490f4ed09ce3f2ac3b93e133cf63da60e186354)，避免以后 main 的变化影响这份说明。

以下列出相对该提交的全部 PR 文件变更。Mesh.h/.cpp 在此基准中尚不存在，因此以新增文件呈现。

## 文件改动

| 文件 | 相比上游的变化 |
|---|---|
| Engine_0/Mesh.h、Mesh.cpp | 新增拥有 CPU 数据的 Mesh、Aabb、属性语义、多流、UInt16/32 索引、子网格、局部包围盒与校验 |
| Engine_0/MeshDrawData.h、MeshDrawData.cpp | 新增纯 CPU 的渲染输入适配；ValidateMeshPipeline 对实际 PipelineDesc 检查 stride、输入率、location/format/offset、拓扑及 patch 大小 |
| Engine_0/examples/cube_reflection/CubeMesh.h | 新增共享的 24 顶点、36 索引立方体几何生成代码 |
| Engine_0/examples/cube_reflection/main.cpp | 从 Mesh 字节上传，显式使用适配后的布局及范围；窗口标题显示数量；增加有限帧运行和可失败的像素回读冒烟 |
| Engine_0/examples/mesh_inspect/main.cpp | 新增无需 Vulkan 的终端属性查看程序 |
| tests/CMakeLists.txt、MeshTests.cpp、MeshDrawDataTests.cpp、README.md | 新增独立与顶层两种构建入口，注册几何、绘制适配、属性输出测试 |
| CMakeLists.txt | 引入 CTest；提供关闭渲染器、仅构建 CPU 测试的选项 |
| README.md | 添加设计、正式测试、属性查看和验证记录的入口 |
| Engine_0/CMakeLists.txt | 注册 Mesh 与适配源文件；允许独立关闭 Retrieve 或 CubeReflection |
| 两个示例的 CMakeLists.txt | 注册对应源文件；CubeReflection 另提供可选 GPU CTest |
| Engine_0/RenderGraphDesc.h | 补全 ComputePassDesc 中现有 ShaderStageDesc 的聚合初始化，保持原默认值，消除新测试以 GCC -Werror 编译该头文件时触发的缺失字段警告 |
| docs/Mesh设计与渲染对接.md | 说明资源边界、语义映射、binding 重映射、上传寿命、图依赖及多物体扩展边界 |
| docs/Mesh验证记录.md、docs/validation/ | 保存执行结果、日志和对应源码摘要，便于复查 |

## 关键行为

- Mesh 中的 Position0 为必需 Float3，所有位置坐标有限；可选通道按明确语义保存。
- 所有逐顶点流共享 vertexCount，流长度严格匹配步长和数量。
- Validate 会检查真实位置是否落入子网格包围盒；RecalculateBounds 失败不部分更新。
- Mesh 资源不持有变换、运行时材质实例或 GPU 句柄；materialSlot 是逻辑槽。
- 源 binding 7 可以转换为 GPU binding 0；着色器 location 来自渲染技术契约。
- 在 DeclarePass 前对实际管线执行 ValidateMeshPipeline；布局或拓扑不匹配时明确拒绝，几何、缓冲同步和 Pass 装配的职责仍然分开。
- 选中非索引子网格时不附加索引流，即使同一 Mesh 还保存其他子网格使用的索引。
- 构建阶段校验失败会给出错误；CPU 测试与 GPU 冒烟失败均返回非零状态。

## 验证

本机实际运行结果：

- GCC 16.2：CPU CTest 3/3，通过 68 项几何、25 项绘制适配、28 项实际管线约束检查及属性输出程序。
- MSVC 19.51 Debug：含真实 Vulkan GPU 冒烟的 CTest 4/4。
- NVIDIA GeForce RTX 5060 Laptop GPU：离屏 3 帧回读到立方体覆盖 339,843 / 921,600 像素，PNG 写入成功。
- 窗口路径绘制 120 帧后退出，退出码 0。

复现命令与日志见 [验证记录](Mesh验证记录.md)，测试入口见 [tests/README.md](../tests/README.md)。本次未验证 Retrieve 或整个 Engine_0 主冒烟程序。

## 范围与后续

本次提供静态 CPU Mesh、单次绘制准备、一个实际渲染示例和测试。Assimp 导入器、ModelAsset、材质系统、GPU 资源缓存、骨骼/Morph 与通用多物体绘制列表仍待实现。当前 RenderPass 只有一个 DrawSource，不能通过反复 SetDraw 累积多个物体。详细分层和后续顺序见设计文档。
