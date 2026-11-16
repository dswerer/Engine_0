#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 资源定位层：把「逻辑路径」解析成磁盘上的绝对路径。
// 只依赖标准库，不感知 Vulkan 与渲染图；构造完成后只读，可多线程共享。
namespace dEngine {

	class AssetPack;

	class ContentResolver {
	public:
		struct Resolved {
			enum class Kind { File, Pack };

			Kind                kind = Kind::File;
			std::optional<std::filesystem::path> absolutePath; // 仅 File 源有值
			std::string         identity;     // 缓存/去重键："fs:<abs>" 或 "pack:<hash>:<logical>"
			std::string         logicalPath;  // 规范化后的逻辑路径（保留 pack: 头）
			uint64_t            size = 0;
			uint64_t            hash = 0;     // 包索引提供；文件源为 0
			// 最近写入时间（毫秒）。同一平台内可用于变更检测，不保证是 Unix epoch
			uint64_t            mtime = 0;
		};

		// 路径解析结果：scheme 决定走磁盘还是内存包，logical 是剥离标记后的规范化路径
		struct ParsedPath {
			enum class Scheme { File, Pack };

			Scheme      scheme = Scheme::File;
			std::string logical;
		};

		// 前缀 -> 根目录。例：Mount("shaders", ENGINE_SHADER_DIR)、Mount("assets", ENGINE_ASSET_DIR)
		void Mount(std::string prefix, std::filesystem::path root);
		// 无前缀路径的兜底根（发布期资源包）；为空时未挂载前缀直接失败
		void SetFallbackRoot(std::filesystem::path root);

		// 内存包：非拥有；pack 必须比 resolver 活得久。仅 pack: 路径会查包
		void SetPack(const AssetPack* pack);

		// 解析 scheme 并规范化 remainder；失败返回 nullopt
		static std::optional<ParsedPath> ParsePath(std::string_view raw);
		// 解析 + 规范化并保留 pack: 头，供清单登记使用；非法返回空串
		static std::string NormalizePath(std::string_view raw);
		// 清单来源继承：base 为 pack: 时，无 scheme 的条目路径补上 pack:
		static std::string InheritPath(std::string_view basePath, std::string_view entryPath);

		// 逻辑路径的第一段作为挂载前缀匹配（最长前缀优先），失配时退到兜底根
		std::optional<Resolved> Resolve(std::string_view logical, std::string* error = nullptr) const;
		bool Exists(std::string_view logical) const;
		// 零拷贝读取：仅内存包提供；磁盘源返回 nullopt，调用方回退 ReadBinary
		std::optional<std::span<const uint8_t>> ViewBinary(std::string_view logical,
			std::string* error = nullptr) const;
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
		const AssetPack*        m_pack = nullptr;
	};
}
