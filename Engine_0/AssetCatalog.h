#pragma once
#include "ContentResolver.h"
#include "rhi/RHIDesc.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 资产清单（纯描述层）：逻辑键 -> 来源与创建参数。
// 不解码、不上传、不持有 GPU 资源；与 ShaderLibrary 同构，装载仍由宿主完成。
namespace dEngine {

	struct ImageAsset {
		std::string      name;        // 清单键（不是图资源名）
		std::string      logicalPath; // 相对 ContentResolver："assets/skyview.jpg"
		rhi::ImageFormat format = rhi::ImageFormat::RGBA8;
		rhi::ImageUsage  usage = rhi::ImageUsage::Sampled;
		rhi::ImageLayout initialLayout = rhi::ImageLayout::ShaderReadOnly;
		// 资源级描述：0 = 由解码结果决定（width/height），mipLevels 0 = 完整链
		uint32_t         width = 0;
		uint32_t         height = 0;
		uint32_t         depth = 1;
		uint32_t         arrayLayers = 1;
		uint32_t         mipLevels = 1;
		uint32_t         samples = 1;
		rhi::ImageViewType viewType = rhi::ImageViewType::Dim2D;
		bool             srgb = false;   // 解码时是否按 sRGB 处理（宿主提示）
		bool             flipY = false;  // 解码时是否翻转（宿主提示）
		bool             generateMips = false; // 由加载器补齐 mip 链
	};

	struct BufferAsset {
		std::string      name;
		std::string      logicalPath;
		rhi::BufferUsage usage = rhi::BufferUsage::None;
		bool             hostVisible = false;
		uint64_t         byteSize = 0;   // 0 = 整个文件
	};

	struct AssetRevision {
		uint64_t mtime = 0;
		uint64_t size = 0;
		uint64_t hash = 0; // 包索引提供；文件源为 0
	};

	class AssetCatalog {
	public:
		explicit AssetCatalog(const ContentResolver& resolver) : m_resolver(&resolver) {}

		// 读取一份清单（可多次调用聚合多份）；errors 收全部问题
		bool LoadManifest(std::string_view logicalPath, std::vector<std::string>* errors = nullptr);
		// 将当前登记表序列化为与 LoadManifest 兼容的 JSON；不访问文件系统，也不校验路径是否存在
		std::string ToManifestJson() const;
		// 将清单写入磁盘路径（父目录不存在时自动创建）；枚举值非法时返回 false
		bool SaveManifest(const std::filesystem::path& path, std::string* error = nullptr) const;
		bool RegisterImage(ImageAsset asset, std::string* error = nullptr);
		bool RegisterBuffer(BufferAsset asset, std::string* error = nullptr);

		const ImageAsset* FindImage(std::string_view name) const;
		const BufferAsset* FindBuffer(std::string_view name) const;

		// 变更检测：文件缺失返回 nullopt
		std::optional<AssetRevision> Revision(std::string_view name) const;

		std::vector<std::string> ImageNames() const;
		std::vector<std::string> BufferNames() const;

		// 存在性校验：逐条通过 ContentResolver 解析，缺失或非法路径写入 errors
		bool Validate(std::vector<std::string>* errors = nullptr) const;

		const ContentResolver& Resolver() const { return *m_resolver; }

	private:
		const ContentResolver* m_resolver = nullptr;
		std::map<std::string, ImageAsset>  m_images;
		std::map<std::string, BufferAsset> m_buffers;
	};
}
