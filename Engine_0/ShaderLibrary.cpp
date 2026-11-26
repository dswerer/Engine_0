#include "ShaderLibrary.h"

#include "ManifestJson.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(ENGINE0_HAS_SHADERC)
#include <shaderc/shaderc.h>
#endif

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#endif

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

		bool EndsWith(std::string_view text, std::string_view suffix) {
			return text.size() >= suffix.size()
				&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		// 源扩展名；".glsl" 无法从名字推断阶段，登记时必须显式给出 stage
		bool IsGlslSourcePath(std::string_view logicalPath) {
			const std::string lower = ToLower(logicalPath);
			static constexpr std::string_view kExtensions[] = {
				".vert", ".frag", ".comp", ".tesc", ".tese", ".geom", ".task", ".mesh", ".glsl"
			};
			for (const std::string_view extension : kExtensions) {
				if (EndsWith(lower, extension))
					return true;
			}
			return false;
		}

#if defined(ENGINE0_HAS_SHADERC)
		shaderc_shader_kind ShadercKindFor(rhi::ShaderStage stage) {
			switch (stage) {
			case rhi::ShaderStage::Vertex: return shaderc_glsl_vertex_shader;
			case rhi::ShaderStage::Fragment: return shaderc_glsl_fragment_shader;
			case rhi::ShaderStage::Compute: return shaderc_glsl_compute_shader;
			case rhi::ShaderStage::TessControl: return shaderc_glsl_tess_control_shader;
			case rhi::ShaderStage::TessEval: return shaderc_glsl_tess_evaluation_shader;
			case rhi::ShaderStage::Geometry: return shaderc_glsl_geometry_shader;
			case rhi::ShaderStage::Task: return shaderc_glsl_task_shader;
			case rhi::ShaderStage::Mesh: return shaderc_glsl_mesh_shader;
			}
			return shaderc_glsl_infer_from_source;
		}

		// 进程内编译，不走子进程；失败诊断直接来自 shaderc/glslang。
		bool CompileGlslWithShaderc(const ShaderEntry& entry, std::span<const uint8_t> source,
			std::vector<uint32_t>* spirv, std::string* error) {
			shaderc_compiler_t compiler = shaderc_compiler_initialize();
			if (compiler == nullptr) {
				SetError(error, "cannot initialize shaderc: " + entry.logicalPath);
				return false;
			}
			shaderc_compile_options_t options = shaderc_compile_options_initialize();
			if (options == nullptr) {
				shaderc_compiler_release(compiler);
				SetError(error, "cannot initialize shaderc compile options: " + entry.logicalPath);
				return false;
			}

			shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
				shaderc_env_version_vulkan_1_3);
			shaderc_compile_options_set_source_language(options, shaderc_source_language_glsl);
			shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_zero);
			for (const std::string& define : entry.defines) {
				if (define.empty())
					continue;
				const size_t equals = define.find('=');
				const bool hasValue = equals != std::string::npos;
				const std::string name = hasValue ? define.substr(0, equals) : define;
				const std::string value = hasValue ? define.substr(equals + 1) : std::string();
				if (name.empty()) {
					shaderc_compile_options_release(options);
					shaderc_compiler_release(compiler);
					SetError(error, "empty shader define name: " + entry.logicalPath);
					return false;
				}
				shaderc_compile_options_add_macro_definition(options, name.data(), name.size(),
					hasValue ? value.data() : nullptr, hasValue ? value.size() : 0);
			}

			shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler,
				reinterpret_cast<const char*>(source.data()), source.size(),
				ShadercKindFor(*entry.stage), entry.logicalPath.c_str(),
				entry.entryPoint.c_str(), options);
			if (result == nullptr) {
				shaderc_compile_options_release(options);
				shaderc_compiler_release(compiler);
				SetError(error, "shaderc returned no result: " + entry.logicalPath);
				return false;
			}

			const shaderc_compilation_status status =
				shaderc_result_get_compilation_status(result);
			if (status != shaderc_compilation_status_success) {
				std::string message = shaderc_result_get_error_message(result);
				if (message.empty())
					message = "shaderc compilation failed with status "
						+ std::to_string(static_cast<int>(status));
				shaderc_result_release(result);
				shaderc_compile_options_release(options);
				shaderc_compiler_release(compiler);
				SetError(error, "shader compile failed: " + entry.logicalPath + ": " + message);
				return false;
			}

			const size_t byteCount = shaderc_result_get_length(result);
			const char* bytes = shaderc_result_get_bytes(result);
			if (bytes == nullptr || byteCount < 4 || byteCount % 4 != 0) {
				shaderc_result_release(result);
				shaderc_compile_options_release(options);
				shaderc_compiler_release(compiler);
				SetError(error, "shaderc produced an invalid SPIR-V module: " + entry.logicalPath);
				return false;
			}
			std::vector<uint32_t> words(byteCount / sizeof(uint32_t));
			std::memcpy(words.data(), bytes, byteCount);
			if (words.front() != kSpirvMagic) {
				shaderc_result_release(result);
				shaderc_compile_options_release(options);
				shaderc_compiler_release(compiler);
				SetError(error, "shaderc produced an invalid SPIR-V module: " + entry.logicalPath);
				return false;
			}

			shaderc_result_release(result);
			shaderc_compile_options_release(options);
			shaderc_compiler_release(compiler);
			*spirv = std::move(words);
			return true;
		}
