#pragma once
#include "RenderGraphDesc.h"

// 资源声明等价比较：声明期合并（后端 ApplyMergeTask）与前端 ImportDeclarations 共用。
namespace dEngine {

	inline bool MergeImageDescEqual(const ImageDesc& a, const ImageDesc& b) {
		return a.width == b.width && a.height == b.height && a.depth == b.depth
			&& a.arrayLayers == b.arrayLayers && a.mipLevels == b.mipLevels
			&& a.samples == b.samples && a.format == b.format && a.viewType == b.viewType
			&& a.crossFrame == b.crossFrame;
	}

	inline bool MergeBufferDescEqual(const BufferDesc& a, const BufferDesc& b) {
		return a.size == b.size && a.usage == b.usage
			&& a.hostVisible == b.hostVisible && a.crossFrame == b.crossFrame;
	}

	inline bool MergeResourceDescEqual(const ResourceDesc& a, const ResourceDesc& b) {
		if (a.index() != b.index())
			return false;
		if (auto* x = std::get_if<ImageDesc>(&a))
			return MergeImageDescEqual(*x, *std::get_if<ImageDesc>(&b));
		if (auto* x = std::get_if<BufferDesc>(&a))
			return MergeBufferDescEqual(*x, *std::get_if<BufferDesc>(&b));
		return true;
	}

	inline bool MergeSubresourceEqual(const ImageSubresource& a, const ImageSubresource& b) {
		return a.mipLevel == b.mipLevel && a.layer == b.layer;
	}

	inline bool MergeRangeEqual(const ImageSubresourceRange& a, const ImageSubresourceRange& b) {
		return MergeSubresourceEqual(a.base, b.base)
			&& a.mipLevelCount == b.mipLevelCount && a.layerCount == b.layerCount;
	}
}
