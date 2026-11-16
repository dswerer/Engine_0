#include "ContentResolver.h"

#include "AssetPack.h"

#include <chrono>
#include <fstream>
#include <system_error>

namespace dEngine {

	namespace {

		void SetError(std::string* error, std::string message) {
			if (error) *error = std::move(message);
		}

		// 文件元信息：不存在或不可读时返回 false，不抛异常
		bool QueryFile(const std::filesystem::path& path, uint64_t* size, uint64_t* mtime) {
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec) || ec)
				return false;
			const auto bytes = std::filesystem::file_size(path, ec);
			if (ec) return false;
			const auto stamp = std::filesystem::last_write_time(path, ec);
			if (ec) return false;
			if (size) *size = static_cast<uint64_t>(bytes);
			if (mtime)
				*mtime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
					stamp.time_since_epoch()).count());
			return true;
		}
	}

	std::string ContentResolver::Normalize(std::string_view logical) {
		if (logical.empty())
			return {};

		std::string text;
		text.reserve(logical.size());
		for (char ch : logical)
			text.push_back(ch == '\\' ? '/' : ch);

		// 绝对路径与盘符：逻辑路径只能是相对路径
		if (text.front() == '/')
			return {};
		if (text.size() >= 2 && text[1] == ':')
			return {};

		// 逐段校验：拒绝 ".."，丢弃空段与 "."
		std::vector<std::string_view> segments;
		size_t begin = 0;
		while (begin <= text.size()) {
			const size_t slash = text.find('/', begin);
			const size_t end = slash == std::string::npos ? text.size() : slash;
			const std::string_view segment(text.data() + begin, end - begin);
			if (segment == "..")
				return {};
			if (!segment.empty() && segment != ".")
				segments.push_back(segment);
			if (slash == std::string::npos)
				break;
			begin = slash + 1;
		}
		if (segments.empty())
			return {};

		std::string normalized;
		for (size_t i = 0; i < segments.size(); ++i) {
			if (i != 0) normalized.push_back('/');
			normalized.append(segments[i]);
		}
		return normalized;
	}

	void ContentResolver::Mount(std::string prefix, std::filesystem::path root) {
		while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\'))
			prefix.pop_back();
		if (prefix.empty())
			return;
		m_mounts.push_back({ std::move(prefix), std::move(root) });
	}

	void ContentResolver::SetFallbackRoot(std::filesystem::path root) {
		m_fallback = std::move(root);
	}

	void ContentResolver::SetPack(const AssetPack* pack) {
		m_pack = pack;
	}

	std::optional<ContentResolver::ParsedPath> ContentResolver::ParsePath(std::string_view raw) {
		constexpr std::string_view kPackScheme = "pack:";
		if (raw.substr(0, kPackScheme.size()) == kPackScheme) {
			const std::string logical = Normalize(raw.substr(kPackScheme.size()));
			if (logical.empty())
				return std::nullopt;
			return ParsedPath{ ParsedPath::Scheme::Pack, logical };
		}
		const std::string logical = Normalize(raw);
		if (logical.empty())
			return std::nullopt;
		return ParsedPath{ ParsedPath::Scheme::File, logical };
	}

	std::string ContentResolver::NormalizePath(std::string_view raw) {
		const auto parsed = ParsePath(raw);
		if (!parsed)
			return {};
		if (parsed->scheme == ParsedPath::Scheme::Pack)
			return "pack:" + parsed->logical;
		return parsed->logical;
	}

	std::string ContentResolver::InheritPath(std::string_view basePath, std::string_view entryPath) {
		const auto base = ParsePath(basePath);
		if (!base || base->scheme != ParsedPath::Scheme::Pack)
			return std::string(entryPath);
		const auto entry = ParsePath(entryPath);
		if (!entry)
			return std::string(entryPath); // 交给登记方报非法路径
		return "pack:" + entry->logical;
	}

	std::optional<ContentResolver::Resolved> ContentResolver::Resolve(
		std::string_view logical, std::string* error) const {
		const auto parsed = ParsePath(logical);
		if (!parsed) {
			SetError(error, "invalid asset path: " + std::string(logical));
			return std::nullopt;
		}

		if (parsed->scheme == ParsedPath::Scheme::Pack) {
			if (m_pack == nullptr) {
				SetError(error, "no asset pack registered: " + parsed->logical);
				return std::nullopt;
			}
			const auto entry = m_pack->Stat(parsed->logical);
			if (!entry) {
				SetError(error, "asset not found in pack: " + parsed->logical);
				return std::nullopt;
			}
			Resolved resolved;
			resolved.kind = Resolved::Kind::Pack;
			resolved.identity = "pack:" + std::to_string(m_pack->Hash()) + ":" + parsed->logical;
			resolved.logicalPath = "pack:" + parsed->logical;
			resolved.size = entry->size;
			resolved.hash = entry->hash;
			return resolved;
		}

		const std::string& normalized = parsed->logical;
		std::filesystem::path root = m_fallback;
		std::string relative = normalized;
		size_t bestPrefix = 0;
		for (const auto& mount : m_mounts) {
			if (normalized.size() <= mount.prefix.size())
				continue;
			if (normalized.compare(0, mount.prefix.size(), mount.prefix) != 0)
				continue;
			if (normalized[mount.prefix.size()] != '/')
				continue;
			if (mount.prefix.size() < bestPrefix)
				continue;
			bestPrefix = mount.prefix.size();
			root = mount.root;
			relative = normalized.substr(mount.prefix.size() + 1);
		}

		if (root.empty()) {
			SetError(error, "no mount for asset: " + normalized);
			return std::nullopt;
		}

		const std::filesystem::path absolute = root / std::filesystem::path(relative);
		uint64_t size = 0;
		uint64_t mtime = 0;
		if (!QueryFile(absolute, &size, &mtime)) {
			SetError(error, "asset not found: " + normalized + " (root: " + root.string() + ")");
			return std::nullopt;
		}

		Resolved resolved;
		resolved.kind = Resolved::Kind::File;
		resolved.identity = "fs:" + absolute.generic_string();
		resolved.absolutePath = absolute;
		resolved.logicalPath = normalized;
		resolved.size = size;
		resolved.mtime = mtime;
		return resolved;
	}

	std::optional<std::span<const uint8_t>> ContentResolver::ViewBinary(
		std::string_view logical, std::string* error) const {
		const auto parsed = ParsePath(logical);
		if (!parsed) {
			SetError(error, "invalid asset path: " + std::string(logical));
			return std::nullopt;
		}
		if (parsed->scheme != ParsedPath::Scheme::Pack)
			return std::nullopt; // 磁盘源没有零拷贝视图，调用方回退 ReadBinary
		if (m_pack == nullptr) {
			SetError(error, "no asset pack registered: " + parsed->logical);
			return std::nullopt;
		}
		const auto view = m_pack->View(parsed->logical);
		if (!view)
			SetError(error, "asset not found in pack: " + parsed->logical);
		return view;
	}

	bool ContentResolver::Exists(std::string_view logical) const {
		return Resolve(logical, nullptr).has_value();
	}

	std::optional<std::vector<uint8_t>> ContentResolver::ReadBinary(
		std::string_view logical, std::string* error) const {
		const auto parsed = ParsePath(logical);
		if (!parsed) {
			SetError(error, "invalid asset path: " + std::string(logical));
			return std::nullopt;
		}
		if (parsed->scheme == ParsedPath::Scheme::Pack) {
			if (m_pack == nullptr) {
				SetError(error, "no asset pack registered: " + parsed->logical);
				return std::nullopt;
			}
			auto data = m_pack->Read(parsed->logical);
			if (!data)
				SetError(error, "asset not found in pack: " + parsed->logical);
			return data;
		}

		const auto resolved = Resolve(parsed->logical, error);
		if (!resolved || !resolved->absolutePath)
			return std::nullopt;

		std::ifstream file(*resolved->absolutePath, std::ios::binary);
		if (!file) {
			SetError(error, "cannot open asset: " + resolved->logicalPath);
			return std::nullopt;
		}

		std::vector<uint8_t> data(static_cast<size_t>(resolved->size));
		if (!data.empty()) {
			file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
			if (!file) {
				SetError(error, "cannot read asset: " + resolved->logicalPath);
				return std::nullopt;
			}
		}
		return data;
	}
}
