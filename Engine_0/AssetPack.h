#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 单文件资产包：整包读入内存，按索引切片读取，不向工作目录解压。
// 索引键为完整逻辑路径（"assets/foo.png"、"shaders/bar.comp.spv"）。
namespace dEngine {

	struct PackInput {
		std::string           path;
		std::vector<uint8_t>  data;
	};

	class AssetPack {
	public:
		struct Entry {
			uint64_t offset = 0;
			uint64_t size = 0;
			uint64_t hash = 0;
			uint32_t flags = 0; // 0 = raw；预留压缩
		};

		// 整包字节（通常来自一次文件读取）；解析失败返回 nullopt
		static std::optional<AssetPack> Load(std::vector<uint8_t> bytes, std::string* error = nullptr);
		static std::optional<AssetPack> LoadFile(const std::string& path, std::string* error = nullptr);

		bool Contains(std::string_view logical) const;
		std::optional<Entry> Stat(std::string_view logical) const;
		// 未压缩条目返回包内切片；生命周期 = AssetPack
		std::optional<std::span<const uint8_t>> View(std::string_view logical) const;
		std::optional<std::vector<uint8_t>> Read(std::string_view logical) const;

		uint64_t Hash() const { return m_hash; }
		size_t EntryCount() const { return m_entries.size(); }
		const std::vector<uint8_t>& Bytes() const { return m_bytes; }

	private:
		std::vector<uint8_t>            m_bytes;
		std::map<std::string, Entry>    m_entries;
		uint64_t                        m_hash = 0;
	};

	// 测试/打包工具共用：按路径排序后写出确定性包
	std::optional<std::vector<uint8_t>> BuildAssetPack(const std::vector<PackInput>& entries,
		std::string* error = nullptr);
}
