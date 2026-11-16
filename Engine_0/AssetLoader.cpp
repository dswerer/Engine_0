#include "AssetLoader.h"

#include "external/stb_image/stb_image.h"
#include "rhi/RHI.h"

#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace dEngine {

	namespace {
		void SetError(std::string* error, std::string message) {
			if (error) *error = std::move(message);
		}

		// 解码支持的格式 -> 每像素通道数（0 = 不支持）
		int ChannelsOf(rhi::ImageFormat format) {
			switch (format) {
			case rhi::ImageFormat::RGBA8:
			case rhi::ImageFormat::RGBA8Srgb:
			case rhi::ImageFormat::BGRA8:
			case rhi::ImageFormat::BGRA8Srgb: return 4;
			case rhi::ImageFormat::RG8:
			case rhi::ImageFormat::RG8Srgb:   return 2;
			case rhi::ImageFormat::R8:
			case rhi::ImageFormat::R8Srgb:    return 1;
			default:                          return 0;
			}
		}
	}

	AssetLoader::AssetLoader(rhi::Device& device, const AssetCatalog& catalog)
		: m_device(device), m_catalog(catalog) {}

	AssetLoader::~AssetLoader() = default;

	std::optional<std::string> AssetLoader::ResolveIdentity(std::string_view assetKey) const {
		const ImageAsset* image = m_catalog.FindImage(assetKey);
		const BufferAsset* buffer = image == nullptr ? m_catalog.FindBuffer(assetKey) : nullptr;
		const std::string logical = image != nullptr ? image->logicalPath
			: (buffer != nullptr ? buffer->logicalPath : std::string{});
		if (logical.empty())
			return std::nullopt;
		const auto resolved = m_catalog.Resolver().Resolve(logical, nullptr);
		if (!resolved)
			return std::nullopt;
		return resolved->identity;
	}

	const AssetLoader::ImageEntry* AssetLoader::FindImage(const std::string& identity,
		const ImageAsset& asset) const {
		const auto it = m_images.find(identity);
		if (it == m_images.end())
			return nullptr;
		const rhi::ImageFormat effective = rhi::ResolveSrgbFormat(asset.format, asset.srgb);
		for (const auto& entry : it->second) {
			if (entry.info.format != effective) continue;
			if (entry.info.usage != asset.usage) continue;
			if (entry.info.viewType != asset.viewType) continue;
			if (entry.flipY != asset.flipY) continue;
			if (asset.width != 0 && entry.info.width != asset.width) continue;
			if (asset.height != 0 && entry.info.height != asset.height) continue;
			if (asset.depth != entry.info.depth) continue;
			if (asset.arrayLayers != entry.info.arrayLayers) continue;
			if (asset.samples != entry.info.samples) continue;
			if (asset.mipLevels != 0 && entry.info.mipLevels != asset.mipLevels) continue;
			return &entry;
		}
		return nullptr;
	}

	const AssetLoader::BufferEntry* AssetLoader::FindBuffer(const std::string& identity,
		const BufferAsset& asset) const {
		const auto it = m_buffers.find(identity);
		if (it == m_buffers.end())
			return nullptr;
		for (const auto& entry : it->second) {
			if (entry.usage != asset.usage) continue;
			if (entry.hostVisible != asset.hostVisible) continue;
			if (asset.byteSize != 0 && entry.byteSize != asset.byteSize) continue;
			return &entry;
		}
		return nullptr;
	}

	const rhi::Image* AssetLoader::AcquireImage(std::string_view assetKey, std::string* error) {
		const ImageAsset* asset = m_catalog.FindImage(assetKey);
		if (asset == nullptr) {
			SetError(error, "未知图像资产: " + std::string(assetKey));
			return nullptr;
		}

		const auto resolved = m_catalog.Resolver().Resolve(asset->logicalPath, nullptr);
		if (!resolved) {
			SetError(error, "无法解析资产路径: " + asset->logicalPath);
			return nullptr;
		}
		const std::string key = resolved->identity;

		if (const ImageEntry* cached = FindImage(key, *asset))
			return cached->image.get();
		if (!m_images[key].empty()) {
			SetError(error, "同一资源的描述不一致: " + key);
			return nullptr;
		}

		if (asset->viewType != rhi::ImageViewType::Dim2D
			|| asset->depth != 1 || asset->arrayLayers != 1) {
			SetError(error, "加载器暂只支持 Dim2D 单层图像: " + asset->name);
			return nullptr;
		}
		if (asset->samples != 1) {
			SetError(error, "资产图像不支持多重采样: " + asset->name);
			return nullptr;
		}
		if (asset->generateMips || asset->mipLevels != 1) {
			SetError(error, "加载器暂不支持 mip 生成: " + asset->name);
			return nullptr;
		}
		const rhi::ImageFormat effective = rhi::ResolveSrgbFormat(asset->format, asset->srgb);
		if (effective == rhi::ImageFormat::None) {
			SetError(error, "srgb 标记缺少对应的 sRGB 格式: " + asset->name);
			return nullptr;
		}
		const int channels = ChannelsOf(effective);
		if (channels == 0) {
			SetError(error, "加载器暂不支持该格式: " + asset->name);
			return nullptr;
		}

		const auto view = m_catalog.Resolver().ViewBinary(asset->logicalPath, nullptr);
		std::vector<uint8_t> ownedBytes;
		std::span<const uint8_t> bytes;
		if (view) {
			bytes = *view;
		}
		else {
			std::string readError;
			auto data = m_catalog.Resolver().ReadBinary(asset->logicalPath, &readError);
			if (!data) {
				SetError(error, readError);
				return nullptr;
			}
			ownedBytes = std::move(*data);
			bytes = ownedBytes;
		}
		if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
			SetError(error, "图像文件过大: " + asset->logicalPath);
			return nullptr;
		}

		stbi_set_flip_vertically_on_load(asset->flipY ? 1 : 0);
		int width = 0, height = 0, sourceChannels = 0;
		stbi_uc* pixels = stbi_load_from_memory(bytes.data(),
			static_cast<int>(bytes.size()), &width, &height, &sourceChannels, channels);
		if (pixels == nullptr) {
			SetError(error, "图像解码失败: " + asset->logicalPath);
			return nullptr;
		}
		if ((asset->width != 0 && static_cast<uint32_t>(width) != asset->width)
			|| (asset->height != 0 && static_cast<uint32_t>(height) != asset->height)) {
			stbi_image_free(pixels);
			SetError(error, "解码尺寸与清单不一致: " + asset->name);
			return nullptr;
		}

		// BGRA 需要交换 R/B 通道
		if (effective == rhi::ImageFormat::BGRA8 || effective == rhi::ImageFormat::BGRA8Srgb) {
			const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
			for (size_t i = 0; i < pixelCount; ++i) {
				std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
			}
		}

		rhi::ImageCreateInfo info;
		info.width = static_cast<uint32_t>(width);
		info.height = static_cast<uint32_t>(height);
		info.depth = 1;
		info.arrayLayers = 1;
		info.mipLevels = 1;
		info.samples = 1;
		info.format = effective;
		info.viewType = rhi::ImageViewType::Dim2D;
		info.usage = asset->usage;

		const uint64_t byteSize = static_cast<uint64_t>(width) * static_cast<uint64_t>(height)
			* static_cast<uint64_t>(channels);
		auto image = m_device.UploadImage(pixels, byteSize, info, asset->initialLayout);
		stbi_image_free(pixels);
		if (!image) {
			SetError(error, "图像上传失败: " + asset->name);
			return nullptr;
		}

		ImageEntry entry;
		entry.identity = key;
		entry.info = info;
		entry.flipY = asset->flipY;
		entry.image = std::move(image);
		m_images[key].push_back(std::move(entry));
		return m_images[key].back().image.get();
	}

	const rhi::Buffer* AssetLoader::AcquireBuffer(std::string_view assetKey, std::string* error) {
		const BufferAsset* asset = m_catalog.FindBuffer(assetKey);
		if (asset == nullptr) {
			SetError(error, "未知缓冲资产: " + std::string(assetKey));
			return nullptr;
		}

		const auto resolved = m_catalog.Resolver().Resolve(asset->logicalPath, nullptr);
		if (!resolved) {
			SetError(error, "无法解析资产路径: " + asset->logicalPath);
			return nullptr;
		}
		const std::string key = resolved->identity;

		if (const BufferEntry* cached = FindBuffer(key, *asset))
			return cached->buffer.get();
		if (!m_buffers[key].empty()) {
			SetError(error, "同一资源的描述不一致: " + key);
			return nullptr;
		}

		const auto view = m_catalog.Resolver().ViewBinary(asset->logicalPath, nullptr);
		std::vector<uint8_t> ownedBytes;
		std::span<const uint8_t> bytes;
		if (view) {
			bytes = *view;
		}
		else {
			std::string readError;
			auto data = m_catalog.Resolver().ReadBinary(asset->logicalPath, &readError);
			if (!data) {
				SetError(error, readError);
				return nullptr;
			}
			ownedBytes = std::move(*data);
			bytes = ownedBytes;
		}

		const uint64_t byteSize = asset->byteSize == 0
			? static_cast<uint64_t>(bytes.size()) : asset->byteSize;
		if (byteSize == 0 || byteSize > static_cast<uint64_t>(bytes.size())) {
			SetError(error, "缓冲大小超出文件: " + asset->name);
			return nullptr;
		}

		std::unique_ptr<rhi::Buffer> buffer;
		if (asset->hostVisible) {
			buffer = m_device.UploadBuffer(bytes.data(), byteSize, asset->usage);
		}
		else {
			buffer = m_device.CreateBuffer({ byteSize, asset->usage, false });
			auto staging = m_device.CreateBuffer({ byteSize, rhi::BufferUsage::TransferSrc, true });
			if (buffer && staging) {
				std::memcpy(staging->Map(), bytes.data(), static_cast<size_t>(byteSize));
				staging->Flush();
				rhi::CommandList cmd = m_device.BeginCommandList();
				cmd.TransitionBuffer(*staging, rhi::PipelineStage::Host, rhi::Access::HostWrite,
					rhi::PipelineStage::Transfer, rhi::Access::TransferRead);
				cmd.CopyBuffer(*staging, *buffer);
				cmd.TransitionBuffer(*buffer, rhi::PipelineStage::Transfer, rhi::Access::TransferWrite,
					rhi::PipelineStage::AllCommands, rhi::Access::MemoryRead);
				rhi::SubmitInfo submit;
				submit.commandLists = std::span<const rhi::CommandList>(&cmd, 1);
				m_device.Submit(submit);
				m_device.WaitForFrame();
			}
		}
		if (!buffer) {
			SetError(error, "缓冲创建失败: " + asset->name);
			return nullptr;
		}

		BufferEntry entry;
		entry.identity = key;
		entry.byteSize = byteSize;
		entry.usage = asset->usage;
		entry.hostVisible = asset->hostVisible;
		entry.buffer = std::move(buffer);
		m_buffers[key].push_back(std::move(entry));
		return m_buffers[key].back().buffer.get();
	}

	size_t AssetLoader::ImageCount() const {
		size_t count = 0;
		for (const auto& pair : m_images)
			count += pair.second.size();
		return count;
	}

	size_t AssetLoader::BufferCount() const {
		size_t count = 0;
		for (const auto& pair : m_buffers)
			count += pair.second.size();
		return count;
	}

	void AssetLoader::Clear() {
		m_images.clear();
		m_buffers.clear();
	}
}
