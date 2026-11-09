#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 资源定位层：把「逻辑路径」解析成磁盘上的绝对路径。
// 只依赖标准库，不感知 Vulkan 与渲染图；构造完成后只读，可多线程共享。
namespace dEngine {

	class AssetResolver {
	public:
		struct Resolved {
			std::filesystem::path absolutePath;
			std::string           logicalPath; // 规范化后的逻辑路径（'/' 分隔）
			uint64_t              size = 0;
			// 最近写入时间（毫秒）。同一平台内可用于变更检测，不保证是 Unix epoch
			uint64_t              mtime = 0;
		};

		// 前缀 -> 根目录。例：Mount("shaders", ENGINE_SHADER_DIR)、Mount("assets", ENGINE_ASSET_DIR)
		void Mount(std::string prefix, std::filesystem::path root);
		// 无前缀路径的兜底根（发布期资源包）；为空时未挂载前缀直接失败
		void SetFallbackRoot(std::filesystem::path root);

		// 逻辑路径的第一段作为挂载前缀匹配（最长前缀优先），失配时退到兜底根
		std::optional<Resolved> Resolve(std::string_view logical, std::string* error = nullptr) const;
		bool Exists(std::string_view logical) const;
		std::optional<std::vector<uint8_t>> ReadBinary(std::string_view logical,
			std::string* error = nullptr) const;

		// 规范化并校验：统一分隔符、折叠空段与 "."、拒绝绝对路径与 ".." 越界。
		// 返回空串表示非法路径。
		static std::string Normalize(std::string_view logical);

	private:
		struct MountPoint {
			std::string           prefix;
			std::filesystem::path root;
		};

		std::vector<MountPoint> m_mounts;
		std::filesystem::path   m_fallback;
	};
}
