#include "AssetCatalog.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace dEngine {

	namespace {

		void SetError(std::string* error, std::string message) {
			if (error) *error = std::move(message);
		}

		std::string ToLower(std::string_view text) {
			std::string lower;
			lower.reserve(text.size());
			for (char ch : text)
				lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
			return lower;
		}

		// ---- 词表 ----

		const std::pair<rhi::ImageFormat, const char*> kImageFormats[] = {
			{ rhi::ImageFormat::None, "None" },
			{ rhi::ImageFormat::R8, "R8" }, { rhi::ImageFormat::R8Srgb, "R8Srgb" },
			{ rhi::ImageFormat::RG8, "RG8" }, { rhi::ImageFormat::RG8Srgb, "RG8Srgb" },
			{ rhi::ImageFormat::RGBA8, "RGBA8" }, { rhi::ImageFormat::RGBA8Srgb, "RGBA8Srgb" },
			{ rhi::ImageFormat::BGRA8, "BGRA8" }, { rhi::ImageFormat::BGRA8Srgb, "BGRA8Srgb" },
			{ rhi::ImageFormat::R16F, "R16F" }, { rhi::ImageFormat::RG16F, "RG16F" },
			{ rhi::ImageFormat::RGBA16F, "RGBA16F" },
			{ rhi::ImageFormat::R32F, "R32F" }, { rhi::ImageFormat::RG32F, "RG32F" },
			{ rhi::ImageFormat::RGBA32F, "RGBA32F" },
			{ rhi::ImageFormat::R11G11B10F, "R11G11B10F" },
			{ rhi::ImageFormat::D32F, "D32F" }, { rhi::ImageFormat::D24S8, "D24S8" },
			{ rhi::ImageFormat::S8, "S8" },
		};

		const std::pair<rhi::ImageUsage, const char*> kImageUsages[] = {
			{ rhi::ImageUsage::Sampled, "Sampled" },
			{ rhi::ImageUsage::Storage, "Storage" },
			{ rhi::ImageUsage::ColorAttachment, "ColorAttachment" },
			{ rhi::ImageUsage::DepthStencil, "DepthStencil" },
			{ rhi::ImageUsage::TransferSrc, "TransferSrc" },
			{ rhi::ImageUsage::TransferDst, "TransferDst" },
		};

		const std::pair<rhi::ImageLayout, const char*> kImageLayouts[] = {
			{ rhi::ImageLayout::Undefined, "Undefined" },
			{ rhi::ImageLayout::General, "General" },
			{ rhi::ImageLayout::ColorAttachment, "ColorAttachment" },
			{ rhi::ImageLayout::DepthStencilAttachment, "DepthStencilAttachment" },
			{ rhi::ImageLayout::ShaderReadOnly, "ShaderReadOnly" },
			{ rhi::ImageLayout::TransferSrc, "TransferSrc" },
			{ rhi::ImageLayout::TransferDst, "TransferDst" },
			{ rhi::ImageLayout::PresentSrc, "PresentSrc" },
		};

		const std::pair<rhi::ImageViewType, const char*> kViewTypes[] = {
			{ rhi::ImageViewType::None, "None" },
			{ rhi::ImageViewType::Dim1D, "Dim1D" },
			{ rhi::ImageViewType::Dim2D, "Dim2D" },
			{ rhi::ImageViewType::Dim3D, "Dim3D" },
			{ rhi::ImageViewType::Dim1DArray, "Dim1DArray" },
			{ rhi::ImageViewType::Dim2DArray, "Dim2DArray" },
			{ rhi::ImageViewType::Cube, "Cube" },
			{ rhi::ImageViewType::CubeArray, "CubeArray" },
		};

		const std::pair<rhi::BufferUsage, const char*> kBufferUsages[] = {
			{ rhi::BufferUsage::Vertex, "Vertex" },
			{ rhi::BufferUsage::Index, "Index" },
			{ rhi::BufferUsage::Uniform, "Uniform" },
			{ rhi::BufferUsage::Storage, "Storage" },
			{ rhi::BufferUsage::TransferSrc, "TransferSrc" },
			{ rhi::BufferUsage::TransferDst, "TransferDst" },
			{ rhi::BufferUsage::Indirect, "Indirect" },
		};

		template <typename E, size_t N>
		std::optional<E> ParseFrom(std::string_view text, const std::pair<E, const char*> (&table)[N]) {
			for (const auto& entry : table) {
				if (text == entry.second)
					return entry.first;
			}
			return std::nullopt;
		}

		bool ValidateImageAsset(const ImageAsset& asset, std::string& error) {
			if (asset.format == rhi::ImageFormat::None) {
				error = "format is None";
				return false;
			}
			if (static_cast<uint32_t>(asset.usage) == 0) {
				error = "usage is empty";
				return false;
			}
			if (asset.srgb && !rhi::IsSrgbFormat(asset.format)
				&& rhi::ToSrgbFormat(asset.format) == rhi::ImageFormat::None) {
				error = "srgb flag requires a format with an sRGB variant";
				return false;
			}
			if (asset.viewType == rhi::ImageViewType::None) {
				error = "viewType is None";
				return false;
			}
			if (asset.samples != 1) {
				error = "asset images require samples = 1";
				return false;
			}
			if ((asset.width == 0) != (asset.height == 0)) {
				error = "width/height must both be 0 (decode) or both non-zero";
				return false;
			}
			if (asset.mipLevels == 0 && !asset.generateMips) {
				error = "mipLevels = 0 requires generateMips";
				return false;
			}
			if (asset.generateMips && asset.mipLevels == 1) {
				error = "generateMips conflicts with mipLevels = 1";
				return false;
			}

			auto fail = [&](std::string message) {
				error = std::move(message);
				return false;
			};
			switch (asset.viewType) {
			case rhi::ImageViewType::Dim1D:
				if (asset.height != 0 && asset.height != 1) return fail("Dim1D requires height = 1");
				if (asset.depth != 1) return fail("Dim1D requires depth = 1");
				if (asset.arrayLayers != 1) return fail("Dim1D requires arrayLayers = 1");
				break;
			case rhi::ImageViewType::Dim1DArray:
				if (asset.height != 0 && asset.height != 1) return fail("Dim1DArray requires height = 1");
				if (asset.depth != 1) return fail("Dim1DArray requires depth = 1");
				break;
			case rhi::ImageViewType::Dim2D:
				if (asset.depth != 1) return fail("Dim2D requires depth = 1");
				if (asset.arrayLayers != 1) return fail("Dim2D requires arrayLayers = 1");
				break;
			case rhi::ImageViewType::Dim2DArray:
				if (asset.depth != 1) return fail("Dim2DArray requires depth = 1");
				break;
			case rhi::ImageViewType::Dim3D:
				if (asset.arrayLayers != 1) return fail("Dim3D requires arrayLayers = 1");
				if (asset.depth == 0) return fail("Dim3D requires depth > 0");
				break;
			case rhi::ImageViewType::Cube:
				if (asset.arrayLayers != 6) return fail("Cube requires arrayLayers = 6");
				break;
			case rhi::ImageViewType::CubeArray:
				if (asset.arrayLayers < 6 || asset.arrayLayers % 6 != 0)
					return fail("CubeArray requires arrayLayers to be a multiple of 6");
				break;
			default:
				return fail("unsupported viewType");
			}
			return true;
		}

		// ---- 极简 JSON 游标：只覆盖清单需要的对象/字符串/数组/数字 ----

		class Cursor {
		public:
			explicit Cursor(std::string_view text) : m_text(text) {}

			bool Eat(char expected) {
				SkipWhitespace();
				if (m_pos < m_text.size() && m_text[m_pos] == expected) {
					++m_pos;
					return true;
				}
				return false;
			}

			bool ReadString(std::string& out) {
				SkipWhitespace();
				if (m_pos >= m_text.size() || m_text[m_pos] != '"')
					return false;
				++m_pos;
				out.clear();
				while (m_pos < m_text.size()) {
					const char ch = m_text[m_pos++];
					if (ch == '"')
						return true;
					if (ch != '\\') {
						out.push_back(ch);
						continue;
					}
					if (m_pos >= m_text.size())
						return false;
					switch (m_text[m_pos++]) {
					case '"':  out.push_back('"');  break;
					case '\\': out.push_back('\\'); break;
					case '/':  out.push_back('/');  break;
					case 'b':  out.push_back('\b'); break;
					case 'f':  out.push_back('\f'); break;
					case 'n':  out.push_back('\n'); break;
					case 'r':  out.push_back('\r'); break;
					case 't':  out.push_back('\t'); break;
					case 'u':
						if (m_pos + 4 > m_text.size())
							return false;
						m_pos += 4;
						out.push_back('?');
						break;
					default:
						return false;
					}
				}
				return false;
			}

			bool ReadUInt64(uint64_t& out) {
				SkipWhitespace();
				const size_t begin = m_pos;
				while (m_pos < m_text.size()) {
					const char ch = m_text[m_pos];
					if (ch < '0' || ch > '9') break;
					++m_pos;
				}
				if (m_pos == begin)
					return false;
				out = std::strtoull(std::string(m_text.substr(begin, m_pos - begin)).c_str(), nullptr, 10);
				return true;
			}

			bool ReadBool(bool& out) {
				SkipWhitespace();
				if (m_text.compare(m_pos, 4, "true") == 0) { m_pos += 4; out = true; return true; }
				if (m_text.compare(m_pos, 5, "false") == 0) { m_pos += 5; out = false; return true; }
				return false;
			}

			bool ReadStringArray(std::vector<std::string>& out) {
				if (!Eat('['))
					return false;
				if (Eat(']'))
					return true;
				while (true) {
					std::string value;
					if (!ReadString(value))
						return false;
					out.push_back(std::move(value));
					if (Eat(',')) continue;
					return Eat(']');
				}
			}

			bool SkipValue() { return SkipValue(0); }

		private:
			void SkipWhitespace() {
				while (m_pos < m_text.size()) {
					const char ch = m_text[m_pos];
					if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
						break;
					++m_pos;
				}
			}

			char Peek() {
				SkipWhitespace();
				return m_pos < m_text.size() ? m_text[m_pos] : '\0';
			}

			bool SkipValue(int depth) {
				if (depth > 32)
					return false;
				const char ch = Peek();
				if (ch == '\0')
					return false;
				if (ch == '"') {
					std::string ignored;
					return ReadString(ignored);
				}
				if (ch == '{') {
					++m_pos;
					if (Eat('}')) return true;
					while (true) {
						std::string key;
						if (!ReadString(key) || !Eat(':') || !SkipValue(depth + 1))
							return false;
						if (Eat(',')) continue;
						return Eat('}');
					}
				}
				if (ch == '[') {
					++m_pos;
					if (Eat(']')) return true;
					while (true) {
						if (!SkipValue(depth + 1))
							return false;
						if (Eat(',')) continue;
						return Eat(']');
					}
				}
				const size_t begin = m_pos;
				while (m_pos < m_text.size()) {
					const char c = m_text[m_pos];
					if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
						break;
					++m_pos;
				}
				return m_pos > begin;
			}

			std::string_view m_text;
			size_t m_pos = 0;
		};

		struct ParsedCatalog {
			std::vector<ImageAsset>  images;
			std::vector<BufferAsset> buffers;
		};

		bool ParseImageEntry(Cursor& cursor, ImageAsset& asset, std::string& error) {
			if (!cursor.Eat('{'))
				return false;
			if (cursor.Eat('}'))
				return true;
			auto readU32 = [&](uint32_t& out, const char* label) {
				uint64_t value = 0;
				if (!cursor.ReadUInt64(value)) {
					error = std::string(label) + " must be an integer";
					return false;
				}
				if (value > 0xFFFFFFFFull) {
					error = std::string(label) + " out of range";
					return false;
				}
				out = static_cast<uint32_t>(value);
				return true;
			};
			while (true) {
				std::string key;
				if (!cursor.ReadString(key) || !cursor.Eat(':')) {
					error = "malformed image field";
					return false;
				}
				if (key == "path") {
					if (!cursor.ReadString(asset.logicalPath)) {
						error = "image path must be a string";
						return false;
					}
				}
				else if (key == "format") {
					std::string text;
					if (!cursor.ReadString(text)) { error = "image format must be a string"; return false; }
					const auto parsed = ParseFrom<rhi::ImageFormat>(text, kImageFormats);
					if (!parsed) { error = "unknown image format \"" + text + "\""; return false; }
					asset.format = *parsed;
				}
				else if (key == "usage") {
					std::vector<std::string> names;
					if (!cursor.ReadStringArray(names)) { error = "image usage must be a string array"; return false; }
					uint32_t bits = 0;
					for (const auto& name : names) {
						const auto parsed = ParseFrom<rhi::ImageUsage>(name, kImageUsages);
						if (!parsed) { error = "unknown image usage \"" + name + "\""; return false; }
						bits |= static_cast<uint32_t>(*parsed);
					}
					asset.usage = static_cast<rhi::ImageUsage>(bits);
				}
				else if (key == "initialLayout") {
					std::string text;
					if (!cursor.ReadString(text)) { error = "initialLayout must be a string"; return false; }
					const auto parsed = ParseFrom<rhi::ImageLayout>(text, kImageLayouts);
					if (!parsed) { error = "unknown image layout \"" + text + "\""; return false; }
					asset.initialLayout = *parsed;
				}
				else if (key == "srgb") {
					if (!cursor.ReadBool(asset.srgb)) { error = "srgb must be a boolean"; return false; }
				}
				else if (key == "flipY") {
					if (!cursor.ReadBool(asset.flipY)) { error = "flipY must be a boolean"; return false; }
				}
				else if (key == "width") {
					if (!readU32(asset.width, "width")) return false;
				}
				else if (key == "height") {
					if (!readU32(asset.height, "height")) return false;
				}
				else if (key == "depth") {
					if (!readU32(asset.depth, "depth")) return false;
				}
				else if (key == "arrayLayers") {
					if (!readU32(asset.arrayLayers, "arrayLayers")) return false;
				}
				else if (key == "mipLevels") {
					if (!readU32(asset.mipLevels, "mipLevels")) return false;
				}
				else if (key == "samples") {
					if (!readU32(asset.samples, "samples")) return false;
				}
				else if (key == "viewType") {
					std::string text;
					if (!cursor.ReadString(text)) { error = "viewType must be a string"; return false; }
					const auto parsed = ParseFrom<rhi::ImageViewType>(text, kViewTypes);
					if (!parsed) { error = "unknown viewType \"" + text + "\""; return false; }
					asset.viewType = *parsed;
				}
				else if (key == "generateMips") {
					if (!cursor.ReadBool(asset.generateMips)) { error = "generateMips must be a boolean"; return false; }
				}
				else if (!cursor.SkipValue()) {
					error = "malformed image field \"" + key + "\"";
					return false;
				}
				if (cursor.Eat(',')) continue;
				return cursor.Eat('}');
			}
		}

		bool ParseBufferEntry(Cursor& cursor, BufferAsset& asset, std::string& error) {
			if (!cursor.Eat('{'))
				return false;
			if (cursor.Eat('}'))
				return true;
			while (true) {
				std::string key;
				if (!cursor.ReadString(key) || !cursor.Eat(':')) {
					error = "malformed buffer field";
					return false;
				}
				if (key == "path") {
					if (!cursor.ReadString(asset.logicalPath)) {
						error = "buffer path must be a string";
						return false;
					}
				}
				else if (key == "usage") {
					std::vector<std::string> names;
					if (!cursor.ReadStringArray(names)) { error = "buffer usage must be a string array"; return false; }
					uint32_t bits = 0;
					for (const auto& name : names) {
						const auto parsed = ParseFrom<rhi::BufferUsage>(name, kBufferUsages);
						if (!parsed) { error = "unknown buffer usage \"" + name + "\""; return false; }
						bits |= static_cast<uint32_t>(*parsed);
					}
					asset.usage = static_cast<rhi::BufferUsage>(bits);
				}
				else if (key == "hostVisible") {
					if (!cursor.ReadBool(asset.hostVisible)) { error = "hostVisible must be a boolean"; return false; }
				}
				else if (key == "byteSize") {
					if (!cursor.ReadUInt64(asset.byteSize)) { error = "byteSize must be an integer"; return false; }
				}
				else if (!cursor.SkipValue()) {
					error = "malformed buffer field \"" + key + "\"";
					return false;
				}
				if (cursor.Eat(',')) continue;
				return cursor.Eat('}');
			}
		}

		bool ParseManifest(std::string_view text, ParsedCatalog& out, std::string& error) {
			Cursor cursor(text);
			if (!cursor.Eat('{')) {
				error = "manifest must start with '{'";
				return false;
			}
			if (cursor.Eat('}')) {
				error = "manifest is empty";
				return false;
			}
			while (true) {
				std::string key;
				if (!cursor.ReadString(key) || !cursor.Eat(':')) {
					error = "malformed manifest key";
					return false;
				}
				if (key == "images" || key == "buffers") {
					const bool images = key == "images";
					if (!cursor.Eat('{')) {
						error = "\"" + key + "\" must be an object";
						return false;
					}
					if (!cursor.Eat('}')) {
						while (true) {
							std::string name;
							if (!cursor.ReadString(name) || !cursor.Eat(':')) {
								error = "malformed " + key + " entry name";
								return false;
							}
							if (images) {
								ImageAsset asset;
								asset.name = name;
								if (!ParseImageEntry(cursor, asset, error)) {
									error = "\"" + name + "\": " + error;
									return false;
								}
								out.images.push_back(std::move(asset));
							}
							else {
								BufferAsset asset;
								asset.name = name;
								if (!ParseBufferEntry(cursor, asset, error)) {
									error = "\"" + name + "\": " + error;
									return false;
								}
								out.buffers.push_back(std::move(asset));
							}
							if (cursor.Eat(',')) continue;
							if (!cursor.Eat('}')) {
								error = "malformed \"" + key + "\" object";
								return false;
							}
							break;
						}
					}
				}
				else if (key == "version") {
					uint64_t version = 0;
					if (!cursor.ReadUInt64(version)) { error = "version must be an integer"; return false; }
					if (version != 1) { error = "unsupported manifest version"; return false; }
				}
				else if (!cursor.SkipValue()) {
					error = "malformed manifest field \"" + key + "\"";
					return false;
				}
				if (cursor.Eat(',')) continue;
				if (!cursor.Eat('}')) {
					error = "manifest is not a closed object";
					return false;
				}
				break;
			}
			return true;
		}
	}

	bool AssetCatalog::RegisterImage(ImageAsset asset, std::string* error) {
		if (asset.name.empty()) {
			SetError(error, "image asset has empty name");
			return false;
		}
		if (m_images.find(asset.name) != m_images.end()
			|| m_buffers.find(asset.name) != m_buffers.end()) {
			SetError(error, "duplicate asset name: " + asset.name);
			return false;
		}
		const std::string normalized = ContentResolver::NormalizePath(asset.logicalPath);
		if (normalized.empty()) {
			SetError(error, "invalid asset path \"" + asset.logicalPath + "\" for " + asset.name);
			return false;
		}
		std::string validationError;
		if (!ValidateImageAsset(asset, validationError)) {
			SetError(error, "image asset \"" + asset.name + "\": " + validationError);
			return false;
		}
		asset.logicalPath = normalized;
		m_images.emplace(asset.name, std::move(asset));
		return true;
	}

	bool AssetCatalog::RegisterBuffer(BufferAsset asset, std::string* error) {
		if (asset.name.empty()) {
			SetError(error, "buffer asset has empty name");
			return false;
		}
		if (m_images.find(asset.name) != m_images.end()
			|| m_buffers.find(asset.name) != m_buffers.end()) {
			SetError(error, "duplicate asset name: " + asset.name);
			return false;
		}
		const std::string normalized = ContentResolver::NormalizePath(asset.logicalPath);
		if (normalized.empty()) {
			SetError(error, "invalid asset path \"" + asset.logicalPath + "\" for " + asset.name);
			return false;
		}
		asset.logicalPath = normalized;
		m_buffers.emplace(asset.name, std::move(asset));
		return true;
	}

	bool AssetCatalog::LoadManifest(std::string_view logicalPath, std::vector<std::string>* errors) {
		auto Add = [errors](std::string message) {
			if (errors) errors->push_back(std::move(message));
		};

		if (!m_resolver) {
			Add("asset catalog has no resolver");
			return false;
		}

		std::string readError;
		const auto bytes = m_resolver->ReadBinary(logicalPath, &readError);
		if (!bytes) {
			Add(readError);
			return false;
		}
		if (bytes->empty()) {
			Add("empty asset manifest: " + std::string(logicalPath));
			return false;
		}

		ParsedCatalog parsed;
		std::string parseError;
		if (!ParseManifest(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
				parsed, parseError)) {
			Add("cannot parse asset manifest " + std::string(logicalPath) + ": " + parseError);
			return false;
		}

		bool ok = true;
		for (auto& asset : parsed.images) {
			asset.logicalPath = ContentResolver::InheritPath(logicalPath, asset.logicalPath);
			std::string registerError;
			if (!RegisterImage(std::move(asset), &registerError)) {
				Add(registerError);
				ok = false;
			}
		}
		for (auto& asset : parsed.buffers) {
			asset.logicalPath = ContentResolver::InheritPath(logicalPath, asset.logicalPath);
			std::string registerError;
			if (!RegisterBuffer(std::move(asset), &registerError)) {
				Add(registerError);
				ok = false;
			}
		}
		return ok;
	}

	const ImageAsset* AssetCatalog::FindImage(std::string_view name) const {
		const auto it = m_images.find(std::string(name));
		return it == m_images.end() ? nullptr : &it->second;
	}

	const BufferAsset* AssetCatalog::FindBuffer(std::string_view name) const {
		const auto it = m_buffers.find(std::string(name));
		return it == m_buffers.end() ? nullptr : &it->second;
	}

	std::optional<AssetRevision> AssetCatalog::Revision(std::string_view name) const {
		if (!m_resolver)
			return std::nullopt;
		std::string path;
		if (const auto* image = FindImage(name))
			path = image->logicalPath;
		else if (const auto* buffer = FindBuffer(name))
			path = buffer->logicalPath;
		else
			return std::nullopt;

		const auto resolved = m_resolver->Resolve(path, nullptr);
		if (!resolved)
			return std::nullopt;
		return AssetRevision{ resolved->mtime, resolved->size, resolved->hash };
	}

	std::vector<std::string> AssetCatalog::ImageNames() const {
		std::vector<std::string> names;
		names.reserve(m_images.size());
		for (const auto& pair : m_images)
			names.push_back(pair.first);
		return names;
	}

	std::vector<std::string> AssetCatalog::BufferNames() const {
		std::vector<std::string> names;
		names.reserve(m_buffers.size());
		for (const auto& pair : m_buffers)
			names.push_back(pair.first);
		return names;
	}

	bool AssetCatalog::Validate(std::vector<std::string>* errors) const {
		bool ok = true;
		auto check = [&](const std::string& name, const std::string& path) {
			if (!m_resolver) {
				if (errors) errors->push_back("asset catalog has no resolver");
				ok = false;
				return;
			}
			std::string resolveError;
			if (!m_resolver->Resolve(path, &resolveError)) {
				if (errors) errors->push_back(name + ": " + resolveError);
				ok = false;
			}
		};
		for (const auto& pair : m_images) {
			std::string validationError;
			if (!ValidateImageAsset(pair.second, validationError)) {
				if (errors) errors->push_back(pair.first + ": " + validationError);
				ok = false;
			}
			check(pair.first, pair.second.logicalPath);
		}
		for (const auto& pair : m_buffers)
			check(pair.first, pair.second.logicalPath);
		return ok;
	}
}
