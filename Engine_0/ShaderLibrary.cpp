#include "ShaderLibrary.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <utility>

namespace dEngine {

	namespace {

		constexpr uint32_t kSpirvMagic = 0x07230203u;

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

		// ---- 极简 JSON 游标：只覆盖清单需要的对象/字符串/数组，不引入第三方依赖 ----

		class JsonCursor {
		public:
			explicit JsonCursor(std::string_view text) : m_text(text) {}

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
						// 清单只用 ASCII 标识符：消费 4 位十六进制，不做码点解码
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

			// 跳过任意值：未知字段用，保证向前兼容
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

		struct ManifestShader {
			std::string name;
			std::string path;
			std::string entry = "main";
			std::string stage;
			bool        hasStage = false;
			std::vector<std::string> defines;
		};

		bool ParseStringArray(JsonCursor& cursor, std::vector<std::string>& out) {
			if (!cursor.Eat('['))
				return false;
			if (cursor.Eat(']'))
				return true;
			while (true) {
				std::string value;
				if (!cursor.ReadString(value))
					return false;
				out.push_back(std::move(value));
				if (cursor.Eat(',')) continue;
				return cursor.Eat(']');
			}
		}

		bool ParseShaderObject(JsonCursor& cursor, ManifestShader& shader,
			std::vector<std::string>* warnings) {
			if (!cursor.Eat('{'))
				return false;
			if (cursor.Eat('}'))
				return true;
			while (true) {
				std::string key;
				if (!cursor.ReadString(key) || !cursor.Eat(':'))
					return false;
				if (key == "path") {
					if (!cursor.ReadString(shader.path)) return false;
				}
				else if (key == "stage") {
					if (!cursor.ReadString(shader.stage)) return false;
					shader.hasStage = true;
				}
				else if (key == "entry") {
					if (!cursor.ReadString(shader.entry)) return false;
				}
				else if (key == "defines") {
					if (!ParseStringArray(cursor, shader.defines)) return false;
				}
				else {
					if (warnings)
						warnings->push_back("unknown shader field \"" + key + "\" (shader " + shader.name + ")");
					if (!cursor.SkipValue()) return false;
				}
				if (cursor.Eat(',')) continue;
				return cursor.Eat('}');
			}
		}

		bool ParseManifest(std::string_view text, std::vector<ManifestShader>* out,
			std::vector<std::string>* warnings, std::string& error) {
			JsonCursor cursor(text);
			if (!cursor.Eat('{')) {
				error = "manifest must start with '{'";
				return false;
			}
			if (cursor.Eat('}')) {
				error = "manifest is empty";
				return false;
			}

			bool sawShaders = false;
			while (true) {
				std::string key;
				if (!cursor.ReadString(key) || !cursor.Eat(':')) {
					error = "malformed manifest key";
					return false;
				}
				if (key == "shaders") {
					sawShaders = true;
					if (!cursor.Eat('{')) {
						error = "\"shaders\" must be an object";
						return false;
					}
					if (!cursor.Eat('}')) {
						while (true) {
							ManifestShader shader;
							if (!cursor.ReadString(shader.name) || !cursor.Eat(':')) {
								error = "malformed shader entry name";
								return false;
							}
							if (!ParseShaderObject(cursor, shader, warnings)) {
								error = "malformed shader entry: " + shader.name;
								return false;
							}
							out->push_back(std::move(shader));
							if (cursor.Eat(',')) continue;
							if (!cursor.Eat('}')) {
								error = "malformed \"shaders\" object";
								return false;
							}
							break;
						}
					}
				}
				else if (key == "version") {
					if (!cursor.SkipValue()) {
						error = "malformed manifest value for \"version\"";
						return false;
					}
				}
				else {
					if (warnings)
						warnings->push_back("unknown manifest field \"" + key + "\"");
					if (!cursor.SkipValue()) {
						error = "malformed manifest value for \"" + key + "\"";
						return false;
					}
				}
				if (cursor.Eat(',')) continue;
				if (!cursor.Eat('}')) {
					error = "manifest is not a closed object";
					return false;
				}
				break;
			}

			if (!sawShaders) {
				error = "manifest has no \"shaders\" object";
				return false;
			}
			return true;
		}
	}

