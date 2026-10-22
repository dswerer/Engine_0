# CMakeList.txt: Engine_0 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#

# 将源代码添加到此项目的可执行文件。
add_executable (Engine_0 "Engine_0.cpp" "Engine_0.h" "RenderGraphDesc.h" "RenderGraphAbstracted.h" "RenderGraph.cpp" "RenderGraphNodes.h" "RenderGraphNodes.cpp" "vmaExporter.cpp" "rhi/RHIDesc.h" "rhi/RHI.h" "rhi/RHI.cpp")

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

target_include_directories(Engine_0 PUBLIC ${CMAKE_SOURCE_DIR}/external)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Engine_0 PROPERTY CXX_STANDARD 20)
endif()

# 源文件以 UTF-8 保存，显式告知 MSVC，避免按代码页 936 误读中文注释
if (MSVC)
  target_compile_options (Engine_0 PRIVATE /utf-8)
endif()

target_link_libraries(Engine_0 PUBLIC
	Vulkan::Vulkan
	glfw
	glm::glm)

# ---- Shader 编译：GLSL -> SPIR-V（glslc，Vulkan SDK 自带）----
# 优先用 find_package(Vulkan) 已定位的 glslc；兜底按 VULKAN_SDK 环境变量搜索
set(ENGINE_SHADER_DIR "${CMAKE_BINARY_DIR}/shaders")
set(ENGINE_SHADER_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/textured.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/ubo.frag")

if (Vulkan_GLSLC_EXECUTABLE)
	set(ENGINE_GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
else()
	find_program(ENGINE_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
endif()

if (ENGINE_GLSLC)
	set(ENGINE_SPIRV_OUTPUTS "")
	foreach(src ${ENGINE_SHADER_SOURCES})
		get_filename_component(name "${src}" NAME_WE)
		get_filename_component(ext "${src}" EXT)
		set(out "${ENGINE_SHADER_DIR}/${name}${ext}.spv")
		add_custom_command(
			OUTPUT "${out}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${ENGINE_SHADER_DIR}"
			COMMAND "${ENGINE_GLSLC}" --target-env=vulkan1.3 "${src}" -o "${out}"
			DEPENDS "${src}"
			VERBATIM)
		list(APPEND ENGINE_SPIRV_OUTPUTS "${out}")
	endforeach()
	add_custom_target(Engine_0Shaders ALL DEPENDS ${ENGINE_SPIRV_OUTPUTS})
	add_dependencies(Engine_0 Engine_0Shaders)
	target_compile_definitions(Engine_0 PRIVATE ENGINE_SHADER_DIR="${ENGINE_SHADER_DIR}")
else()
	message(WARNING "未找到 glslc，跳过 shader 编译（管线冒烟将被跳过）")
endif()

# TODO: 如有需要，请添加测试并安装目标。
