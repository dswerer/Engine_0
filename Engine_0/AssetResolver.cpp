#include "AssetResolver.h"

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

	std::string AssetResolver::Normalize(std::string_view logical) {
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

	void AssetResolver::Mount(std::string prefix, std::filesystem::path root) {
		while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\'))
			prefix.pop_back();
		if (prefix.empty())
			return;
		m_mounts.push_back({ std::move(prefix), std::move(root) });
	}

	void AssetResolver::SetFallbackRoot(std::filesystem::path root) {
		m_fallback = std::move(root);
	}

	std::optional<AssetResolver::Resolved> AssetResolver::Resolve(
		std::string_view logical, std::string* error) const {
		const std::string normalized = Normalize(logical);
		if (normalized.empty()) {
			SetError(error, "invalid asset path: " + std::string(logical));
			return std::nullopt;
		}

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
		resolved.absolutePath = absolute;
		resolved.logicalPath = normalized;
		resolved.size = size;
		resolved.mtime = mtime;
		return resolved;
	}

	bool AssetResolver::Exists(std::string_view logical) const {
		return Resolve(logical, nullptr).has_value();
	}

	std::optional<std::vector<uint8_t>> AssetResolver::ReadBinary(
		std::string_view logical, std::string* error) const {
		const auto resolved = Resolve(logical, error);
		if (!resolved)
			return std::nullopt;

		std::ifstream file(resolved->absolutePath, std::ios::binary);
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
