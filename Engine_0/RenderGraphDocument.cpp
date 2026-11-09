#include "RenderGraphDocument.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <utility>

namespace dEngine {

	namespace {

		// ================= JSON DOM（对象保序，输出稳定，便于 diff 与 round-trip 比对）=================

		struct Json {
			enum class Kind { Null, Bool, Number, String, Array, Object };

			Kind        kind = Kind::Null;
			bool        boolean = false;
			bool        isInteger = false;
			int64_t     integer = 0;
			double      number = 0.0;
			std::string text;
			std::vector<Json> items;
			std::vector<std::pair<std::string, Json>> members;

			static Json Obj() { Json j; j.kind = Kind::Object; return j; }
			static Json Arr() { Json j; j.kind = Kind::Array; return j; }
			static Json Str(std::string value) { Json j; j.kind = Kind::String; j.text = std::move(value); return j; }
			static Json Int(int64_t value) {
				Json j; j.kind = Kind::Number; j.isInteger = true; j.integer = value; return j;
			}
			static Json Real(double value) { Json j; j.kind = Kind::Number; j.number = value; return j; }
			static Json Bool(bool value) { Json j; j.kind = Kind::Bool; j.boolean = value; return j; }

			void Set(std::string key, Json value) { members.emplace_back(std::move(key), std::move(value)); }
			void Push(Json value) { items.push_back(std::move(value)); }

			const Json* Find(std::string_view key) const {
				if (kind != Kind::Object)
					return nullptr;
				for (const auto& member : members) {
					if (member.first == key)
						return &member.second;
				}
				return nullptr;
			}
		};

		void DumpString(std::string_view value, std::string& out) {
			out.push_back('"');
			for (char ch : value) {
				switch (ch) {
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:   out.push_back(ch); break;
				}
			}
			out.push_back('"');
		}

		void DumpIndent(int indent, std::string& out) {
			out.append(static_cast<size_t>(indent) * 2, ' ');
		}

		void Dump(const Json& value, std::string& out, int indent) {
			switch (value.kind) {
			case Json::Kind::Null:
				out += "null";
				break;
			case Json::Kind::Bool:
				out += value.boolean ? "true" : "false";
				break;
			case Json::Kind::Number:
				if (value.isInteger) {
					out += std::to_string(value.integer);
				}
				else {
					std::ostringstream stream;
					stream << std::setprecision(9) << value.number;
					out += stream.str();
				}
				break;
			case Json::Kind::String:
				DumpString(value.text, out);
				break;
			case Json::Kind::Array:
				if (value.items.empty()) {
					out += "[]";
					break;
				}
				out += "[\n";
				for (size_t i = 0; i < value.items.size(); ++i) {
					DumpIndent(indent + 1, out);
					Dump(value.items[i], out, indent + 1);
					if (i + 1 < value.items.size()) out += ',';
					out.push_back('\n');
				}
				DumpIndent(indent, out);
				out.push_back(']');
				break;
			case Json::Kind::Object:
				if (value.members.empty()) {
					out += "{}";
					break;
				}
				out += "{\n";
				for (size_t i = 0; i < value.members.size(); ++i) {
					DumpIndent(indent + 1, out);
					DumpString(value.members[i].first, out);
					out += ": ";
					Dump(value.members[i].second, out, indent + 1);
					if (i + 1 < value.members.size()) out += ',';
					out.push_back('\n');
				}
				DumpIndent(indent, out);
				out.push_back('}');
				break;
			}
		}

		class JsonParser {
		public:
			explicit JsonParser(std::string_view text) : m_text(text) {}

			bool Parse(Json& out, std::string& error) {
				SkipWhitespace();
				if (!ParseValue(out, 0)) {
					error = m_error.empty() ? "malformed JSON" : m_error;
					return false;
				}
				SkipWhitespace();
				if (m_pos != m_text.size()) {
					error = "trailing characters after JSON value";
					return false;
				}
				return true;
			}

		private:
			bool Fail(std::string message) {
				if (m_error.empty())
					m_error = std::move(message);
				return false;
			}

			void SkipWhitespace() {
				while (m_pos < m_text.size()) {
					const char ch = m_text[m_pos];
					if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
						break;
					++m_pos;
				}
			}

			bool ParseValue(Json& out, int depth) {
				if (depth > 64)
					return Fail("JSON nesting too deep");
				SkipWhitespace();
				if (m_pos >= m_text.size())
					return Fail("unexpected end of JSON");
				const char ch = m_text[m_pos];
				if (ch == '{')
					return ParseObject(out, depth);
				if (ch == '[')
					return ParseArray(out, depth);
				if (ch == '"') {
					out.kind = Json::Kind::String;
					return ParseString(out.text);
				}
				if (MatchLiteral("true")) { out = Json::Bool(true); return true; }
				if (MatchLiteral("false")) { out = Json::Bool(false); return true; }
				if (MatchLiteral("null")) { out = Json(); return true; }
				if (ch == '-' || (ch >= '0' && ch <= '9'))
					return ParseNumber(out);
				return Fail("unexpected character in JSON");
			}

			bool MatchLiteral(std::string_view literal) {
				if (m_text.compare(m_pos, literal.size(), literal) != 0)
					return false;
				m_pos += literal.size();
				return true;
			}

			bool ParseObject(Json& out, int depth) {
				++m_pos; // '{'
				out = Json::Obj();
				SkipWhitespace();
				if (m_pos < m_text.size() && m_text[m_pos] == '}') { ++m_pos; return true; }
				while (true) {
					std::string key;
					if (!ParseString(key))
						return false;
					SkipWhitespace();
					if (m_pos >= m_text.size() || m_text[m_pos] != ':')
						return Fail("expected ':' in object");
					++m_pos;
					Json value;
					if (!ParseValue(value, depth + 1))
						return false;
					out.members.emplace_back(std::move(key), std::move(value));
					SkipWhitespace();
					if (m_pos < m_text.size() && m_text[m_pos] == ',') { ++m_pos; continue; }
					if (m_pos < m_text.size() && m_text[m_pos] == '}') { ++m_pos; return true; }
					return Fail("expected ',' or '}' in object");
				}
			}

			bool ParseArray(Json& out, int depth) {
				++m_pos; // '['
				out = Json::Arr();
				SkipWhitespace();
				if (m_pos < m_text.size() && m_text[m_pos] == ']') { ++m_pos; return true; }
				while (true) {
					Json value;
					if (!ParseValue(value, depth + 1))
						return false;
					out.items.push_back(std::move(value));
					SkipWhitespace();
					if (m_pos < m_text.size() && m_text[m_pos] == ',') { ++m_pos; continue; }
					if (m_pos < m_text.size() && m_text[m_pos] == ']') { ++m_pos; return true; }
					return Fail("expected ',' or ']' in array");
				}
			}

			bool ParseString(std::string& out) {
				SkipWhitespace();
				if (m_pos >= m_text.size() || m_text[m_pos] != '"')
					return Fail("expected string");
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
						return Fail("unterminated escape");
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
							return Fail("truncated \\u escape");
						m_pos += 4;
						out.push_back('?');
						break;
					default:
						return Fail("invalid escape");
					}
				}
				return Fail("unterminated string");
			}

			bool ParseNumber(Json& out) {
				const size_t begin = m_pos;
				bool real = false;
				if (m_text[m_pos] == '-') ++m_pos;
				while (m_pos < m_text.size()) {
					const char ch = m_text[m_pos];
					if (ch >= '0' && ch <= '9') { ++m_pos; continue; }
					if (ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
						real = true;
						++m_pos;
						continue;
					}
					break;
				}
				if (m_pos == begin)
					return Fail("malformed number");
				const std::string token(m_text.substr(begin, m_pos - begin));
				if (real) {
					out = Json::Real(std::strtod(token.c_str(), nullptr));
				}
				else {
					out = Json::Int(static_cast<int64_t>(std::strtoll(token.c_str(), nullptr, 10)));
				}
				return true;
			}

