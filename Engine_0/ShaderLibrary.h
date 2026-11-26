#pragma once
#include "ContentResolver.h"
#include "rhi/RHIDesc.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 着色器登记层：逻辑名 -> (阶段, 逻辑路径, 入口点)。
// 负责定位与校验；构建期 SPIR-V 产物直接读取，GLSL 源则在 Resolve 时按需编译为
// SPIR-V（优先进程内 shaderc；未编译进 shaderc 时回退到 glslc）。本层不创建 VkShaderModule。
namespace dEngine {

	struct ShaderEntry {
		std::string                      name;        // 逻辑键："blur.comp"
		std::optional<rhi::ShaderStage>  stage;       // 空则由逻辑路径扩展名推断
		std::string                      logicalPath; // 相对 ContentResolver：SPIR-V 或 GLSL 源
		std::string                      entryPoint = "main";
		std::vector<std::string>         defines;     // 运行期编译宏："NAME" 或 "NAME=VALUE"
	};

	struct ShaderRevision {
		uint64_t mtime = 0;
		uint64_t size = 0;
	};

	class ShaderLibrary {
	public:
		explicit ShaderLibrary(const ContentResolver& resolver) : m_resolver(&resolver) {}

		// 由构建期清单批量登记（格式见 docs 注释）；warnings 收非致命问题。
		// 文件不可读、JSON 解析失败或有条目登记失败时返回 false。
		bool LoadManifest(std::string_view logicalPath, std::vector<std::string>* warnings = nullptr);
		// 将当前登记表序列化为与 LoadManifest 兼容的 JSON；不访问文件系统，也不校验路径是否存在
		std::string ToManifestJson() const;
		// 将清单写入磁盘路径（父目录不存在时自动创建）。空登记表或条目缺少阶段时返回 false
		bool SaveManifest(const std::filesystem::path& path, std::string* error = nullptr) const;
		// 手工登记。重名、空名字/路径、阶段与扩展名冲突时返回 false
		bool Register(ShaderEntry entry, std::string* error = nullptr);

		// 运行期编译开关；关闭时 Resolve 只接受 SPIR-V。默认开启，无可用编译器时编译会报错
		void SetRuntimeCompilation(bool enabled) { m_runtimeCompilation = enabled; }
		bool RuntimeCompilation() const { return m_runtimeCompilation; }
		// 仅 glslc 回退路径使用；为空时依次尝试 ENGINE0_GLSLC、构建期记录、VULKAN_SDK、PATH
		void SetGlslcPath(std::filesystem::path path) { m_glslcPath = std::move(path); }
		const std::filesystem::path& GlslcPath() const { return m_glslcPath; }
		// 编译结果持久缓存目录；为空时只缓存于内存（跨进程不保留）
		void SetCompileCacheDirectory(std::filesystem::path directory) {
			m_compileCacheDirectory = std::move(directory);
		}
		const std::filesystem::path& CompileCacheDirectory() const { return m_compileCacheDirectory; }

		bool Contains(std::string_view name) const;
		const ShaderEntry* Find(std::string_view name) const;
		std::vector<std::string> Names() const; // 按名字升序
		const ContentResolver& Resolver() const { return *m_resolver; }

		// 主出口：解析为 RHI 可消费的阶段描述。
		// 磁盘 SPIR-V 填 spirvPath；GLSL 源与包内 SPIR-V 填内存 spirv。失败返回 nullopt
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
		const ContentResolver* m_resolver = nullptr;
		std::map<std::string, ShaderEntry> m_entries; // 有序，便于诊断输出稳定

		bool CompileGlsl(const ShaderEntry& entry, std::span<const uint8_t> source,
			std::vector<uint32_t>* spirv, std::string* error) const;
		std::string CompileCacheKey(const ShaderEntry& entry,
			std::span<const uint8_t> source) const;
		std::filesystem::path FindGlslc() const;

		bool m_runtimeCompilation = true;
		std::filesystem::path m_glslcPath;
		std::filesystem::path m_compileCacheDirectory;
		mutable std::mutex m_compileMutex;
		mutable std::mutex m_processMutex; // glslc 子进程会重定向全局 stdout/stderr，必须串行
		mutable std::map<std::string, std::vector<uint32_t>> m_compileCache; // key -> SPIR-V words
	};
}
