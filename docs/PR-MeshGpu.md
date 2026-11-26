# 接入静态 Mesh GPU 上传与间接绘制元数据

## 背景

CPU `Mesh` 已经描述了顶点流、索引和子网格，但示例还要逐个上传缓冲，并在建图时重复填写顶点布局、拓扑和绘制范围。此次将这些步骤收进可复用的接口，让同一份 GPU 几何可以供多个 Pass 使用，直接和间接绘制也能共用同一份子网格数据。

## 改动

| 接口 | 职责 |
|---|---|
| `UploadMesh` / `MeshGpu` | 校验 CPU 网格并同步上传顶点流与可选索引。GPU 对象按源 binding 持有缓冲，同时保存已校验的布局和子网格快照。 |
| `BindMeshGeometry` | 根据子网格与 shader 输入契约填写 `PipelineDesc` 的顶点布局、拓扑和细分控制点数，导入实际使用的顶点与索引缓冲，并返回直接绘制范围。 |
| `Mesh::GetDrawMetadata` / `MeshGpu::GetDrawMetadata` | 按子网格下标查询元素数量、起点、带符号的 `baseVertex` 和索引类型。结果也包含拓扑、细分控制点数、局部包围盒和材质槽。直接绘制适配复用该查询。 |

Pass 选择 shader、材质、附件及深度、混合和剔除状态，并配置纹理、Uniform、Storage 和实例数据。RenderGraph 从顶点与索引绑定推导读依赖。多个 Pass 使用同一个 `MeshGpu` 和同一资源前缀时，共用图中的缓冲声明。调用方持有 `MeshGpu`，直到引用它的图和 GPU 工作结束。

- **CubeReflection** 使用位置、法线两条独立顶点流，验证源 binding 重映射和几何读依赖的推导。
- **Retrieve** 的 Assimp 示例导入器生成三控制点 patch。阴影与主场景 Pass 使用同一份 `MeshGpu`，各自配置 shader、附件和实例数据。

## 间接命令示例

```cpp
const auto geometry = meshGpu.GetDrawMetadata(submeshIndex);
if (geometry && geometry->indexType) {
    const VkDrawIndexedIndirectCommand command{
        geometry->count, instanceCount, geometry->first, geometry->baseVertex, 0
    };
    // 将命令写入渲染层管理的间接参数缓冲。
}
```

索引子网格的 `count` 和 `first` 分别对应 `indexCount` 和 `firstIndex`。非索引子网格的 `indexType` 为空，`count` 和 `first` 分别对应 `vertexCount` 和 `firstVertex`。`DrawSource::count` 在间接模式下表示命令条数。实例数、首实例、间接参数缓冲及其字节偏移由渲染层配置。

## 验证

| 检查 | 结果 |
|---|---|
| GCC 16.2 | CPU CTest 3/3 通过。包括 75 项几何与元数据检查、25 项绘制适配检查和 29 项管线检查。 |
| MSVC 19.51 + Vulkan | CTest 4/4 通过。CubeReflection 离屏绘制 3 帧，覆盖 339,843 / 921,600 像素，PNG 与改动前逐字节一致。 |
| Vulkan 间接绘制回读 | 在释放原 CPU 网格后查询 `MeshGpu` 并组装命令。UInt16 索引命令使用 `firstIndex = 1`、`baseVertex = -1`，非索引命令使用 `firstVertex = 3`。两种回读均与各自的直接绘制逐像素一致。 |
| Retrieve | 主程序源文件通过 MSVC 编译。整例链接和离屏运行受验证环境缺少 Assimp 阻塞。 |

构建和 CTest 命令见 [测试说明](../tests/README.md)。完整接口示例和缓冲生命周期见 [Mesh 设计与渲染对接](Mesh设计与渲染对接.md)。