#endif

		constexpr uint64_t kFnvOffset = 14695981039346656037ull;
		constexpr uint64_t kFnvPrime = 1099511628211ull;

		void HashBytes(uint64_t& hash, const void* data, size_t size) {
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i) {
				hash ^= bytes[i];
				hash *= kFnvPrime;
			}
		}

		void HashString(uint64_t& hash, std::string_view text) {
			HashBytes(hash, text.data(), text.size());
			const uint8_t separator = 0;
			HashBytes(hash, &separator, sizeof(separator));
		}

		std::string ToHex(uint64_t value) {
			static constexpr char kDigits[] = "0123456789abcdef";
			std::string text(16, '0');
			for (size_t i = 0; i < text.size(); ++i) {
				text[text.size() - 1 - i] = kDigits[value & 0xfu];
				value >>= 4;
			}
			return text;
		}

		bool ReadCacheFile(const std::filesystem::path& path, std::vector<uint32_t>& words) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file)
				return false;
			const std::streamoff size = file.tellg();
			if (size < 4 || size % 4 != 0)
				return false;
			file.seekg(0, std::ios::beg);
			words.resize(static_cast<size_t>(size) / sizeof(uint32_t));
			file.read(reinterpret_cast<char*>(words.data()), size);
			if (!file)
				return false;
			return words.front() == kSpirvMagic;
		}

		// 缓存写失败不影响编译结果；下次重新编译即可
		void WriteCacheFile(const std::filesystem::path& path, const std::vector<uint32_t>& words) {
			std::error_code error;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), error);
			if (error)
				return;

			std::filesystem::path temporary = path;
			static std::atomic<uint64_t> counter{ 0 };
			temporary += ".tmp" + std::to_string(counter.fetch_add(1));
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			if (!file)
				return;
			file.write(reinterpret_cast<const char*>(words.data()),
				static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
			if (!file) {
				file.close();
				std::filesystem::remove(temporary, error);
				return;
			}
			file.close();

			std::filesystem::remove(path, error);
			error.clear();
			std::filesystem::rename(temporary, path, error);
			if (error)
				std::filesystem::remove(temporary, error);
		}

		std::string QuoteCommandArg(const std::string& argument) {
#if defined(_WIN32)
			std::string quoted = "\"";
			for (char ch : argument) {
				if (ch == '"')
					quoted += "\\\"";
				else
					quoted += ch;
			}
			quoted += '"';
			return quoted;
#else
			std::string quoted = "'";
			for (char ch : argument) {
				if (ch == '\'')
					quoted += "'\\''";
				else
					quoted += ch;
			}
			quoted += '\'';
			return quoted;
#endif
		}

		bool WriteBytesFile(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file)
				return false;
			if (!bytes.empty())
				file.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			return static_cast<bool>(file);
		}

		std::string ReadTextFile(const std::filesystem::path& path) {
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return {};
			std::ostringstream stream;
			stream << file.rdbuf();
			return stream.str();
		}

		std::string TrimText(std::string text) {
			size_t begin = 0;
			while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
				++begin;
			size_t end = text.size();
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
				--end;
			return text.substr(begin, end - begin);
		}

		std::optional<std::string> ReadEnvironment(const char* name) {
#if defined(_WIN32)
			char* value = nullptr;
			size_t capacity = 0;
			if (_dupenv_s(&value, &capacity, name) != 0 || value == nullptr)
				return std::nullopt;
			std::string result(value);
			std::free(value);
			return result;
#else
			if (const char* value = std::getenv(name); value != nullptr)
				return std::string(value);
			return std::nullopt;
#endif
		}

		// 编译用的临时目录，析构时递归删除（失败也不影响调用方）
		struct TempDirectory {
			explicit TempDirectory(std::filesystem::path directory)
				: path(std::move(directory)) {}
			~TempDirectory() {
				if (!path.empty()) {
					std::error_code error;
					std::filesystem::remove_all(path, error);
				}
			}
			TempDirectory(const TempDirectory&) = delete;
			TempDirectory& operator=(const TempDirectory&) = delete;

			std::filesystem::path path;
		};

		const char* GlslcStageFlag(rhi::ShaderStage stage) {
			switch (stage) {
			case rhi::ShaderStage::Vertex:      return "vertex";
			case rhi::ShaderStage::Fragment:    return "fragment";
			case rhi::ShaderStage::TessControl: return "tesscontrol";
			case rhi::ShaderStage::TessEval:    return "tesseval";
			case rhi::ShaderStage::Geometry:    return "geometry";
			case rhi::ShaderStage::Task:        return "task";
			case rhi::ShaderStage::Mesh:        return "mesh";
			case rhi::ShaderStage::Compute:     return "compute";
			}
			return "vertex";
		}

		// 不经过 cmd.exe：参数数组直接交给进程，避免引号被 cmd 重新解释。
		int RunGlslcProcess(const std::filesystem::path& executable,
			const std::vector<std::string>& arguments,
			const std::filesystem::path& logPath, std::string* error) {
#if defined(_WIN32)
			int logFd = -1;
			const errno_t openError = _wsopen_s(&logFd, logPath.wstring().c_str(),
				_O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
			if (openError != 0 || logFd < 0) {
				if (error) *error = "cannot open glslc log file: "
					+ std::error_code(openError, std::generic_category()).message();
				return -1;
			}
			std::fflush(stdout);
			std::fflush(stderr);
			const int savedOut = _dup(_fileno(stdout));
			const int savedErr = _dup(_fileno(stderr));
			if (savedOut < 0 || savedErr < 0) {
				_close(logFd);
				if (savedOut >= 0) _close(savedOut);
				if (savedErr >= 0) _close(savedErr);
				if (error) *error = "cannot redirect glslc output";
				return -1;
			}
			_dup2(logFd, _fileno(stdout));
			_dup2(logFd, _fileno(stderr));
			_close(logFd);

			std::vector<std::wstring> wideArguments;
			wideArguments.reserve(arguments.size() + 1);
			wideArguments.push_back(executable.wstring());
			for (const std::string& argument : arguments)
				wideArguments.push_back(std::filesystem::path(argument).wstring());
			std::vector<const wchar_t*> argv;
			argv.reserve(wideArguments.size() + 1);
			for (const std::wstring& argument : wideArguments)
				argv.push_back(argument.c_str());
			argv.push_back(nullptr);

			const intptr_t result = _wspawnv(_P_WAIT, executable.wstring().c_str(), argv.data());
			const int spawnError = errno;
			std::fflush(stdout);
			std::fflush(stderr);
			_dup2(savedOut, _fileno(stdout));
			_dup2(savedErr, _fileno(stderr));
			_close(savedOut);
			_close(savedErr);
			if (result == -1) {
				if (error) *error = "cannot start glslc: "
					+ std::error_code(spawnError, std::generic_category()).message();
				return -1;
			}
			return static_cast<int>(result);
#else
			std::string command = QuoteCommandArg(executable.string());
			for (const std::string& argument : arguments)
				command += " " + QuoteCommandArg(argument);
			command += " > " + QuoteCommandArg(logPath.string()) + " 2>&1";
			return std::system(command.c_str());
#endif
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
		const std::string normalized = ContentResolver::NormalizePath(entry.logicalPath);
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
			entry.logicalPath = ContentResolver::InheritPath(logicalPath, manifestShader.path);
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

	std::string ShaderLibrary::ToManifestJson() const {
		std::string out = "{\n  \"version\": 1,\n  \"shaders\": {\n";
		bool first = true;
		for (const auto& [name, entry] : m_entries) {
			if (!first)
				out += ",\n";
			first = false;

			out += "    ";
			detail::AppendJsonString(out, name);
			out += ": {\n";
			out += "      \"stage\": ";
			detail::AppendJsonString(out, entry.stage ? StageToString(*entry.stage) : "Unknown");
			out += ",\n      \"path\": ";
			detail::AppendJsonString(out, entry.logicalPath);
			out += ",\n      \"entry\": ";
			detail::AppendJsonString(out, entry.entryPoint);
			if (!entry.defines.empty()) {
				out += ",\n      \"defines\": [";
				for (size_t i = 0; i < entry.defines.size(); ++i) {
					if (i != 0)
						out += ", ";
					detail::AppendJsonString(out, entry.defines[i]);
				}
				out += "]";
			}
			out += "\n    }";
		}

		out += first ? "  }" : "\n  }";
		out += "\n}\n";
		return out;
	}

	bool ShaderLibrary::SaveManifest(const std::filesystem::path& path, std::string* error) const {
		if (path.empty()) {
			SetError(error, "shader manifest path is empty");
			return false;
		}
		if (m_entries.empty()) {
			SetError(error, "shader library has no entries; LoadManifest would reject the result");
			return false;
		}
		for (const auto& [name, entry] : m_entries) {
			if (!entry.stage) {
				SetError(error, "shader \"" + name + "\" has no stage and cannot be serialized");
				return false;
			}
			if (entry.logicalPath.empty()) {
				SetError(error, "shader \"" + name + "\" has an empty logical path");
				return false;
			}
		}

		std::error_code ec;
		const std::filesystem::path parent = path.parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent, ec);
		if (ec) {
			SetError(error, "cannot create shader manifest directory \"" + parent.string() + "\": " + ec.message());
			return false;
		}

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file) {
			SetError(error, "cannot open shader manifest for writing: " + path.string());
			return false;
		}
		const std::string json = ToManifestJson();
		file.write(json.data(), static_cast<std::streamsize>(json.size()));
		if (!file) {
			SetError(error, "cannot write shader manifest: " + path.string());
			return false;
		}
		return true;
	}

	bool ShaderLibrary::Contains(std::string_view name) const {
		return m_entries.find(std::string(name)) != m_entries.end();
	}

	const ShaderEntry* ShaderLibrary::Find(std::string_view name) const {
		const auto it = m_entries.find(std::string(name));
		return it == m_entries.end() ? nullptr : &it->second;
	}

	std::vector<std::string> ShaderLibrary::Names() const {
		std::vector<std::string> names;
		names.reserve(m_entries.size());
		for (const auto& pair : m_entries)
			names.push_back(pair.first);
		return names;
	}

	std::string ShaderLibrary::CompileCacheKey(const ShaderEntry& entry,
		std::span<const uint8_t> source) const {
		uint64_t hash = kFnvOffset;
		HashString(hash, "engine0-shader-compile-v1");
		HashString(hash, entry.logicalPath);
		HashString(hash, entry.stage ? StageToString(*entry.stage) : "Unknown");
		HashString(hash, entry.entryPoint);
		for (const std::string& define : entry.defines)
			HashString(hash, define);
		HashString(hash, "vulkan1.3");
		HashString(hash, "O0");
#if defined(ENGINE0_HAS_SHADERC)
		HashString(hash, "shaderc");
#else
		HashString(hash, "glslc");
#endif
		HashBytes(hash, source.data(), source.size());
		return ToHex(hash);
	}

	std::filesystem::path ShaderLibrary::FindGlslc() const {
		if (!m_glslcPath.empty())
			return m_glslcPath;
		if (const std::optional<std::string> environment = ReadEnvironment("ENGINE0_GLSLC");
			environment && !environment->empty())
			return std::filesystem::path(*environment);
#if defined(ENGINE0_GLSLC_PATH)
		{
			std::error_code error;
			const std::filesystem::path built = ENGINE0_GLSLC_PATH;
			if (std::filesystem::is_regular_file(built, error))
				return built;
		}
#endif
#if defined(_WIN32)
		constexpr char kPathSeparator = ';';
		constexpr const char* kExecutableName = "glslc.exe";
#else
		constexpr char kPathSeparator = ':';
		constexpr const char* kExecutableName = "glslc";
#endif
		if (const std::optional<std::string> sdk = ReadEnvironment("VULKAN_SDK");
			sdk && !sdk->empty()) {
			std::error_code error;
#if defined(_WIN32)
			const std::filesystem::path candidate =
				std::filesystem::path(*sdk) / "Bin" / kExecutableName;
#else
			const std::filesystem::path candidate =
				std::filesystem::path(*sdk) / "bin" / kExecutableName;
#endif
			if (std::filesystem::is_regular_file(candidate, error))
				return candidate;
		}
		if (const std::optional<std::string> path = ReadEnvironment("PATH");
			path && !path->empty()) {
			std::string_view remaining(*path);
			while (!remaining.empty()) {
				const size_t separator = remaining.find(kPathSeparator);
				const std::string_view entry = remaining.substr(0, separator);
				if (!entry.empty()) {
					std::error_code error;
					const std::filesystem::path candidate =
						std::filesystem::path(entry) / kExecutableName;
					if (std::filesystem::is_regular_file(candidate, error))
						return candidate;
				}
				if (separator == std::string_view::npos)
					break;
				remaining.remove_prefix(separator + 1);
			}
		}
		return {};
	}

	bool ShaderLibrary::CompileGlsl(const ShaderEntry& entry, std::span<const uint8_t> source,
		std::vector<uint32_t>* spirv, std::string* error) const {
		if (!m_runtimeCompilation) {
			SetError(error, "runtime shader compilation is disabled: " + entry.logicalPath);
			return false;
		}
		if (!entry.stage || spirv == nullptr) {
			SetError(error, "cannot compile shader without a stage: " + entry.logicalPath);
			return false;
		}
		// GLSL 的入口点固定为 main；HLSL 的入口点参数不适用于这里
		if (entry.entryPoint != "main") {
			SetError(error, "GLSL requires the entry point to be 'main' (got '"
				+ entry.entryPoint + "'): " + entry.logicalPath);
			return false;
		}

		const std::string key = CompileCacheKey(entry, source);
		{
			std::lock_guard<std::mutex> lock(m_compileMutex);
			const auto cached = m_compileCache.find(key);
			if (cached != m_compileCache.end()) {
				*spirv = cached->second;
				return true;
			}
		}
		if (!m_compileCacheDirectory.empty()) {
			std::vector<uint32_t> cached;
			if (ReadCacheFile(m_compileCacheDirectory / (key + ".spv"), cached)) {
				std::lock_guard<std::mutex> lock(m_compileMutex);
				m_compileCache.emplace(key, cached);
				*spirv = std::move(cached);
				return true;
			}
		}

#if defined(ENGINE0_HAS_SHADERC)
		// shaderc 是进程内编译，不占用 stdout/stderr，也不经过临时文件。
		std::vector<uint32_t> compiled;
		std::string compileError;
		if (!CompileGlslWithShaderc(entry, source, &compiled, &compileError)) {
			SetError(error, compileError);
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(m_compileMutex);
			m_compileCache[key] = compiled;
		}
		if (!m_compileCacheDirectory.empty())
			WriteCacheFile(m_compileCacheDirectory / (key + ".spv"), compiled);
		*spirv = std::move(compiled);
		return true;
#else
		// glslc 子进程会重定向全局 stdout/stderr，编译阶段必须串行；拿到锁后重查缓存，
		// 避免多个线程同时未命中时重复编译同一份源码。
		std::lock_guard<std::mutex> processLock(m_processMutex);
		{
			std::lock_guard<std::mutex> lock(m_compileMutex);
			const auto cached = m_compileCache.find(key);
			if (cached != m_compileCache.end()) {
				*spirv = cached->second;
				return true;
			}
		}
		if (!m_compileCacheDirectory.empty()) {
			std::vector<uint32_t> cached;
			if (ReadCacheFile(m_compileCacheDirectory / (key + ".spv"), cached)) {
				std::lock_guard<std::mutex> lock(m_compileMutex);
				m_compileCache.emplace(key, cached);
				*spirv = std::move(cached);
				return true;
			}
		}

		const std::filesystem::path glslc = FindGlslc();
		if (glslc.empty()) {
			SetError(error, "runtime shader compilation unavailable: glslc not found "
				"(set ENGINE0_GLSLC or ShaderLibrary::SetGlslcPath): " + entry.logicalPath);
			return false;
		}

		std::error_code fileError;
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		static std::atomic<uint64_t> counter{ 0 };
		const std::filesystem::path directory = std::filesystem::temp_directory_path(fileError)
			/ ("engine0-shader-" + key + "-" + std::to_string(stamp) + "-"
				+ std::to_string(counter.fetch_add(1)));
		if (fileError) {
			SetError(error, "cannot create shader compile directory: " + fileError.message());
			return false;
		}
		std::filesystem::create_directories(directory, fileError);
		if (fileError) {
			SetError(error, "cannot create shader compile directory: " + fileError.message());
			return false;
		}
		TempDirectory temporary(directory);

		const std::filesystem::path sourcePath = directory / "source.glsl";
		const std::filesystem::path outputPath = directory / "shader.spv";
		const std::filesystem::path logPath = directory / "glslc.log";
		if (!WriteBytesFile(sourcePath, source)) {
			SetError(error, "cannot write temporary shader source: " + entry.logicalPath);
			return false;
		}

		std::vector<std::string> arguments;
		arguments.push_back("--target-env=vulkan1.3");
		arguments.push_back(std::string("-fshader-stage=") + GlslcStageFlag(*entry.stage));
		for (const std::string& define : entry.defines) {
			if (!define.empty())
				arguments.push_back("-D" + define);
		}
		arguments.push_back("-o");
		arguments.push_back(outputPath.string());
		arguments.push_back(sourcePath.string());

		std::string processError;
		const int exitCode = RunGlslcProcess(glslc, arguments, logPath, &processError);
		if (exitCode < 0) {
			SetError(error, processError + ": " + entry.logicalPath);
			return false;
		}
		if (exitCode != 0) {
			std::string message = TrimText(ReadTextFile(logPath));
			if (message.empty())
				message = "glslc exited with code " + std::to_string(exitCode);
			SetError(error, "shader compile failed: " + entry.logicalPath + ": " + message);
			return false;
		}

		std::vector<uint32_t> words;
		if (!ReadCacheFile(outputPath, words)) {
			SetError(error, "glslc produced an invalid SPIR-V module: " + entry.logicalPath);
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(m_compileMutex);
			m_compileCache[key] = words;
		}
		if (!m_compileCacheDirectory.empty())
			WriteCacheFile(m_compileCacheDirectory / (key + ".spv"), words);
		*spirv = std::move(words);
		return true;
#endif
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

		// 内存源与磁盘源统一走 Resolver；GLSL 源在下面按需编译，SPIR-V 直接校验
		const auto view = m_resolver->ViewBinary(entry.logicalPath, nullptr);
		std::vector<uint8_t> owned;
		std::span<const uint8_t> bytes;
		if (view) {
			bytes = *view;
		}
		else {
			auto data = m_resolver->ReadBinary(entry.logicalPath, &resolveError);
			if (!data) {
				SetError(error, "cannot read shader: " + entry.logicalPath + " (" + resolveError + ")");
				return std::nullopt;
			}
			owned = std::move(*data);
			bytes = owned;
		}
		if (IsGlslSourcePath(entry.logicalPath)) {
			rhi::ShaderStageDesc desc;
			desc.stage = *entry.stage;
			desc.entryPoint = entry.entryPoint;
			if (!CompileGlsl(entry, bytes, &desc.spirv, error))
				return std::nullopt;
			return desc;
		}

		if (bytes.size() < 4 || bytes.size() % 4 != 0) {
			SetError(error, "not a SPIR-V module: " + entry.logicalPath);
			return std::nullopt;
		}
		uint32_t magic = 0;
		std::memcpy(&magic, bytes.data(), sizeof(magic));
		if (magic != kSpirvMagic) {
			SetError(error, "not a SPIR-V module: " + entry.logicalPath);
			return std::nullopt;
		}

		rhi::ShaderStageDesc desc;
		desc.stage = *entry.stage;
		if (file->absolutePath) {
			desc.spirvPath = file->absolutePath->string();
		}
		else {
			desc.spirv.resize(bytes.size() / sizeof(uint32_t));
			std::memcpy(desc.spirv.data(), bytes.data(), bytes.size());
		}
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
