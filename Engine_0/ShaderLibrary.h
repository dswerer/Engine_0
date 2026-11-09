#pragma once
#include "AssetResolver.h"
#include "rhi/RHIDesc.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 着色器登记层：逻辑名 -> (阶段, 逻辑路径, 入口点)。
// 只负责定位与校验，不编译 GLSL、不创建 VkShaderModule；编译仍在构建期由 glslc 完成。
namespace dEngine {

	struct ShaderEntry {
		std::string                      name;        // 逻辑键："blur.comp"
		std::optional<rhi::ShaderStage>  stage;       // 空则由逻辑路径扩展名推断
		std::string                      logicalPath; // 相对 AssetResolver："shaders/blur.comp.spv"
		std::string                      entryPoint = "main";
		std::vector<std::string>         defines;     // 仅记录用于对照构建参数，运行期不参与解析
	};

	struct ShaderRevision {
		uint64_t mtime = 0;
		uint64_t size = 0;
	};

	class ShaderLibrary {
	public:
		explicit ShaderLibrary(const AssetResolver& resolver) : m_resolver(&resolver) {}

		// 由构建期清单批量登记（格式见 docs 注释）；warnings 收非致命问题。
		// 文件不可读、JSON 解析失败或有条目登记失败时返回 false。
		bool LoadManifest(std::string_view logicalPath, std::vector<std::string>* warnings = nullptr);
		// 手工登记。重名、空名字/路径、阶段与扩展名冲突时返回 false
		bool Register(ShaderEntry entry, std::string* error = nullptr);

		bool Contains(std::string_view name) const;
		std::vector<std::string> Names() const; // 按名字升序

		// 主出口：解析为 RHI 可消费的阶段描述（含绝对路径与入口点）。
		// 失败返回 nullopt，error 给出原因
		std::optional<rhi::ShaderStageDesc> Resolve(std::string_view name,
			std::string* error = nullptr) const;
		// 批量解析：任一失败则整体返回 false，errors 收全部原因
		bool ResolveStages(const std::vector<std::string>& names,
			std::vector<rhi::ShaderStageDesc>* out, std::vector<std::string>* errors) const;

		// 变更检测：文件缺失返回 nullopt；mtime/size 变化即视为需要重新解析
		std::optional<ShaderRevision> Revision(std::string_view name) const;

		static std::optional<rhi::ShaderStage> StageFromString(std::string_view text);
		static const char* StageToString(rhi::ShaderStage stage);
		// 由 "x.vert.spv" / "x.comp" 等路径推断阶段；无法识别返回 nullopt
		static std::optional<rhi::ShaderStage> StageFromExtension(std::string_view logicalPath);

	private:
		const AssetResolver* m_resolver = nullptr;
		std::map<std::string, ShaderEntry> m_entries; // 有序，便于诊断输出稳定
	};
}
