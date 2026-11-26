# Mesh 测试与属性查看

此前运行过的 68 项检查位于被忽略的 `out/mesh-check`，没有纳入项目测试入口。现在正式源码在本目录，CMake/CTest 可以独立构建和运行，不依赖旧的 out 文件或已编译二进制。

## 仅运行 CPU 测试

依赖 C++20 编译器、CMake 3.15+ 和 GLM。`ENGINE0_GLM_INCLUDE_DIR` 指向包含 glm 子目录的目录；若已通过包管理器提供 glm::glm，可以不传此变量，改用自己的 CMAKE_PREFIX_PATH / toolchain。

从仓库根目录运行，下面的 GLM 路径需要替换为本机实际位置：

```powershell
cmake -S tests -B out/mesh-tests -G Ninja `
  "-DENGINE0_GLM_INCLUDE_DIR=D:/dependencies/glm-1.0.1"
cmake --build out/mesh-tests
ctest --test-dir out/mesh-tests --output-on-failure -V
```

MSVC 需在 VS 开发者终端执行，可追加 `"-DCMAKE_CXX_COMPILER=cl.exe"`；MinGW 可追加 `"-DCMAKE_CXX_COMPILER=g++.exe"`。更换编译器时使用不同构建目录。MinGW 下测试程序静态链接其运行库，避免从 CTest 或资源管理器启动时找不到运行库 DLL。

也可以从顶层配置：

```powershell
cmake -S . -B out/mesh-tests-root -G Ninja `
  -DENGINE0_BUILD_RENDERER=OFF `
  "-DENGINE0_GLM_INCLUDE_DIR=D:/dependencies/glm-1.0.1"
cmake --build out/mesh-tests-root
ctest --test-dir out/mesh-tests-root --output-on-failure -V
```

| CTest 名称 | 覆盖内容 |
|---|---|
| mesh.cpu | 75 项几何与元数据查询检查，覆盖多流、索引/非索引、baseVertex、范围溢出、语义、多 UV/颜色、NaN/Inf、包围盒及失败不部分修改 |
| mesh.draw_mapping | 25 项绘制适配检查和 29 项管线检查，覆盖自动填充、技术状态保留、stride、输入率、location/format/offset、拓扑、patch 大小、重复/额外输入及向量乱序 |
| mesh.inspect | 运行实际属性输出程序并检查成功状态 |

这些数量对应测试程序内的断言。CTest 共注册 3 项 CPU 测试。两个断言程序在失败时返回非零状态，Release 构建也会执行检查。

## 查看可观测属性

独立 tests 构建的程序路径如下；顶层构建则位于构建目录的 tests 子目录：

```powershell
.\out\mesh-tests\Engine0MeshInspect.exe
```

预期输出：

```text
Mesh: Cube
vertices=24 indices=36 submeshes=1
bounds.min=(-1,-1,-1) bounds.max=(1,1,1)
Position0 sourceBinding=7 -> gpuBinding=0 location=0 offset=0
Normal0 sourceBinding=7 -> gpuBinding=0 location=1 offset=12
draw=DrawIndexed count=36 first=0 baseVertex=0 materialSlot=0
Validate=PASS
PipelineCompatible=PASS
```

## 验证实际渲染

CubeReflection 使用源 binding 7 和 2 的双顶点流。测试通过 `UploadMesh` 和 `BindMeshGeometry` 覆盖整个上传、建图和绘制过程。运行该用例需要本项目原有的 Vulkan SDK、GLFW 和 GLM。可用 `ENGINE0_BUILD_RETRIEVE=OFF` 单独构建立方体示例。

```powershell
cmake -S . -B out/mesh-render -G Ninja `
  -DENGINE0_BUILD_RETRIEVE=OFF -DENGINE0_ENABLE_GPU_TESTS=ON `
  "-DCMAKE_PREFIX_PATH=D:/dependencies/install"
cmake --build out/mesh-render --target CubeReflection Engine0MeshTests Engine0MeshDrawTests Engine0MeshInspect
ctest --test-dir out/mesh-render --output-on-failure -V
```

注册 GPU 测试需保持 BUILD_TESTING=ON（默认值）。GPU 用例标记为 gpu，可用 `ctest --test-dir out/mesh-render -L gpu -V` 单独运行。图像输出位于该构建目录的 out/cube_reflection_smoke.png。

也可以手动运行：

```powershell
# 有窗口，Esc 退出；窗口标题显示实际 Mesh 的顶点/索引数。
.\out\mesh-render\Engine_0\examples\cube_reflection\CubeReflection.exe
# 窗口自动绘制 120 帧后退出，适合确认呈现路径。
.\out\mesh-render\Engine_0\examples\cube_reflection\CubeReflection.exe --frames 120
# 无窗口离屏绘制 3 帧，固定姿态，回读图像并验证立方体覆盖像素。
.\out\mesh-render\Engine_0\examples\cube_reflection\CubeReflection.exe --smoke 3
```

手动冒烟图像的 out 路径相对于运行时工作目录。SceneColor 的 alpha 清为 0，立方体片元写 1；测试要求覆盖像素大于 0 且小于全屏，避免只有环境背景也被当作几何绘制成功。缺失回读、图像写入失败或覆盖检查失败均返回非零状态。它是基础 GPU 冒烟，不替代所有材质/多物体场景验证。

启用 Assimp 并构建 Retrieve 后，可用 `Retrieve.exe --smoke 3` 运行共用 GPU 几何的阴影和主场景 Pass。

此前的运行结果、日志副本和图像见 [Mesh 验证记录](../docs/Mesh验证记录.md)。
