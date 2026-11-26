#include "ModulePack.h"

#include "AssetCatalog.h"
#include "ContentResolver.h"
#include "RenderGraphDocument.h"
#include "ShaderLibrary.h"

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace dEngine {

	namespace {

		void SetError(std::string* error, std::string message) {
			if (error) *error = std::move(message);
		}

		std::string EscapeJson(std::string_view text) {
			std::string out;
			out.reserve(text.size());
			for (char ch : text) {
				switch (ch) {
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:   out.push_back(ch); break;
				}
			}
			return out;
		}

		const char* FormatName(rhi::ImageFormat format) {
			switch (format) {
			case rhi::ImageFormat::R8:         return "R8";
			case rhi::ImageFormat::R8Srgb:     return "R8Srgb";
			case rhi::ImageFormat::RG8:        return "RG8";
			case rhi::ImageFormat::RG8Srgb:    return "RG8Srgb";
			case rhi::ImageFormat::RGBA8:      return "RGBA8";
			case rhi::ImageFormat::RGBA8Srgb:  return "RGBA8Srgb";
			case rhi::ImageFormat::BGRA8:      return "BGRA8";
			case rhi::ImageFormat::BGRA8Srgb:  return "BGRA8Srgb";
			case rhi::ImageFormat::R16F:       return "R16F";
			case rhi::ImageFormat::RG16F:      return "RG16F";
			case rhi::ImageFormat::RGBA16F:    return "RGBA16F";
			case rhi::ImageFormat::R32F:       return "R32F";
			case rhi::ImageFormat::RG32F:      return "RG32F";
			case rhi::ImageFormat::RGBA32F:    return "RGBA32F";
			case rhi::ImageFormat::R11G11B10F: return "R11G11B10F";
			case rhi::ImageFormat::D32F:       return "D32F";
			case rhi::ImageFormat::D24S8:      return "D24S8";
			case rhi::ImageFormat::S8:         return "S8";
			default:                           return "None";
			}
		}

		const char* LayoutName(rhi::ImageLayout layout) {
			switch (layout) {
			case rhi::ImageLayout::General:                return "General";
			case rhi::ImageLayout::ColorAttachment:        return "ColorAttachment";
			case rhi::ImageLayout::DepthStencilAttachment: return "DepthStencilAttachment";
			case rhi::ImageLayout::ShaderReadOnly:         return "ShaderReadOnly";
			case rhi::ImageLayout::TransferSrc:            return "TransferSrc";
			case rhi::ImageLayout::TransferDst:            return "TransferDst";
			case rhi::ImageLayout::PresentSrc:             return "PresentSrc";
			default:                                       return "Undefined";
			}
		}

		const char* ViewTypeName(rhi::ImageViewType type) {
			switch (type) {
			case rhi::ImageViewType::Dim1D:      return "Dim1D";
			case rhi::ImageViewType::Dim2D:      return "Dim2D";
			case rhi::ImageViewType::Dim3D:      return "Dim3D";
			case rhi::ImageViewType::Dim1DArray: return "Dim1DArray";
			case rhi::ImageViewType::Dim2DArray: return "Dim2DArray";
			case rhi::ImageViewType::Cube:       return "Cube";
			case rhi::ImageViewType::CubeArray:  return "CubeArray";
			default:                             return "None";
			}
		}

		std::string ImageUsageList(rhi::ImageUsage usage) {
			const uint32_t bits = static_cast<uint32_t>(usage);
			std::vector<const char*> names;
			if (bits & static_cast<uint32_t>(rhi::ImageUsage::Sampled)) names.push_back("Sampled");
			if (bits & static_cast<uint32_t>(rhi::ImageUsage::Storage)) names.push_back("Storage");
			if (bits & static_cast<uint32_t>(rhi::ImageUsage::ColorAttachment)) names.push_back("ColorAttachment");
			if (bits & static_cast<uint32_t>(rhi::ImageUsage::DepthStencil)) names.push_back("DepthStencil");
			if (bits & static_cast<uint32_t>(rhi::ImageUsage::TransferSrc)) names.push_back("TransferSrc");
			if (bits & static_cast<uint32_t>(rhi::ImageUsage::TransferDst)) names.push_back("TransferDst");

			std::string out = "[";
			for (size_t i = 0; i < names.size(); ++i) {
				if (i != 0) out += ", ";
				out += "\"";
				out += names[i];
				out += "\"";
			}
			out += "]";
			return out;
		}

		std::string BufferUsageList(rhi::BufferUsage usage) {
			const uint32_t bits = static_cast<uint32_t>(usage);
			std::vector<const char*> names;
			if (bits & static_cast<uint32_t>(rhi::BufferUsage::Vertex)) names.push_back("Vertex");
			if (bits & static_cast<uint32_t>(rhi::BufferUsage::Index)) names.push_back("Index");
			if (bits & static_cast<uint32_t>(rhi::BufferUsage::Uniform)) names.push_back("Uniform");
			if (bits & static_cast<uint32_t>(rhi::BufferUsage::Storage)) names.push_back("Storage");
			if (bits & static_cast<uint32_t>(rhi::BufferUsage::TransferSrc)) names.push_back("TransferSrc");
			if (bits & static_cast<uint32_t>(rhi::BufferUsage::TransferDst)) names.push_back("TransferDst");
			if (bits & static_cast<uint32_t>(rhi::BufferUsage::Indirect)) names.push_back("Indirect");

			std::string out = "[";
			for (size_t i = 0; i < names.size(); ++i) {
				if (i != 0) out += ", ";
				out += "\"";
				out += names[i];
				out += "\"";
			}
			out += "]";
			return out;
		}

		std::vector<uint8_t> ToBytes(std::string text) {
			return std::vector<uint8_t>(text.begin(), text.end());
		}

		bool AddFile(std::map<std::string, std::vector<uint8_t>>& files,
			const std::string& path, std::vector<uint8_t> bytes, std::string* error) {
			const auto it = files.find(path);
			if (it != files.end()) {
				if (it->second != bytes) {
					SetError(error, "依赖路径内容冲突: " + path);
					return false;
				}
				return true;
			}
			files.emplace(path, std::move(bytes));
			return true;
		}

		bool ReadDependency(const ContentResolver& resolver, const std::string& logical,
			std::string& path, std::vector<uint8_t>& bytes, std::string* error) {
			const auto parsed = ContentResolver::ParsePath(logical);
			if (!parsed) {
				SetError(error, "非法依赖路径: " + logical);
				return false;
			}
			std::string readError;
			auto data = resolver.ReadBinary(logical, &readError);
			if (!data) {
				SetError(error, readError);
				return false;
			}
			path = parsed->logical;
			bytes = std::move(*data);
			return true;
		}

		// 包内清单必须指向 SPIR-V；GLSL 源在打包时已经编译，只改写扩展名
		std::string PackedShaderPath(const std::string& logical) {
			const auto parsed = ContentResolver::ParsePath(logical);
			std::string path = parsed ? parsed->logical : logical;
			const bool hasSpvSuffix = path.size() >= 4 && path.compare(path.size() - 4, 4, ".spv") == 0;
			if (!hasSpvSuffix)
				path += ".spv";
			return path;
		}

		struct ShaderRec {
			std::string name;
			std::string stage;
			std::string path;
			std::string entry;
		};
		struct ImageRec {
			std::string key;
			std::string path;
			ImageAsset  asset;
		};
		struct BufferRec {
			std::string key;
			std::string path;
			BufferAsset asset;
		};

		std::string BuildShaderManifest(const std::vector<ShaderRec>& shaders) {
			std::ostringstream out;
			out << "{\n  \"version\": 1,\n  \"shaders\": {\n";
			for (size_t i = 0; i < shaders.size(); ++i) {
				const auto& s = shaders[i];
				out << "    \"" << EscapeJson(s.name) << "\": { "
					<< "\"stage\": \"" << EscapeJson(s.stage) << "\", "
					<< "\"path\": \"" << EscapeJson(s.path) << "\", "
					<< "\"entry\": \"" << EscapeJson(s.entry) << "\" }";
				out << (i + 1 < shaders.size() ? ",\n" : "\n");
			}
			out << "  }\n}\n";
			return out.str();
		}

		std::string BuildAssetManifest(const std::vector<ImageRec>& images,
			const std::vector<BufferRec>& buffers) {
			std::ostringstream out;
			out << "{\n  \"version\": 1";
			if (!images.empty()) {
				out << ",\n  \"images\": {\n";
				for (size_t i = 0; i < images.size(); ++i) {
					const auto& rec = images[i];
					const ImageAsset& a = rec.asset;
					out << "    \"" << EscapeJson(rec.key) << "\": {\n"
						<< "      \"path\": \"" << EscapeJson(rec.path) << "\",\n"
						<< "      \"format\": \"" << FormatName(a.format) << "\",\n"
						<< "      \"usage\": " << ImageUsageList(a.usage) << ",\n"
						<< "      \"initialLayout\": \"" << LayoutName(a.initialLayout) << "\",\n"
						<< "      \"width\": " << a.width << ",\n"
						<< "      \"height\": " << a.height << ",\n"
						<< "      \"depth\": " << a.depth << ",\n"
						<< "      \"arrayLayers\": " << a.arrayLayers << ",\n"
						<< "      \"mipLevels\": " << a.mipLevels << ",\n"
						<< "      \"samples\": " << a.samples << ",\n"
						<< "      \"viewType\": \"" << ViewTypeName(a.viewType) << "\",\n"
						<< "      \"srgb\": " << (a.srgb ? "true" : "false") << ",\n"
						<< "      \"flipY\": " << (a.flipY ? "true" : "false") << ",\n"
						<< "      \"generateMips\": " << (a.generateMips ? "true" : "false") << "\n"
						<< "    }" << (i + 1 < images.size() ? ",\n" : "\n");
				}
				out << "  }";
			}
			if (!buffers.empty()) {
				out << ",\n  \"buffers\": {\n";
				for (size_t i = 0; i < buffers.size(); ++i) {
					const auto& rec = buffers[i];
					const BufferAsset& a = rec.asset;
					out << "    \"" << EscapeJson(rec.key) << "\": {\n"
						<< "      \"path\": \"" << EscapeJson(rec.path) << "\",\n"
						<< "      \"usage\": " << BufferUsageList(a.usage) << ",\n"
						<< "      \"hostVisible\": " << (a.hostVisible ? "true" : "false") << ",\n"
						<< "      \"byteSize\": " << a.byteSize << "\n"
						<< "    }" << (i + 1 < buffers.size() ? ",\n" : "\n");
				}
				out << "  }";
			}
			out << "\n}\n";
			return out.str();
		}

		std::string BuildModuleIndex(const std::string& name, const std::string& path) {
			std::ostringstream out;
			out << "{\n  \"version\": 1,\n  \"modules\": {\n"
				<< "    \"" << EscapeJson(name) << "\": \"" << EscapeJson(path) << "\"\n"
				<< "  }\n}\n";
			return out.str();
		}
	}

	bool BuildModulePack(const RenderGraphModuleDocument& module,
		const ShaderLibrary& shaders,
		const AssetCatalog& assets,
		ModulePackResult* out,
		std::string* error) {
		if (out == nullptr) {
			SetError(error, "module pack result is null");
			return false;
		}
		if (module.Name().empty()) {
			SetError(error, "module name is empty");
			return false;
		}
		if (module.Name().find('/') != std::string::npos
			|| module.Name().find('\\') != std::string::npos) {
			SetError(error, "module name cannot contain path separators: " + module.Name());
			return false;
		}

		std::map<std::string, std::vector<uint8_t>> files;
		std::vector<ShaderRec> shaderRecs;
		for (const auto& ref : module.ShaderRefs()) {
			const ShaderEntry* entry = shaders.Find(ref);
			if (entry == nullptr) {
				SetError(error, "shader not registered: " + ref);
				return false;
			}
			std::string resolveError;
			const auto stage = shaders.Resolve(ref, &resolveError);
			if (!stage) {
				SetError(error, resolveError);
				return false;
			}
			std::string path;
			std::vector<uint8_t> bytes;
			if (!stage->spirv.empty()) {
				path = PackedShaderPath(entry->logicalPath);
				bytes.resize(stage->spirv.size() * sizeof(uint32_t));
				std::memcpy(bytes.data(), stage->spirv.data(), bytes.size());
			}
			else if (!ReadDependency(shaders.Resolver(), entry->logicalPath, path, bytes, error)) {
				return false;
			}
			if (!AddFile(files, path, std::move(bytes), error))
				return false;
			shaderRecs.push_back({ ref, ShaderLibrary::StageToString(stage->stage),
				path, stage->entryPoint });
		}

		std::vector<ImageRec> images;
		std::vector<BufferRec> buffers;
		for (const auto& key : module.AssetKeys()) {
			if (const ImageAsset* image = assets.FindImage(key)) {
				std::string path;
				std::vector<uint8_t> bytes;
				if (!ReadDependency(assets.Resolver(), image->logicalPath, path, bytes, error)
					|| !AddFile(files, path, std::move(bytes), error))
					return false;
				ImageRec rec;
				rec.key = key;
				rec.path = path;
				rec.asset = *image;
				rec.asset.logicalPath = path;
				images.push_back(std::move(rec));
				continue;
			}
			if (const BufferAsset* buffer = assets.FindBuffer(key)) {
				std::string path;
				std::vector<uint8_t> bytes;
				if (!ReadDependency(assets.Resolver(), buffer->logicalPath, path, bytes, error)
					|| !AddFile(files, path, std::move(bytes), error))
					return false;
				BufferRec rec;
				rec.key = key;
				rec.path = path;
				rec.asset = *buffer;
				rec.asset.logicalPath = path;
				buffers.push_back(std::move(rec));
				continue;
			}
			SetError(error, "asset key not registered: " + key);
			return false;
		}

		const std::string modulePath = "modules/" + module.Name() + ".json";
		out->moduleName = module.Name();
		out->shaderRefs = module.ShaderRefs();
		out->assetKeys = module.AssetKeys();
		out->inputs.clear();
		out->inputs.push_back({ modulePath, ToBytes(module.ToJson()) });
		out->inputs.push_back({ "modules/index.json",
			ToBytes(BuildModuleIndex(module.Name(), modulePath)) });
		if (!shaderRecs.empty())
			out->inputs.push_back({ "manifests/shaders.json",
				ToBytes(BuildShaderManifest(shaderRecs)) });
		if (!images.empty() || !buffers.empty())
			out->inputs.push_back({ "manifests/assets.json",
				ToBytes(BuildAssetManifest(images, buffers)) });
		for (auto& file : files)
			out->inputs.push_back({ file.first, std::move(file.second) });

		const auto bytes = BuildAssetPack(out->inputs, error);
		if (!bytes)
			return false;
		out->bytes = *bytes;
		return true;
	}

	bool BuildModulePackFile(const std::string& moduleJsonPath,
		const ShaderLibrary& shaders,
		const AssetCatalog& assets,
		const std::string& outputPackPath,
		ModulePackResult* out,
		std::string* error) {
		const auto module = RenderGraphModuleDocument::Load(moduleJsonPath, error);
		if (!module)
			return false;
		return BuildModulePackFile(*module, shaders, assets, outputPackPath, out, error);
	}

	bool BuildModulePackFile(const RenderGraphModuleDocument& module,
		const ShaderLibrary& shaders,
		const AssetCatalog& assets,
		const std::string& outputPackPath,
		ModulePackResult* out,
		std::string* error) {
		ModulePackResult result;
		if (!BuildModulePack(module, shaders, assets, &result, error))
			return false;

		std::ofstream file(outputPackPath, std::ios::binary | std::ios::trunc);
		if (!file) {
			SetError(error, "cannot write pack: " + outputPackPath);
			return false;
		}
		file.write(reinterpret_cast<const char*>(result.bytes.data()),
			static_cast<std::streamsize>(result.bytes.size()));
		if (!file) {
			SetError(error, "cannot write pack: " + outputPackPath);
			return false;
		}
		if (out)
			*out = std::move(result);
		return true;
	}
}
