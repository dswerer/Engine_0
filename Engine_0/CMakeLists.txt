# CMakeList.txt: Engine_0 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#

# 将源代码添加到此项目的可执行文件。
add_executable (Engine_0 
"Engine_0.cpp" 
"Engine_0.h" 
"AssetResolver.h"
"AssetResolver.cpp"
"ShaderLibrary.h"
"ShaderLibrary.cpp"
"RenderGraphDesc.h" 
"RenderGraphAbstracted.h" 
"RenderGraphDocument.h"
"RenderGraphDocument.cpp"
"RenderGraph.cpp" 
"RenderGraphNodes.h" 
"RenderGraphNodes.cpp" 
"vmaExporter.cpp" 
"rhi/RHIDesc.h" 
"rhi/RHI.h" 
"rhi/RHI.cpp"
"external/spv_reflect/spirv_reflect.h"
"external/spv_reflect/spirv_reflect.c")

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

# spv_reflect 默认引用仓库内 spirv-headers 子目录；改用 Vulkan SDK 自带的 SPIRV-Headers
target_compile_definitions (Engine_0 PRIVATE SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

# 资源目录（测试贴图等）：运行时按绝对路径读取，避免依赖工作目录
target_compile_definitions (Engine_0 PRIVATE ENGINE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

target_link_libraries(Engine_0 PUBLIC
	Vulkan::Vulkan
	glfw
	glm::glm)

# ---- Shader 清单：逻辑名 -> { stage, path }，与 glslc 共用同一份源列表 ----
# 运行期由 ShaderLibrary 读取，避免在 C++ 中硬编码 SPIR-V 路径。
function(engine0_write_shader_manifest manifest_path)
	get_filename_component(manifest_dir "${manifest_path}" DIRECTORY)
	file(MAKE_DIRECTORY "${manifest_dir}")
	set(entries "")
	foreach(src ${ARGN})
		get_filename_component(name "${src}" NAME_WE)
		get_filename_component(ext "${src}" EXT)
		string(TOLOWER "${ext}" ext_lower)
		if(ext_lower STREQUAL ".vert")
			set(stage "Vertex")
		elseif(ext_lower STREQUAL ".frag")
			set(stage "Fragment")
		elseif(ext_lower STREQUAL ".comp")
			set(stage "Compute")
		elseif(ext_lower STREQUAL ".tesc")
			set(stage "TessControl")
		elseif(ext_lower STREQUAL ".tese")
			set(stage "TessEval")
		elseif(ext_lower STREQUAL ".geom")
			set(stage "Geometry")
		else()
			message(WARNING "未知着色器扩展名 ${ext}：${src}")
			set(stage "Unknown")
		endif()
		list(APPEND entries "    \"${name}${ext}\": { \"stage\": \"${stage}\", \"path\": \"shaders/${name}${ext}.spv\", \"entry\": \"main\" }")
	endforeach()
	string(JOIN ",\n" body ${entries})
	file(WRITE "${manifest_path}" "{\n  \"version\": 1,\n  \"shaders\": {\n${body}\n  }\n}\n")
endfunction()

# ---- Shader 编译：GLSL -> SPIR-V（glslc，Vulkan SDK 自带）----
# 优先用 find_package(Vulkan) 已定位的 glslc；兜底按 VULKAN_SDK 环境变量搜索
set(ENGINE_SHADER_DIR "${CMAKE_BINARY_DIR}/shaders")
set(ENGINE_SHADER_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/textured.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/ubo.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/static_texture.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/dual_output.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/mux_sample.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/push_constant.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/blur.comp"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/tess.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/tess.tesc"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/tess.tese"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/tess.geom"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/tess.frag")

engine0_write_shader_manifest("${ENGINE_SHADER_DIR}/shaders.manifest.json" ${ENGINE_SHADER_SOURCES})

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

# 示例：独立可执行目标
add_subdirectory("examples/retrieve")
add_subdirectory("examples/cube_reflection")

# TODO: 如有需要，请添加测试并安装目标。
