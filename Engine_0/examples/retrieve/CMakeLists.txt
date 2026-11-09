# retrieve 复现示例：独立可执行目标

add_executable(Retrieve
	"main.cpp"
	"AssimpMesh.h"
	"AssimpMesh.cpp"
	"FrameData.h"
	"Camera.h"
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
find_package(assimp CONFIG REQUIRED)

target_include_directories(Retrieve PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/../.."
	"${CMAKE_CURRENT_SOURCE_DIR}/../../external")

set_property(TARGET Retrieve PROPERTY CXX_STANDARD 20)
set_property(TARGET Retrieve PROPERTY CXX_STANDARD_REQUIRED ON)

if (MSVC)
	target_compile_options(Retrieve PRIVATE /utf-8)
endif()

target_compile_definitions(Retrieve PRIVATE
	SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
	RETRIEVE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

target_link_libraries(Retrieve PRIVATE
	Vulkan::Vulkan
	glfw
	glm::glm
	assimp::assimp)

# ---- Shader 编译：GLSL -> SPIR-V（glslc，Vulkan SDK 自带）----
set(RETRIEVE_SHADER_DIR "${CMAKE_BINARY_DIR}/retrieve_shaders")
set(RETRIEVE_SHADER_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/transmittance.comp"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/multiple_scattering.comp"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky_ambient.comp"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/boids_eval.comp"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/boids_update.comp"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fish.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fish.tesc"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fish.tese"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fish.geom"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fish.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.geom"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/atmosphere.frag")

engine0_write_shader_manifest("${RETRIEVE_SHADER_DIR}/shaders.manifest.json" ${RETRIEVE_SHADER_SOURCES})

if (Vulkan_GLSLC_EXECUTABLE)
	set(RETRIEVE_GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
else()
	find_program(RETRIEVE_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
endif()

if (RETRIEVE_GLSLC)
	set(RETRIEVE_SPIRV_OUTPUTS "")
	foreach(src ${RETRIEVE_SHADER_SOURCES})
		get_filename_component(name "${src}" NAME_WE)
		get_filename_component(ext "${src}" EXT)
		set(out "${RETRIEVE_SHADER_DIR}/${name}${ext}.spv")
		add_custom_command(
			OUTPUT "${out}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${RETRIEVE_SHADER_DIR}"
			COMMAND "${RETRIEVE_GLSLC}" --target-env=vulkan1.3 "${src}" -o "${out}"
			DEPENDS "${src}"
			VERBATIM)
		list(APPEND RETRIEVE_SPIRV_OUTPUTS "${out}")
	endforeach()
	add_custom_target(RetrieveShaders ALL DEPENDS ${RETRIEVE_SPIRV_OUTPUTS})
	add_dependencies(Retrieve RetrieveShaders)
	target_compile_definitions(Retrieve PRIVATE RETRIEVE_SHADER_DIR="${RETRIEVE_SHADER_DIR}")
else()
	message(WARNING "未找到 glslc，retrieve 示例的着色器不会被编译")
endif()