			std::string_view m_text;
			size_t           m_pos = 0;
			std::string      m_error;
		};

		// ================= 枚举词表 =================

		template <typename E, size_t N>
		const char* NameOf(E value, const std::pair<E, const char*> (&table)[N]) {
			for (const auto& entry : table) {
				if (entry.first == value)
					return entry.second;
			}
			return "Unknown";
		}

		template <typename E, size_t N>
		std::optional<E> ParseFrom(std::string_view text, const std::pair<E, const char*> (&table)[N]) {
			for (const auto& entry : table) {
				if (text == entry.second)
					return entry.first;
			}
			return std::nullopt;
		}

		const std::pair<ImageFormat, const char*> kImageFormats[] = {
			{ ImageFormat::None, "None" },
			{ ImageFormat::R8, "R8" }, { ImageFormat::RG8, "RG8" },
			{ ImageFormat::RGBA8, "RGBA8" }, { ImageFormat::BGRA8, "BGRA8" },
			{ ImageFormat::R16F, "R16F" }, { ImageFormat::RG16F, "RG16F" }, { ImageFormat::RGBA16F, "RGBA16F" },
			{ ImageFormat::R32F, "R32F" }, { ImageFormat::RG32F, "RG32F" }, { ImageFormat::RGBA32F, "RGBA32F" },
			{ ImageFormat::R11G11B10F, "R11G11B10F" },
			{ ImageFormat::D32F, "D32F" }, { ImageFormat::D24S8, "D24S8" }, { ImageFormat::S8, "S8" },
		};

		const std::pair<ImageViewType, const char*> kViewTypes[] = {
			{ ImageViewType::None, "None" }, { ImageViewType::Dim1D, "Dim1D" },
			{ ImageViewType::Dim2D, "Dim2D" }, { ImageViewType::Dim3D, "Dim3D" },
			{ ImageViewType::Dim1DArray, "Dim1DArray" }, { ImageViewType::Dim2DArray, "Dim2DArray" },
			{ ImageViewType::Cube, "Cube" }, { ImageViewType::CubeArray, "CubeArray" },
		};

		const std::pair<LoadOp, const char*> kLoadOps[] = {
			{ LoadOp::Load, "Load" }, { LoadOp::Clear, "Clear" }, { LoadOp::DontCare, "DontCare" },
		};

		const std::pair<StoreOp, const char*> kStoreOps[] = {
			{ StoreOp::Store, "Store" }, { StoreOp::DontCare, "DontCare" },
		};

		const std::pair<BufferUsage, const char*> kBufferUsages[] = {
			{ BufferUsage::Vertex, "Vertex" }, { BufferUsage::Index, "Index" },
			{ BufferUsage::Uniform, "Uniform" }, { BufferUsage::Storage, "Storage" },
			{ BufferUsage::TransferSrc, "TransferSrc" }, { BufferUsage::TransferDst, "TransferDst" },
			{ BufferUsage::Indirect, "Indirect" },
		};

		const std::pair<rhi::ImageLayout, const char*> kImageLayouts[] = {
			{ rhi::ImageLayout::Undefined, "Undefined" }, { rhi::ImageLayout::General, "General" },
			{ rhi::ImageLayout::ColorAttachment, "ColorAttachment" },
			{ rhi::ImageLayout::DepthStencilAttachment, "DepthStencilAttachment" },
			{ rhi::ImageLayout::ShaderReadOnly, "ShaderReadOnly" },
			{ rhi::ImageLayout::TransferSrc, "TransferSrc" },
			{ rhi::ImageLayout::TransferDst, "TransferDst" },
			{ rhi::ImageLayout::PresentSrc, "PresentSrc" },
		};

		const std::pair<rhi::QueueClass, const char*> kQueueClasses[] = {
			{ rhi::QueueClass::Graphics, "Graphics" },
			{ rhi::QueueClass::AsyncCompute, "AsyncCompute" },
			{ rhi::QueueClass::Transfer, "Transfer" },
		};

		const std::pair<Usage, const char*> kUsages[] = {
			{ Usage::Auto, "Auto" },
			{ Usage::ColorAttachment, "ColorAttachment" },
			{ Usage::DepthStencilAttachment, "DepthStencilAttachment" },
			{ Usage::ResolveSrc, "ResolveSrc" }, { Usage::ResolveDst, "ResolveDst" },
			{ Usage::SampledRead, "SampledRead" },
			{ Usage::StorageRead, "StorageRead" }, { Usage::StorageWrite, "StorageWrite" },
			{ Usage::UniformRead, "UniformRead" },
			{ Usage::VertexInputRead, "VertexInputRead" }, { Usage::IndexRead, "IndexRead" },
			{ Usage::IndirectRead, "IndirectRead" },
			{ Usage::TransferRead, "TransferRead" }, { Usage::TransferWrite, "TransferWrite" },
			{ Usage::HostWrite, "HostWrite" }, { Usage::HostRead, "HostRead" },
			{ Usage::PresentSrc, "PresentSrc" },
			{ Usage::MuxPrepare, "MuxPrepare" }, { Usage::MuxSelect, "MuxSelect" },
		};

		const std::pair<Pass::Type, const char*> kPassTypes[] = {
			{ Pass::None, "None" }, { Pass::Render, "Render" }, { Pass::Compute, "Compute" },
			{ Pass::Host, "Host" }, { Pass::Transfer, "Transfer" }, { Pass::Mux, "Mux" },
			{ Pass::Present, "Present" },
		};

		const std::pair<Resource::Type, const char*> kResourceTypes[] = {
			{ Resource::None, "None" }, { Resource::Image, "Image" }, { Resource::Buffer, "Buffer" },
		};

		const std::pair<rhi::ShaderStage, const char*> kShaderStages[] = {
			{ rhi::ShaderStage::Vertex, "Vertex" }, { rhi::ShaderStage::Fragment, "Fragment" },
			{ rhi::ShaderStage::TessControl, "TessControl" }, { rhi::ShaderStage::TessEval, "TessEval" },
			{ rhi::ShaderStage::Geometry, "Geometry" }, { rhi::ShaderStage::Task, "Task" },
			{ rhi::ShaderStage::Mesh, "Mesh" }, { rhi::ShaderStage::Compute, "Compute" },
		};

		const std::pair<PrimitiveTopology, const char*> kTopologies[] = {
			{ PrimitiveTopology::PointList, "PointList" }, { PrimitiveTopology::LineList, "LineList" },
			{ PrimitiveTopology::LineStrip, "LineStrip" }, { PrimitiveTopology::TriangleList, "TriangleList" },
			{ PrimitiveTopology::TriangleStrip, "TriangleStrip" }, { PrimitiveTopology::PatchList, "PatchList" },
		};

		const std::pair<VertexFormat, const char*> kVertexFormats[] = {
			{ VertexFormat::Float, "Float" }, { VertexFormat::Float2, "Float2" },
			{ VertexFormat::Float3, "Float3" }, { VertexFormat::Float4, "Float4" },
		};

		const std::pair<CullMode, const char*> kCullModes[] = {
			{ CullMode::None, "None" }, { CullMode::Front, "Front" }, { CullMode::Back, "Back" },
		};

		const std::pair<CompareOp, const char*> kCompareOps[] = {
			{ CompareOp::Never, "Never" }, { CompareOp::Less, "Less" }, { CompareOp::Equal, "Equal" },
			{ CompareOp::LessEqual, "LessEqual" }, { CompareOp::Greater, "Greater" },
			{ CompareOp::NotEqual, "NotEqual" }, { CompareOp::GreaterEqual, "GreaterEqual" },
			{ CompareOp::Always, "Always" },
		};

		const std::pair<rhi::BlendFactor, const char*> kBlendFactors[] = {
			{ rhi::BlendFactor::Zero, "Zero" }, { rhi::BlendFactor::One, "One" },
			{ rhi::BlendFactor::SrcColor, "SrcColor" }, { rhi::BlendFactor::OneMinusSrcColor, "OneMinusSrcColor" },
			{ rhi::BlendFactor::DstColor, "DstColor" }, { rhi::BlendFactor::OneMinusDstColor, "OneMinusDstColor" },
			{ rhi::BlendFactor::SrcAlpha, "SrcAlpha" }, { rhi::BlendFactor::OneMinusSrcAlpha, "OneMinusSrcAlpha" },
			{ rhi::BlendFactor::DstAlpha, "DstAlpha" }, { rhi::BlendFactor::OneMinusDstAlpha, "OneMinusDstAlpha" },
			{ rhi::BlendFactor::ConstantColor, "ConstantColor" },
			{ rhi::BlendFactor::OneMinusConstantColor, "OneMinusConstantColor" },
			{ rhi::BlendFactor::ConstantAlpha, "ConstantAlpha" },
			{ rhi::BlendFactor::OneMinusConstantAlpha, "OneMinusConstantAlpha" },
			{ rhi::BlendFactor::SrcAlphaSaturate, "SrcAlphaSaturate" },
		};

		const std::pair<rhi::BlendOp, const char*> kBlendOps[] = {
			{ rhi::BlendOp::Add, "Add" }, { rhi::BlendOp::Subtract, "Subtract" },
			{ rhi::BlendOp::ReverseSubtract, "ReverseSubtract" },
			{ rhi::BlendOp::Min, "Min" }, { rhi::BlendOp::Max, "Max" },
		};

		const std::pair<rhi::StencilOp, const char*> kStencilOps[] = {
			{ rhi::StencilOp::Keep, "Keep" }, { rhi::StencilOp::Zero, "Zero" },
			{ rhi::StencilOp::Replace, "Replace" }, { rhi::StencilOp::IncrClamp, "IncrClamp" },
			{ rhi::StencilOp::DecrClamp, "DecrClamp" }, { rhi::StencilOp::Invert, "Invert" },
			{ rhi::StencilOp::IncrWrap, "IncrWrap" }, { rhi::StencilOp::DecrWrap, "DecrWrap" },
		};

		const std::pair<IndexType, const char*> kIndexTypes[] = {
			{ IndexType::UInt16, "UInt16" }, { IndexType::UInt32, "UInt32" },
		};

		const std::pair<rhi::Filter, const char*> kFilters[] = {
			{ rhi::Filter::Nearest, "Nearest" }, { rhi::Filter::Linear, "Linear" },
		};

		const std::pair<rhi::SamplerMipmapMode, const char*> kMipmapModes[] = {
			{ rhi::SamplerMipmapMode::Nearest, "Nearest" }, { rhi::SamplerMipmapMode::Linear, "Linear" },
		};

		const std::pair<rhi::SamplerAddressMode, const char*> kAddressModes[] = {
			{ rhi::SamplerAddressMode::Repeat, "Repeat" },
			{ rhi::SamplerAddressMode::MirroredRepeat, "MirroredRepeat" },
			{ rhi::SamplerAddressMode::ClampToEdge, "ClampToEdge" },
			{ rhi::SamplerAddressMode::ClampToBorder, "ClampToBorder" },
		};

		const std::pair<TransferKind, const char*> kTransferKinds[] = {
			{ TransferKind::BufferToImage, "BufferToImage" },
			{ TransferKind::ImageToBuffer, "ImageToBuffer" },
			{ TransferKind::BufferToBuffer, "BufferToBuffer" },
			{ TransferKind::ImageToImage, "ImageToImage" },
			{ TransferKind::ClearImage, "ClearImage" },
		};

		const std::pair<rhi::ColorWriteMask, const char*> kWriteChannels[] = {
			{ rhi::ColorWriteMask::R, "R" }, { rhi::ColorWriteMask::G, "G" },
			{ rhi::ColorWriteMask::B, "B" }, { rhi::ColorWriteMask::A, "A" },
		};

		// ================= 读取辅助 =================

		struct Reader {
			const Json&               node;
			std::string               path;
			std::vector<std::string>* errors = nullptr;

			void Error(std::string message) const {
				if (errors) errors->push_back(path + ": " + std::move(message));
			}

			void ErrorField(const char* key, const char* message) const {
				Error(std::string("\"") + key + "\" " + message);
			}

			const Json* Find(const char* key) const { return node.Find(key); }

			uint32_t U32(const char* key, uint32_t fallback) const {
				const Json* value = Find(key);
				if (value == nullptr) return fallback;
				if (value->kind != Json::Kind::Number || !value->isInteger) {
					ErrorField(key, "须为整数");
					return fallback;
				}
				return static_cast<uint32_t>(value->integer);
			}

			int32_t I32(const char* key, int32_t fallback) const {
				const Json* value = Find(key);
				if (value == nullptr) return fallback;
				if (value->kind != Json::Kind::Number || !value->isInteger) {
					ErrorField(key, "须为整数");
					return fallback;
				}
				return static_cast<int32_t>(value->integer);
			}

			uint64_t U64(const char* key, uint64_t fallback) const {
				const Json* value = Find(key);
				if (value == nullptr) return fallback;
				if (value->kind != Json::Kind::Number || !value->isInteger) {
					ErrorField(key, "须为整数");
					return fallback;
				}
				return static_cast<uint64_t>(value->integer);
			}

			float F32(const char* key, float fallback) const {
				const Json* value = Find(key);
				if (value == nullptr) return fallback;
				if (value->kind != Json::Kind::Number) {
					ErrorField(key, "须为数字");
					return fallback;
				}
				return value->isInteger ? static_cast<float>(value->integer)
					: static_cast<float>(value->number);
			}

			bool Bool(const char* key, bool fallback) const {
				const Json* value = Find(key);
				if (value == nullptr) return fallback;
				if (value->kind != Json::Kind::Bool) {
					ErrorField(key, "须为布尔值");
					return fallback;
				}
				return value->boolean;
			}

			std::string Str(const char* key, std::string fallback = {}) const {
				const Json* value = Find(key);
				if (value == nullptr) return fallback;
				if (value->kind != Json::Kind::String) {
					ErrorField(key, "须为字符串");
					return fallback;
				}
				return value->text;
			}

			const Json* Array(const char* key) const {
				const Json* value = Find(key);
				if (value == nullptr) return nullptr;
				if (value->kind != Json::Kind::Array) {
					ErrorField(key, "须为数组");
					return nullptr;
				}
				return value;
			}

			const Json* Object(const char* key) const {
				const Json* value = Find(key);
				if (value == nullptr) return nullptr;
				if (value->kind != Json::Kind::Object) {
					ErrorField(key, "须为对象");
					return nullptr;
				}
				return value;
			}

			template <typename E, size_t N>
			E EnumOf(const char* key, E fallback, const std::pair<E, const char*> (&table)[N]) const {
				const Json* value = Find(key);
				if (value == nullptr) return fallback;
				if (value->kind != Json::Kind::String) {
					ErrorField(key, "须为字符串");
					return fallback;
				}
				if (const auto parsed = ParseFrom<E>(value->text, table))
					return *parsed;
				ErrorField(key, (std::string("含未知取值 \"") + value->text + "\"").c_str());
				return fallback;
			}
		};

		Json NumberArray(const std::vector<uint8_t>& values) {
			Json array = Json::Arr();
			for (uint8_t value : values)
				array.Push(Json::Int(value));
			return array;
		}

		// ================= 基础结构编解码 =================

		Json EncodeSubresource(const ImageSubresource& view) {
			Json object = Json::Obj();
			object.Set("mip", Json::Int(view.mipLevel));
			object.Set("layer", Json::Int(view.layer));
			return object;
		}

		ImageSubresource DecodeSubresource(const Json* node, std::vector<std::string>* errors,
			const std::string& path) {
			ImageSubresource view;
			if (node == nullptr) return view;
			Reader reader{ *node, path, errors };
			view.mipLevel = reader.U32("mip", 0);
			view.layer = reader.U32("layer", 0);
			return view;
		}

		Json EncodeRange(const ImageSubresourceRange& range) {
			Json object = Json::Obj();
			object.Set("base", EncodeSubresource(range.base));
			object.Set("mipCount", Json::Int(range.mipLevelCount));
			object.Set("layerCount", Json::Int(range.layerCount));
			return object;
		}

		ImageSubresourceRange DecodeRange(const Json* node, std::vector<std::string>* errors,
			const std::string& path) {
			ImageSubresourceRange range{ {}, 1, 1 };
			if (node == nullptr) return range;
			Reader reader{ *node, path, errors };
			range.base = DecodeSubresource(reader.Find("base"), errors, path + ".base");
			range.mipLevelCount = reader.U32("mipCount", 1);
			range.layerCount = reader.U32("layerCount", 1);
			return range;
		}

		Json EncodeClearValue(const ClearValue& value) {
			Json object = Json::Obj();
			if (const auto* color = std::get_if<std::array<float, 4>>(&value.value)) {
				object.Set("kind", Json::Str("color"));
				Json rgba = Json::Arr();
				for (float channel : *color)
					rgba.Push(Json::Real(channel));
				object.Set("rgba", std::move(rgba));
			}
			else if (const auto* depth = std::get_if<DepthStencilValue>(&value.value)) {
				object.Set("kind", Json::Str("depthStencil"));
				object.Set("depth", Json::Real(depth->depth));
				object.Set("stencil", Json::Int(depth->stencil));
			}
			return object;
		}

		ClearValue DecodeClearValue(const Json* node, std::vector<std::string>* errors,
			const std::string& path) {
			ClearValue value {};
			if (node == nullptr) return value;
			Reader reader{ *node, path, errors };
			const std::string kind = reader.Str("kind", "color");
			if (kind == "depthStencil") {
				DepthStencilValue depth;
				depth.depth = reader.F32("depth", 1.0f);
				depth.stencil = reader.U32("stencil", 0);
				value.value = depth;
			}
			else {
				std::array<float, 4> rgba { 0.0f, 0.0f, 0.0f, 0.0f };
				if (const Json* array = reader.Array("rgba")) {
					for (size_t i = 0; i < array->items.size() && i < rgba.size(); ++i) {
						const Json& item = array->items[i];
						if (item.kind == Json::Kind::Number)
							rgba[i] = item.isInteger ? static_cast<float>(item.integer)
								: static_cast<float>(item.number);
					}
				}
				value.value = rgba;
			}
			return value;
		}

		Json EncodeSampler(const SamplerDesc& sampler) {
			Json object = Json::Obj();
			object.Set("magFilter", Json::Str(NameOf(sampler.magFilter, kFilters)));
			object.Set("minFilter", Json::Str(NameOf(sampler.minFilter, kFilters)));
			object.Set("mipmapMode", Json::Str(NameOf(sampler.mipmapMode, kMipmapModes)));
			object.Set("addressU", Json::Str(NameOf(sampler.addressU, kAddressModes)));
			object.Set("addressV", Json::Str(NameOf(sampler.addressV, kAddressModes)));
			object.Set("addressW", Json::Str(NameOf(sampler.addressW, kAddressModes)));
			object.Set("anisotropyEnable", Json::Bool(sampler.anisotropyEnable));
			object.Set("maxAnisotropy", Json::Real(sampler.maxAnisotropy));
			object.Set("mipLodBias", Json::Real(sampler.mipLodBias));
			object.Set("minLod", Json::Real(sampler.minLod));
			object.Set("maxLod", Json::Real(sampler.maxLod));
			return object;
		}

		SamplerDesc DecodeSampler(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			const SamplerDesc defaults {};
			Reader reader{ node, path, errors };
			SamplerDesc sampler;
			sampler.magFilter = reader.EnumOf("magFilter", defaults.magFilter, kFilters);
			sampler.minFilter = reader.EnumOf("minFilter", defaults.minFilter, kFilters);
			sampler.mipmapMode = reader.EnumOf("mipmapMode", defaults.mipmapMode, kMipmapModes);
			sampler.addressU = reader.EnumOf("addressU", defaults.addressU, kAddressModes);
			sampler.addressV = reader.EnumOf("addressV", defaults.addressV, kAddressModes);
			sampler.addressW = reader.EnumOf("addressW", defaults.addressW, kAddressModes);
			sampler.anisotropyEnable = reader.Bool("anisotropyEnable", defaults.anisotropyEnable);
			sampler.maxAnisotropy = reader.F32("maxAnisotropy", defaults.maxAnisotropy);
			sampler.mipLodBias = reader.F32("mipLodBias", defaults.mipLodBias);
			sampler.minLod = reader.F32("minLod", defaults.minLod);
			sampler.maxLod = reader.F32("maxLod", defaults.maxLod);
			return sampler;
		}

		// ================= 资源描述 =================

		Json EncodeImageDesc(const ImageDesc& desc) {
			Json object = Json::Obj();
			object.Set("kind", Json::Str("image"));
			object.Set("width", Json::Int(desc.width));
			object.Set("height", Json::Int(desc.height));
			object.Set("depth", Json::Int(desc.depth));
			object.Set("arrayLayers", Json::Int(desc.arrayLayers));
			object.Set("mipLevels", Json::Int(desc.mipLevels));
			object.Set("samples", Json::Int(desc.samples));
			object.Set("format", Json::Str(NameOf(desc.format, kImageFormats)));
			object.Set("viewType", Json::Str(NameOf(desc.viewType, kViewTypes)));
			object.Set("crossFrame", Json::Bool(desc.crossFrame));
			return object;
		}

		Json EncodeBufferDesc(const BufferDesc& desc) {
			Json object = Json::Obj();
			object.Set("kind", Json::Str("buffer"));
			object.Set("size", Json::Int(static_cast<int64_t>(desc.size)));
			Json usage = Json::Arr();
			for (const auto& entry : kBufferUsages) {
				if ((static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(entry.first)) != 0)
					usage.Push(Json::Str(entry.second));
			}
			object.Set("usage", std::move(usage));
			object.Set("hostVisible", Json::Bool(desc.hostVisible));
			object.Set("crossFrame", Json::Bool(desc.crossFrame));
			return object;
		}

		Json EncodeResourceDesc(const ResourceDesc& desc) {
			if (const auto* image = std::get_if<ImageDesc>(&desc))
				return EncodeImageDesc(*image);
			if (const auto* buffer = std::get_if<BufferDesc>(&desc))
				return EncodeBufferDesc(*buffer);
			Json object = Json::Obj();
			object.Set("kind", Json::Str("none"));
			return object;
		}

		ResourceDesc DecodeResourceDesc(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			const std::string kind = reader.Str("kind", "none");
			if (kind == "image") {
				const ImageDesc defaults {};
				ImageDesc desc;
				desc.width = reader.U32("width", defaults.width);
				desc.height = reader.U32("height", defaults.height);
				desc.depth = reader.U32("depth", defaults.depth);
				desc.arrayLayers = reader.U32("arrayLayers", defaults.arrayLayers);
				desc.mipLevels = reader.U32("mipLevels", defaults.mipLevels);
				desc.samples = reader.U32("samples", defaults.samples);
				desc.format = reader.EnumOf("format", defaults.format, kImageFormats);
				desc.viewType = reader.EnumOf("viewType", defaults.viewType, kViewTypes);
				desc.crossFrame = reader.Bool("crossFrame", defaults.crossFrame);
				return desc;
			}
			if (kind == "buffer") {
				const BufferDesc defaults {};
				BufferDesc desc;
				desc.size = reader.U64("size", defaults.size);
				if (const Json* usage = reader.Array("usage")) {
					uint32_t bits = 0;
					for (const Json& item : usage->items) {
						if (item.kind != Json::Kind::String) continue;
						const auto parsed = ParseFrom<BufferUsage>(item.text, kBufferUsages);
						if (parsed)
							bits |= static_cast<uint32_t>(*parsed);
						else
							reader.Error("usage 含未知取值 \"" + item.text + "\"");
					}
					desc.usage = static_cast<BufferUsage>(bits);
				}
				desc.hostVisible = reader.Bool("hostVisible", defaults.hostVisible);
				desc.crossFrame = reader.Bool("crossFrame", defaults.crossFrame);
				return desc;
			}
			return std::monostate{};
		}

		// ================= 管线与 Pass 描述 =================

		Json EncodeShaderStage(const rhi::ShaderStageDesc& stage) {
			Json object = Json::Obj();
			object.Set("stage", Json::Str(NameOf(stage.stage, kShaderStages)));
			object.Set("path", Json::Str(stage.spirvPath));
			object.Set("entry", Json::Str(stage.entryPoint));
			return object;
		}

		rhi::ShaderStageDesc DecodeShaderStage(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			rhi::ShaderStageDesc stage;
			stage.stage = reader.EnumOf("stage", rhi::ShaderStage::Vertex, kShaderStages);
			stage.spirvPath = reader.Str("path");
			stage.entryPoint = reader.Str("entry", "main");
			return stage;
		}

		Json EncodeColorState(const ColorAttachmentState& state) {
			Json object = Json::Obj();
			object.Set("enableBlend", Json::Bool(state.enableBlend));
			object.Set("srcColor", Json::Str(NameOf(state.srcColor, kBlendFactors)));
			object.Set("dstColor", Json::Str(NameOf(state.dstColor, kBlendFactors)));
			object.Set("srcAlpha", Json::Str(NameOf(state.srcAlpha, kBlendFactors)));
			object.Set("dstAlpha", Json::Str(NameOf(state.dstAlpha, kBlendFactors)));
			object.Set("colorOp", Json::Str(NameOf(state.colorOp, kBlendOps)));
			object.Set("alphaOp", Json::Str(NameOf(state.alphaOp, kBlendOps)));
			Json channels = Json::Arr();
			for (const auto& entry : kWriteChannels) {
				if ((static_cast<uint8_t>(state.writeMask) & static_cast<uint8_t>(entry.first)) != 0)
					channels.Push(Json::Str(entry.second));
			}
			object.Set("writeMask", std::move(channels));
			return object;
		}

		ColorAttachmentState DecodeColorState(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			const ColorAttachmentState defaults {};
			Reader reader{ node, path, errors };
			ColorAttachmentState state;
			state.enableBlend = reader.Bool("enableBlend", defaults.enableBlend);
			state.srcColor = reader.EnumOf("srcColor", defaults.srcColor, kBlendFactors);
			state.dstColor = reader.EnumOf("dstColor", defaults.dstColor, kBlendFactors);
			state.srcAlpha = reader.EnumOf("srcAlpha", defaults.srcAlpha, kBlendFactors);
			state.dstAlpha = reader.EnumOf("dstAlpha", defaults.dstAlpha, kBlendFactors);
			state.colorOp = reader.EnumOf("colorOp", defaults.colorOp, kBlendOps);
			state.alphaOp = reader.EnumOf("alphaOp", defaults.alphaOp, kBlendOps);
			if (const Json* channels = reader.Array("writeMask")) {
				uint8_t bits = 0;
				for (const Json& item : channels->items) {
					if (item.kind != Json::Kind::String) continue;
					const auto parsed = ParseFrom<rhi::ColorWriteMask>(item.text, kWriteChannels);
					if (parsed)
						bits |= static_cast<uint8_t>(*parsed);
					else
						reader.Error("writeMask 含未知取值 \"" + item.text + "\"");
				}
				state.writeMask = static_cast<rhi::ColorWriteMask>(bits);
			}
			return state;
		}

		Json EncodeStencilFace(const rhi::StencilFaceState& face) {
			Json object = Json::Obj();
			object.Set("compareOp", Json::Str(NameOf(face.compareOp, kCompareOps)));
			object.Set("failOp", Json::Str(NameOf(face.failOp, kStencilOps)));
			object.Set("passOp", Json::Str(NameOf(face.passOp, kStencilOps)));
			object.Set("depthFailOp", Json::Str(NameOf(face.depthFailOp, kStencilOps)));
			object.Set("reference", Json::Int(face.reference));
			object.Set("compareMask", Json::Int(face.compareMask));
			object.Set("writeMask", Json::Int(face.writeMask));
			return object;
		}

		rhi::StencilFaceState DecodeStencilFace(const Json* node, std::vector<std::string>* errors,
			const std::string& path) {
			const rhi::StencilFaceState defaults {};
			if (node == nullptr) return defaults;
			Reader reader{ *node, path, errors };
			rhi::StencilFaceState face;
			face.compareOp = reader.EnumOf("compareOp", defaults.compareOp, kCompareOps);
			face.failOp = reader.EnumOf("failOp", defaults.failOp, kStencilOps);
			face.passOp = reader.EnumOf("passOp", defaults.passOp, kStencilOps);
			face.depthFailOp = reader.EnumOf("depthFailOp", defaults.depthFailOp, kStencilOps);
			face.reference = reader.U32("reference", defaults.reference);
			face.compareMask = reader.U32("compareMask", defaults.compareMask);
			face.writeMask = reader.U32("writeMask", defaults.writeMask);
			return face;
		}

		Json EncodeDepthStencil(const DepthStencilState& state) {
			Json object = Json::Obj();
			object.Set("depthTestEnable", Json::Bool(state.depthTestEnable));
			object.Set("depthWriteEnable", Json::Bool(state.depthWriteEnable));
			object.Set("depthCompareOp", Json::Str(NameOf(state.depthCompareOp, kCompareOps)));
			object.Set("depthBiasEnable", Json::Bool(state.depthBiasEnable));
			object.Set("depthBiasConstant", Json::Real(state.depthBiasConstant));
			object.Set("depthBiasSlope", Json::Real(state.depthBiasSlope));
			object.Set("depthBiasClamp", Json::Real(state.depthBiasClamp));
			object.Set("depthBoundsTestEnable", Json::Bool(state.depthBoundsTestEnable));
			object.Set("minDepthBounds", Json::Real(state.minDepthBounds));
			object.Set("maxDepthBounds", Json::Real(state.maxDepthBounds));
			object.Set("stencilTestEnable", Json::Bool(state.stencilTestEnable));
			object.Set("front", EncodeStencilFace(state.front));
			object.Set("back", EncodeStencilFace(state.back));
			return object;
		}

		DepthStencilState DecodeDepthStencil(const Json* node, std::vector<std::string>* errors,
			const std::string& path) {
			const DepthStencilState defaults {};
			if (node == nullptr) return defaults;
			Reader reader{ *node, path, errors };
			DepthStencilState state;
			state.depthTestEnable = reader.Bool("depthTestEnable", defaults.depthTestEnable);
			state.depthWriteEnable = reader.Bool("depthWriteEnable", defaults.depthWriteEnable);
			state.depthCompareOp = reader.EnumOf("depthCompareOp", defaults.depthCompareOp, kCompareOps);
			state.depthBiasEnable = reader.Bool("depthBiasEnable", defaults.depthBiasEnable);
			state.depthBiasConstant = reader.F32("depthBiasConstant", defaults.depthBiasConstant);
			state.depthBiasSlope = reader.F32("depthBiasSlope", defaults.depthBiasSlope);
			state.depthBiasClamp = reader.F32("depthBiasClamp", defaults.depthBiasClamp);
			state.depthBoundsTestEnable = reader.Bool("depthBoundsTestEnable", defaults.depthBoundsTestEnable);
			state.minDepthBounds = reader.F32("minDepthBounds", defaults.minDepthBounds);
			state.maxDepthBounds = reader.F32("maxDepthBounds", defaults.maxDepthBounds);
			state.stencilTestEnable = reader.Bool("stencilTestEnable", defaults.stencilTestEnable);
			state.front = DecodeStencilFace(reader.Find("front"), errors, path + ".front");
			state.back = DecodeStencilFace(reader.Find("back"), errors, path + ".back");
			return state;
		}

		Json EncodePipeline(const PipelineDesc& pipeline) {
			Json object = Json::Obj();
			Json refs = Json::Arr();
			for (const auto& ref : pipeline.shaderRefs)
				refs.Push(Json::Str(ref));
			object.Set("shaderRefs", std::move(refs));
			if (!pipeline.shaders.empty()) {
				Json stages = Json::Arr();
				for (const auto& stage : pipeline.shaders)
					stages.Push(EncodeShaderStage(stage));
				object.Set("shaders", std::move(stages));
			}
			object.Set("topology", Json::Str(NameOf(pipeline.topology, kTopologies)));
			object.Set("tessPatchControlPoints", Json::Int(pipeline.tessPatchControlPoints));
			Json bindings = Json::Arr();
			for (const auto& binding : pipeline.vertexBindings) {
				Json item = Json::Obj();
				item.Set("binding", Json::Int(binding.binding));
				item.Set("stride", Json::Int(binding.stride));
				item.Set("perInstance", Json::Bool(binding.perInstance));
				bindings.Push(std::move(item));
			}
			object.Set("vertexBindings", std::move(bindings));
			Json attributes = Json::Arr();
			for (const auto& attribute : pipeline.vertexAttributes) {
				Json item = Json::Obj();
				item.Set("location", Json::Int(attribute.location));
				item.Set("binding", Json::Int(attribute.binding));
				item.Set("format", Json::Str(NameOf(attribute.format, kVertexFormats)));
				item.Set("offset", Json::Int(attribute.offset));
				attributes.Push(std::move(item));
			}
			object.Set("vertexAttributes", std::move(attributes));
			object.Set("cullMode", Json::Str(NameOf(pipeline.cullMode, kCullModes)));
			Json states = Json::Arr();
			for (const auto& state : pipeline.colorStates)
				states.Push(EncodeColorState(state));
			object.Set("colorStates", std::move(states));
			object.Set("depthStencil", EncodeDepthStencil(pipeline.depthStencil));
			Json blend = Json::Arr();
			for (float channel : pipeline.blendConstants)
				blend.Push(Json::Real(channel));
			object.Set("blendConstants", std::move(blend));
			object.Set("pushConstantSize", Json::Int(pipeline.pushConstantSize));
			return object;
		}

		PipelineDesc DecodePipeline(const Json* node, std::vector<std::string>* errors,
			const std::string& path) {
			const PipelineDesc defaults {};
			PipelineDesc pipeline;
			if (node == nullptr) return pipeline;
			Reader reader{ *node, path, errors };
			if (const Json* refs = reader.Array("shaderRefs")) {
				for (const Json& item : refs->items) {
					if (item.kind != Json::Kind::String) continue;
					pipeline.shaderRefs.push_back(item.text);
				}
			}
			if (const Json* stages = reader.Array("shaders")) {
				for (size_t i = 0; i < stages->items.size(); ++i) {
					if (stages->items[i].kind != Json::Kind::Object) continue;
					pipeline.shaders.push_back(DecodeShaderStage(stages->items[i], errors,
						path + ".shaders[" + std::to_string(i) + "]"));
				}
			}
			pipeline.topology = reader.EnumOf("topology", defaults.topology, kTopologies);
			pipeline.tessPatchControlPoints = reader.U32("tessPatchControlPoints",
				defaults.tessPatchControlPoints);
			if (const Json* bindings = reader.Array("vertexBindings")) {
				for (size_t i = 0; i < bindings->items.size(); ++i) {
					if (bindings->items[i].kind != Json::Kind::Object) continue;
					Reader item{ bindings->items[i], path + ".vertexBindings[" + std::to_string(i) + "]", errors };
					VertexInputBinding binding;
					binding.binding = item.U32("binding", 0);
					binding.stride = item.U32("stride", 0);
					binding.perInstance = item.Bool("perInstance", false);
					pipeline.vertexBindings.push_back(binding);
				}
			}
			if (const Json* attributes = reader.Array("vertexAttributes")) {
				for (size_t i = 0; i < attributes->items.size(); ++i) {
					if (attributes->items[i].kind != Json::Kind::Object) continue;
					Reader item{ attributes->items[i], path + ".vertexAttributes[" + std::to_string(i) + "]", errors };
					VertexAttribute attribute;
					attribute.location = item.U32("location", 0);
					attribute.binding = item.U32("binding", 0);
					attribute.format = item.EnumOf("format", VertexFormat::Float3, kVertexFormats);
					attribute.offset = item.U32("offset", 0);
					pipeline.vertexAttributes.push_back(attribute);
				}
			}
			pipeline.cullMode = reader.EnumOf("cullMode", defaults.cullMode, kCullModes);
			if (const Json* states = reader.Array("colorStates")) {
				for (size_t i = 0; i < states->items.size(); ++i) {
					if (states->items[i].kind != Json::Kind::Object) continue;
					pipeline.colorStates.push_back(DecodeColorState(states->items[i], errors,
						path + ".colorStates[" + std::to_string(i) + "]"));
				}
			}
			pipeline.depthStencil = DecodeDepthStencil(reader.Find("depthStencil"), errors,
				path + ".depthStencil");
			if (const Json* blend = reader.Array("blendConstants")) {
				for (size_t i = 0; i < blend->items.size() && i < pipeline.blendConstants.size(); ++i) {
					const Json& item = blend->items[i];
					if (item.kind != Json::Kind::Number) continue;
					pipeline.blendConstants[i] = item.isInteger
						? static_cast<float>(item.integer) : static_cast<float>(item.number);
				}
			}
			pipeline.pushConstantSize = reader.U32("pushConstantSize", defaults.pushConstantSize);
			return pipeline;
		}

		Json EncodeAttachment(const AttachmentBinding& attachment) {
			Json object = Json::Obj();
			object.Set("resource", Json::Str(attachment.resourceName));
			object.Set("usage", Json::Str(NameOf(attachment.usage, kUsages)));
			object.Set("slot", Json::Int(attachment.slot));
			object.Set("load", Json::Str(NameOf(attachment.load, kLoadOps)));
			object.Set("store", Json::Str(NameOf(attachment.store, kStoreOps)));
			object.Set("clear", EncodeClearValue(attachment.clear));
			if (attachment.range.has_value())
				object.Set("range", EncodeRange(*attachment.range));
			return object;
		}

		AttachmentBinding DecodeAttachment(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			const AttachmentBinding defaults {};
			Reader reader{ node, path, errors };
			AttachmentBinding attachment;
			attachment.resourceName = reader.Str("resource");
			attachment.usage = reader.EnumOf("usage", defaults.usage, kUsages);
			attachment.slot = reader.U32("slot", defaults.slot);
			attachment.load = reader.EnumOf("load", defaults.load, kLoadOps);
			attachment.store = reader.EnumOf("store", defaults.store, kStoreOps);
			attachment.clear = DecodeClearValue(reader.Find("clear"), errors, path + ".clear");
			if (const Json* range = reader.Find("range"))
				attachment.range = DecodeRange(range, errors, path + ".range");
			return attachment;
		}

		Json EncodeShaderBinding(const ShaderBinding& binding) {
			Json object = Json::Obj();
			object.Set("name", Json::Str(binding.resourceName));
			if (!binding.graphResourceName.empty())
				object.Set("graphResource", Json::Str(binding.graphResourceName));
			object.Set("usage", Json::Str(NameOf(binding.usage, kUsages)));
			object.Set("offset", Json::Int(static_cast<int64_t>(binding.offset)));
			object.Set("range", Json::Int(static_cast<int64_t>(binding.range)));
			if (binding.sampler.has_value())
				object.Set("sampler", EncodeSampler(*binding.sampler));
			return object;
		}

		ShaderBinding DecodeShaderBinding(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			const ShaderBinding defaults {};
			Reader reader{ node, path, errors };
			ShaderBinding binding;
			binding.resourceName = reader.Str("name");
			binding.graphResourceName = reader.Str("graphResource");
			binding.usage = reader.EnumOf("usage", defaults.usage, kUsages);
			binding.offset = reader.U64("offset", defaults.offset);
			binding.range = reader.U64("range", defaults.range);
			if (const Json* sampler = reader.Object("sampler"))
				binding.sampler = DecodeSampler(*sampler, errors, path + ".sampler");
			return binding;
		}

		Json EncodeVertexStream(const VertexStreamBinding& stream) {
			Json object = Json::Obj();
			object.Set("resource", Json::Str(stream.resourceName));
			object.Set("offset", Json::Int(static_cast<int64_t>(stream.offset)));
			return object;
		}

		Json EncodeIndexStream(const IndexStreamBinding& stream) {
			Json object = Json::Obj();
			object.Set("resource", Json::Str(stream.resourceName));
			object.Set("offset", Json::Int(static_cast<int64_t>(stream.offset)));
			object.Set("type", Json::Str(NameOf(stream.type, kIndexTypes)));
			return object;
		}

		Json EncodeViewport(const ViewportDesc& viewport) {
			Json object = Json::Obj();
			object.Set("autoFromColor", Json::Bool(viewport.autoFromColor));
			object.Set("x", Json::Real(viewport.x));
			object.Set("y", Json::Real(viewport.y));
			object.Set("w", Json::Real(viewport.w));
			object.Set("h", Json::Real(viewport.h));
			object.Set("minDepth", Json::Real(viewport.minDepth));
			object.Set("maxDepth", Json::Real(viewport.maxDepth));
			return object;
		}

		ViewportDesc DecodeViewport(const Json* node, std::vector<std::string>* errors,
			const std::string& path) {
			const ViewportDesc defaults {};
			ViewportDesc viewport;
			if (node == nullptr) return viewport;
			Reader reader{ *node, path, errors };
			viewport.autoFromColor = reader.Bool("autoFromColor", defaults.autoFromColor);
			viewport.x = reader.F32("x", defaults.x);
			viewport.y = reader.F32("y", defaults.y);
			viewport.w = reader.F32("w", defaults.w);
			viewport.h = reader.F32("h", defaults.h);
			viewport.minDepth = reader.F32("minDepth", defaults.minDepth);
			viewport.maxDepth = reader.F32("maxDepth", defaults.maxDepth);
			return viewport;
		}

		Json EncodeBufferImageRegion(const rhi::BufferImageCopyRegion& region) {
			Json object = Json::Obj();
			object.Set("bufferOffset", Json::Int(static_cast<int64_t>(region.bufferOffset)));
			object.Set("bufferRowLength", Json::Int(region.bufferRowLength));
			object.Set("bufferImageHeight", Json::Int(region.bufferImageHeight));
			object.Set("subresource", EncodeRange(region.subresource));
			object.Set("imageOffsetX", Json::Int(region.imageOffsetX));
			object.Set("imageOffsetY", Json::Int(region.imageOffsetY));
			object.Set("imageOffsetZ", Json::Int(region.imageOffsetZ));
			object.Set("imageExtentWidth", Json::Int(region.imageExtentWidth));
			object.Set("imageExtentHeight", Json::Int(region.imageExtentHeight));
			object.Set("imageExtentDepth", Json::Int(region.imageExtentDepth));
			return object;
		}

		Json EncodeImageCopyRegion(const rhi::ImageCopyRegion& region) {
			Json object = Json::Obj();
			object.Set("srcSubresource", EncodeRange(region.srcSubresource));
			object.Set("dstSubresource", EncodeRange(region.dstSubresource));
			object.Set("srcOffsetX", Json::Int(region.srcOffsetX));
			object.Set("srcOffsetY", Json::Int(region.srcOffsetY));
			object.Set("srcOffsetZ", Json::Int(region.srcOffsetZ));
			object.Set("dstOffsetX", Json::Int(region.dstOffsetX));
			object.Set("dstOffsetY", Json::Int(region.dstOffsetY));
			object.Set("dstOffsetZ", Json::Int(region.dstOffsetZ));
			object.Set("extentWidth", Json::Int(region.extentWidth));
			object.Set("extentHeight", Json::Int(region.extentHeight));
			object.Set("extentDepth", Json::Int(region.extentDepth));
			return object;
		}

		Json EncodeBlitRegion(const rhi::ImageBlitRegion& region) {
			Json object = Json::Obj();
			object.Set("srcSubresource", EncodeRange(region.srcSubresource));
			object.Set("dstSubresource", EncodeRange(region.dstSubresource));
			object.Set("srcOffsetX0", Json::Int(region.srcOffsetX0));
			object.Set("srcOffsetY0", Json::Int(region.srcOffsetY0));
			object.Set("srcOffsetZ0", Json::Int(region.srcOffsetZ0));
			object.Set("srcOffsetX1", Json::Int(region.srcOffsetX1));
			object.Set("srcOffsetY1", Json::Int(region.srcOffsetY1));
			object.Set("srcOffsetZ1", Json::Int(region.srcOffsetZ1));
			object.Set("dstOffsetX0", Json::Int(region.dstOffsetX0));
			object.Set("dstOffsetY0", Json::Int(region.dstOffsetY0));
			object.Set("dstOffsetZ0", Json::Int(region.dstOffsetZ0));
			object.Set("dstOffsetX1", Json::Int(region.dstOffsetX1));
			object.Set("dstOffsetY1", Json::Int(region.dstOffsetY1));
			object.Set("dstOffsetZ1", Json::Int(region.dstOffsetZ1));
			return object;
		}

		rhi::BufferImageCopyRegion DecodeBufferImageRegion(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			rhi::BufferImageCopyRegion region;
			region.bufferOffset = reader.U64("bufferOffset", 0);
			region.bufferRowLength = reader.U32("bufferRowLength", 0);
			region.bufferImageHeight = reader.U32("bufferImageHeight", 0);
			region.subresource = DecodeRange(reader.Find("subresource"), errors, path + ".subresource");
			region.imageOffsetX = reader.I32("imageOffsetX", 0);
			region.imageOffsetY = reader.I32("imageOffsetY", 0);
			region.imageOffsetZ = reader.I32("imageOffsetZ", 0);
			region.imageExtentWidth = reader.U32("imageExtentWidth", 0);
			region.imageExtentHeight = reader.U32("imageExtentHeight", 0);
			region.imageExtentDepth = reader.U32("imageExtentDepth", 0);
			return region;
		}

		rhi::ImageCopyRegion DecodeImageCopyRegion(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			rhi::ImageCopyRegion region;
			region.srcSubresource = DecodeRange(reader.Find("srcSubresource"), errors, path + ".srcSubresource");
			region.dstSubresource = DecodeRange(reader.Find("dstSubresource"), errors, path + ".dstSubresource");
			region.srcOffsetX = reader.I32("srcOffsetX", 0);
			region.srcOffsetY = reader.I32("srcOffsetY", 0);
			region.srcOffsetZ = reader.I32("srcOffsetZ", 0);
			region.dstOffsetX = reader.I32("dstOffsetX", 0);
			region.dstOffsetY = reader.I32("dstOffsetY", 0);
			region.dstOffsetZ = reader.I32("dstOffsetZ", 0);
			region.extentWidth = reader.U32("extentWidth", 0);
			region.extentHeight = reader.U32("extentHeight", 0);
			region.extentDepth = reader.U32("extentDepth", 0);
			return region;
		}

		rhi::ImageBlitRegion DecodeBlitRegion(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			rhi::ImageBlitRegion region;
			region.srcSubresource = DecodeRange(reader.Find("srcSubresource"), errors, path + ".srcSubresource");
			region.dstSubresource = DecodeRange(reader.Find("dstSubresource"), errors, path + ".dstSubresource");
			region.srcOffsetX0 = reader.I32("srcOffsetX0", 0);
			region.srcOffsetY0 = reader.I32("srcOffsetY0", 0);
			region.srcOffsetZ0 = reader.I32("srcOffsetZ0", 0);
			region.srcOffsetX1 = reader.I32("srcOffsetX1", 0);
			region.srcOffsetY1 = reader.I32("srcOffsetY1", 0);
			region.srcOffsetZ1 = reader.I32("srcOffsetZ1", 0);
			region.dstOffsetX0 = reader.I32("dstOffsetX0", 0);
			region.dstOffsetY0 = reader.I32("dstOffsetY0", 0);
			region.dstOffsetZ0 = reader.I32("dstOffsetZ0", 0);
			region.dstOffsetX1 = reader.I32("dstOffsetX1", 0);
			region.dstOffsetY1 = reader.I32("dstOffsetY1", 0);
			region.dstOffsetZ1 = reader.I32("dstOffsetZ1", 0);
			return region;
		}

		Json EncodeRenderPassDesc(const RenderPassDesc& desc) {
			Json object = Json::Obj();
			Json attachments = Json::Arr();
			for (const auto& attachment : desc.attachments)
				attachments.Push(EncodeAttachment(attachment));
			object.Set("attachments", std::move(attachments));
			Json bindings = Json::Arr();
			for (const auto& binding : desc.shaderBindings)
				bindings.Push(EncodeShaderBinding(binding));
			object.Set("shaderBindings", std::move(bindings));
			Json streams = Json::Arr();
			for (const auto& stream : desc.vertexStreams)
				streams.Push(EncodeVertexStream(stream));
			object.Set("vertexStreams", std::move(streams));
			if (desc.indexStream.has_value())
				object.Set("indexStream", EncodeIndexStream(*desc.indexStream));
			object.Set("viewport", EncodeViewport(desc.viewport));
			object.Set("pipeline", EncodePipeline(desc.pipeline));
			if (!desc.pushConstantData.empty())
				object.Set("pushConstantData", NumberArray(desc.pushConstantData));
			object.Set("bindStaticHeap", Json::Bool(desc.bindStaticHeap));
			return object;
		}

		RenderPassDesc DecodeRenderPassDesc(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			RenderPassDesc desc;
			if (const Json* attachments = reader.Array("attachments")) {
				for (size_t i = 0; i < attachments->items.size(); ++i) {
					if (attachments->items[i].kind != Json::Kind::Object) continue;
					desc.attachments.push_back(DecodeAttachment(attachments->items[i], errors,
						path + ".attachments[" + std::to_string(i) + "]"));
				}
			}
			if (const Json* bindings = reader.Array("shaderBindings")) {
				for (size_t i = 0; i < bindings->items.size(); ++i) {
					if (bindings->items[i].kind != Json::Kind::Object) continue;
					desc.shaderBindings.push_back(DecodeShaderBinding(bindings->items[i], errors,
						path + ".shaderBindings[" + std::to_string(i) + "]"));
				}
			}
			if (const Json* streams = reader.Array("vertexStreams")) {
				for (size_t i = 0; i < streams->items.size(); ++i) {
					if (streams->items[i].kind != Json::Kind::Object) continue;
					Reader item{ streams->items[i], path + ".vertexStreams[" + std::to_string(i) + "]", errors };
					VertexStreamBinding stream;
					stream.resourceName = item.Str("resource");
					stream.offset = item.U64("offset", 0);
					desc.vertexStreams.push_back(std::move(stream));
				}
			}
			if (const Json* index = reader.Object("indexStream")) {
				Reader item{ *index, path + ".indexStream", errors };
				IndexStreamBinding stream;
				stream.resourceName = item.Str("resource");
				stream.offset = item.U64("offset", 0);
				stream.type = item.EnumOf("type", IndexType::UInt32, kIndexTypes);
				desc.indexStream = stream;
			}
			desc.viewport = DecodeViewport(reader.Find("viewport"), errors, path + ".viewport");
			desc.pipeline = DecodePipeline(reader.Find("pipeline"), errors, path + ".pipeline");
			if (const Json* push = reader.Array("pushConstantData")) {
				for (const Json& item : push->items) {
					if (item.kind == Json::Kind::Number)
						desc.pushConstantData.push_back(static_cast<uint8_t>(item.integer));
				}
			}
			desc.bindStaticHeap = reader.Bool("bindStaticHeap", false);
			return desc;
		}

		Json EncodeComputePassDesc(const ComputePassDesc& desc) {
			Json object = Json::Obj();
			Json bindings = Json::Arr();
			for (const auto& binding : desc.shaderBindings)
				bindings.Push(EncodeShaderBinding(binding));
			object.Set("shaderBindings", std::move(bindings));
			object.Set("shaderRef", Json::Str(desc.shaderRef));
			object.Set("shader", EncodeShaderStage(desc.shader));
			object.Set("pushConstantSize", Json::Int(desc.pushConstantSize));
			Json groups = Json::Arr();
			groups.Push(Json::Int(desc.groupCountX));
			groups.Push(Json::Int(desc.groupCountY));
			groups.Push(Json::Int(desc.groupCountZ));
			object.Set("groupCount", std::move(groups));
			if (!desc.indirectBuffer.empty())
				object.Set("indirectBuffer", Json::Str(desc.indirectBuffer));
			object.Set("indirectOffset", Json::Int(static_cast<int64_t>(desc.indirectOffset)));
			if (!desc.pushConstantData.empty())
				object.Set("pushConstantData", NumberArray(desc.pushConstantData));
			object.Set("bindStaticHeap", Json::Bool(desc.bindStaticHeap));
			return object;
		}

		ComputePassDesc DecodeComputePassDesc(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			ComputePassDesc desc;
			if (const Json* bindings = reader.Array("shaderBindings")) {
				for (size_t i = 0; i < bindings->items.size(); ++i) {
					if (bindings->items[i].kind != Json::Kind::Object) continue;
					desc.shaderBindings.push_back(DecodeShaderBinding(bindings->items[i], errors,
						path + ".shaderBindings[" + std::to_string(i) + "]"));
				}
			}
			desc.shaderRef = reader.Str("shaderRef");
			if (const Json* stage = reader.Object("shader"))
				desc.shader = DecodeShaderStage(*stage, errors, path + ".shader");
			desc.pushConstantSize = reader.U32("pushConstantSize", 0);
			if (const Json* groups = reader.Array("groupCount")) {
				desc.groupCountX = groups->items.size() > 0 ? static_cast<uint32_t>(groups->items[0].integer) : 1;
				desc.groupCountY = groups->items.size() > 1 ? static_cast<uint32_t>(groups->items[1].integer) : 1;
				desc.groupCountZ = groups->items.size() > 2 ? static_cast<uint32_t>(groups->items[2].integer) : 1;
			}
			desc.indirectBuffer = reader.Str("indirectBuffer");
			desc.indirectOffset = reader.U64("indirectOffset", 0);
			if (const Json* push = reader.Array("pushConstantData")) {
				for (const Json& item : push->items) {
					if (item.kind == Json::Kind::Number)
						desc.pushConstantData.push_back(static_cast<uint8_t>(item.integer));
				}
			}
			desc.bindStaticHeap = reader.Bool("bindStaticHeap", false);
			return desc;
		}

		Json EncodeTransferPassDesc(const TransferPassDesc& desc) {
			Json object = Json::Obj();
			object.Set("kind", Json::Str(NameOf(desc.kind, kTransferKinds)));
			object.Set("source", Json::Str(desc.sourceName));
			object.Set("dest", Json::Str(desc.destName));
			object.Set("srcOffset", Json::Int(static_cast<int64_t>(desc.srcOffset)));
			object.Set("dstOffset", Json::Int(static_cast<int64_t>(desc.dstOffset)));
			object.Set("byteSize", Json::Int(static_cast<int64_t>(desc.byteSize)));
			object.Set("blit", Json::Bool(desc.blit));
			object.Set("linearFilter", Json::Bool(desc.linearFilter));
			if (!desc.bufferImageRegions.empty()) {
				Json regions = Json::Arr();
				for (const auto& region : desc.bufferImageRegions)
					regions.Push(EncodeBufferImageRegion(region));
				object.Set("bufferImageRegions", std::move(regions));
			}
			if (!desc.imageRegions.empty()) {
				Json regions = Json::Arr();
				for (const auto& region : desc.imageRegions)
					regions.Push(EncodeImageCopyRegion(region));
				object.Set("imageRegions", std::move(regions));
			}
			if (!desc.blitRegions.empty()) {
				Json regions = Json::Arr();
				for (const auto& region : desc.blitRegions)
					regions.Push(EncodeBlitRegion(region));
				object.Set("blitRegions", std::move(regions));
			}
			object.Set("clearRange", EncodeRange(desc.clearRange));
			object.Set("clearValue", EncodeClearValue(desc.clearValue));
			return object;
		}

		TransferPassDesc DecodeTransferPassDesc(const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			Reader reader{ node, path, errors };
			TransferPassDesc desc;
			desc.kind = reader.EnumOf("kind", TransferKind::BufferToImage, kTransferKinds);
			desc.sourceName = reader.Str("source");
			desc.destName = reader.Str("dest");
			desc.srcOffset = reader.U64("srcOffset", 0);
			desc.dstOffset = reader.U64("dstOffset", 0);
			desc.byteSize = reader.U64("byteSize", 0);
			desc.blit = reader.Bool("blit", false);
			desc.linearFilter = reader.Bool("linearFilter", true);
			if (const Json* regions = reader.Array("bufferImageRegions")) {
				for (size_t i = 0; i < regions->items.size(); ++i)
					desc.bufferImageRegions.push_back(DecodeBufferImageRegion(regions->items[i], errors,
						path + ".bufferImageRegions[" + std::to_string(i) + "]"));
			}
			if (const Json* regions = reader.Array("imageRegions")) {
				for (size_t i = 0; i < regions->items.size(); ++i)
					desc.imageRegions.push_back(DecodeImageCopyRegion(regions->items[i], errors,
						path + ".imageRegions[" + std::to_string(i) + "]"));
			}
			if (const Json* regions = reader.Array("blitRegions")) {
				for (size_t i = 0; i < regions->items.size(); ++i)
					desc.blitRegions.push_back(DecodeBlitRegion(regions->items[i], errors,
						path + ".blitRegions[" + std::to_string(i) + "]"));
			}
			desc.clearRange = DecodeRange(reader.Find("clearRange"), errors, path + ".clearRange");
			desc.clearValue = DecodeClearValue(reader.Find("clearValue"), errors, path + ".clearValue");
			return desc;
		}

		Json EncodePassDesc(Pass::Type type, const PassDesc& desc) {
			switch (type) {
			case Pass::Render:
				if (const auto* render = std::get_if<RenderPassDesc>(&desc))
					return EncodeRenderPassDesc(*render);
				break;
			case Pass::Compute:
				if (const auto* compute = std::get_if<ComputePassDesc>(&desc))
					return EncodeComputePassDesc(*compute);
				break;
			case Pass::Host:
				if (const auto* host = std::get_if<HostPassDesc>(&desc)) {
					Json object = Json::Obj();
					object.Set("callbackKey", Json::Str(host->callbackKey));
					return object;
				}
				break;
			case Pass::Transfer:
				if (const auto* transfer = std::get_if<TransferPassDesc>(&desc))
					return EncodeTransferPassDesc(*transfer);
				break;
			case Pass::Mux:
				if (const auto* mux = std::get_if<MuxPassDesc>(&desc)) {
					Json object = Json::Obj();
					Json alternatives = Json::Arr();
					for (const auto& name : mux->alternatives)
						alternatives.Push(Json::Str(name));
					object.Set("alternatives", std::move(alternatives));
					return object;
				}
				break;
			case Pass::Present:
				return Json::Obj();
			default:
				break;
			}
			Json object = Json::Obj();
			object.Set("kind", Json::Str("none"));
			return object;
		}

		PassDesc DecodePassDesc(Pass::Type type, const Json& node, std::vector<std::string>* errors,
			const std::string& path) {
			switch (type) {
			case Pass::Render:
				return DecodeRenderPassDesc(node, errors, path);
			case Pass::Compute:
				return DecodeComputePassDesc(node, errors, path);
			case Pass::Host: {
				Reader reader{ node, path, errors };
				HostPassDesc host;
				host.callbackKey = reader.Str("callbackKey");
				return host;
			}
			case Pass::Transfer:
				return DecodeTransferPassDesc(node, errors, path);
			case Pass::Mux: {
				Reader reader{ node, path, errors };
				MuxPassDesc mux;
				if (const Json* alternatives = reader.Array("alternatives")) {
					for (const Json& item : alternatives->items) {
						if (item.kind == Json::Kind::String)
							mux.alternatives.push_back(item.text);
					}
				}
				return mux;
			}
			case Pass::Present:
				return PresentPassDesc{};
			default:
				return std::monostate{};
			}
		}
	}

	// ================= 文档本体 =================

	RenderGraphDocument RenderGraphDocument::Capture(const RenderGraphBuilder& builder) {
		RenderGraphDocument document;
		document.m_resources.reserve(builder.m_resources.size());
		for (const auto& decl : builder.m_resources) {
			ResourceRecord record;
			record.name = decl.name;
			record.type = decl.type;
			record.desc = decl.desc;
			record.view = decl.view;
			record.viewFormat = decl.viewFormat;
			record.barrierRange = decl.barrierRange;
			record.initialLayout = decl.initialLayout;
			record.crossFrame = decl.crossFrame;
			record.externalImage = decl.externalImage != nullptr;
			record.externalBuffer = decl.externalBuffer != nullptr;
			document.m_resources.push_back(std::move(record));
		}

		document.m_passes.reserve(builder.m_passes.size());
		for (const auto& decl : builder.m_passes) {
			PassRecord record;
			record.name = decl.name;
			record.type = decl.type;
			record.queue = decl.queue;
			record.reads = decl.reads;
			record.writes = decl.writes;
			record.desc = decl.desc;
			document.m_passes.push_back(std::move(record));
		}

		document.m_edges = builder.m_edges;
		document.m_aliasGroups = builder.m_aliasGroups;
		return document;
	}

	std::string RenderGraphDocument::ToJson() const {
		Json root = Json::Obj();
		root.Set("format", Json::Str("dEngine.rendergraph"));
		root.Set("version", Json::Int(1));

		Json resources = Json::Arr();
		for (const auto& record : m_resources) {
			Json item = Json::Obj();
			item.Set("name", Json::Str(record.name));
			item.Set("type", Json::Str(NameOf(record.type, kResourceTypes)));
			if (record.externalImage)
				item.Set("external", Json::Str("image"));
			else if (record.externalBuffer)
				item.Set("external", Json::Str("buffer"));
			item.Set("desc", EncodeResourceDesc(record.desc));
			item.Set("view", EncodeSubresource(record.view));
			item.Set("viewFormat", Json::Str(NameOf(record.viewFormat, kImageFormats)));
			item.Set("barrierRange", EncodeRange(record.barrierRange));
			item.Set("initialLayout", Json::Str(NameOf(record.initialLayout, kImageLayouts)));
			item.Set("crossFrame", Json::Bool(record.crossFrame));
			resources.Push(std::move(item));
		}
		root.Set("resources", std::move(resources));

		Json passes = Json::Arr();
		for (const auto& record : m_passes) {
			Json item = Json::Obj();
			item.Set("name", Json::Str(record.name));
			item.Set("type", Json::Str(NameOf(record.type, kPassTypes)));
			item.Set("queue", Json::Str(NameOf(record.queue, kQueueClasses)));
			Json reads = Json::Arr();
			for (const auto& name : record.reads)
				reads.Push(Json::Str(name));
			item.Set("reads", std::move(reads));
			Json writes = Json::Arr();
			for (const auto& name : record.writes)
				writes.Push(Json::Str(name));
			item.Set("writes", std::move(writes));
			item.Set("desc", EncodePassDesc(record.type, record.desc));
			passes.Push(std::move(item));
		}
		root.Set("passes", std::move(passes));

		Json edges = Json::Arr();
		for (const auto& edge : m_edges) {
			Json item = Json::Obj();
			item.Set("from", Json::Str(edge.from));
			item.Set("to", Json::Str(edge.to));
			edges.Push(std::move(item));
		}
		root.Set("edges", std::move(edges));

		Json aliases = Json::Arr();
		for (const auto& group : m_aliasGroups) {
			Json names = Json::Arr();
			for (const auto& name : group)
				names.Push(Json::Str(name));
			aliases.Push(std::move(names));
		}
		root.Set("aliases", std::move(aliases));

		std::string text;
		Dump(root, text, 0);
		text.push_back('\n');
		return text;
	}

	std::optional<RenderGraphDocument> RenderGraphDocument::FromJson(std::string_view text,
		std::string* error) {
		Json root;
		JsonParser parser(text);
		std::string parseError;
		if (!parser.Parse(root, parseError)) {
			if (error) *error = "JSON 解析失败: " + parseError;
			return std::nullopt;
		}
		if (root.kind != Json::Kind::Object) {
			if (error) *error = "文档根节点须为对象";
			return std::nullopt;
		}

		std::vector<std::string> errors;
		Reader reader{ root, "", &errors };
		const std::string format = reader.Str("format");
		if (format != "dEngine.rendergraph")
			errors.push_back("format 字段缺失或不匹配: " + format);
		const uint32_t version = reader.U32("version", 0);
		if (version != 1)
			errors.push_back("不支持的文档版本: " + std::to_string(version));

		RenderGraphDocument document;

		if (const Json* resources = reader.Array("resources")) {
			for (size_t i = 0; i < resources->items.size(); ++i) {
				const Json& node = resources->items[i];
				if (node.kind != Json::Kind::Object) continue;
				const std::string path = "resources[" + std::to_string(i) + "]";
				Reader item{ node, path, &errors };
				ResourceRecord record;
				record.name = item.Str("name");
				record.type = item.EnumOf("type", Resource::None, kResourceTypes);
				const std::string external = item.Str("external");
				record.externalImage = external == "image";
				record.externalBuffer = external == "buffer";
				if (const Json* desc = item.Object("desc"))
					record.desc = DecodeResourceDesc(*desc, &errors, path + ".desc");
				record.view = DecodeSubresource(item.Find("view"), &errors, path + ".view");
				record.viewFormat = item.EnumOf("viewFormat", ImageFormat::None, kImageFormats);
				record.barrierRange = DecodeRange(item.Find("barrierRange"), &errors, path + ".barrierRange");
				record.initialLayout = item.EnumOf("initialLayout", rhi::ImageLayout::Undefined, kImageLayouts);
				record.crossFrame = item.Bool("crossFrame", false);
				document.m_resources.push_back(std::move(record));
			}
		}

		if (const Json* passes = reader.Array("passes")) {
			for (size_t i = 0; i < passes->items.size(); ++i) {
				const Json& node = passes->items[i];
				if (node.kind != Json::Kind::Object) continue;
				const std::string path = "passes[" + std::to_string(i) + "]";
				Reader item{ node, path, &errors };
				PassRecord record;
				record.name = item.Str("name");
				record.type = item.EnumOf("type", Pass::None, kPassTypes);
				record.queue = item.EnumOf("queue", rhi::QueueClass::Graphics, kQueueClasses);
				if (const Json* reads = item.Array("reads")) {
					for (const Json& name : reads->items) {
						if (name.kind == Json::Kind::String)
							record.reads.insert(name.text);
					}
				}
				if (const Json* writes = item.Array("writes")) {
					for (const Json& name : writes->items) {
						if (name.kind == Json::Kind::String)
							record.writes.insert(name.text);
					}
				}
				if (const Json* desc = item.Object("desc"))
					record.desc = DecodePassDesc(record.type, *desc, &errors, path + ".desc");
				document.m_passes.push_back(std::move(record));
			}
		}

		if (const Json* edges = reader.Array("edges")) {
			for (size_t i = 0; i < edges->items.size(); ++i) {
				const Json& node = edges->items[i];
				if (node.kind != Json::Kind::Object) continue;
				Reader item{ node, "edges[" + std::to_string(i) + "]", &errors };
				BuilderEdge edge;
				edge.from = item.Str("from");
				edge.to = item.Str("to");
				document.m_edges.push_back(std::move(edge));
			}
		}

		if (const Json* aliases = reader.Array("aliases")) {
			for (const Json& group : aliases->items) {
				if (group.kind != Json::Kind::Array) continue;
				std::vector<std::string> names;
				for (const Json& name : group.items) {
					if (name.kind == Json::Kind::String)
						names.push_back(name.text);
				}
				document.m_aliasGroups.push_back(std::move(names));
			}
		}

		if (!errors.empty()) {
			if (error) {
				std::string message = "文档校验失败:";
				for (const auto& entry : errors)
					message += "\n  " + entry;
				*error = std::move(message);
			}
			return std::nullopt;
		}
		return document;
	}

	bool RenderGraphDocument::Save(const std::string& path, std::string* error) const {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file) {
			if (error) *error = "无法写入文档: " + path;
			return false;
		}
		const std::string text = ToJson();
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		if (!file) {
			if (error) *error = "写入文档失败: " + path;
			return false;
		}
		return true;
	}

	std::optional<RenderGraphDocument> RenderGraphDocument::Load(const std::string& path,
		std::string* error) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			if (error) *error = "无法读取文档: " + path;
			return std::nullopt;
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		return FromJson(buffer.str(), error);
	}

	bool RenderGraphDocument::Restore(RenderGraphBuilder& builder, const ExternalBindings& externals,
		std::vector<std::string>* errors) const {
		bool ok = true;
		auto Fail = [&](std::string message) {
			ok = false;
			if (errors) errors->push_back(std::move(message));
		};

		for (const auto& record : m_resources) {
			if (record.externalImage) {
				const rhi::Image* image = externals.image ? externals.image(record.name) : nullptr;
				if (image == nullptr) {
					Fail("外部图像未绑定: " + record.name);
					continue;
				}
				ResourceOptions options;
				options.view = record.view;
				options.viewFormat = record.viewFormat;
				options.barrierRange = record.barrierRange;
				options.initialLayout = record.initialLayout;
				options.crossFrame = record.crossFrame;
				builder.DeclareResource(record.name, image, options);
				continue;
			}
			if (record.externalBuffer) {
				const rhi::Buffer* buffer = externals.buffer ? externals.buffer(record.name) : nullptr;
				if (buffer == nullptr) {
					Fail("外部缓冲未绑定: " + record.name);
					continue;
				}
				builder.DeclareResource(record.name, buffer);
				continue;
			}
			ResourceOptions options;
			options.view = record.view;
			options.viewFormat = record.viewFormat;
			options.barrierRange = record.barrierRange;
			options.initialLayout = record.initialLayout;
			options.crossFrame = record.crossFrame;
			builder.DeclareResource(record.name, record.desc, options);
		}

		for (const auto& record : m_passes) {
			PassDesc desc = record.desc;
			if (auto* host = std::get_if<HostPassDesc>(&desc)) {
				if (!host->callbackKey.empty()) {
					const auto found = externals.hostCallbacks.find(host->callbackKey);
					if (found == externals.hostCallbacks.end())
						Fail("Host 回调未绑定: " + host->callbackKey);
					else
						host->callback = found->second;
				}
			}
			builder.DeclarePass(record.name, record.reads, record.writes, record.type,
				std::move(desc), record.queue);
		}

		if (!m_edges.empty()) {
			RenderGraphBuilder::Topology topology;
			topology.edges = m_edges;
			builder.DeclareTopology(std::move(topology));
		}
		for (const auto& group : m_aliasGroups)
			builder.AliasLogicalResources(group);

		return ok;
	}
}
