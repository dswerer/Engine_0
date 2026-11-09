# cube_reflection 示例：俯视旋转立方体 + 环境贴图镜面反射

add_executable(QUAD
	"main.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../RenderGraph.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../RenderGraphNodes.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../AssetResolver.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../ShaderLibrary.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../rhi/RHI.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../vmaExporter.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../external/spv_reflect/spirv_reflect.c")

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

target_include_directories(QUAD PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/../.."
	"${CMAKE_CURRENT_SOURCE_DIR}/../../external")

set_property(TARGET QUAD PROPERTY CXX_STANDARD 20)
set_property(TARGET QUAD PROPERTY CXX_STANDARD_REQUIRED ON)

if (MSVC)
	target_compile_options(QUAD PRIVATE /utf-8)
endif()

target_compile_definitions(QUAD PRIVATE
	SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
	CUBE_REFLECTION_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../assets")

target_link_libraries(QUAD PRIVATE
	Vulkan::Vulkan
	glfw
	glm::glm)

# ---- Shader 编译：GLSL -> SPIR-V（glslc，Vulkan SDK 自带）----
set(CUBE_REFLECTION_SHADER_DIR "${CMAKE_BINARY_DIR}/cube_reflection_shaders")
set(CUBE_REFLECTION_SHADER_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite.frag")

engine0_write_shader_manifest("${CUBE_REFLECTION_SHADER_DIR}/shaders.manifest.json" ${CUBE_REFLECTION_SHADER_SOURCES})

if (Vulkan_GLSLC_EXECUTABLE)
	set(CUBE_REFLECTION_GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
else()
	find_program(CUBE_REFLECTION_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
endif()

if (CUBE_REFLECTION_GLSLC)
	set(CUBE_REFLECTION_SPIRV_OUTPUTS "")
	foreach(src ${CUBE_REFLECTION_SHADER_SOURCES})
		get_filename_component(name "${src}" NAME_WE)
		get_filename_component(ext "${src}" EXT)
		set(out "${CUBE_REFLECTION_SHADER_DIR}/${name}${ext}.spv")
		add_custom_command(
			OUTPUT "${out}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${CUBE_REFLECTION_SHADER_DIR}"
			COMMAND "${CUBE_REFLECTION_GLSLC}" --target-env=vulkan1.3 "${src}" -o "${out}"
			DEPENDS "${src}"
			VERBATIM)
		list(APPEND CUBE_REFLECTION_SPIRV_OUTPUTS "${out}")
	endforeach()
	add_custom_target(CubeReflectionShaders ALL DEPENDS ${CUBE_REFLECTION_SPIRV_OUTPUTS})
	add_dependencies(QUAD CubeReflectionShaders)
	target_compile_definitions(QUAD PRIVATE
		CUBE_REFLECTION_SHADER_DIR="${CUBE_REFLECTION_SHADER_DIR}")
else()
	message(WARNING "未找到 glslc，cube_reflection 示例的着色器不会被编译")
endif()