	std::optional<rhi::ShaderStage> ShaderLibrary::StageFromString(std::string_view text) {
		const std::string lower = ToLower(text);
		if (lower == "vertex")      return rhi::ShaderStage::Vertex;
		if (lower == "fragment" || lower == "pixel") return rhi::ShaderStage::Fragment;
		if (lower == "tesscontrol" || lower == "hull") return rhi::ShaderStage::TessControl;
		if (lower == "tesseval" || lower == "domain") return rhi::ShaderStage::TessEval;
		if (lower == "geometry")    return rhi::ShaderStage::Geometry;
		if (lower == "task" || lower == "amplification") return rhi::ShaderStage::Task;
		if (lower == "mesh")        return rhi::ShaderStage::Mesh;
		if (lower == "compute")     return rhi::ShaderStage::Compute;
		return std::nullopt;
	}

	const char* ShaderLibrary::StageToString(rhi::ShaderStage stage) {
		switch (stage) {
		case rhi::ShaderStage::Vertex:      return "Vertex";
		case rhi::ShaderStage::Fragment:    return "Fragment";
		case rhi::ShaderStage::TessControl: return "TessControl";
		case rhi::ShaderStage::TessEval:    return "TessEval";
		case rhi::ShaderStage::Geometry:    return "Geometry";
		case rhi::ShaderStage::Task:        return "Task";
		case rhi::ShaderStage::Mesh:        return "Mesh";
		case rhi::ShaderStage::Compute:     return "Compute";
		}
		return "Unknown";
	}

	std::optional<rhi::ShaderStage> ShaderLibrary::StageFromExtension(std::string_view logicalPath) {
		// 去掉构建产物的 ".spv" 后缀，再取一次 GLSL 扩展名
		std::string path(logicalPath);
		if (path.size() > 4) {
			const std::string tail = ToLower(std::string_view(path).substr(path.size() - 4));
			if (tail == ".spv")
				path.resize(path.size() - 4);
		}
		const size_t dot = path.find_last_of('.');
		if (dot == std::string::npos)
			return std::nullopt;
		const std::string ext = ToLower(std::string_view(path).substr(dot));
		if (ext == ".vert") return rhi::ShaderStage::Vertex;
		if (ext == ".frag") return rhi::ShaderStage::Fragment;
		if (ext == ".tesc") return rhi::ShaderStage::TessControl;
		if (ext == ".tese") return rhi::ShaderStage::TessEval;
		if (ext == ".geom") return rhi::ShaderStage::Geometry;
		if (ext == ".task") return rhi::ShaderStage::Task;
		if (ext == ".mesh") return rhi::ShaderStage::Mesh;
		if (ext == ".comp") return rhi::ShaderStage::Compute;
		return std::nullopt;
	}

	bool ShaderLibrary::Register(ShaderEntry entry, std::string* error) {
		if (entry.name.empty()) {
			SetError(error, "shader entry has empty name");
			return false;
		}
		const std::string normalized = AssetResolver::Normalize(entry.logicalPath);
		if (normalized.empty()) {
			SetError(error, "invalid shader path \"" + entry.logicalPath + "\" for " + entry.name);
			return false;
		}
		if (m_entries.find(entry.name) != m_entries.end()) {
			SetError(error, "duplicate shader name: " + entry.name);
			return false;
		}

		const std::optional<rhi::ShaderStage> extensionStage = StageFromExtension(normalized);
		if (entry.stage && extensionStage && *entry.stage != *extensionStage) {
			SetError(error, "shader stage mismatch: " + entry.name + " declares " +
				StageToString(*entry.stage) + " but path implies " + StageToString(*extensionStage));
			return false;
		}
		if (!entry.stage)
			entry.stage = extensionStage;
		if (entry.entryPoint.empty())
			entry.entryPoint = "main";
		entry.logicalPath = normalized;

		m_entries.emplace(entry.name, std::move(entry));
		return true;
	}

