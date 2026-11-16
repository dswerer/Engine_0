#pragma once
#include "AssetPack.h"

#include <string>
#include <vector>

// 模块自动打包：解析模块文档的 shaderRefs / asset 键，收集依赖并生成自包含包。
namespace dEngine {

	class RenderGraphModuleDocument;
	class ShaderLibrary;
	class AssetCatalog;

	struct ModulePackResult {
		std::string              moduleName;
		std::vector<std::string> shaderRefs;
		std::vector<std::string> assetKeys;
		std::vector<PackInput>   inputs;
		std::vector<uint8_t>     bytes;
	};

	bool BuildModulePack(const RenderGraphModuleDocument& module,
		const ShaderLibrary& shaders,
		const AssetCatalog& assets,
		ModulePackResult* out,
		std::string* error = nullptr);

	// 便捷入口：磁盘模块 JSON -> 构建 -> 落盘
	bool BuildModulePackFile(const std::string& moduleJsonPath,
		const ShaderLibrary& shaders,
		const AssetCatalog& assets,
		const std::string& outputPackPath,
		ModulePackResult* out = nullptr,
		std::string* error = nullptr);

	// 内存模块文档 -> 构建 -> 落盘（发射流程不需要中间 JSON）
	bool BuildModulePackFile(const RenderGraphModuleDocument& module,
		const ShaderLibrary& shaders,
		const AssetCatalog& assets,
		const std::string& outputPackPath,
		ModulePackResult* out = nullptr,
		std::string* error = nullptr);
}
