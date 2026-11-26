# deferred 示例：当前只验证窗口与 Vulkan 设备创建。
# 延迟渲染的 G-buffer / 光照 / 合成 Pass 后续再往这里加。

add_executable(Deferred
	"main.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../rhi/RHI.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../vmaExporter.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../external/spv_reflect/spirv_reflect.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../AssetCatalog.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../AssetPack.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../ContentResolver.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../AssetLoader.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../StbImageImpl.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../ShaderLibrary.cpp")

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

target_include_directories(Deferred PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/../.."
	"${CMAKE_CURRENT_SOURCE_DIR}/../../external")

set_property(TARGET Deferred PROPERTY CXX_STANDARD 20)
set_property(TARGET Deferred PROPERTY CXX_STANDARD_REQUIRED ON)

target_compile_definitions (Deferred PRIVATE DEFERRED_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_compile_definitions (Deferred PRIVATE DEFERRED_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")

if (MSVC)
	target_compile_options(Deferred PRIVATE /utf-8)
endif()

target_compile_definitions(Deferred PRIVATE SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

target_link_libraries(Deferred PRIVATE
	Vulkan::Vulkan
	glfw)
	if(TARGET Engine0ShaderCompile)
		target_link_libraries(Deferred PRIVATE Engine0ShaderCompile)
	endif()