	bool ShaderLibrary::LoadManifest(std::string_view logicalPath, std::vector<std::string>* warnings) {
		auto Warn = [warnings](std::string message) {
			if (warnings) warnings->push_back(std::move(message));
		};

		if (!m_resolver) {
			Warn("shader library has no asset resolver");
			return false;
		}

		std::string readError;
		const auto bytes = m_resolver->ReadBinary(logicalPath, &readError);
		if (!bytes) {
			Warn(readError);
			return false;
		}
		if (bytes->empty()) {
			Warn("empty shader manifest: " + std::string(logicalPath));
			return false;
		}

		std::vector<ManifestShader> shaders;
		std::string parseError;
		if (!ParseManifest(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
				&shaders, warnings, parseError)) {
			Warn("cannot parse shader manifest " + std::string(logicalPath) + ": " + parseError);
			return false;
		}
		if (shaders.empty()) {
			Warn("shader manifest has no entries: " + std::string(logicalPath));
			return false;
		}

		bool allOk = true;
		for (auto& manifestShader : shaders) {
			ShaderEntry entry;
			entry.name = manifestShader.name;
			entry.logicalPath = manifestShader.path;
			entry.entryPoint = manifestShader.entry;
			entry.defines = std::move(manifestShader.defines);
			if (manifestShader.hasStage) {
				entry.stage = StageFromString(manifestShader.stage);
				if (!entry.stage) {
					Warn("unknown shader stage \"" + manifestShader.stage + "\" for " + manifestShader.name);
					allOk = false;
					continue;
				}
			}

			std::string registerError;
			if (!Register(std::move(entry), &registerError)) {
				Warn(registerError);
				allOk = false;
			}
		}
		return allOk;
	}

	bool ShaderLibrary::Contains(std::string_view name) const {
		return m_entries.find(std::string(name)) != m_entries.end();
	}

	std::vector<std::string> ShaderLibrary::Names() const {
		std::vector<std::string> names;
		names.reserve(m_entries.size());
		for (const auto& pair : m_entries)
			names.push_back(pair.first);
		return names;
	}

	std::optional<rhi::ShaderStageDesc> ShaderLibrary::Resolve(
		std::string_view name, std::string* error) const {
		const std::string key(name);
		const auto it = m_entries.find(key);
		if (it == m_entries.end()) {
			SetError(error, "shader not registered: " + key);
			return std::nullopt;
		}
		const ShaderEntry& entry = it->second;
		if (!entry.stage) {
			SetError(error, "cannot infer shader stage for " + key + " (path: " + entry.logicalPath + ")");
			return std::nullopt;
		}
		if (!m_resolver) {
			SetError(error, "shader library has no asset resolver");
			return std::nullopt;
		}

		std::string resolveError;
		const auto file = m_resolver->Resolve(entry.logicalPath, &resolveError);
		if (!file) {
			SetError(error, "shader file missing: " + entry.logicalPath + " (" + resolveError + ")");
			return std::nullopt;
		}

		// 提前拦住误把 GLSL 源当 SPIR-V 配置的情况
		std::ifstream stream(file->absolutePath, std::ios::binary);
		uint32_t magic = 0;
		stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
		if (!stream || magic != kSpirvMagic) {
			SetError(error, "not a SPIR-V module: " + entry.logicalPath);
			return std::nullopt;
		}

		rhi::ShaderStageDesc desc;
		desc.stage = *entry.stage;
		desc.spirvPath = file->absolutePath.string();
		desc.entryPoint = entry.entryPoint;
		return desc;
	}

	bool ShaderLibrary::ResolveStages(const std::vector<std::string>& names,
		std::vector<rhi::ShaderStageDesc>* out, std::vector<std::string>* errors) const {
		if (out) out->clear();
		bool ok = true;
		for (const auto& name : names) {
			std::string stageError;
			auto stage = Resolve(name, &stageError);
			if (!stage) {
				ok = false;
				if (errors) errors->push_back(stageError);
				continue;
			}
			if (out) out->push_back(std::move(*stage));
		}
		return ok;
	}

	std::optional<ShaderRevision> ShaderLibrary::Revision(std::string_view name) const {
		const auto it = m_entries.find(std::string(name));
		if (it == m_entries.end() || !m_resolver)
			return std::nullopt;
		const auto file = m_resolver->Resolve(it->second.logicalPath, nullptr);
		if (!file)
			return std::nullopt;
		return ShaderRevision{ file->mtime, file->size };
	}
}
