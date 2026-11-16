#pragma once
#include "AssetCatalog.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dEngine::rhi {
	class Device;
	class Image;
	class Buffer;
}

// 资源装载层：资产键 -> GPU 句柄，按已解析的绝对路径缓存。
// 同一路径重复 Acquire 直接返回已加载句柄；描述不一致时拒绝复用。
namespace dEngine {

	class AssetLoader {
	public:
		AssetLoader(rhi::Device& device, const AssetCatalog& catalog);
		~AssetLoader();
		AssetLoader(const AssetLoader&) = delete;
		AssetLoader& operator=(const AssetLoader&) = delete;

		// 失败返回 nullptr，error 给出原因；成功后句柄由 loader 持有。
		const rhi::Image* AcquireImage(std::string_view assetKey, std::string* error = nullptr);
		const rhi::Buffer* AcquireBuffer(std::string_view assetKey, std::string* error = nullptr);

		// 只解析身份，不加载（缓存键："fs:<abs>" 或 "pack:<hash>:<logical>"）
		std::optional<std::string> ResolveIdentity(std::string_view assetKey) const;

		size_t ImageCount() const;
		size_t BufferCount() const;
		void Clear();

	private:
		struct ImageEntry {
			std::string             identity;
			rhi::ImageCreateInfo    info;
			bool                    flipY = false;
			std::unique_ptr<rhi::Image> image;
		};
		struct BufferEntry {
			std::string             identity;
			uint64_t                byteSize = 0;
			rhi::BufferUsage        usage = rhi::BufferUsage::None;
			bool                    hostVisible = false;
			std::unique_ptr<rhi::Buffer> buffer;
		};

		const ImageEntry* FindImage(const std::string& path, const ImageAsset& asset) const;
		const BufferEntry* FindBuffer(const std::string& path, const BufferAsset& asset) const;

		rhi::Device&         m_device;
		const AssetCatalog&  m_catalog;
		std::map<std::string, std::vector<ImageEntry>>  m_images;
		std::map<std::string, std::vector<BufferEntry>> m_buffers;
	};
}
