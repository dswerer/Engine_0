#include "AssetPack.h"

#include "ContentResolver.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <utility>

namespace dEngine {

	namespace {

		constexpr char     kMagic[8] = { 'D', 'E', 'N', 'G', 'P', 'A', 'K', '1' };
		constexpr uint32_t kVersion = 1;
		constexpr size_t   kHeaderSize = 24;

		void SetError(std::string* error, std::string message) {
			if (error) *error = std::move(message);
		}

		uint32_t ReadU32(const uint8_t* p) {
			return static_cast<uint32_t>(p[0])
				| (static_cast<uint32_t>(p[1]) << 8)
				| (static_cast<uint32_t>(p[2]) << 16)
				| (static_cast<uint32_t>(p[3]) << 24);
		}

		uint64_t ReadU64(const uint8_t* p) {
			return static_cast<uint64_t>(ReadU32(p))
				| (static_cast<uint64_t>(ReadU32(p + 4)) << 32);
		}

		void WriteU32At(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
			for (size_t i = 0; i < 4; ++i)
				out[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
		}

		void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
			for (size_t i = 0; i < 4; ++i)
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
		}

		void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
			for (size_t i = 0; i < 8; ++i)
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
		}

		uint64_t HashBytes(std::span<const uint8_t> bytes) {
			uint64_t hash = 14695981039346656037ull; // FNV-1a 64 offset basis
			for (uint8_t byte : bytes) {
				hash ^= byte;
				hash *= 1099511628211ull;
			}
			return hash;
		}
	}

	std::optional<AssetPack> AssetPack::Load(std::vector<uint8_t> bytes, std::string* error) {
		if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), kMagic, 8) != 0) {
			SetError(error, "not an asset pack (bad magic)");
			return std::nullopt;
		}
		const uint8_t* p = bytes.data();
		const uint32_t version = ReadU32(p + 8);
		if (version != kVersion) {
			SetError(error, "unsupported pack version: " + std::to_string(version));
			return std::nullopt;
		}
		const uint32_t entryCount = ReadU32(p + 12);
		const uint32_t indexOffset = ReadU32(p + 16);
		const uint32_t indexSize = ReadU32(p + 20);
		if (static_cast<uint64_t>(indexOffset) + indexSize > bytes.size()) {
			SetError(error, "pack index out of bounds");
			return std::nullopt;
		}

		AssetPack pack;
		size_t cursor = indexOffset;
		const size_t indexEnd = static_cast<size_t>(indexOffset) + indexSize;
		for (uint32_t i = 0; i < entryCount; ++i) {
			if (cursor + 32 > indexEnd) {
				SetError(error, "truncated pack index");
				return std::nullopt;
			}
			const uint32_t pathSize = ReadU32(p + cursor); cursor += 4;
			const uint32_t flags = ReadU32(p + cursor); cursor += 4;
			const uint64_t offset = ReadU64(p + cursor); cursor += 8;
			const uint64_t size = ReadU64(p + cursor); cursor += 8;
			const uint64_t hash = ReadU64(p + cursor); cursor += 8;
			if (cursor + pathSize > indexEnd) {
				SetError(error, "truncated pack path");
				return std::nullopt;
			}
			std::string path(reinterpret_cast<const char*>(p + cursor), pathSize);
			cursor += pathSize;

			if (flags != 0) {
				SetError(error, "unsupported pack entry flags: " + std::to_string(flags));
				return std::nullopt;
			}
			const std::string normalized = ContentResolver::Normalize(path);
			if (normalized.empty()) {
				SetError(error, "invalid pack entry path: " + path);
				return std::nullopt;
			}
			if (offset > bytes.size() || size > bytes.size() - offset) {
				SetError(error, "pack entry out of bounds: " + normalized);
				return std::nullopt;
			}
			Entry entry;
			entry.offset = offset;
			entry.size = size;
			entry.hash = hash;
			entry.flags = flags;
			if (!pack.m_entries.emplace(normalized, entry).second) {
				SetError(error, "duplicate pack entry: " + normalized);
				return std::nullopt;
			}
		}
		if (cursor != indexEnd) {
			SetError(error, "pack index has trailing bytes");
			return std::nullopt;
		}

		pack.m_bytes = std::move(bytes);
		pack.m_hash = HashBytes(pack.m_bytes);
		return pack;
	}

	std::optional<AssetPack> AssetPack::LoadFile(const std::string& path, std::string* error) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) {
			SetError(error, "cannot open pack: " + path);
			return std::nullopt;
		}
		const std::streamsize size = file.tellg();
		if (size <= 0) {
			SetError(error, "empty pack: " + path);
			return std::nullopt;
		}
		file.seekg(0, std::ios::beg);
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
			SetError(error, "cannot read pack: " + path);
			return std::nullopt;
		}
		return Load(std::move(bytes), error);
	}

	bool AssetPack::Contains(std::string_view logical) const {
		return m_entries.find(std::string(logical)) != m_entries.end();
	}

	std::optional<AssetPack::Entry> AssetPack::Stat(std::string_view logical) const {
		const auto it = m_entries.find(std::string(logical));
		if (it == m_entries.end())
			return std::nullopt;
		return it->second;
	}

	std::optional<std::span<const uint8_t>> AssetPack::View(std::string_view logical) const {
		const auto it = m_entries.find(std::string(logical));
		if (it == m_entries.end() || it->second.flags != 0)
			return std::nullopt;
		return std::span<const uint8_t>(m_bytes.data() + it->second.offset,
			static_cast<size_t>(it->second.size));
	}

	std::optional<std::vector<uint8_t>> AssetPack::Read(std::string_view logical) const {
		const auto view = View(logical);
		if (!view)
			return std::nullopt;
		return std::vector<uint8_t>(view->begin(), view->end());
	}

	std::optional<std::vector<uint8_t>> BuildAssetPack(const std::vector<PackInput>& entries,
		std::string* error) {
		struct Item {
			std::string          path;
			const std::vector<uint8_t>* data = nullptr;
		};

		std::vector<Item> items;
		std::set<std::string> seen;
		items.reserve(entries.size());
		for (const auto& input : entries) {
			const std::string normalized = ContentResolver::Normalize(input.path);
			if (normalized.empty()) {
				SetError(error, "invalid pack input path: " + input.path);
				return std::nullopt;
			}
			if (!seen.insert(normalized).second) {
				SetError(error, "duplicate pack input path: " + normalized);
				return std::nullopt;
			}
			items.push_back({ normalized, &input.data });
		}
		std::sort(items.begin(), items.end(),
			[](const Item& a, const Item& b) { return a.path < b.path; });

		std::vector<uint8_t> out(kHeaderSize, 0);
		std::memcpy(out.data(), kMagic, 8);
		WriteU32At(out, 8, kVersion);
		WriteU32At(out, 12, static_cast<uint32_t>(items.size()));

		struct Layout {
			std::string path;
			uint64_t    offset = 0;
			uint64_t    size = 0;
			uint64_t    hash = 0;
		};
		std::vector<Layout> layout;
		layout.reserve(items.size());
		for (const auto& item : items) {
			while (out.size() % 4 != 0)
				out.push_back(0);
			Layout entry;
			entry.path = item.path;
			entry.offset = out.size();
			entry.size = item.data->size();
			entry.hash = HashBytes(*item.data);
			out.insert(out.end(), item.data->begin(), item.data->end());
			layout.push_back(std::move(entry));
		}

		const uint64_t indexOffset = out.size();
		for (const auto& entry : layout) {
			AppendU32(out, static_cast<uint32_t>(entry.path.size()));
			AppendU32(out, 0);
			AppendU64(out, entry.offset);
			AppendU64(out, entry.size);
			AppendU64(out, entry.hash);
			out.insert(out.end(), entry.path.begin(), entry.path.end());
		}
		const uint64_t indexSize = out.size() - indexOffset;
		WriteU32At(out, 16, static_cast<uint32_t>(indexOffset));
		WriteU32At(out, 20, static_cast<uint32_t>(indexSize));
		return out;
	}
}
