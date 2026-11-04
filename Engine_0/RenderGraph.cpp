#include "RenderGraphAbstracted.h"
#include "RenderGraphNodes.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <stack>
#include <queue>
#include <cstdlib>

using namespace dEngine;

static const char* UsageName(Usage u) {
	switch (u) {
	case Usage::Auto:                   return "Auto";
	case Usage::ColorAttachment:        return "ColorAttachment";
	case Usage::DepthStencilAttachment: return "DepthStencilAttachment";
	case Usage::ResolveSrc:             return "ResolveSrc";
	case Usage::ResolveDst:             return "ResolveDst";
	case Usage::SampledRead:            return "SampledRead";
	case Usage::StorageRead:            return "StorageRead";
	case Usage::StorageWrite:           return "StorageWrite";
	case Usage::UniformRead:            return "UniformRead";
	case Usage::VertexInputRead:        return "VertexInputRead";
	case Usage::IndexRead:              return "IndexRead";
	case Usage::IndirectRead:           return "IndirectRead";
	case Usage::TransferRead:           return "TransferRead";
	case Usage::TransferWrite:          return "TransferWrite";
	case Usage::HostWrite:              return "HostWrite";
	case Usage::HostRead:               return "HostRead";
	case Usage::PresentSrc:             return "PresentSrc";
	}
	return "Unknown";
}

static bool IsDepthFormat(ImageFormat f) {
	return f == ImageFormat::D32F || f == ImageFormat::D24S8 || f == ImageFormat::S8;
}

enum class AliasRejectReason {
	None,
	TooFewMembers,
	NameNotFound,
	TypeMismatch,
	BorrowedResource,
	CrossFrameMismatch,
	AlreadyAssigned,
	DescMismatch,
	LifetimeOverlap
};

static const char* AliasRejectText(AliasRejectReason reason) {
	switch (reason) {
	case AliasRejectReason::TooFewMembers:     return "need at least 2 members";
	case AliasRejectReason::NameNotFound:      return "name not registered";
	case AliasRejectReason::TypeMismatch:      return "type mismatch";
	case AliasRejectReason::BorrowedResource:  return "borrowed resource";
	case AliasRejectReason::CrossFrameMismatch:return "crossFrame mismatch";
	case AliasRejectReason::AlreadyAssigned:   return "already in another alias group";
	case AliasRejectReason::DescMismatch:      return "desc mismatch";
	case AliasRejectReason::LifetimeOverlap:   return "lifetime overlap";
	default:                                   return "none";
	}
}

void RenderGraph::HandleError(ErrorCode code) {
	switch (code) {
	case ErrorCode::EmptyResourceName:
		std::cerr << "RenderGraph: 存在空的 resourceName" << std::endl; break;
	case ErrorCode::ResourceNotRegistered:
		std::cerr << "RenderGraph: 绑定了未注册的资源" << std::endl; break;
	case ErrorCode::InvalidAttachmentUsage:
		std::cerr << "RenderGraph: 附件使用了非附件类 usage" << std::endl; break;
	case ErrorCode::InvalidShaderUsage:
		std::cerr << "RenderGraph: shader 绑定使用了非 shader 类 usage" << std::endl; break;
	case ErrorCode::ColorSlotOutOfRange:
		std::cerr << "RenderGraph: 颜色附件 slot 超出上限" << std::endl; break;
	case ErrorCode::DuplicateColorSlot:
		std::cerr << "RenderGraph: 颜色附件 slot 重复" << std::endl; break;
	case ErrorCode::MultipleDepthAttachments:
		std::cerr << "RenderGraph: 声明了多于一个深度/模板附件" << std::endl; break;
	case ErrorCode::AttachmentWriteSetMismatch:
		std::cerr << "RenderGraph: 附件应在 write 集" << std::endl; break;
	case ErrorCode::ResolveSrcReadSetMismatch:
		std::cerr << "RenderGraph: ResolveSrc 应在 read 集" << std::endl; break;
	case ErrorCode::ShaderReadSetMismatch:
		std::cerr << "RenderGraph: 读类 shader 绑定应在 read 集" << std::endl; break;
	case ErrorCode::ShaderWriteSetMismatch:
		std::cerr << "RenderGraph: StorageWrite 绑定应在 write 集" << std::endl; break;
	case ErrorCode::DrawReadSetMismatch:
		std::cerr << "RenderGraph: 顶点/索引流资源应在 read 集" << std::endl; break;
	case ErrorCode::DrawStreamCountMismatch:
		std::cerr << "RenderGraph: 缺省顶点布局隐含单一流，vertexStreams 不能多于一条（显式布局请填 vertexBindings）" << std::endl; break;
	case ErrorCode::AttachmentShaderConflict:
		std::cerr << "RenderGraph: 资源同时作为附件与 shader 绑定（同 pass 又读又写）" << std::endl; break;
	case ErrorCode::PassTypeUnspecified:
		std::cerr << "RenderGraph: Pass 类型未指定（None 不是合法节点类型）" << std::endl; break;
	case ErrorCode::ResourceTypeUnspecified:
		std::cerr << "RenderGraph: Resource 类型未指定（None 不是合法节点类型）" << std::endl; break;
	case ErrorCode::PassTypeDescMismatch:
		std::cerr << "RenderGraph: Pass 类型与 Desc 不匹配" << std::endl; break;
	case ErrorCode::ResourceTypeDescMismatch:
		std::cerr << "RenderGraph: Resource 类型与 Desc 不匹配（或该类型节点尚未实现）" << std::endl; break;
	case ErrorCode::DeviceNotSet:
		std::cerr << "RenderGraph: 未设置 Device（先调用 SetDevice 再 Realize）" << std::endl; break;
	case ErrorCode::ColorSlotNotDense:
		std::cerr << "RenderGraph: 颜色附件 slot 不连续（须稠密 0..n-1，供 Realize 派生格式签名）" << std::endl; break;
	case ErrorCode::ShaderBindingTypeMismatch:
		std::cerr << "RenderGraph: shader 绑定的资源类型×usage 与反射类型不匹配" << std::endl; break;
	case ErrorCode::ShaderBindingNameNotFound:
		std::cerr << "RenderGraph: shader 绑定的资源在反射布局中无同名变量（shader 不使用该资源）" << std::endl; break;
	case ErrorCode::ShaderVariableUnbound:
		std::cerr << "RenderGraph: 反射变量在图中无同名资源（漏接）" << std::endl; break;
	case ErrorCode::PipelineCreationFailed:
		std::cerr << "RenderGraph: 管线创建失败（详细原因见上方 RHI 日志）" << std::endl; break;
	case ErrorCode::TransferBindingTypeMismatch:
		std::cerr << "RenderGraph: Transfer 的方向与两端资源类型不匹配（BufferToImage 需 Buffer->Image，"
			"ImageToBuffer 需 Image->Buffer，BufferToBuffer 需 Buffer->Buffer，"
			"ImageToImage 需 Image->Image 且格式一致）" << std::endl; break;
	case ErrorCode::DispatchReadSetMismatch:
		std::cerr << "RenderGraph: 间接派发的参数 Buffer 须登记在本 pass 的读集" << std::endl; break;
	case ErrorCode::TransferReadSetMismatch:
		std::cerr << "RenderGraph: Transfer 的源应在 read 集" << std::endl; break;
	case ErrorCode::TransferWriteSetMismatch:
		std::cerr << "RenderGraph: Transfer 的目的应在 write 集" << std::endl; break;
	case ErrorCode::PresentReadSetMismatch:
		std::cerr << "RenderGraph: Present 须恰好读一个 Image（presentable image），且不得有写边" << std::endl; break;
	case ErrorCode::StaticHeapUnsupported:
		std::cerr << "RenderGraph: 设备不支持静态纹理堆（需 descriptorIndexing + 采样图像数组动态索引）" << std::endl; break;
	case ErrorCode::StaticHeapCapacityExceeded:
		std::cerr << "RenderGraph: 静态纹理堆槽位耗尽（容量 " << kStaticTextureHeapCapacity << "）" << std::endl; break;
	case ErrorCode::PassOrderUnknownName:
		std::cerr << "RenderGraph: RegisterPass 的 PassOrder 引用了未注册的 pass 名" << std::endl; break;
	case ErrorCode::PassOrderSelfReference:
		std::cerr << "RenderGraph: RegisterPass 的 PassOrder 出现自引用（pass 不能排在自己前后）" << std::endl; break;
	case ErrorCode::PassOrderCycle:
		std::cerr << "RenderGraph: 依赖图成环（真实读写边或显式顺序约束矛盾）" << std::endl; break;
	case ErrorCode::PassNotRegistered:
		std::cerr << "RenderGraph: SetPassActive 的目标 pass 未注册" << std::endl; break;
	case ErrorCode::MuxPassInvalid:
		std::cerr << "RenderGraph: Mux pass 声明无效（需 >=2 个候选、恰好 1 个输出，且候选取自 read 集）" << std::endl; break;
	case ErrorCode::MuxOutputNotBorrowed:
		std::cerr << "RenderGraph: Mux 输出必须由 RegisterMuxOutput 注册为 Borrow-later 图像" << std::endl; break;
	case ErrorCode::MuxCandidateDescMismatch:
		std::cerr << "RenderGraph: Mux 候选/输出描述不一致（format/尺寸/层/mip/samples/viewType 必须相同）" << std::endl; break;
	case ErrorCode::InvalidImageSubresourceRange:
		std::cerr << "RenderGraph: 图像 view/barrierRange 越界" << std::endl; break;
	case ErrorCode::ViewOutsideBarrierRange:
		std::cerr << "RenderGraph: 图像 view 不在 barrierRange 内" << std::endl; break;
	case ErrorCode::AccessRangeOutsideBarrierRange:
		std::cerr << "RenderGraph: 附件访问范围不在 barrierRange 内" << std::endl; break;
	case ErrorCode::AttachmentLayerCountMismatch:
		std::cerr << "RenderGraph: 同一 Pass 的附件层数不一致" << std::endl; break;
	default:
		std::cerr << "RenderGraph: 未知校验错误" << std::endl; break;
	}
	std::exit(EXIT_FAILURE);
}

void RenderGraph::ResolvePassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	RenderPassDesc& desc)
{
	auto resolve = [&](const std::string& resName) -> uint32_t {
		auto it = tmp_resources.value().find(resName);
		if (resName.empty())
			throw ErrorCode::EmptyResourceName;
		if (it == tmp_resources.value().end())
			throw ErrorCode::ResourceNotRegistered;
		return it->second.id;
	};
	auto formatOf = [&](const std::string& resName) -> ImageFormat {
		auto it = tmp_resources.value().find(resName);
		if (it != tmp_resources.value().end()) {
			if (auto* img = std::get_if<ImageDesc>(&it->second.desc))
				return img->format;
		}
		return ImageFormat::None;
	};
	auto typeOf = [&](const std::string& resName) -> Resource::Type {
		auto it = tmp_resources.value().find(resName);
		return it != tmp_resources.value().end() ? it->second.type : Resource::None;
	};

	for (auto& att : desc.attachments) {
		att.resourceId = resolve(att.resourceName);
		if (att.usage == Usage::Auto) {
			att.usage = IsDepthFormat(formatOf(att.resourceName))
				? Usage::DepthStencilAttachment : Usage::ColorAttachment;
		}
		if (att.range.has_value()) {
			Resource* res = GetResource(*att.resourceId);
			if (res != nullptr && res->get_type() == Resource::Image) {
				Image* img = static_cast<Image*>(res);
				const ImageDesc& d = img->get_desc();
				rhi::ImageSubresourceRange range = *att.range;
				if (!rhi::NormalizeImageSubresourceRange(range, d.mipLevels, d.arrayLayers))
					throw ErrorCode::InvalidImageSubresourceRange;
				rhi::ImageSubresourceRange barrier = img->get_barrier_range();
				if (!rhi::NormalizeImageSubresourceRange(barrier, d.mipLevels, d.arrayLayers)
					|| !rhi::ImageSubresourceRangeContains(barrier, range))
					throw ErrorCode::AccessRangeOutsideBarrierRange;
				att.range = range;
			}
		}
	}
	for (auto& sb : desc.shaderBindings) {
		sb.resourceId = resolve(sb.resourceName);
		if (sb.usage == Usage::Auto)
			sb.usage = (typeOf(sb.resourceName) == Resource::Buffer)
				? Usage::UniformRead : Usage::SampledRead;
	}

	for (auto& vs : desc.vertexStreams) {
		vs.resourceId = resolve(vs.resourceName);
		if (typeOf(vs.resourceName) != Resource::Buffer)
			throw ErrorCode::ResourceTypeDescMismatch;
	}
	if (desc.indexStream.has_value()) {
		desc.indexStream->resourceId = resolve(desc.indexStream->resourceName);
		if (typeOf(desc.indexStream->resourceName) != Resource::Buffer)
			throw ErrorCode::ResourceTypeDescMismatch;
	}

	ValidateAttachments(read, write, desc);
	ValidateShaderBindings(read, write, desc.shaderBindings);
	ValidateDrawBindings(read, desc);
	ValidateBindingConflict(desc);
}

void RenderGraph::ResolveComputePassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	ComputePassDesc& desc)
{
	if (desc.shader.stage != rhi::ShaderStage::Compute)
		throw ErrorCode::PassTypeDescMismatch;

	auto resolve = [&](const std::string& resName) -> uint32_t {
		auto it = tmp_resources.value().find(resName);
		if (resName.empty())
			throw ErrorCode::EmptyResourceName;
		if (it == tmp_resources.value().end())
			throw ErrorCode::ResourceNotRegistered;
		return it->second.id;
	};

	auto typeOf = [&](const std::string& resName) -> Resource::Type {
		auto it = tmp_resources.value().find(resName);
		return it != tmp_resources.value().end() ? it->second.type : Resource::None;
	};

	for (auto& sb : desc.shaderBindings) {
		sb.resourceId = resolve(sb.resourceName);
		if (sb.usage == Usage::Auto)
			sb.usage = (typeOf(sb.resourceName) == Resource::Buffer)
				? Usage::UniformRead : Usage::SampledRead;
	}
	ValidateShaderBindings(read, write, desc.shaderBindings);

	if (!desc.indirectBuffer.empty()) {
		auto it = tmp_resources.value().find(desc.indirectBuffer);
		if (it == tmp_resources.value().end())
			throw ErrorCode::ResourceNotRegistered;
		if (it->second.type != Resource::Buffer)
			throw ErrorCode::ResourceTypeDescMismatch;
		desc.indirectBufferId = it->second.id;
		if (read.count(it->second.id) == 0)
			throw ErrorCode::DispatchReadSetMismatch;
	}
}

void RenderGraph::ResolveTransferPassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	TransferPassDesc& desc)
{
	auto resolve = [&](const std::string& resName) -> uint32_t {
		auto it = tmp_resources.value().find(resName);
		if (resName.empty())
			throw ErrorCode::EmptyResourceName;
		if (it == tmp_resources.value().end())
			throw ErrorCode::ResourceNotRegistered;
		return it->second.id;
	};
	auto typeOf = [&](const std::string& resName) -> Resource::Type {
		auto it = tmp_resources.value().find(resName);
		return it != tmp_resources.value().end() ? it->second.type : Resource::None;
	};
	auto imageDescOf = [&](const std::string& resName) -> const ImageDesc* {
		auto it = tmp_resources.value().find(resName);
		if (it == tmp_resources.value().end())
			return nullptr;
		return std::get_if<ImageDesc>(&it->second.desc);
	};

	if (desc.kind == TransferKind::ClearImage) {
		if (desc.destName.empty())
			throw ErrorCode::EmptyResourceName;
		desc.destId = resolve(desc.destName);
		if (typeOf(desc.destName) != Resource::Image)
			throw ErrorCode::TransferBindingTypeMismatch;
		if (write.count(*desc.destId) == 0)
			throw ErrorCode::TransferWriteSetMismatch;
		Resource* res = GetResource(*desc.destId);
		if (res != nullptr && res->get_type() == Resource::Image) {
			Image* img = static_cast<Image*>(res);
			const ImageDesc& d = img->get_desc();
			rhi::ImageSubresourceRange range = desc.clearRange;
			if (!rhi::NormalizeImageSubresourceRange(range, d.mipLevels, d.arrayLayers))
				throw ErrorCode::InvalidImageSubresourceRange;
			desc.clearRange = range;
		}
		return;
	}

	desc.sourceId = resolve(desc.sourceName);
	desc.destId = resolve(desc.destName);

	const Resource::Type srcType = typeOf(desc.sourceName);
	const Resource::Type dstType = typeOf(desc.destName);
	bool typeOk = false;
	switch (desc.kind) {
	case TransferKind::BufferToImage:
		typeOk = srcType == Resource::Buffer && dstType == Resource::Image;
		break;
	case TransferKind::ImageToBuffer:
		typeOk = srcType == Resource::Image && dstType == Resource::Buffer;
		break;
	case TransferKind::BufferToBuffer:
		typeOk = srcType == Resource::Buffer && dstType == Resource::Buffer;
		break;
	case TransferKind::ImageToImage: {
		typeOk = srcType == Resource::Image && dstType == Resource::Image;
		if (typeOk) {
			const ImageDesc* s = imageDescOf(desc.sourceName);
			const ImageDesc* d = imageDescOf(desc.destName);
			if (s == nullptr || d == nullptr || s->format != d->format)
				throw ErrorCode::TransferBindingTypeMismatch;
			if (!desc.blit && (s->width != d->width || s->height != d->height
				|| s->depth != d->depth || s->arrayLayers != d->arrayLayers))
				throw ErrorCode::TransferBindingTypeMismatch;
		}
		break;
	}
	}
	if (!typeOk)
		throw ErrorCode::TransferBindingTypeMismatch;
	if (read.count(*desc.sourceId) == 0)
		throw ErrorCode::TransferReadSetMismatch;
	if (write.count(*desc.destId) == 0)
		throw ErrorCode::TransferWriteSetMismatch;
}

void RenderGraph::ResolvePresentPassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	const PresentPassDesc& desc)
{
	(void)desc;
	if (read.size() != 1 || !write.empty())
		throw ErrorCode::PresentReadSetMismatch;
	const uint32_t id = *read.begin();
	for (const auto& r : tmp_resources.value())
		if (r.second.id == id && r.second.type != Resource::Image)
			throw ErrorCode::PresentReadSetMismatch;
}

void RenderGraph::ValidateAttachments(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	const RenderPassDesc& desc)
{
	std::set<uint32_t> colorSlots;
	uint32_t depthCount = 0;
	for (auto& att : desc.attachments) {
		switch (att.usage) {
		case Usage::ColorAttachment:
		case Usage::DepthStencilAttachment:
		case Usage::ResolveSrc:
		case Usage::ResolveDst:
			break;
		default:
			throw ErrorCode::InvalidAttachmentUsage;
		}

		if (att.usage == Usage::ColorAttachment) {
			if (att.slot >= kMaxColorAttachments)
				throw ErrorCode::ColorSlotOutOfRange;
			if (!colorSlots.insert(att.slot).second)
				throw ErrorCode::DuplicateColorSlot;
		}
		else if (att.usage == Usage::DepthStencilAttachment) {
			if (++depthCount > 1)
				throw ErrorCode::MultipleDepthAttachments;
		}

		uint32_t id = *att.resourceId;
		bool needWrite = (att.usage == Usage::ColorAttachment ||
			att.usage == Usage::DepthStencilAttachment ||
			att.usage == Usage::ResolveDst);
		if (needWrite && write.count(id) == 0)
			throw ErrorCode::AttachmentWriteSetMismatch;
		if (att.usage == Usage::ResolveSrc && read.count(id) == 0)
			throw ErrorCode::ResolveSrcReadSetMismatch;
	}

	if (!colorSlots.empty() && *colorSlots.rbegin() + 1 != colorSlots.size())
		throw ErrorCode::ColorSlotNotDense;
}

void RenderGraph::ValidateShaderBindings(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	const std::vector<ShaderBinding>& bindings)
{
	for (const auto& sb : bindings) {
		switch (sb.usage) {
		case Usage::SampledRead:
		case Usage::StorageRead:
		case Usage::StorageWrite:
		case Usage::UniformRead:
			break;
		default:
			throw ErrorCode::InvalidShaderUsage;
		}

		uint32_t id = *sb.resourceId;
		if (sb.usage == Usage::StorageWrite) {
			if (write.count(id) == 0)
				throw ErrorCode::ShaderWriteSetMismatch;
		}
		else {
			if (read.count(id) == 0)
				throw ErrorCode::ShaderReadSetMismatch;
		}
	}
}

void RenderGraph::ValidateDrawBindings(const std::set<uint32_t>& read, const RenderPassDesc& desc)
{
	for (const auto& vs : desc.vertexStreams) {
		if (read.count(*vs.resourceId) == 0)
			throw ErrorCode::DrawReadSetMismatch;
	}
	if (desc.indexStream.has_value() && read.count(*desc.indexStream->resourceId) == 0)
		throw ErrorCode::DrawReadSetMismatch;

	if (desc.pipeline.vertexBindings.empty() && desc.pipeline.vertexAttributes.empty()
		&& desc.vertexStreams.size() > 1)
		throw ErrorCode::DrawStreamCountMismatch;
}

void RenderGraph::ValidateBindingConflict(const RenderPassDesc& desc)
{
	std::set<uint32_t> boundIds;
	for (auto& sb : desc.shaderBindings)
		boundIds.insert(*sb.resourceId);

	for (auto& att : desc.attachments) {
		if (att.resourceId.has_value() && boundIds.count(*att.resourceId))
			throw ErrorCode::AttachmentShaderConflict;
	}
}

static rhi::QueueClass DefaultQueueForPass(Pass::Type type, const PassDesc& desc) {
	switch (type) {
		case Pass::Transfer: {
			if (auto* t = std::get_if<TransferPassDesc>(&desc)) {
				if (t->kind == TransferKind::ImageToImage && t->blit)
					return rhi::QueueClass::Graphics;
				if (t->kind == TransferKind::ClearImage)
					return rhi::QueueClass::Graphics;
			}
			return rhi::QueueClass::Transfer;
		}
		case Pass::Render:
		case Pass::Present:
		case Pass::Compute:
		case Pass::Mux:
		case Pass::Host:
		default:
			return rhi::QueueClass::Graphics;
	}
}

void RenderGraph::RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write,
	Pass::Type type, PassDesc desc, PassOrder order, std::optional<rhi::QueueClass> queue) {
	if (type == Pass::None) HandleError(ErrorCode::PassTypeUnspecified);
	if (!tmp_passes.has_value()) {
		tmp_passes.emplace();
	}
	reg_tmp_pass rtp;
	rtp.id = ++this->pass_top_id;
	rtp.to_read_name = read;
	rtp.to_write_name = write;
	rtp.type = type;
	rtp.queue = queue.value_or(DefaultQueueForPass(type, desc));
	rtp.order = std::move(order);
	rtp.desc = std::move(desc);
	tmp_passes.value()[name] = std::move(rtp);
}

void RenderGraph::RegisterResource(std::string name, std::set<std::string> reader, std::string writer,Resource::Type type,ResourceDesc desc,
	ImageSubresource view, rhi::ImageFormat viewFormat, ImageSubresourceRange barrierRange) {
	if (type == Resource::None) HandleError(ErrorCode::ResourceTypeUnspecified);
	if (!tmp_resources.has_value()) {
		tmp_resources.emplace();
	}
	reg_tmp_resource rtr;
	rtr.id = ++this->resource_top_id;
	rtr.readers_name = reader;
	rtr.writer_name = writer;
	rtr.type = type;
	rtr.desc = std::move(desc);
	rtr.view = view;
	rtr.viewFormat = viewFormat;
	rtr.barrierRange = barrierRange;
	tmp_resources.value()[name] = rtr;
}

void RenderGraph::RegisterResource(std::string name, std::set<std::string> reader, std::string writer,
	const rhi::Image* external, ImageSubresource view, rhi::ImageLayout initialLayout,
	rhi::ImageFormat viewFormat, ImageSubresourceRange barrierRange, bool crossFrame) {
	if (external == nullptr) HandleError(ErrorCode::ResourceNotRegistered);
	if (!tmp_resources.has_value()) {
		tmp_resources.emplace();
	}
	reg_tmp_resource rtr;
	rtr.id = ++this->resource_top_id;
	rtr.readers_name = reader;
	rtr.writer_name = writer;
	rtr.type = Resource::Image;
	rtr.view = view;
	rtr.viewFormat = viewFormat;
	rtr.barrierRange = barrierRange;
	rtr.externalImage = external;
	rtr.initialLayout = initialLayout;
	rtr.crossFrame = crossFrame;
	tmp_resources.value()[name] = rtr;
}

void RenderGraph::RegisterResource(std::string name, std::set<std::string> reader, std::string writer,
	const rhi::Buffer* external) {
	if (external == nullptr) HandleError(ErrorCode::ResourceNotRegistered);
	if (!tmp_resources.has_value()) {
		tmp_resources.emplace();
	}
	reg_tmp_resource rtr;
	rtr.id = ++this->resource_top_id;
	rtr.readers_name = reader;
	rtr.writer_name = writer;
	rtr.type = Resource::Buffer;
	rtr.externalBuffer = external;
	tmp_resources.value()[name] = rtr;
}

static uint32_t BytesPerPixel(ImageFormat f) {
	switch (f) {
	case ImageFormat::R8:          return 1;
	case ImageFormat::RG8:         return 2;
	case ImageFormat::RGBA8:
	case ImageFormat::BGRA8:       return 4;
	case ImageFormat::R16F:        return 2;
	case ImageFormat::RG16F:       return 4;
	case ImageFormat::RGBA16F:     return 8;
	case ImageFormat::R32F:        return 4;
	case ImageFormat::RG32F:       return 8;
	case ImageFormat::RGBA32F:     return 16;
	case ImageFormat::R11G11B10F:  return 4;
	case ImageFormat::D32F:        return 4;
	case ImageFormat::D24S8:       return 4;
	case ImageFormat::S8:          return 1;
	default:                       return 0;
	}
}

static uint64_t StagingBytesForRange(const ImageDesc& d, const rhi::ImageSubresourceRange& range) {
	const uint64_t bpp = BytesPerPixel(d.format);
	uint64_t total = 0;
	for (uint32_t mip = range.base.mipLevel; mip < range.base.mipLevel + range.mipLevelCount; ++mip) {
		const uint64_t w = std::max(1u, d.width >> mip);
		const uint64_t h = std::max(1u, (d.height == 0 ? 1u : d.height) >> mip);
		const uint64_t z = std::max(1u, (d.depth == 0 ? 1u : d.depth) >> mip);
		total += w * h * z * range.layerCount * bpp;
	}
	return total;
}

static void ValidateImageNodeRange(const Image& img, const char* context) {
	const ImageDesc& d = img.get_desc();
	rhi::ImageSubresourceRange viewRange{ img.get_view(), 1, 1 };
	rhi::ImageSubresourceRange barrierRange = img.get_barrier_range();
	if (!rhi::NormalizeImageSubresourceRange(viewRange, d.mipLevels, d.arrayLayers)
		|| !rhi::NormalizeImageSubresourceRange(barrierRange, d.mipLevels, d.arrayLayers)) {
		std::cerr << "RenderGraph: " << context << " 图像 \"" << img.get_name()
			<< "\" 的 view/barrierRange 越界" << std::endl;
		RenderGraph::HandleError(RenderGraph::ErrorCode::InvalidImageSubresourceRange);
	}
	if (!rhi::ImageSubresourceRangeContains(barrierRange, viewRange)) {
		std::cerr << "RenderGraph: " << context << " 图像 \"" << img.get_name()
			<< "\" 的 view 不在 barrierRange 内" << std::endl;
		RenderGraph::HandleError(RenderGraph::ErrorCode::ViewOutsideBarrierRange);
	}
}

void RenderGraph::ProcessRegister() {
	if (!tmp_passes.has_value()) tmp_passes.emplace();
	if (!tmp_resources.has_value()) tmp_resources.emplace();
	passPool.resize(tmp_passes.value().size());
	resourcePool.resize(tmp_resources.value().size());
	m_extraEdges.clear();
	m_hostImageStaging.clear();

	for (auto& pass : tmp_passes.value()) {
		uint32_t id = pass.second.id;
		std::string name = pass.first;
		Pass::Type type = pass.second.type;
		PassDesc desc = std::move(pass.second.desc);

		std::set<uint32_t> read, write;
		for (auto& read_name : pass.second.to_read_name) {
			auto it = tmp_resources.value().find(read_name);
			if (it == tmp_resources.value().end())
				std::cerr << "RenderGraph: \"" << read_name << "\" is not registered" << std::endl;
			else
				read.insert(it->second.id);
		}
		for (auto& write_name : pass.second.to_write_name) {
			auto it = tmp_resources.value().find(write_name);
			if (it == tmp_resources.value().end())
				std::cerr << "RenderGraph: \"" << write_name << "\" is not registered" << std::endl;
			else
				write.insert(it->second.id);
		}

		if (auto* rpd = std::get_if<RenderPassDesc>(&desc)) {
			try {
				ResolvePassDesc(read, write, *rpd);
			}
			catch (const ErrorCode& code) {
				HandleError(code);
			}
		}
		else if (auto* cpd = std::get_if<ComputePassDesc>(&desc)) {
			try {
				ResolveComputePassDesc(read, write, *cpd);
			}
			catch (const ErrorCode& code) {
				HandleError(code);
			}
		}
		else if (auto* tpd = std::get_if<TransferPassDesc>(&desc)) {
			try {
				ResolveTransferPassDesc(read, write, *tpd);
			}
			catch (const ErrorCode& code) {
				HandleError(code);
			}
		}
		else if (auto* ppd = std::get_if<PresentPassDesc>(&desc)) {
			try {
				ResolvePresentPassDesc(read, write, *ppd);
			}
			catch (const ErrorCode& code) {
				HandleError(code);
			}
		}
		else if (auto* mpd = std::get_if<MuxPassDesc>(&desc)) {
			if (read.size() < 2 || write.size() != 1 || mpd->alternatives.size() < 2) {
				std::cerr << "RenderGraph: Mux pass \"" << name << "\" 候选/输出数量不合法" << std::endl;
				HandleError(ErrorCode::MuxPassInvalid);
			}
			for (const auto& altName : mpd->alternatives) {
				if (pass.second.to_read_name.find(altName) == pass.second.to_read_name.end()) {
					std::cerr << "RenderGraph: Mux pass \"" << name << "\" 的候选 \"" << altName
						<< "\" 未在 read 集声明" << std::endl;
					HandleError(ErrorCode::MuxPassInvalid);
				}
			}
		}

		PassTopology topo;
		topo.id = id;
		topo.name = name;
		topo.queue = tmp_passes.value().at(name).queue;
		auto usageOf = [&desc](uint32_t resId, bool isWrite) -> Usage {
			if (std::get_if<MuxPassDesc>(&desc) != nullptr)
				return isWrite ? Usage::MuxSelect : Usage::MuxPrepare;
			if (auto* rpd = std::get_if<RenderPassDesc>(&desc)) {
				for (auto& att : rpd->attachments)
					if (att.resourceId && *att.resourceId == resId) return att.usage;
				for (auto& sb : rpd->shaderBindings)
					if (sb.resourceId && *sb.resourceId == resId) return sb.usage;
				for (auto& vs : rpd->vertexStreams)
					if (vs.resourceId && *vs.resourceId == resId) return Usage::VertexInputRead;
				if (rpd->indexStream && rpd->indexStream->resourceId
					&& *rpd->indexStream->resourceId == resId)
					return Usage::IndexRead;
			}
			else if (auto* cpd = std::get_if<ComputePassDesc>(&desc)) {
				for (auto& sb : cpd->shaderBindings)
					if (sb.resourceId && *sb.resourceId == resId) return sb.usage;
				if (cpd->indirectBufferId && *cpd->indirectBufferId == resId) return Usage::IndirectRead;
			}
			else if (auto* tpd = std::get_if<TransferPassDesc>(&desc)) {
				if (tpd->sourceId && *tpd->sourceId == resId) return Usage::TransferRead;
				if (tpd->destId && *tpd->destId == resId) return Usage::TransferWrite;
			}
			else if (std::get_if<PresentPassDesc>(&desc)) {
				return isWrite ? Usage::Auto : Usage::PresentSrc;
			}
			else if (std::get_if<HostPassDesc>(&desc)) {
				return isWrite ? Usage::HostWrite : Usage::HostRead;
			}
			return Usage::Auto;
		};
		for (uint32_t resId : write)
			topo.writes.push_back(ResourceUse{ resId, usageOf(resId, true) });
		for (uint32_t resId : read)
			topo.reads.push_back(ResourceUse{ resId, usageOf(resId, false) });

		passPool[id] = Pass::AutoAllocator(std::move(topo), type, std::move(desc));
	}

	for (auto& resource : tmp_resources.value()) {
		uint32_t id = resource.second.id;
		std::string name = resource.first;
		Resource::Type type = resource.second.type;
		std::set<uint32_t> reader;
		for (auto& reader_name : resource.second.readers_name) {
			auto it = tmp_passes.value().find(reader_name);
			if (it == tmp_passes.value().end())
				std::cerr << "RenderGraph: \"" << reader_name << "\" is not registered" << std::endl;
			else
				reader.insert(it->second.id);
		}
		uint32_t writer_id = static_cast<uint32_t>(-1);
		if (!resource.second.writer_name.empty()) {
			auto it = tmp_passes.value().find(resource.second.writer_name);
			if (it == tmp_passes.value().end())
				std::cerr << "RenderGraph: \"" << resource.second.writer_name << "\" is not registered" << std::endl;
			else
				writer_id = it->second.id;
		}
		ResourceTopology topo;
		topo.id = id;
		topo.name = name;
		topo.readers = std::move(reader);
		topo.writer = writer_id;
		topo.external = (resource.second.externalImage != nullptr
			|| resource.second.externalBuffer != nullptr
			|| resource.second.borrowedLater);
		topo.initialLayout = resource.second.initialLayout;
		topo.crossFrame = resource.second.crossFrame;
		if (auto* id = std::get_if<ImageDesc>(&resource.second.desc))
			topo.crossFrame = id->crossFrame;
		else if (auto* bd = std::get_if<BufferDesc>(&resource.second.desc))
			topo.crossFrame = bd->crossFrame;
		topo.aliasGroup = static_cast<uint32_t>(-1);
		topo.memoryBlockId = id;
		resourcePool[id] = Resource::AutoAllocator(std::move(topo), type, std::move(resource.second.desc),
			resource.second.view, resource.second.externalImage, resource.second.externalBuffer,
			resource.second.viewFormat, resource.second.barrierRange, resource.second.borrowedLater);
		Resource* created = resourcePool[id].get();
		if (created != nullptr && created->get_type() == Resource::Image)
			ValidateImageNodeRange(*static_cast<Image*>(created), "RegisterResource");
	}

	m_muxes.clear();
	auto findResByName = [this](const std::string& n) -> Resource* {
		for (auto& r : resourcePool)
			if (r != nullptr && r->get_name() == n) return r.get();
		return nullptr;
	};
	for (auto& holder : passPool) {
		Pass* p = holder.get();
		if (p == nullptr || p->get_type() != Pass::Mux)
			continue;
		auto* mp = static_cast<MuxPass*>(p);
		const PassTopology& topo = p->get_topology();
		if (topo.writes.size() != 1)
			HandleError(ErrorCode::MuxPassInvalid);

		MuxEntry entry;
		entry.passId = p->get_id();
		entry.outputResId = topo.writes[0].resourceId;
		Resource* outRes = GetResource(entry.outputResId);
		if (outRes == nullptr
			|| (outRes->get_type() != Resource::Image && outRes->get_type() != Resource::Buffer))
			HandleError(ErrorCode::MuxOutputNotBorrowed);
		entry.outputType = outRes->get_type();
		const bool outBorrowed = entry.outputType == Resource::Image
			? static_cast<Image*>(outRes)->is_borrowed()
			: static_cast<Buffer*>(outRes)->is_borrowed();
		if (!outBorrowed || outRes->m_topo.writer != p->get_id())
			HandleError(ErrorCode::MuxOutputNotBorrowed);

		if (entry.outputType == Resource::Image) {
			Image* outImg = static_cast<Image*>(outRes);
			const ImageDesc& od = outImg->get_desc();
			for (const auto& altName : mp->get_desc().alternatives) {
				Resource* altRes = findResByName(altName);
				if (altRes == nullptr || altRes->get_type() != Resource::Image)
					HandleError(ErrorCode::MuxPassInvalid);
				Image* altImg = static_cast<Image*>(altRes);
				const ImageDesc& ad = altImg->get_desc();
				if (ad.width != od.width || ad.height != od.height || ad.depth != od.depth
					|| ad.arrayLayers != od.arrayLayers || ad.mipLevels != od.mipLevels
					|| ad.samples != od.samples || ad.format != od.format
					|| ad.viewType != od.viewType
					|| altImg->get_view_format() != outImg->get_view_format()
					|| altImg->get_view().mipLevel != outImg->get_view().mipLevel
					|| altImg->get_view().layer != outImg->get_view().layer)
					HandleError(ErrorCode::MuxCandidateDescMismatch);

				rhi::ImageSubresourceRange outBarrier = outImg->get_barrier_range();
				rhi::ImageSubresourceRange altBarrier = altImg->get_barrier_range();
				if (!rhi::NormalizeImageSubresourceRange(outBarrier, od.mipLevels, od.arrayLayers)
					|| !rhi::NormalizeImageSubresourceRange(altBarrier, ad.mipLevels, ad.arrayLayers)
					|| outBarrier.base.mipLevel != altBarrier.base.mipLevel
					|| outBarrier.mipLevelCount != altBarrier.mipLevelCount
					|| outBarrier.base.layer != altBarrier.base.layer
					|| outBarrier.layerCount != altBarrier.layerCount)
					HandleError(ErrorCode::MuxCandidateDescMismatch);
				entry.alternativeIds.push_back(altRes->get_id());
			}
		}
		else {
			Buffer* outBuf = static_cast<Buffer*>(outRes);
			const BufferDesc& od = outBuf->get_desc();
			for (const auto& altName : mp->get_desc().alternatives) {
				Resource* altRes = findResByName(altName);
				if (altRes == nullptr || altRes->get_type() != Resource::Buffer)
					HandleError(ErrorCode::MuxPassInvalid);
				const BufferDesc& ad = static_cast<Buffer*>(altRes)->get_desc();
				if (ad.size != od.size || ad.usage != od.usage
					|| ad.hostVisible != od.hostVisible)
					HandleError(ErrorCode::MuxCandidateDescMismatch);
				entry.alternativeIds.push_back(altRes->get_id());
			}
		}

		for (auto& qh : passPool) {
			Pass* q = qh.get();
			if (q == nullptr || q->get_id() == p->get_id())
				continue;
			const PassTopology& qt = q->get_topology();
			for (const auto& use : qt.reads)
				if (use.resourceId == entry.outputResId)
					m_extraEdges.emplace_back(p->get_id(), q->get_id());
			for (const auto& use : qt.writes)
				if (use.resourceId == entry.outputResId)
					m_extraEdges.emplace_back(p->get_id(), q->get_id());
		}
		m_muxes.push_back(std::move(entry));
	}

	SynthesizeHostImageStaging();

	m_orderEdges.clear();
	auto passIdOf = [this](const std::string& passName) -> uint32_t {
		auto it = tmp_passes.value().find(passName);
		if (it == tmp_passes.value().end())
			throw ErrorCode::PassOrderUnknownName;
		return it->second.id;
	};
	try {
		for (const auto& entry : tmp_passes.value()) {
			const uint32_t target = entry.second.id;
			for (const auto& beforeName : entry.second.order.after) {
				const uint32_t src = passIdOf(beforeName);
				if (src == target) throw ErrorCode::PassOrderSelfReference;
				m_orderEdges.emplace_back(src, target);
			}
			for (const auto& afterName : entry.second.order.before) {
				const uint32_t dst = passIdOf(afterName);
				if (dst == target) throw ErrorCode::PassOrderSelfReference;
				m_orderEdges.emplace_back(target, dst);
			}
		}
	}
	catch (const ErrorCode& code) {
		HandleError(code);
	}
	std::sort(m_orderEdges.begin(), m_orderEdges.end());
	m_orderEdges.erase(std::unique(m_orderEdges.begin(), m_orderEdges.end()), m_orderEdges.end());

	tmp_passes.reset();
	tmp_resources.reset();
}

void RenderGraph::SetPassActive(const std::string& name, bool active) {
	Pass* target = nullptr;
	for (auto& p : passPool) {
		if (p != nullptr && p->get_name() == name) {
			target = p.get();
			break;
		}
	}
	if (target == nullptr) {
		HandleError(ErrorCode::PassNotRegistered);
		return;
	}
	if (target->get_type() != Pass::Render && target->get_type() != Pass::Compute) {
		std::cerr << "RenderGraph: 只有 Render/Compute pass 支持执行层门控（" << name << "）" << std::endl;
		return;
	}

	if (!active) {
		for (const auto& use : target->get_topology().writes) {
			Resource* res = GetResource(use.resourceId);
			if (res == nullptr)
				continue;
			for (uint32_t readerId : res->m_topo.readers) {
				if (readerId == target->get_id())
					continue;
				Pass* reader = GetPass(readerId);
				if (reader != nullptr && reader->is_active()) {
					std::cerr << "RenderGraph: 不能关闭 pass \"" << name << "\"：资源 \""
						<< res->get_name() << "\" 仍被活跃 pass \"" << reader->get_name()
						<< "\" 读取" << std::endl;
					return;
				}
			}
		}
	}
	target->set_active(active);
}

uint32_t RenderGraph::FrameSlotCount() const {
	return m_graphicDevice != nullptr
		? std::max(1u, m_graphicDevice->FramesInFlight())
		: 1u;
}

void RenderGraph::EnsureFrameSlots(uint32_t count) {
	if (m_frameSlots.size() >= count)
		return;
	const size_t oldCount = m_frameSlots.size();
	m_frameSlots.resize(count);
	for (size_t s = oldCount; s < m_frameSlots.size(); ++s)
		m_frameSlots[s].passSets.resize(passPool.size());
}

void RenderGraph::BeginFrame(uint32_t slot) {
	m_currentSlot = slot;
	EnsureFrameSlots(slot + 1);
	m_frameSlots[slot].frameSerial++;
	if (ApplyMuxSelection(slot))
		RefreshSlotDescriptorSets(slot);
}

void RenderGraph::SetMuxSelection(const std::string& muxPass, uint32_t slot, uint32_t index) {
	Pass* pass = nullptr;
	for (auto& p : passPool)
		if (p != nullptr && p->get_name() == muxPass) { pass = p.get(); break; }
	if (pass == nullptr || pass->get_type() != Pass::Mux) {
		std::cerr << "RenderGraph: SetMuxSelection 目标不是 Mux pass（" << muxPass << "）" << std::endl;
		return;
	}
	size_t muxIndex = m_muxes.size();
	for (size_t i = 0; i < m_muxes.size(); ++i)
		if (m_muxes[i].passId == pass->get_id()) { muxIndex = i; break; }
	if (muxIndex == m_muxes.size())
		return;
	if (index >= m_muxes[muxIndex].alternativeIds.size()) {
		std::cerr << "RenderGraph: mux 选择越界（" << muxPass << " index=" << index
			<< " candidates=" << m_muxes[muxIndex].alternativeIds.size() << "）" << std::endl;
		return;
	}
	EnsureFrameSlots(slot + 1);
	FrameSlotState& st = m_frameSlots[slot];
	if (st.muxSelections.size() < m_muxes.size()) st.muxSelections.resize(m_muxes.size(), 0);
	if (st.appliedSelections.size() < m_muxes.size())
		st.appliedSelections.resize(m_muxes.size(), UINT32_MAX);
	st.muxSelections[muxIndex] = index;
}

bool RenderGraph::ApplyMuxSelection(uint32_t slot) {
	if (slot >= m_frameSlots.size() || m_muxes.empty())
		return false;
	FrameSlotState& st = m_frameSlots[slot];
	if (st.muxSelections.size() < m_muxes.size()) st.muxSelections.resize(m_muxes.size(), 0);
	if (st.appliedSelections.size() < m_muxes.size())
		st.appliedSelections.resize(m_muxes.size(), UINT32_MAX);

	bool changed = false;
	for (size_t i = 0; i < m_muxes.size(); ++i) {
		const MuxEntry& mux = m_muxes[i];
		Resource* outRes = GetResource(mux.outputResId);
		if (outRes == nullptr || outRes->get_type() != mux.outputType)
			continue;
		const uint32_t index = st.muxSelections[i];
		if (index >= mux.alternativeIds.size())
			continue;
		Resource* altRes = GetResource(mux.alternativeIds[index]);
		if (altRes == nullptr || altRes->get_type() != mux.outputType)
			continue;
		RealizeResource(altRes);

		if (mux.outputType == Resource::Image) {
			const rhi::Image* handle = static_cast<Image*>(altRes)->get_rhi();
			if (handle == nullptr) {
				std::cerr << "RenderGraph: mux 候选 \"" << altRes->get_name() << "\" 尚未实例化" << std::endl;
				continue;
			}
			Image* outImg = static_cast<Image*>(outRes);
			if (outImg->mExternal != handle || st.appliedSelections[i] != index) {
				RetargetBorrowedImage(outRes->get_name(), handle);
				changed = true;
			}
		}
		else {
			const rhi::Buffer* handle = static_cast<Buffer*>(altRes)->get_rhi();
			if (handle == nullptr) {
				std::cerr << "RenderGraph: mux 候选 \"" << altRes->get_name() << "\" 尚未实例化" << std::endl;
				continue;
			}
			Buffer* outBuf = static_cast<Buffer*>(outRes);
			if (outBuf->mExternal != handle || st.appliedSelections[i] != index) {
				RetargetBorrowedBuffer(outRes->get_name(), handle);
				changed = true;
			}
		}
		st.appliedSelections[i] = index;
	}
	return changed;
}

void RenderGraph::RefreshSlotDescriptorSets(uint32_t slot) {
	if (slot >= m_frameSlots.size())
		return;
	FrameSlotState& st = m_frameSlots[slot];
	if (st.passSets.size() < passPool.size())
		st.passSets.resize(passPool.size());

	for (auto& holder : passPool) {
		Pass* p = holder.get();
		if (p == nullptr)
			continue;
		const uint32_t id = p->get_id();
		auto& sets = st.passSets[id];
		if (p->get_type() == Pass::Render) {
			auto* rp = static_cast<RenderPass*>(p);
			if (rp->m_rhiHandle)
				RealizeShaderSets(*rp->m_rhiHandle, rp->mDesc.shaderBindings, sets, rp->m_topo,
					rp->mDesc.bindStaticHeap ? 1u : 0u);
		}
		else if (p->get_type() == Pass::Compute) {
			auto* cp = static_cast<ComputePass*>(p);
			if (cp->m_rhiHandle)
				RealizeShaderSets(*cp->m_rhiHandle, cp->mDesc.shaderBindings, sets, cp->m_topo,
					cp->mDesc.bindStaticHeap ? 1u : 0u);
		}
	}
}

void RenderGraph::RefreshAllSlotDescriptorSets() {
	for (uint32_t slot = 0; slot < m_frameSlots.size(); ++slot)
		RefreshSlotDescriptorSets(slot);
}

void RenderGraph::SynthesizeHostImageStaging()
{
	struct HostImageUse {
		uint32_t    hostId = 0;
		std::string hostName;
		uint32_t    imageId = 0;
		std::string imageName;
		bool        read = false;
		bool        write = false;
	};
	std::vector<HostImageUse> uses;
	for (auto& pe : tmp_passes.value()) {
		if (pe.second.type != Pass::Host)
			continue;
		std::map<uint32_t, HostImageUse> perImage;
		auto scan = [&](const std::set<std::string>& names, bool isRead) {
			for (const auto& n : names) {
				auto it = tmp_resources.value().find(n);
				if (it == tmp_resources.value().end() || it->second.type != Resource::Image)
					continue;
				HostImageUse& u = perImage[it->second.id];
				u.hostId = pe.second.id;
				u.hostName = pe.first;
				u.imageId = it->second.id;
				u.imageName = n;
				(isRead ? u.read : u.write) = true;
			}
		};
		scan(pe.second.to_read_name, true);
		scan(pe.second.to_write_name, false);
		for (auto& kv : perImage)
			uses.push_back(kv.second);
	}

	for (const HostImageUse& u : uses) {
		Resource* imageRes = GetResource(u.imageId);
		Pass* hostPass = GetPass(u.hostId);
		if (imageRes == nullptr || hostPass == nullptr)
			continue;
		if (imageRes->get_type() != Resource::Image)
			continue;

		const ImageDesc& desc = static_cast<Image*>(imageRes)->get_desc();
		rhi::ImageSubresourceRange range{ static_cast<Image*>(imageRes)->get_view(), 1, 1 };
		if (!rhi::NormalizeImageSubresourceRange(range, desc.mipLevels, desc.arrayLayers)) {
			std::cerr << "RenderGraph: Host 段 \"" << u.hostName << "\" 的图像 \"" << u.imageName
				<< "\" 视图越界，跳过 staging 中转" << std::endl;
			continue;
		}
		const uint64_t bytes = StagingBytesForRange(desc, range);
		if (bytes == 0) {
			std::cerr << "RenderGraph: Host 段 \"" << u.hostName << "\" 的读/写图像 \"" << u.imageName
				<< "\" 尺寸或格式无效，跳过 staging 中转" << std::endl;
			continue;
		}

		rhi::BufferImageCopyRegion region;
		region.subresource = range;
		region.imageExtentWidth = std::max(1u, desc.width >> range.base.mipLevel);
		region.imageExtentHeight = std::max(1u, (desc.height == 0 ? 1u : desc.height) >> range.base.mipLevel);
		region.imageExtentDepth = std::max(1u, (desc.depth == 0 ? 1u : desc.depth) >> range.base.mipLevel);

		const uint32_t stagingId = ++resource_top_id;
		const std::string stagingName = "__hoststage_" + u.hostName + "_" + u.imageName;
		ResourceTopology stTopo;
		stTopo.id = stagingId;
		stTopo.name = stagingName;
		stTopo.aliasGroup = static_cast<uint32_t>(-1);
		stTopo.memoryBlockId = stagingId;
		BufferDesc stDesc;
		stDesc.size = bytes;
		stDesc.usage = rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst;
		stDesc.hostVisible = true;
		resourcePool.push_back(Resource::AutoAllocator(std::move(stTopo), Resource::Buffer, stDesc));
		Resource* staging = resourcePool.back().get();

		uint32_t readTransferId = static_cast<uint32_t>(-1);
		uint32_t writeTransferId = static_cast<uint32_t>(-1);

		if (u.read) {
			readTransferId = ++pass_top_id;
			PassTopology tp;
			tp.id = readTransferId;
			tp.name = "__hostread_" + u.hostName + "_" + u.imageName;
			tp.queue = rhi::QueueClass::Graphics;
			tp.reads.push_back(ResourceUse{ u.imageId, Usage::TransferRead });
			tp.writes.push_back(ResourceUse{ stagingId, Usage::TransferWrite });
			TransferPassDesc td;
			td.kind = TransferKind::ImageToBuffer;
			td.sourceName = u.imageName;
			td.sourceId = u.imageId;
			td.destName = stagingName;
			td.destId = stagingId;
			td.bufferImageRegions.push_back(region);
			passPool.push_back(Pass::AutoAllocator(std::move(tp), Pass::Transfer, td));
		}

		if (u.write) {
			writeTransferId = ++pass_top_id;
			PassTopology tp;
			tp.id = writeTransferId;
			tp.name = "__hostwrite_" + u.hostName + "_" + u.imageName;
			tp.queue = rhi::QueueClass::Graphics;
			tp.reads.push_back(ResourceUse{ stagingId, Usage::TransferRead });
			tp.writes.push_back(ResourceUse{ u.imageId, Usage::TransferWrite });
			TransferPassDesc td;
			td.kind = TransferKind::BufferToImage;
			td.sourceName = stagingName;
			td.sourceId = stagingId;
			td.destName = u.imageName;
			td.destId = u.imageId;
			td.bufferImageRegions.push_back(region);
			passPool.push_back(Pass::AutoAllocator(std::move(tp), Pass::Transfer, td));
		}

		auto& hostTopo = hostPass->m_topo;
		auto dropImage = [&](std::vector<ResourceUse>& edges) {
			edges.erase(std::remove_if(edges.begin(), edges.end(),
				[&](const ResourceUse& ru) { return ru.resourceId == u.imageId; }), edges.end());
		};
		dropImage(hostTopo.reads);
		dropImage(hostTopo.writes);
		if (u.read)
			hostTopo.reads.push_back(ResourceUse{ stagingId, Usage::HostRead });
		if (u.write)
			hostTopo.writes.push_back(ResourceUse{ stagingId, Usage::HostWrite });

		auto& imgTopo = imageRes->m_topo;
		imgTopo.readers.erase(u.hostId);
		if (u.read && readTransferId != static_cast<uint32_t>(-1)) {
			if (imgTopo.writer != u.hostId)
				imgTopo.readers.insert(readTransferId);
		}
		if (u.write && writeTransferId != static_cast<uint32_t>(-1)) {
			if (imgTopo.writer == u.hostId) {
				imgTopo.writer = writeTransferId;
			}
			else {
				for (uint32_t reader : imgTopo.readers) {
					if (reader == u.hostId || reader == readTransferId)
						continue;
					Pass* rp = GetPass(reader);
					if (rp != nullptr && rp->get_type() == Pass::Host)
						continue;
					m_extraEdges.emplace_back(writeTransferId, reader);
				}
			}
		}

		if (u.write) {
			staging->m_topo.writer = u.hostId;
			if (writeTransferId != static_cast<uint32_t>(-1))
				staging->m_topo.readers.insert(writeTransferId);
		}
		else if (readTransferId != static_cast<uint32_t>(-1)) {
			staging->m_topo.writer = readTransferId;
			staging->m_topo.readers.insert(u.hostId);
		}
		if (readTransferId != static_cast<uint32_t>(-1))
			m_extraEdges.emplace_back(readTransferId, u.hostId);
		if (writeTransferId != static_cast<uint32_t>(-1))
			m_extraEdges.emplace_back(u.hostId, writeTransferId);

		m_hostImageStaging[u.hostId].push_back(HostImageStaging{ u.imageName, stagingId, u.read, u.write });
	}
}

void RenderGraph::GenerateSubmitSequence() {
	submitSeq.clear();
	std::vector<std::vector<uint32_t>> AdjList;
	std::vector<int> indegree;
	AdjList.resize(static_cast<size_t>(passPool.size()));
	indegree.assign(static_cast<size_t>(passPool.size()),0);
	for (auto& pResource : resourcePool) {
		uint32_t writer = pResource.get()->m_topo.writer;
		if (writer == static_cast<uint32_t>(-1)) {
			continue;
		}
		for (uint32_t reader : pResource.get()->m_topo.readers) {
			if (reader == writer) {
				continue;
			}
			auto& edges = AdjList[writer];
			if (std::find(edges.begin(), edges.end(), reader) == edges.end()) {
				edges.push_back(reader);
				indegree[reader]++;
			}
		}
	}
	auto addEdge = [&AdjList, &indegree](uint32_t from, uint32_t to) {
		if (from >= AdjList.size() || to >= AdjList.size() || from == to)
			return;
		auto& edges = AdjList[from];
		if (std::find(edges.begin(), edges.end(), to) == edges.end()) {
			edges.push_back(to);
			indegree[to]++;
		}
	};
	for (const auto& e : m_extraEdges)
		addEdge(e.first, e.second);
	for (const auto& e : m_orderEdges)
		addEdge(e.first, e.second);

	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> queue;
	for (size_t i = 0; i < indegree.size(); i++) {
		if (indegree[i] == 0) {
			queue.push(static_cast<uint32_t>(i));
			indegree[i] = -1;
		}
	}
	while (!queue.empty()) {
		const uint32_t current = queue.top();
		queue.pop();
		submitSeq.push_back(current);
		for (uint32_t desc : AdjList[current]) {
			indegree[desc]--;
			if (indegree[desc] == 0) {
				indegree[desc] = -1;
				queue.push(desc);
			}
		}
	}
	for (int deg : indegree) {
		if (deg > 0)
			HandleError(ErrorCode::PassOrderCycle);
	}
}

void RenderGraph::AliasResources(std::vector<std::string> names) {
	m_aliasRequests.push_back(std::move(names));
}

void RenderGraph::ResolveAliasGroups() {
	if (m_aliasResolved)
		return;
	m_aliasResolved = true;
	if (m_aliasRequests.empty())
		return;

	BuildSubmitPlan();

	for (size_t g = 0; g < m_aliasRequests.size(); ++g) {
		const std::vector<std::string>& names = m_aliasRequests[g];
		auto degrade = [&names](AliasRejectReason reason) {
			std::cerr << "Alias group [";
			for (size_t i = 0; i < names.size(); ++i) {
				if (i) std::cerr << ",";
				std::cerr << names[i];
			}
			std::cerr << "] degraded: " << AliasRejectText(reason) << std::endl;
		};

		AliasRejectReason reason = AliasRejectReason::None;

		std::vector<Resource*> members;
		for (const auto& name : names) {
			Resource* res = nullptr;
			for (auto& r : resourcePool)
				if (r != nullptr && r->get_name() == name) { res = r.get(); break; }
			if (res == nullptr) { reason = AliasRejectReason::NameNotFound; break; }
			if (res->m_topo.aliasGroup != static_cast<uint32_t>(-1)) {
				reason = AliasRejectReason::AlreadyAssigned; break;
			}
			members.push_back(res);
		}
		if (reason == AliasRejectReason::None && members.size() < 2)
			reason = AliasRejectReason::TooFewMembers;

		if (reason == AliasRejectReason::None) {
			const Resource* first = members.front();
			for (Resource* r : members) {
				if (r->get_type() != first->get_type()) {
					reason = AliasRejectReason::TypeMismatch; break;
				}
				if (r->m_topo.external) {
					reason = AliasRejectReason::BorrowedResource; break;
				}
				if (r->m_topo.crossFrame != first->m_topo.crossFrame) {
					reason = AliasRejectReason::CrossFrameMismatch; break;
				}
				if (r->m_topo.initialLayout != first->m_topo.initialLayout) {
					reason = AliasRejectReason::DescMismatch; break;
				}
				if (r->get_type() == Resource::Image) {
					const ImageDesc& a = static_cast<const Image*>(r)->get_desc();
					const ImageDesc& b = static_cast<const Image*>(first)->get_desc();
					if (a.format != b.format || a.width != b.width || a.height != b.height
						|| a.depth != b.depth || a.mipLevels != b.mipLevels
						|| a.arrayLayers != b.arrayLayers || a.samples != b.samples
						|| a.viewType != b.viewType) {
						reason = AliasRejectReason::DescMismatch; break;
					}
				}
				else {
					const BufferDesc& a = static_cast<const Buffer*>(r)->get_desc();
					const BufferDesc& b = static_cast<const Buffer*>(first)->get_desc();
					if (a.size != b.size || a.usage != b.usage || a.hostVisible != b.hostVisible) {
						reason = AliasRejectReason::DescMismatch; break;
					}
				}
			}
		}

		if (reason == AliasRejectReason::None && !submitSeq.empty()) {
			struct Span {
				uint32_t first = static_cast<uint32_t>(-1);
				uint32_t last = 0;
				bool     used = false;
			};
			std::vector<Span> spans(members.size());
			for (uint32_t i = 0; i < submitSeq.size(); ++i) {
				Pass* p = GetPass(submitSeq[i]);
				if (p == nullptr)
					continue;
				const uint32_t batch = m_passBatch[p->get_id()];
				if (batch == static_cast<uint32_t>(-1))
					continue;
				const PassTopology& topo = p->get_topology();
				for (uint32_t m = 0; m < members.size(); ++m) {
					const uint32_t id = members[m]->get_id();
					bool touched = false;
					for (const auto& use : topo.reads)
						if (use.resourceId == id) { touched = true; break; }
					if (!touched)
						for (const auto& use : topo.writes)
							if (use.resourceId == id) { touched = true; break; }
					if (!touched)
						continue;
					Span& s = spans[m];
					s.used = true;
					s.first = std::min(s.first, batch);
					s.last = std::max(s.last, batch);
				}
			}
			for (size_t a = 0; a < spans.size() && reason == AliasRejectReason::None; ++a) {
				if (!spans[a].used)
					continue;
				for (size_t b = a + 1; b < spans.size(); ++b) {
					if (!spans[b].used)
						continue;
					if (spans[a].first <= spans[b].last && spans[b].first <= spans[a].last) {
						reason = AliasRejectReason::LifetimeOverlap; break;
					}
				}
			}
		}

		if (reason != AliasRejectReason::None) {
			degrade(reason);
			continue;
		}

		uint32_t owner = static_cast<uint32_t>(-1);
		for (Resource* r : members)
			owner = std::min(owner, r->get_id());
		for (Resource* r : members) {
			r->m_topo.aliasGroup = static_cast<uint32_t>(g);
			r->m_topo.memoryBlockId = owner;
		}
		std::cout << "Alias group [";
		for (size_t i = 0; i < names.size(); ++i)
			std::cout << (i ? "," : "") << names[i];
		std::cout << "] ok (memory block #" << owner << ")\n";
	}
}

void RenderGraph::PrintGraph() {
	auto PassType = [](Pass::Type t) -> const char* {
		switch (t) {
		case Pass::Render:   return "Render";
		case Pass::Compute:  return "Compute";
		case Pass::Host:     return "Host";
		case Pass::Transfer: return "Transfer";
		case Pass::Mux:      return "Mux";
		case Pass::Present:  return "Present";
		default:             return "None";
		}
	};
	auto ResourceType = [](Resource::Type t) -> const char* {
		switch (t) {
		case Resource::Image:  return "Image";
		case Resource::Buffer: return "Buffer";
		default:               return "None";
		}
	};
	auto FormatName = [](ImageFormat f) -> const char* {
		switch (f) {
		case ImageFormat::R8:         return "R8";
		case ImageFormat::RG8:        return "RG8";
		case ImageFormat::RGBA8:      return "RGBA8";
		case ImageFormat::BGRA8:      return "BGRA8";
		case ImageFormat::R16F:       return "R16F";
		case ImageFormat::RG16F:      return "RG16F";
		case ImageFormat::RGBA16F:    return "RGBA16F";
		case ImageFormat::R32F:       return "R32F";
		case ImageFormat::RG32F:      return "RG32F";
		case ImageFormat::RGBA32F:    return "RGBA32F";
		case ImageFormat::R11G11B10F: return "R11G11B10F";
		case ImageFormat::D32F:       return "D32F";
		case ImageFormat::D24S8:      return "D24S8";
		case ImageFormat::S8:         return "S8";
		default:                      return "None";
		}
	};
	auto ViewTypeName = [](ImageViewType v) -> const char* {
		switch (v) {
		case ImageViewType::Dim1D:      return "1D";
		case ImageViewType::Dim2D:      return "2D";
		case ImageViewType::Dim3D:      return "3D";
		case ImageViewType::Dim1DArray: return "1DArray";
		case ImageViewType::Dim2DArray: return "2DArray";
		case ImageViewType::Cube:       return "Cube";
		case ImageViewType::CubeArray:  return "CubeArray";
		default:                        return "None";
		}
	};
	auto LoadOpName = [](LoadOp o) -> const char* {
		switch (o) {
		case LoadOp::Load:     return "Load";
		case LoadOp::Clear:    return "Clear";
		default:               return "DontCare";
		}
	};
	auto StoreOpName = [](StoreOp o) -> const char* {
		switch (o) {
		case StoreOp::Store: return "Store";
		default:             return "DontCare";
		}
	};
	auto BufferUsageName = [](BufferUsage u) -> std::string {
		std::string s;
		auto add = [&s](const char* n) { if (!s.empty()) s += '|'; s += n; };
		if ((static_cast<uint32_t>(u) & static_cast<uint32_t>(BufferUsage::Vertex)) != 0) add("Vertex");
		if ((static_cast<uint32_t>(u) & static_cast<uint32_t>(BufferUsage::Index)) != 0) add("Index");
		if ((static_cast<uint32_t>(u) & static_cast<uint32_t>(BufferUsage::Uniform)) != 0) add("Uniform");
		if ((static_cast<uint32_t>(u) & static_cast<uint32_t>(BufferUsage::Storage)) != 0) add("Storage");
		if ((static_cast<uint32_t>(u) & static_cast<uint32_t>(BufferUsage::TransferSrc)) != 0) add("TransferSrc");
		if ((static_cast<uint32_t>(u) & static_cast<uint32_t>(BufferUsage::TransferDst)) != 0) add("TransferDst");
		return s.empty() ? std::string("None") : s;
	};
	auto PassName = [this](uint32_t id) -> std::string {
		if (id >= static_cast<uint32_t>(passPool.size())) return "<none>";
		return passPool[id]->get_name();
	};
	auto ResourceName = [this](uint32_t id) -> std::string {
		if (id >= static_cast<uint32_t>(resourcePool.size())) return "<none>";
		return resourcePool[id]->get_name();
	};

	std::cout << "===== RenderGraph =====\n";

	std::cout << "[Resources] count=" << resourcePool.size() << "\n";
	for (auto& r : resourcePool) {
		std::cout << "  #" << r->get_id() << " " << r->get_name()
			<< " type=" << ResourceType(r->get_type()) << " writer=";
		if (r->m_topo.writer == static_cast<uint32_t>(-1))
			std::cout << "<none>";
		else
			std::cout << PassName(r->m_topo.writer) << "(#" << r->m_topo.writer << ")";
		std::cout << " readers=[";
		for (uint32_t id : r->m_topo.readers) std::cout << PassName(id) << "(#" << id << ") ";
		std::cout << "] crossFrame=" << (r->m_topo.crossFrame ? "true" : "false")
			<< " external=" << (r->m_topo.external ? "true" : "false");
		if (auto* img = dynamic_cast<Image*>(r.get())) {
			const ImageDesc& d = img->get_desc();
			std::cout << " desc=(" << FormatName(d.format) << " "
				<< ViewTypeName(d.viewType) << " " << d.width << "x" << d.height
				<< " depth=" << d.depth << " layers=" << d.arrayLayers
				<< " mip=" << d.mipLevels << " samples=" << d.samples << ")";
		}
		else if (auto* buf = dynamic_cast<Buffer*>(r.get())) {
			const BufferDesc& d = buf->get_desc();
			std::cout << " desc=(" << d.size << "B usage=" << BufferUsageName(d.usage)
				<< " hostVisible=" << (d.hostVisible ? "true" : "false") << ")";
		}
		std::cout << "\n";
	}

	std::cout << "[Passes] count=" << passPool.size() << "\n";
	for (auto& p : passPool) {
		std::cout << "  #" << p->get_id() << " " << p->get_name()
			<< " type=" << PassType(p->get_type()) << "\n";
		std::cout << "      reads =[";
		for (auto& use : p->m_topo.reads)
			std::cout << ResourceName(use.resourceId) << "(#" << use.resourceId
				<< "," << UsageName(use.usage) << ") ";
		std::cout << "]\n      writes=[";
		for (auto& use : p->m_topo.writes)
			std::cout << ResourceName(use.resourceId) << "(#" << use.resourceId
				<< "," << UsageName(use.usage) << ") ";
		std::cout << "]\n";
		if (auto* rp = dynamic_cast<RenderPass*>(p.get())) {
			const RenderPassDesc& d = rp->get_desc();
			std::cout << "      att  =[";
			for (auto& a : d.attachments) {
				std::cout << a.resourceName << "(#"
					<< (a.resourceId ? std::to_string(*a.resourceId) : std::string("-"))
					<< " slot" << a.slot << "," << UsageName(a.usage) << ","
					<< LoadOpName(a.load) << "/" << StoreOpName(a.store) << ") ";
			}
			std::cout << "]\n      bind =[";
			for (auto& b : d.shaderBindings) {
				std::cout << "set" << (b.set ? std::to_string(*b.set) : std::string("-"))
					<< ".binding" << (b.binding ? std::to_string(*b.binding) : std::string("-"))
					<< " " << b.resourceName << "(#"
					<< (b.resourceId ? std::to_string(*b.resourceId) : std::string("-"))
					<< "," << UsageName(b.usage) << ") ";
			}
			std::cout << "]\n";
		}
	}

	std::cout << "[SubmitSequence] ";
	for (uint32_t id : submitSeq) std::cout << PassName(id) << "(#" << id << ") -> ";
	std::cout << "END\n";
}

#pragma region Realize

void RenderGraph::SetDevice(rhi::Device* graphic) {
	m_graphicDevice = graphic;
}

static rhi::ImageUsage UsageToImageBits(Usage u, bool isWrite) {
	switch (u) {
	case Usage::ColorAttachment:        return rhi::ImageUsage::ColorAttachment;
	case Usage::DepthStencilAttachment: return rhi::ImageUsage::DepthStencil;
	case Usage::SampledRead:            return rhi::ImageUsage::Sampled;
	case Usage::StorageRead:            return rhi::ImageUsage::Storage;
	case Usage::StorageWrite:           return rhi::ImageUsage::Storage;
	case Usage::UniformRead:            return rhi::ImageUsage::Storage;
	case Usage::VertexInputRead:
	case Usage::IndexRead:
	case Usage::IndirectRead:           return rhi::ImageUsage::None;
	case Usage::ResolveSrc:             return rhi::ImageUsage::ColorAttachment;
	case Usage::ResolveDst:             return rhi::ImageUsage::ColorAttachment;
	case Usage::TransferRead:           return rhi::ImageUsage::TransferSrc;
	case Usage::TransferWrite:          return rhi::ImageUsage::TransferDst;
	case Usage::HostWrite:              return rhi::ImageUsage::None;
	case Usage::HostRead:               return rhi::ImageUsage::None;
	case Usage::PresentSrc:             return rhi::ImageUsage::None;
	case Usage::MuxPrepare:             return rhi::ImageUsage::Sampled;
	case Usage::MuxSelect:              return rhi::ImageUsage::None;
	case Usage::Auto:
	default:
		return isWrite ? rhi::ImageUsage::Storage
			: (rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage);
	}
}

rhi::ImageUsage RenderGraph::DeriveImageUsage(uint32_t memoryBlockId) const {
	std::set<uint32_t> memberIds;
	for (auto& r : resourcePool)
		if (r != nullptr && r->m_topo.memoryBlockId == memoryBlockId)
			memberIds.insert(r->get_id());

	rhi::ImageUsage bits = rhi::ImageUsage::None;
	std::set<uint32_t> muxOutputIds;
	for (const MuxEntry& mux : m_muxes)
		for (uint32_t candId : mux.alternativeIds)
			if (memberIds.count(candId))
				muxOutputIds.insert(mux.outputResId);
	for (auto& p : passPool) {
		if (p == nullptr)
			continue;
		const PassTopology& topo = p->get_topology();
		for (auto& use : topo.writes)
			if (memberIds.count(use.resourceId))
				bits = bits | UsageToImageBits(use.usage, true);
		for (auto& use : topo.reads)
			if (memberIds.count(use.resourceId))
				bits = bits | UsageToImageBits(use.usage, false);
		for (auto& use : topo.writes)
			if (muxOutputIds.count(use.resourceId))
				bits = bits | UsageToImageBits(use.usage, true);
		for (auto& use : topo.reads)
			if (muxOutputIds.count(use.resourceId))
				bits = bits | UsageToImageBits(use.usage, false);
	}
	return bits;
}

void RenderGraph::RealizeResource(Resource* resource) {
	if (m_graphicDevice == nullptr)
		HandleError(ErrorCode::DeviceNotSet);

	switch (resource->get_type()) {
	case Resource::Image: {
		auto* img = static_cast<Image*>(resource);
		if (img->m_topo.memoryBlockId != img->get_id()) {
			Resource* owner = GetResource(img->m_topo.memoryBlockId);
			if (owner == nullptr || owner->get_type() != Resource::Image)
				HandleError(ErrorCode::ResourceTypeDescMismatch);
			RealizeResource(owner);
			img->mExternal = static_cast<Image*>(owner)->get_rhi();
			return;
		}
		if (img->is_borrowed())
			return;
		if (img->m_rhiHandle)
			return;
		const ImageDesc& d = img->get_desc();
		rhi::ImageCreateInfo info;
		info.width = d.width;
		info.height = d.height;
		info.depth = d.depth;
		info.arrayLayers = d.arrayLayers;
		info.mipLevels = d.mipLevels;
		info.samples = d.samples;
		info.format = d.format;
		info.viewType = d.viewType;
		info.usage = DeriveImageUsage(img->m_topo.memoryBlockId);
		img->m_rhiHandle = m_graphicDevice->CreateImage(info);
		break;
	}
	case Resource::Buffer: {
		auto* buf = static_cast<Buffer*>(resource);
		if (buf->m_topo.memoryBlockId != buf->get_id()) {
			Resource* owner = GetResource(buf->m_topo.memoryBlockId);
			if (owner == nullptr || owner->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceTypeDescMismatch);
			RealizeResource(owner);
			buf->mExternal = static_cast<Buffer*>(owner)->get_rhi();
			return;
		}
		if (buf->is_borrowed())
			return;
		if (buf->m_rhiHandle)
			return;
		const BufferDesc& d = buf->get_desc();
		rhi::BufferCreateInfo info;
		info.size = d.size;
		info.usage = d.usage;
		info.hostVisible = d.hostVisible;
		buf->m_rhiHandle = m_graphicDevice->CreateBuffer(info);
		break;
	}
	default:
		HandleError(ErrorCode::ResourceTypeUnspecified);
	}
}

void RenderGraph::RealizePass(Pass* pass) {
	if (m_graphicDevice == nullptr)
		HandleError(ErrorCode::DeviceNotSet);

	switch (pass->get_type()) {
	case Pass::Render: {
		auto* rp = static_cast<RenderPass*>(pass);
		if (rp->m_rhiHandle)
			return;
		const RenderPassDesc& desc = rp->get_desc();

		rhi::GraphicsPipelineDesc info;
		info.shaders           = desc.pipeline.shaders;
		info.depthStencil      = desc.pipeline.depthStencil;
		info.colorStates       = desc.pipeline.colorStates;
		info.blendConstants    = desc.pipeline.blendConstants;
		info.cullMode          = desc.pipeline.cullMode;
		info.pushConstantSize  = desc.pipeline.pushConstantSize;
		info.topology         = desc.pipeline.topology;
		info.tessPatchControlPoints = desc.pipeline.tessPatchControlPoints;
		info.vertexBindings   = desc.pipeline.vertexBindings;
		info.vertexAttributes = desc.pipeline.vertexAttributes;

		for (const auto& att : desc.attachments) {
			Resource* res = GetResource(*att.resourceId);
			if (!res)
				HandleError(ErrorCode::ResourceNotRegistered);
			if (res->get_type() != Resource::Image)
				HandleError(ErrorCode::ResourceTypeDescMismatch);
			RealizeResource(res);
			if (att.usage == Usage::ColorAttachment) {
				info.colorFormats[att.slot] = static_cast<Image*>(res)->get_desc().format;
				info.colorAttachmentCount = std::max(info.colorAttachmentCount, att.slot + 1);
			}
			else if (att.usage == Usage::DepthStencilAttachment) {
				info.depthFormat = static_cast<Image*>(res)->get_desc().format;
			}
		}

		const rhi::DescriptorSetLayout* externalSet0 =
			desc.bindStaticHeap ? m_staticHeapLayout.get() : nullptr;
		rp->m_rhiHandle = m_graphicDevice->CreatePipeline(info, externalSet0);
		if (!rp->m_rhiHandle)
			HandleError(ErrorCode::PipelineCreationFailed);

		EnsureFrameSlots(FrameSlotCount());
		for (uint32_t slot = 0; slot < m_frameSlots.size(); ++slot) {
			auto& slotSets = m_frameSlots[slot].passSets;
			if (slotSets.size() < passPool.size())
				slotSets.resize(passPool.size());
			RealizeShaderSets(*rp->m_rhiHandle, rp->mDesc.shaderBindings, slotSets[pass->get_id()],
				rp->m_topo, desc.bindStaticHeap ? 1u : 0u);
		}
		break;
	}
	case Pass::Compute: {
		auto* cp = static_cast<ComputePass*>(pass);
		if (cp->m_rhiHandle)
			return;

		rhi::ComputePipelineDesc info;
		info.shader = cp->mDesc.shader;
		info.pushConstantSize = cp->mDesc.pushConstantSize;
		const rhi::DescriptorSetLayout* externalSet0 =
			cp->mDesc.bindStaticHeap ? m_staticHeapLayout.get() : nullptr;
		cp->m_rhiHandle = m_graphicDevice->CreateComputePipeline(info, externalSet0);
		if (!cp->m_rhiHandle)
			HandleError(ErrorCode::PipelineCreationFailed);

		EnsureFrameSlots(FrameSlotCount());
		for (uint32_t slot = 0; slot < m_frameSlots.size(); ++slot) {
			auto& slotSets = m_frameSlots[slot].passSets;
			if (slotSets.size() < passPool.size())
				slotSets.resize(passPool.size());
			RealizeShaderSets(*cp->m_rhiHandle, cp->mDesc.shaderBindings, slotSets[pass->get_id()],
				cp->m_topo, cp->mDesc.bindStaticHeap ? 1u : 0u);
		}
		break;
	}
	case Pass::Host:
	case Pass::Transfer:
	case Pass::Mux:
	case Pass::Present:
		break;
	default:
		HandleError(ErrorCode::PassTypeUnspecified);
	}
}

rhi::Sampler* RenderGraph::AcquireSampler(const rhi::SamplerDesc& desc)
{
	if (m_graphicDevice == nullptr) return nullptr;
	const SamplerKey key{ desc.magFilter, desc.minFilter, desc.mipmapMode, desc.addressU,
		desc.addressV, desc.addressW, desc.anisotropyEnable, desc.maxAnisotropy,
		desc.mipLodBias, desc.minLod, desc.maxLod };
	if (auto it = m_samplers.find(key); it != m_samplers.end())
		return it->second.get();
	auto sampler = m_graphicDevice->CreateSampler(desc);
	if (!sampler) return nullptr;
	rhi::Sampler* raw = sampler.get();
	m_samplers.emplace(key, std::move(sampler));
	return raw;
}

uint32_t RenderGraph::RegisterStaticTexture(const std::string& name, const rhi::Image& image,
	ImageSubresource view, rhi::ImageFormat viewFormat, const std::optional<rhi::SamplerDesc>& sampler)
{
	if (m_graphicDevice == nullptr) {
		HandleError(ErrorCode::DeviceNotSet);
		return kStaticTextureHeapCapacity;
	}
	if (!m_graphicDevice->SupportsBindlessTextures()) {
		HandleError(ErrorCode::StaticHeapUnsupported);
		return kStaticTextureHeapCapacity;
	}

	auto existing = m_staticTextureSlots.find(name);
	if (existing != m_staticTextureSlots.end())
		return existing->second;
	if (m_staticHeapNextSlot >= kStaticTextureHeapCapacity) {
		HandleError(ErrorCode::StaticHeapCapacityExceeded);
		return kStaticTextureHeapCapacity;
	}

	if (!m_staticHeapLayout) {
		rhi::DescriptorBinding b;
		b.name = "staticTextures";
		b.set = 0;
		b.binding = 0;
		b.type = rhi::ResourceType::SampledImage;
		b.arrayCount = kStaticTextureHeapCapacity;
		b.stages = rhi::ShaderStageMask::None;
		m_staticHeapLayout = m_graphicDevice->CreateDescriptorSetLayout({ b }, true);
		if (!m_staticHeapLayout) {
			HandleError(ErrorCode::PipelineCreationFailed);
			return kStaticTextureHeapCapacity;
		}
		m_staticHeapSet = m_graphicDevice->AllocateDescriptorSet(*m_staticHeapLayout);
		if (!m_staticHeapSet) {
			HandleError(ErrorCode::DescriptorSetAllocationFailed);
			return kStaticTextureHeapCapacity;
		}
	}

	const uint32_t slot = m_staticHeapNextSlot++;
	rhi::ImageSubresourceRange viewRange{ view, 1, 1 };
	if (!rhi::NormalizeImageSubresourceRange(viewRange, image.Desc().mipLevels,
			image.Desc().arrayLayers)) {
		HandleError(ErrorCode::InvalidImageSubresourceRange);
		return kStaticTextureHeapCapacity;
	}
	rhi::Sampler* s = sampler.has_value() ? AcquireSampler(*sampler) : nullptr;
	m_staticHeapSet->Write(0, image, view, 1, s, viewFormat, slot);
	m_staticTextureSlots.emplace(name, slot);
	return slot;
}

void RenderGraph::RegisterMuxOutput(std::string name, std::set<std::string> readers, std::string writer,
	ResourceDesc desc) {
	if (!tmp_resources.has_value())
		tmp_resources.emplace();
	reg_tmp_resource rtr;
	rtr.id = ++resource_top_id;
	rtr.readers_name = std::move(readers);
	rtr.writer_name = std::move(writer);
	if (std::get_if<ImageDesc>(&desc)) {
		rtr.type = Resource::Image;
		rtr.initialLayout = rhi::ImageLayout::ShaderReadOnly;
	}
	else if (std::get_if<BufferDesc>(&desc)) {
		rtr.type = Resource::Buffer;
	}
	else {
		HandleError(ErrorCode::ResourceTypeUnspecified);
	}
	rtr.desc = std::move(desc);
	rtr.borrowedLater = true;
	tmp_resources.value()[std::move(name)] = std::move(rtr);
}

void RenderGraph::RealizeShaderSets(rhi::Pipeline& pipeline, std::vector<ShaderBinding>& shaderBindings,
	std::vector<std::unique_ptr<rhi::DescriptorSet>>& sets, PassTopology& topo, uint32_t firstSetIndex)
{
	uint32_t setCount = firstSetIndex;
	while (pipeline.SetLayout(setCount) != nullptr)
		++setCount;
	sets.resize(setCount);
	for (uint32_t s = firstSetIndex; s < setCount; ++s) {
		if (sets[s])
			continue;
		auto set = m_graphicDevice->AllocateDescriptorSet(*pipeline.SetLayout(s));
		if (!set)
			HandleError(ErrorCode::DescriptorSetAllocationFailed);
		sets[s] = std::move(set);
	}

	for (uint32_t s = firstSetIndex; s < setCount; ++s) {
		for (const auto& db : pipeline.SetLayout(s)->Bindings()) {
			auto it = std::find_if(shaderBindings.begin(), shaderBindings.end(),
				[&db](const ShaderBinding& sb) { return sb.resourceName == db.name; });
			if (it == shaderBindings.end()) {
				std::cerr << "RenderGraph: pass \"" << topo.name << "\" 的 shader 变量 \""
					<< db.name << "\" 没有同名图资源；当前 shaderBindings: ";
				for (const auto& sb : shaderBindings)
					std::cerr << "\"" << sb.resourceName << "\" ";
				std::cerr << std::endl;
				HandleError(ErrorCode::ShaderVariableUnbound);
			}

			Resource* res = GetResource(*it->resourceId);
			if (!res)
				HandleError(ErrorCode::ResourceNotRegistered);
			rhi::ResourceType expected;
			switch (res->get_type()) {
			case Resource::Image:
				if (it->usage == Usage::UniformRead)
					HandleError(ErrorCode::ShaderBindingTypeMismatch);
				expected = (it->usage == Usage::SampledRead)
					? rhi::ResourceType::SampledImage : rhi::ResourceType::StorageImage;
				break;
			case Resource::Buffer:
				if (it->usage == Usage::SampledRead)
					HandleError(ErrorCode::ShaderBindingTypeMismatch);
				expected = (it->usage == Usage::UniformRead)
					? rhi::ResourceType::UniformBuffer : rhi::ResourceType::StorageBuffer;
				break;
			default:
				HandleError(ErrorCode::ResourceTypeUnspecified);
			}
			if (expected != db.type)
				HandleError(ErrorCode::ShaderBindingTypeMismatch);
			it->set = db.set;
			it->binding = db.binding;
			it->arrayCount = db.arrayCount;

			for (auto& u : topo.reads)
				if (u.resourceId == res->get_id()) u.stages = db.stages;
			for (auto& u : topo.writes)
				if (u.resourceId == res->get_id()) u.stages = db.stages;

			switch (res->get_type()) {
			case Resource::Image: {
				auto* img = static_cast<Image*>(res);
				RealizeResource(res);
				if (!img->get_rhi())
					HandleError(ErrorCode::ResourceNotRegistered);
				rhi::Sampler* sampler = it->sampler.has_value()
					? AcquireSampler(*it->sampler) : nullptr;
				rhi::ImageLayout layout = it->usage == Usage::SampledRead
					? rhi::ImageLayout::ShaderReadOnly : rhi::ImageLayout::General;
				auto layoutIt = m_passImageLayouts.find({ topo.id, res->get_id() });
				if (layoutIt != m_passImageLayouts.end()
					&& layoutIt->second != rhi::ImageLayout::Undefined)
					layout = layoutIt->second;
				sets[s]->Write(db.binding, *img->get_rhi(), img->get_view(), db.arrayCount,
					sampler, img->get_view_format(), 0, layout);
				break;
			}
			case Resource::Buffer: {
				auto* buf = static_cast<Buffer*>(res);
				RealizeResource(res);
				if (!buf->get_rhi())
					HandleError(ErrorCode::ResourceNotRegistered);
				const uint64_t total = buf->get_rhi()->Desc().size;
				const uint64_t range = it->range != 0 ? it->range : (total - it->offset);
				sets[s]->Write(db.binding, *buf->get_rhi(), it->offset, range, db.arrayCount);
				break;
			}
			default:
				HandleError(ErrorCode::ResourceTypeUnspecified);
			}
		}
	}
	for (const auto& sb : shaderBindings) {
		if (!sb.set.has_value())
			HandleError(ErrorCode::ShaderBindingNameNotFound);
	}
}

void RenderGraph::RecordPass(rhi::CommandList& cmd, Pass* pass) {
	if (pass == nullptr || !pass->is_active())
		return;
	const uint32_t slot = m_currentSlot < m_frameSlots.size() ? m_currentSlot : 0;
	const std::vector<std::unique_ptr<rhi::DescriptorSet>>* passSets = nullptr;
	if (slot < m_frameSlots.size()) {
		const auto& all = m_frameSlots[slot].passSets;
		if (pass->get_id() < all.size())
			passSets = &all[pass->get_id()];
	}
	switch (pass->get_type()) {
	case Pass::Compute: {
		auto* cp = static_cast<ComputePass*>(pass);
		const rhi::Pipeline* pso = cp->get_rhi();
		if (pso == nullptr)
			HandleError(ErrorCode::PipelineCreationFailed);
		cmd.BindPipeline(*pso);
		{
			const std::vector<uint8_t>& pcData = !cp->get_push_constants().empty()
				? cp->get_push_constants() : cp->get_desc().pushConstantData;
			if (!pcData.empty() && pso->PushConstantSize() > 0) {
				uint32_t n = static_cast<uint32_t>(pcData.size());
				if (n > pso->PushConstantSize()) n = pso->PushConstantSize();
				cmd.PushConstants(pcData.data(), n);
			}
		}
		if (cp->get_desc().bindStaticHeap && m_staticHeapSet)
			cmd.BindDescriptorSet(0, *m_staticHeapSet);
		if (passSets != nullptr) {
			for (uint32_t s = 0; s < static_cast<uint32_t>(passSets->size()); ++s) {
				if (const rhi::DescriptorSet* set = (*passSets)[s].get())
					cmd.BindDescriptorSet(s, *set);
			}
		}
		const ComputePassDesc& d = cp->get_desc();
		if (!d.indirectBuffer.empty() && d.indirectBufferId.has_value()) {
			Resource* argsRes = GetResource(*d.indirectBufferId);
			if (argsRes == nullptr || argsRes->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(argsRes);
			const rhi::Buffer* args = static_cast<Buffer*>(argsRes)->get_rhi();
			if (args == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.DispatchIndirect(*args, d.indirectOffset);
		}
		else {
			cmd.Dispatch(d.groupCountX, d.groupCountY, d.groupCountZ);
		}
		break;
	}
	case Pass::Render: {
		auto* rp = static_cast<RenderPass*>(pass);
		const rhi::Pipeline* pso = rp->get_rhi();
		if (pso == nullptr)
			HandleError(ErrorCode::PipelineCreationFailed);
		const RenderPassDesc& d = rp->get_desc();

		std::array<rhi::RenderingAttachment, kMaxColorAttachments> colors {};
		uint32_t colorCount = 0;
		rhi::RenderingAttachment depth {};
		bool hasDepth = false;
		for (const auto& att : d.attachments) {
			Resource* res = GetResource(*att.resourceId);
			if (res == nullptr || res->get_type() != Resource::Image)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(res);
			Image* img = static_cast<Image*>(res);
			const rhi::Image* raw = img->get_rhi();
			if (raw == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);

			rhi::RenderingAttachment ra;
			ra.image  = const_cast<rhi::Image*>(raw);
			rhi::ImageSubresourceRange range{ img->get_view(), 1, 1 };
			if (att.range.has_value())
				range = *att.range;
			if (!rhi::NormalizeImageSubresourceRange(range, img->get_desc().mipLevels,
					img->get_desc().arrayLayers))
				HandleError(ErrorCode::InvalidImageSubresourceRange);
			ra.range  = range;
			ra.load   = att.load;
			ra.store  = att.store;
			ra.clear  = att.clear;
			if (att.usage == Usage::ColorAttachment) {
				ra.layout = rhi::ImageLayout::ColorAttachment;
				colors[att.slot] = ra;
				colorCount = std::max(colorCount, att.slot + 1);
			}
			else if (att.usage == Usage::DepthStencilAttachment) {
				ra.layout = rhi::ImageLayout::DepthStencilAttachment;
				depth = ra;
				hasDepth = true;
			}
		}

		const rhi::Image* sized = nullptr;
		if (colorCount > 0) sized = colors[0].image;
		else if (hasDepth) sized = depth.image;
		uint32_t width = sized ? sized->Desc().width : 0;
		uint32_t height = sized ? sized->Desc().height : 0;

		uint32_t layerCount = 1;
		bool layerKnown = false;
		auto checkLayer = [&](const rhi::RenderingAttachment& a) {
			if (a.image == nullptr)
				return;
			if (!layerKnown) {
				layerCount = a.range.layerCount;
				layerKnown = true;
			}
			else if (layerCount != a.range.layerCount) {
				HandleError(ErrorCode::AttachmentLayerCountMismatch);
			}
		};
		for (uint32_t i = 0; i < colorCount; ++i) checkLayer(colors[i]);
		if (hasDepth) checkLayer(depth);

		rhi::RenderingInfo info;
		info.colors = colors.data();
		info.colorCount = colorCount;
		info.depth = hasDepth ? &depth : nullptr;
		info.renderArea.width = width;
		info.renderArea.height = height;
		info.layerCount = layerCount;

		rhi::Viewport vp;
		if (d.viewport.autoFromColor) {
			vp.x = 0.0f; vp.y = 0.0f;
			vp.w = static_cast<float>(width);
			vp.h = static_cast<float>(height);
		}
		else {
			vp.x = d.viewport.x; vp.y = d.viewport.y;
			vp.w = d.viewport.w; vp.h = d.viewport.h;
		}
		vp.minDepth = d.viewport.minDepth;
		vp.maxDepth = d.viewport.maxDepth;

		cmd.BeginRendering(info);
		cmd.BindPipeline(*pso);
		{
			const std::vector<uint8_t>& pcData = !rp->get_push_constants().empty()
				? rp->get_push_constants() : d.pushConstantData;
			if (!pcData.empty() && pso->PushConstantSize() > 0) {
				uint32_t n = static_cast<uint32_t>(pcData.size());
				if (n > pso->PushConstantSize()) n = pso->PushConstantSize();
				cmd.PushConstants(pcData.data(), n);
			}
		}
		cmd.SetViewport(vp);
		if (d.bindStaticHeap && m_staticHeapSet)
			cmd.BindDescriptorSet(0, *m_staticHeapSet);
		if (passSets != nullptr) {
			for (uint32_t s = 0; s < static_cast<uint32_t>(passSets->size()); ++s) {
				if (const rhi::DescriptorSet* set = (*passSets)[s].get())
					cmd.BindDescriptorSet(s, *set);
			}
		}

		for (uint32_t i = 0; i < static_cast<uint32_t>(d.vertexStreams.size()); ++i) {
			Resource* res = GetResource(*d.vertexStreams[i].resourceId);
			if (res == nullptr || res->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(res);
			const rhi::Buffer* buf = static_cast<Buffer*>(res)->get_rhi();
			if (buf == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.BindVertexBuffer(*buf, d.vertexStreams[i].offset, i);
		}

		if (d.indexStream.has_value()) {
			Resource* res = GetResource(*d.indexStream->resourceId);
			if (res == nullptr || res->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(res);
			const rhi::Buffer* buf = static_cast<Buffer*>(res)->get_rhi();
			if (buf == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.BindIndexBuffer(*buf, d.indexStream->offset, d.indexStream->type);
		}

		const DrawSource& draw = rp->get_draw();
		if (draw.kind == DrawSource::Direct) {
			if (d.indexStream.has_value())
				cmd.DrawIndexed(draw.count, draw.first, draw.vertexOffset, draw.instanceCount);
			else
				cmd.Draw(draw.count, draw.first, draw.instanceCount);
		}
		else {
			const rhi::Buffer* args = nullptr;
			for (auto& r : resourcePool) {
				if (r->get_name() != draw.indirectBuffer) continue;
				if (r->get_type() != Resource::Buffer)
					HandleError(ErrorCode::ResourceTypeDescMismatch);
				RealizeResource(r.get());
				args = static_cast<Buffer*>(r.get())->get_rhi();
				break;
			}
			if (args == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			if (d.indexStream.has_value())
				cmd.DrawIndexedIndirect(*args, draw.indirectOffset, draw.count);
			else
				cmd.DrawIndirect(*args, draw.indirectOffset, draw.count);
		}

		cmd.EndRendering();
		break;
	}
	case Pass::Host:
		break;
	case Pass::Present:
		break;
	case Pass::Mux:
		break;
	case Pass::Transfer: {
		auto* tp = static_cast<TransferPass*>(pass);
		const TransferPassDesc& d = tp->get_desc();
		if (d.kind == TransferKind::ClearImage) {
			if (!d.destId.has_value())
				HandleError(ErrorCode::ResourceNotRegistered);
			Resource* dstRes = GetResource(*d.destId);
			if (dstRes == nullptr || dstRes->get_type() != Resource::Image)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(dstRes);
			rhi::Image* dst = const_cast<rhi::Image*>(static_cast<Image*>(dstRes)->get_rhi());
			if (dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.ClearImage(*dst, d.clearRange, rhi::ImageLayout::TransferDst, d.clearValue);
			break;
		}

		Resource* srcRes = GetResource(*d.sourceId);
		Resource* dstRes = GetResource(*d.destId);
		if (srcRes == nullptr || dstRes == nullptr)
			HandleError(ErrorCode::ResourceNotRegistered);
		RealizeResource(srcRes);
		RealizeResource(dstRes);

		switch (d.kind) {
		case TransferKind::BufferToImage: {
			const rhi::Buffer* src = static_cast<Buffer*>(srcRes)->get_rhi();
			rhi::Image* dst = const_cast<rhi::Image*>(static_cast<Image*>(dstRes)->get_rhi());
			if (src == nullptr || dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			if (d.bufferImageRegions.empty())
				cmd.CopyBufferToImage(*src, *dst, rhi::ImageLayout::TransferDst, d.srcOffset);
			else
				cmd.CopyBufferToImage(*src, *dst, rhi::ImageLayout::TransferDst, d.bufferImageRegions);
			break;
		}
		case TransferKind::ImageToBuffer: {
			const rhi::Image* src = static_cast<Image*>(srcRes)->get_rhi();
			const rhi::Buffer* dst = static_cast<Buffer*>(dstRes)->get_rhi();
			if (src == nullptr || dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			if (d.bufferImageRegions.empty())
				cmd.CopyImageToBuffer(*src, *dst, rhi::ImageLayout::TransferSrc, d.dstOffset);
			else
				cmd.CopyImageToBuffer(*src, *dst, rhi::ImageLayout::TransferSrc, d.bufferImageRegions);
			break;
		}
		case TransferKind::BufferToBuffer: {
			const rhi::Buffer* src = static_cast<Buffer*>(srcRes)->get_rhi();
			const rhi::Buffer* dst = static_cast<Buffer*>(dstRes)->get_rhi();
			if (src == nullptr || dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.CopyBuffer(*src, *dst, d.srcOffset, d.dstOffset, d.byteSize);
			break;
		}
		case TransferKind::ImageToImage: {
			const rhi::Image* src = static_cast<Image*>(srcRes)->get_rhi();
			rhi::Image* dst = const_cast<rhi::Image*>(static_cast<Image*>(dstRes)->get_rhi());
			if (src == nullptr || dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			if (d.blit) {
				if (d.blitRegions.empty())
					cmd.BlitImage(*src, *dst, rhi::ImageLayout::TransferSrc,
						rhi::ImageLayout::TransferDst, d.linearFilter);
				else
					cmd.BlitImage(*src, *dst, rhi::ImageLayout::TransferSrc,
						rhi::ImageLayout::TransferDst, d.blitRegions, d.linearFilter);
			}
			else {
				if (d.imageRegions.empty())
					cmd.CopyImage(*src, *dst, rhi::ImageLayout::TransferSrc, rhi::ImageLayout::TransferDst);
				else
					cmd.CopyImage(*src, *dst, rhi::ImageLayout::TransferSrc,
						rhi::ImageLayout::TransferDst, d.imageRegions);
			}
			break;
		}
		case TransferKind::ClearImage:
			break;
		}
		break;
	}
	default:
		HandleError(ErrorCode::PassTypeUnspecified);
	}
}

#pragma region BarrierCompiler

namespace {
	struct UsageSync {
		rhi::PipelineStage stage = rhi::PipelineStage::None;
		rhi::Access        access = rhi::Access::None;
		rhi::ImageLayout   layout = rhi::ImageLayout::Undefined;
	};

	rhi::PipelineStage ShaderStageFor(Pass::Type type, rhi::ShaderStageMask stages) {
		if (type == Pass::Compute) return rhi::PipelineStage::ComputeShader;
		if (stages == rhi::ShaderStageMask::None)
			return rhi::PipelineStage::VertexShader | rhi::PipelineStage::FragmentShader;
		const auto has = [stages](rhi::ShaderStageMask bit) {
			return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(bit)) != 0;
		};
		rhi::PipelineStage s = rhi::PipelineStage::None;
		if (has(rhi::ShaderStageMask::Vertex)) s = s | rhi::PipelineStage::VertexShader;
		if (has(rhi::ShaderStageMask::TessControl)) s = s | rhi::PipelineStage::TessControlShader;
		if (has(rhi::ShaderStageMask::TessEval)) s = s | rhi::PipelineStage::TessEvalShader;
		if (has(rhi::ShaderStageMask::Geometry)) s = s | rhi::PipelineStage::GeometryShader;
		if (has(rhi::ShaderStageMask::Fragment)) s = s | rhi::PipelineStage::FragmentShader;
		// Task/Mesh 管线尚未支持，暂按图形管线保守处理
		if (has(rhi::ShaderStageMask::Task) || has(rhi::ShaderStageMask::Mesh))
			s = s | rhi::PipelineStage::VertexShader | rhi::PipelineStage::FragmentShader;
		return s == rhi::PipelineStage::None ? rhi::PipelineStage::FragmentShader : s;
	}

	UsageSync UsageSyncOf(Usage u, Pass::Type type,
		rhi::ShaderStageMask stages = rhi::ShaderStageMask::None) {
		UsageSync s;
		switch (u) {
		case Usage::ColorAttachment:
			s.stage = rhi::PipelineStage::ColorAttachmentOut;
			s.access = rhi::Access::ColorAttachmentRead | rhi::Access::ColorAttachmentWrite;
			s.layout = rhi::ImageLayout::ColorAttachment;
			break;
		case Usage::DepthStencilAttachment:
			s.stage = rhi::PipelineStage::EarlyFragmentTests | rhi::PipelineStage::LateFragmentTests;
			s.access = rhi::Access::DepthStencilRead | rhi::Access::DepthStencilWrite;
			s.layout = rhi::ImageLayout::DepthStencilAttachment;
			break;
		case Usage::ResolveSrc:
			s.stage = rhi::PipelineStage::ColorAttachmentOut;
			s.access = rhi::Access::ColorAttachmentRead;
			s.layout = rhi::ImageLayout::ColorAttachment;
			break;
		case Usage::ResolveDst:
			s.stage = rhi::PipelineStage::ColorAttachmentOut;
			s.access = rhi::Access::ColorAttachmentWrite;
			s.layout = rhi::ImageLayout::ColorAttachment;
			break;
		case Usage::SampledRead:
			s.stage = ShaderStageFor(type, stages);
			s.access = rhi::Access::ShaderRead;
			s.layout = rhi::ImageLayout::ShaderReadOnly;
			break;
		case Usage::StorageRead:
			s.stage = ShaderStageFor(type, stages);
			s.access = rhi::Access::ShaderRead;
			s.layout = rhi::ImageLayout::General;
			break;
		case Usage::StorageWrite:
			s.stage = ShaderStageFor(type, stages);
			s.access = rhi::Access::ShaderWrite;
			s.layout = rhi::ImageLayout::General;
			break;
		case Usage::UniformRead:
			s.stage = ShaderStageFor(type, stages);
			s.access = rhi::Access::ShaderRead;
			s.layout = rhi::ImageLayout::General;
			break;
		case Usage::VertexInputRead:
			s.stage = rhi::PipelineStage::VertexInput;
			s.access = rhi::Access::VertexAttributeRead;
			break;
		case Usage::IndexRead:
			s.stage = rhi::PipelineStage::VertexInput;
			s.access = rhi::Access::IndexRead;
			break;
		case Usage::IndirectRead:
			s.stage = rhi::PipelineStage::DrawIndirect;
			s.access = rhi::Access::IndirectCommandRead;
			break;
		case Usage::TransferRead:
			s.stage = rhi::PipelineStage::Transfer;
			s.access = rhi::Access::TransferRead;
			s.layout = rhi::ImageLayout::TransferSrc;
			break;
		case Usage::TransferWrite:
			s.stage = rhi::PipelineStage::Transfer;
			s.access = rhi::Access::TransferWrite;
			s.layout = rhi::ImageLayout::TransferDst;
			break;
		case Usage::HostWrite:
			s.stage = rhi::PipelineStage::Host;
			s.access = rhi::Access::HostWrite;
			break;
		case Usage::HostRead:
			s.stage = rhi::PipelineStage::Host;
			s.access = rhi::Access::HostRead;
			break;
		case Usage::PresentSrc:
			s.stage = rhi::PipelineStage::AllCommands;
			s.layout = rhi::ImageLayout::PresentSrc;
			break;
		case Usage::MuxPrepare:
		case Usage::MuxSelect:
			s.stage = rhi::PipelineStage::AllCommands;
			s.access = rhi::Access::None;
			s.layout = rhi::ImageLayout::ShaderReadOnly;
			break;
		case Usage::Auto:
		default:
			s.stage = rhi::PipelineStage::AllCommands;
			s.access = rhi::Access::MemoryRead | rhi::Access::MemoryWrite;
			s.layout = rhi::ImageLayout::General;
			break;
		}
		return s;
	}
}

void RenderGraph::BuildSubmitPlan() {
	m_plan = {};
	m_passBatch.assign(passPool.size(), static_cast<uint32_t>(-1));
	if (submitSeq.empty())
		return;

	std::vector<bool> isHost(submitSeq.size(), false);
	std::vector<rhi::QueueClass> queues(submitSeq.size(), rhi::QueueClass::Graphics);
	rhi::QueueClass tailQueue = rhi::QueueClass::Graphics;
	for (size_t i = submitSeq.size(); i-- > 0;) {
		Pass* p = GetPass(submitSeq[i]);
		if (p == nullptr)
			continue;
		if (p->get_type() == Pass::Host) {
			isHost[i] = true;
			queues[i] = tailQueue;
		}
		else {
			queues[i] = p->get_topology().queue;
			tailQueue = queues[i];
		}
	}

	std::map<uint32_t, std::vector<Resource*>> blocks;
	for (auto& holder : resourcePool) {
		Resource* res = holder.get();
		if (res != nullptr)
			blocks[res->m_topo.memoryBlockId].push_back(res);
	}
	std::set<uint32_t> cuts;
	for (auto& entry : blocks) {
		const std::vector<Resource*>& members = entry.second;
		uint32_t prevIndex = static_cast<uint32_t>(-1);
		bool prevWrite = false;
		for (uint32_t i = 0; i < submitSeq.size(); ++i) {
			Pass* p = GetPass(submitSeq[i]);
			if (p == nullptr)
				continue;
			const PassTopology& topo = p->get_topology();
			bool touched = false;
			bool write = false;
			for (Resource* res : members) {
				const uint32_t resId = res->get_id();
				for (const auto& u : topo.writes)
					if (u.resourceId == resId) { touched = true; write = true; break; }
				if (write)
					break;
				if (!touched)
					for (const auto& u : topo.reads)
						if (u.resourceId == resId) { touched = true; break; }
			}
			if (!touched)
				continue;
			if (prevIndex != static_cast<uint32_t>(-1)) {
				const bool need = prevWrite || write;
				if (need && queues[prevIndex] != queues[i])
					cuts.insert(prevIndex);
			}
			prevIndex = i;
			prevWrite = write;
		}
	}

	for (uint32_t i = 0; i < submitSeq.size(); ++i) {
		const bool hostStart = isHost[i] && i > 0 && !isHost[i - 1];
		const bool boundary = m_plan.batches.empty()
			|| m_plan.batches.back().queue != queues[i]
			|| (i > 0 && cuts.find(i - 1) != cuts.end())
			|| hostStart;
		if (boundary) {
			QueueBatch batch;
			batch.queue = queues[i];
			batch.begin = i;
			batch.end = i;
			m_plan.batches.push_back(batch);
		}
		m_plan.batches.back().end = i + 1;
	}

	for (uint32_t bi = 0; bi < m_plan.batches.size(); ++bi)
		for (uint32_t i = m_plan.batches[bi].begin; i < m_plan.batches[bi].end; ++i)
			m_passBatch[submitSeq[i]] = bi;

	BuildHostSync();
}

void RenderGraph::BuildHostSync() {
	m_hostSync.clear();
	m_batchHostPasses.assign(m_plan.batches.size(), std::vector<uint32_t>{});
	m_batchHasGpu.assign(m_plan.batches.size(), false);
	m_hostPassOrder.clear();

	for (uint32_t b = 0; b < m_plan.batches.size(); ++b) {
		for (uint32_t i = m_plan.batches[b].begin; i < m_plan.batches[b].end; ++i) {
			Pass* p = GetPass(submitSeq[i]);
			if (p != nullptr && p->get_type() != Pass::Host) {
				m_batchHasGpu[b] = true;
				break;
			}
		}
	}

	std::vector<uint32_t> lastUse(resourcePool.size(), static_cast<uint32_t>(-1));
	auto blockOf = [this](uint32_t resId) -> uint32_t {
		Resource* res = GetResource(resId);
		return res != nullptr ? res->m_topo.memoryBlockId : resId;
	};
	for (uint32_t i = 0; i < submitSeq.size(); ++i) {
		Pass* p = GetPass(submitSeq[i]);
		if (p == nullptr)
			continue;
		if (p->get_type() == Pass::Host) {
			m_hostPassOrder.push_back(p->get_id());
			HostPassSync sync;
			const uint32_t myBatch = m_passBatch[p->get_id()];
			for (const auto& use : p->get_topology().reads) {
				const uint32_t block = blockOf(use.resourceId);
				if (block >= lastUse.size())
					continue;
				const uint32_t prev = lastUse[block];
				if (prev == static_cast<uint32_t>(-1))
					continue;
				Pass* up = GetPass(submitSeq[prev]);
				if (up == nullptr)
					continue;
				const uint32_t upBatch = m_passBatch[up->get_id()];
				if (upBatch == static_cast<uint32_t>(-1) || upBatch == myBatch)
					continue;
				if (up->get_type() == Pass::Host)
					sync.hostPasses.push_back(up->get_id());
				else
					sync.gpuBatches.push_back(upBatch);
			}
			for (const auto& use : p->get_topology().writes) {
				const uint32_t block = blockOf(use.resourceId);
				if (block >= lastUse.size())
					continue;
				const uint32_t prev = lastUse[block];
				if (prev == static_cast<uint32_t>(-1))
					continue;
				Pass* up = GetPass(submitSeq[prev]);
				if (up == nullptr)
					continue;
				const uint32_t upBatch = m_passBatch[up->get_id()];
				if (upBatch == static_cast<uint32_t>(-1) || upBatch == myBatch)
					continue;
				if (up->get_type() == Pass::Host)
					sync.hostPasses.push_back(up->get_id());
				else
					sync.gpuBatches.push_back(upBatch);
			}
			m_hostSync[p->get_id()] = std::move(sync);
		}
		for (const auto& use : p->get_topology().reads) {
			const uint32_t block = blockOf(use.resourceId);
			if (block < lastUse.size())
				lastUse[block] = i;
		}
		for (const auto& use : p->get_topology().writes) {
			const uint32_t block = blockOf(use.resourceId);
			if (block < lastUse.size())
				lastUse[block] = i;
		}
	}

	for (uint32_t b = 0; b < m_plan.batches.size(); ++b) {
		if (!m_batchHasGpu[b])
			continue;
		for (uint32_t i = m_plan.batches[b].begin; i < m_plan.batches[b].end; ++i) {
			Pass* p = GetPass(submitSeq[i]);
			if (p == nullptr)
				continue;
			if (p->get_type() == Pass::Host) {
				m_batchHostPasses[b].push_back(p->get_id());
				continue;
			}
			for (const auto& use : p->get_topology().reads) {
				const uint32_t block = blockOf(use.resourceId);
				for (auto& holder : resourcePool) {
					Resource* res = holder.get();
					if (res == nullptr || res->m_topo.memoryBlockId != block)
						continue;
					const uint32_t writer = res->get_topology().writer;
					if (writer == static_cast<uint32_t>(-1))
						continue;
					Pass* wp = GetPass(writer);
					if (wp == nullptr || wp->get_type() != Pass::Host)
						continue;
					if (std::find(m_batchHostPasses[b].begin(), m_batchHostPasses[b].end(), writer)
						== m_batchHostPasses[b].end())
						m_batchHostPasses[b].push_back(writer);
				}
			}
		}
	}

	m_batchHostStage.assign(m_plan.batches.size(), rhi::PipelineStage::None);
	for (uint32_t b = 0; b < m_plan.batches.size(); ++b) {
		if (!m_batchHasGpu[b] || m_batchHostPasses[b].empty())
			continue;
		std::set<uint32_t> hostBlocks;
		for (uint32_t hostPassId : m_batchHostPasses[b]) {
			Pass* hostPass = GetPass(hostPassId);
			if (hostPass == nullptr)
				continue;
			for (const auto& use : hostPass->get_topology().reads)
				hostBlocks.insert(blockOf(use.resourceId));
			for (const auto& use : hostPass->get_topology().writes)
				hostBlocks.insert(blockOf(use.resourceId));
		}
		rhi::PipelineStage mask = rhi::PipelineStage::None;
		for (uint32_t i = m_plan.batches[b].begin; i < m_plan.batches[b].end; ++i) {
			Pass* p = GetPass(submitSeq[i]);
			if (p == nullptr || p->get_type() == Pass::Host)
				continue;
			auto accumulate = [&](const std::vector<ResourceUse>& uses) {
				for (const auto& use : uses) {
					if (hostBlocks.count(blockOf(use.resourceId)) == 0)
						continue;
					const UsageSync us = UsageSyncOf(use.usage, p->get_type(), use.stages);
					if (us.stage != rhi::PipelineStage::None && us.stage != rhi::PipelineStage::Host)
						mask = mask | us.stage;
				}
			};
			accumulate(p->get_topology().reads);
			accumulate(p->get_topology().writes);
		}
		m_batchHostStage[b] = mask != rhi::PipelineStage::None ? mask : rhi::PipelineStage::AllCommands;
	}
}

HostContext RenderGraph::MakeHostContext(uint32_t passId) {
	std::vector<HostContext::Upstream> upstream;
	auto it = m_hostSync.find(passId);
	if (it != m_hostSync.end()) {
		for (uint32_t b : it->second.gpuBatches) {
			if (b >= m_batchTimelineValue.size() || m_batchTimelineValue[b] == 0 || b >= m_plan.batches.size())
				continue;
			upstream.push_back(HostContext::Upstream{
				m_graphicDevice->QueueTimeline(m_plan.batches[b].queue), m_batchTimelineValue[b] });
		}
		for (uint32_t hp : it->second.hostPasses) {
			auto vit = m_hostSignalValue.find(hp);
			if (vit != m_hostSignalValue.end() && m_hostTimeline)
				upstream.push_back(HostContext::Upstream{ m_hostTimeline.get(), vit->second });
		}
	}
	const std::uint64_t signalValue = m_hostSignalValue.count(passId) ? m_hostSignalValue[passId] : 0;

	HostContext::Bindings bindings;
	if (Pass* pass = GetPass(passId)) {
		auto collect = [&](const ResourceUse& use, bool writable) {
			Resource* res = GetResource(use.resourceId);
			if (res == nullptr || res->get_type() != Resource::Buffer)
				return;
			const rhi::Buffer* handle = static_cast<const Buffer*>(res)->get_rhi();
			if (handle == nullptr)
				return;
			if (writable) {
				bindings.write.emplace(res->get_name(), const_cast<rhi::Buffer*>(handle));
				if (std::find(bindings.flush.begin(), bindings.flush.end(), handle) == bindings.flush.end())
					bindings.flush.push_back(handle);
			}
			else {
				bindings.read.emplace(res->get_name(), handle);
			}
			if (std::find(bindings.invalidate.begin(), bindings.invalidate.end(), handle) == bindings.invalidate.end())
				bindings.invalidate.push_back(handle);
		};
		for (const auto& use : pass->get_topology().reads)
			collect(use, false);
		for (const auto& use : pass->get_topology().writes)
			collect(use, true);
	}

	auto hs = m_hostImageStaging.find(passId);
	if (hs != m_hostImageStaging.end()) {
		for (const auto& h : hs->second) {
			Resource* stg = GetResource(h.stagingId);
			if (stg == nullptr || stg->get_type() != Resource::Buffer)
				continue;
			const rhi::Buffer* handle = static_cast<const Buffer*>(stg)->get_rhi();
			if (handle == nullptr)
				continue;
			if (h.read)
				bindings.readImages.emplace(h.imageName, handle);
			if (h.write)
				bindings.writeImages.emplace(h.imageName, const_cast<rhi::Buffer*>(handle));
		}
	}

	return HostContext(m_hostTimeline.get(), signalValue, std::move(upstream), std::move(bindings));
}

void RenderGraph::LaunchHostWorkers(uint32_t batchIndex) {
	if (batchIndex >= m_plan.batches.size())
		return;
	const QueueBatch& batch = m_plan.batches[batchIndex];

	std::vector<std::pair<const HostPass*, HostContext>> jobs;
	for (uint32_t i = batch.begin; i < batch.end; ++i) {
		Pass* p = GetPass(submitSeq[i]);
		if (p == nullptr || p->get_type() != Pass::Host)
			continue;
		jobs.emplace_back(static_cast<const HostPass*>(p), MakeHostContext(p->get_id()));
	}
	if (jobs.empty())
		return;

	m_hostThreads.emplace_back([jobs = std::move(jobs)]() mutable {
		for (auto& job : jobs) {
			const std::function<void(HostContext&)>& cb = job.first->get_callback();
			if (cb) cb(job.second);
			if (!job.second.Signaled()) job.second.SignalGpu();
		}
	});
}

void RenderGraph::RunHostPassesInline(uint32_t batchIndex) {
	if (batchIndex >= m_plan.batches.size())
		return;
	const QueueBatch& batch = m_plan.batches[batchIndex];
	for (uint32_t i = batch.begin; i < batch.end; ++i) {
		Pass* p = GetPass(submitSeq[i]);
		if (p == nullptr || p->get_type() != Pass::Host)
			continue;
		HostContext ctx = MakeHostContext(p->get_id());
		const std::function<void(HostContext&)>& cb = static_cast<const HostPass*>(p)->get_callback();
		if (cb) cb(ctx);
		if (!ctx.Signaled()) ctx.SignalGpu();
	}
}

void RenderGraph::ResolveAccessRanges() {
	m_imageAccessRanges.clear();

	std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> seen;
	auto addRange = [&](uint32_t passId, uint32_t resourceId, rhi::ImageSubresourceRange range) {
		Resource* res = GetResource(resourceId);
		if (res == nullptr || res->get_type() != Resource::Image)
			return;
		Image* img = static_cast<Image*>(res);
		const ImageDesc& d = img->get_desc();
		if (!rhi::NormalizeImageSubresourceRange(range, d.mipLevels, d.arrayLayers)) {
			Pass* p = GetPass(passId);
			std::cerr << "RenderGraph: pass \"" << (p ? p->get_name() : "?")
				<< "\" 对图像 \"" << img->get_name() << "\" 的访问范围越界" << std::endl;
			return;
		}
		const auto key = std::make_tuple(passId, resourceId, range.base.mipLevel,
			range.mipLevelCount, range.base.layer, range.layerCount);
		if (!seen.insert(key).second)
			return;
		m_imageAccessRanges.push_back(ImageAccessUse{ passId, resourceId,
			img->get_topology().memoryBlockId, range });
	};

	auto viewRangeOf = [](const Image& img) {
		return rhi::ImageSubresourceRange{ img.get_view(), 1, 1 };
	};

	auto bindingArrayCount = [](Pass* p, uint32_t resourceId) -> uint32_t {
		auto findIn = [resourceId](const std::vector<ShaderBinding>& bindings) -> uint32_t {
			for (const auto& b : bindings)
				if (b.resourceId.has_value() && *b.resourceId == resourceId)
					return std::max(1u, b.arrayCount.value_or(1));
			return 1;
		};
		if (p->get_type() == Pass::Render)
			return findIn(static_cast<RenderPass*>(p)->get_desc().shaderBindings);
		if (p->get_type() == Pass::Compute)
			return findIn(static_cast<ComputePass*>(p)->get_desc().shaderBindings);
		return 1;
	};

	for (auto& holder : passPool) {
		Pass* p = holder.get();
		if (p == nullptr)
			continue;
		const uint32_t passId = p->get_id();
		const PassTopology& topo = p->get_topology();

		if (p->get_type() == Pass::Transfer) {
			const TransferPassDesc& d = static_cast<TransferPass*>(p)->get_desc();
			if (d.kind == TransferKind::ClearImage) {
				if (d.destId.has_value())
					addRange(passId, *d.destId, d.clearRange);
				continue;
			}
			const rhi::ImageSubresourceRange whole{ {}, 1, 0 };
			if (d.kind == TransferKind::BufferToImage) {
				if (d.destId.has_value()) {
					if (d.bufferImageRegions.empty())
						addRange(passId, *d.destId, whole);
					else
						for (const auto& r : d.bufferImageRegions)
							addRange(passId, *d.destId, r.subresource);
				}
			}
			else if (d.kind == TransferKind::ImageToBuffer) {
				if (d.sourceId.has_value()) {
					if (d.bufferImageRegions.empty())
						addRange(passId, *d.sourceId, whole);
					else
						for (const auto& r : d.bufferImageRegions)
							addRange(passId, *d.sourceId, r.subresource);
				}
			}
			else if (d.kind == TransferKind::ImageToImage) {
				if (d.imageRegions.empty() && d.blitRegions.empty()) {
					if (d.sourceId.has_value()) addRange(passId, *d.sourceId, whole);
					if (d.destId.has_value()) addRange(passId, *d.destId, whole);
				}
				else {
					if (d.sourceId.has_value())
						for (const auto& r : d.imageRegions)
							addRange(passId, *d.sourceId, r.srcSubresource);
					if (d.destId.has_value())
						for (const auto& r : d.imageRegions)
							addRange(passId, *d.destId, r.dstSubresource);
					if (d.sourceId.has_value())
						for (const auto& r : d.blitRegions)
							addRange(passId, *d.sourceId, r.srcSubresource);
					if (d.destId.has_value())
						for (const auto& r : d.blitRegions)
							addRange(passId, *d.destId, r.dstSubresource);
				}
			}
			continue;
		}

		if (p->get_type() == Pass::Present) {
			for (const auto& use : topo.reads)
				addRange(passId, use.resourceId, rhi::ImageSubresourceRange{ {}, 0, 0 });
			continue;
		}

		auto addUse = [&](const ResourceUse& use) {
			if (use.range.has_value()) {
				addRange(passId, use.resourceId, *use.range);
				return;
			}
			Resource* res = GetResource(use.resourceId);
			if (res == nullptr || res->get_type() != Resource::Image)
				return;
			Image* img = static_cast<Image*>(res);
			const rhi::ImageSubresourceRange range = viewRangeOf(*img);
			addRange(passId, use.resourceId, range);
			const uint32_t count = bindingArrayCount(p, use.resourceId);
			if (count > 1) {
				rhi::ImageSubresourceRange arrayRange = range;
				arrayRange.layerCount = count;
				addRange(passId, use.resourceId, arrayRange);
			}
		};
		if (p->get_type() == Pass::Render) {
			const RenderPassDesc& d = static_cast<RenderPass*>(p)->get_desc();
			std::set<uint32_t> covered;
			for (const auto& att : d.attachments) {
				if (!att.resourceId.has_value())
					continue;
				Resource* res = GetResource(*att.resourceId);
				if (res == nullptr || res->get_type() != Resource::Image)
					continue;
				Image* img = static_cast<Image*>(res);
				rhi::ImageSubresourceRange range = att.range.has_value()
					? *att.range : viewRangeOf(*img);
				addRange(passId, *att.resourceId, range);
				covered.insert(*att.resourceId);
			}
			for (const auto& use : topo.reads)
				if (covered.count(use.resourceId) == 0) addUse(use);
			for (const auto& use : topo.writes)
				if (covered.count(use.resourceId) == 0) addUse(use);
			continue;
		}

		for (const auto& use : topo.reads) addUse(use);
		for (const auto& use : topo.writes) addUse(use);
	}
}

void RenderGraph::ReportAccessRangeMismatches() const {
	size_t mismatches = 0;
	for (const ImageAccessUse& use : m_imageAccessRanges) {
		Resource* res = GetResource(use.resourceId);
		if (res == nullptr || res->get_type() != Resource::Image)
			continue;
		const Image* img = static_cast<const Image*>(res);
		rhi::ImageSubresourceRange barrierRange = img->get_barrier_range();
		Pass* p = GetPass(use.passId);
		if (!rhi::NormalizeImageSubresourceRange(barrierRange, img->get_desc().mipLevels,
				img->get_desc().arrayLayers)) {
			++mismatches;
			std::cerr << "RenderGraph: 图像 \"" << img->get_name() << "\" 的 barrierRange 越界" << std::endl;
			continue;
		}
		if (rhi::ImageSubresourceRangeContains(barrierRange, use.range))
			continue;
		++mismatches;
		std::cerr << "RenderGraph: 子资源访问超出 barrierRange：pass \""
			<< (p ? p->get_name() : "?") << "\" 资源 \"" << img->get_name()
			<< "\" access(mip " << use.range.base.mipLevel << "+" << use.range.mipLevelCount
			<< ", layer " << use.range.base.layer << "+" << use.range.layerCount
			<< ") barrier(mip " << barrierRange.base.mipLevel << "+" << barrierRange.mipLevelCount
			<< ", layer " << barrierRange.base.layer << "+" << barrierRange.layerCount << ")"
			<< std::endl;
	}
	if (mismatches > 0)
		std::cerr << "RenderGraph: 访问范围超出节点 barrierRange " << mismatches << "/"
			<< m_imageAccessRanges.size() << "（节点声明范围小于实际访问）" << std::endl;
}

void RenderGraph::CompileBarriers() {
	CompileBarriersSubresource();
	RefreshAllSlotDescriptorSets();
}

void RenderGraph::CompileBarriersSubresource() {
	ResolveAliasGroups();
	ResolveAccessRanges();
	m_barriers.clear();
	m_firstFrameHeads.clear();
	m_crossFrameHeads.clear();
	m_passImageLayouts.clear();
	if (submitSeq.empty()) {
		ReportAccessRangeMismatches();
		return;
	}
	BuildSubmitPlan();

	struct BarrierUsePoint {
		uint32_t     passId = 0;
		UsageSync    sync;
		bool         write = false;
		Pass::Type   passType = Pass::None;
	};

	struct Partition {
		uint32_t mip = 0;
		uint32_t layerStart = 0;
		uint32_t layerCount = 0;
	};

	std::map<std::pair<uint32_t, uint32_t>, std::vector<rhi::ImageSubresourceRange>> accessByPassRes;
	for (const auto& a : m_imageAccessRanges)
		accessByPassRes[{ a.passId, a.resourceId }].push_back(a.range);

	struct MuxProxyUse {
		uint32_t              candidateId = 0;
		uint32_t              passId = 0;
		rhi::ImageSubresourceRange range {};
		Usage                 usage = Usage::Auto;
		bool                  write = false;
		rhi::ShaderStageMask  stages = rhi::ShaderStageMask::None;
	};
	std::set<uint32_t> muxOutputIds;
	std::map<std::pair<uint32_t, uint32_t>, std::vector<MuxProxyUse>> proxyByPassRes;
	std::map<std::pair<uint32_t, uint32_t>, rhi::ImageLayout> proxyLayouts;

	for (const MuxEntry& mux : m_muxes) {
		if (mux.outputType != Resource::Image)
			continue;
		muxOutputIds.insert(mux.outputResId);
		for (uint32_t passId : submitSeq) {
			Pass* p = GetPass(passId);
			if (p == nullptr || p->get_id() == mux.passId)
				continue;
			const PassTopology& topo = p->get_topology();
			auto scan = [&](const std::vector<ResourceUse>& uses, bool isWrite) {
				for (const auto& use : uses) {
					if (use.resourceId != mux.outputResId)
						continue;
					if (use.usage == Usage::MuxPrepare || use.usage == Usage::MuxSelect)
						continue;
					auto accIt = accessByPassRes.find({ passId, mux.outputResId });
					if (accIt == accessByPassRes.end())
						continue;
					const UsageSync sync = UsageSyncOf(use.usage, p->get_type(), use.stages);
					const auto layoutKey = std::pair<uint32_t, uint32_t>{ passId, mux.outputResId };
					auto lit = proxyLayouts.find(layoutKey);
					if (lit == proxyLayouts.end())
						proxyLayouts.emplace(layoutKey, sync.layout);
					else if (lit->second != sync.layout)
						lit->second = rhi::ImageLayout::General;

					for (const auto& range : accIt->second) {
						for (uint32_t candId : mux.alternativeIds) {
							proxyByPassRes[{ passId, candId }].push_back(
								MuxProxyUse{ candId, passId, range, use.usage, isWrite, use.stages });
							accessByPassRes[{ passId, candId }].push_back(range);
						}
					}
				}
			};
			scan(topo.reads, false);
			scan(topo.writes, true);
		}
	}
	for (const auto& kv : proxyLayouts)
		m_passImageLayouts[kv.first] = kv.second;

	std::set<uint32_t> warnedCrossFrameBlocks;
	auto emitChain = [&](Resource* owner, bool isImage, const std::vector<BarrierUsePoint>& chain,
		const rhi::ImageSubresourceRange* range) {
		if (chain.empty())
			return;
		auto setRange = [range](GraphImageBarrier& b) {
			if (range != nullptr) b.range = *range;
		};
		const UsageSync first = chain.front().sync;
		const rhi::ImageLayout initLayout = owner->get_topology().initialLayout;
		const bool crossFrame = owner->get_topology().crossFrame;

		if (!crossFrame) {
			if (isImage && initLayout != first.layout) {
				GraphImageBarrier b;
				b.image = static_cast<Image*>(owner);
				b.oldLayout = initLayout;
				b.newLayout = first.layout;
				b.srcStage = rhi::PipelineStage::None;
				b.srcAccess = rhi::Access::None;
				b.dstStage = first.stage;
				b.dstAccess = first.access;
				setRange(b);
				m_barriers[chain.front().passId].images.push_back(b);
			}
		}
		else {
			size_t firstIdx = 0;
			size_t lastIdx = chain.size() - 1;
			if (isImage) {
				while (firstIdx < chain.size() && chain[firstIdx].passType == Pass::Host)
					++firstIdx;
				while (lastIdx > firstIdx && chain[lastIdx].passType == Pass::Host)
					--lastIdx;
				if (firstIdx >= chain.size())
					return;
			}
			const UsageSync firstUse = chain[firstIdx].sync;
			const UsageSync lastUse = chain[lastIdx].sync;
			const bool firstWrite = chain[firstIdx].write;
			const bool lastWrite = chain[lastIdx].write;

			if (isImage && initLayout != firstUse.layout) {
				GraphImageBarrier b;
				b.image = static_cast<Image*>(owner);
				b.oldLayout = initLayout;
				b.newLayout = firstUse.layout;
				b.srcStage = rhi::PipelineStage::None;
				b.srcAccess = rhi::Access::None;
				b.dstStage = firstUse.stage;
				b.dstAccess = firstUse.access;
				setRange(b);
				m_firstFrameHeads[chain[firstIdx].passId].images.push_back(b);
			}

			const bool steadyNeed = lastWrite || firstWrite
				|| (isImage && lastUse.layout != firstUse.layout);
			if (steadyNeed) {
				const uint32_t atPass = chain[firstIdx].passId;
				if (isImage) {
					GraphImageBarrier b;
					b.image = static_cast<Image*>(owner);
					b.oldLayout = lastUse.layout;
					b.newLayout = firstUse.layout;
					b.srcStage = lastUse.stage;
					b.srcAccess = lastUse.access;
					b.dstStage = firstUse.stage;
					b.dstAccess = firstUse.access;
					setRange(b);
					m_crossFrameHeads[atPass].images.push_back(b);
				}
				else {
					GraphBufferBarrier b;
					b.buffer = static_cast<Buffer*>(owner);
					b.srcStage = lastUse.stage;
					b.srcAccess = lastUse.access;
					b.dstStage = firstUse.stage;
					b.dstAccess = firstUse.access;
					m_crossFrameHeads[atPass].buffers.push_back(b);
				}
			}

			uint32_t firstGpuPass = static_cast<uint32_t>(-1);
			uint32_t lastGpuPass = static_cast<uint32_t>(-1);
			for (size_t k = 0; k < chain.size(); ++k)
				if (chain[k].passType != Pass::Host) { firstGpuPass = chain[k].passId; break; }
			for (size_t k = chain.size(); k-- > 0;)
				if (chain[k].passType != Pass::Host) { lastGpuPass = chain[k].passId; break; }
			if (firstGpuPass != static_cast<uint32_t>(-1) && lastGpuPass != static_cast<uint32_t>(-1)) {
				const uint32_t firstBatch = m_passBatch[firstGpuPass];
				const uint32_t lastBatch = m_passBatch[lastGpuPass];
				if (firstBatch != static_cast<uint32_t>(-1) && lastBatch != static_cast<uint32_t>(-1)
					&& m_plan.batches[lastBatch].queue != m_plan.batches[firstBatch].queue
					&& warnedCrossFrameBlocks.insert(owner->get_id()).second) {
					std::cerr << "RenderGraph: 跨帧资源 \"" << owner->get_name()
						<< "\" 跨队列存活，帧间依赖需外部保证串行（WaitForFrame 或单帧在飞）"
						<< std::endl;
				}
			}
		}

		for (size_t i = 0; i + 1 < chain.size(); ++i) {
			const UsageSync prev = chain[i].sync;
			const UsageSync next = chain[i + 1].sync;
			const bool need = chain[i].write || chain[i + 1].write
				|| (isImage && prev.layout != next.layout);
			if (!need)
				continue;
			const uint32_t prevPassId = chain[i].passId;
			const uint32_t atPass = chain[i + 1].passId;
			const uint32_t prevBatch = prevPassId < m_passBatch.size() ? m_passBatch[prevPassId] : 0;

			if (chain[i + 1].passType == Pass::Host) {
				if (chain[i].passType == Pass::Host)
					continue;
				BarrierBatch& tail = m_plan.batches[prevBatch].releases;
				if (isImage) {
					GraphImageBarrier b;
					b.image = static_cast<Image*>(owner);
					b.oldLayout = prev.layout;
					b.newLayout = prev.layout;
					b.srcStage = prev.stage;
					b.srcAccess = prev.access;
					b.dstStage = rhi::PipelineStage::Host;
					b.dstAccess = rhi::Access::HostRead;
					setRange(b);
					tail.images.push_back(b);
				}
				else {
					GraphBufferBarrier b;
					b.buffer = static_cast<Buffer*>(owner);
					b.srcStage = prev.stage;
					b.srcAccess = prev.access;
					b.dstStage = rhi::PipelineStage::Host;
					b.dstAccess = rhi::Access::HostRead;
					tail.buffers.push_back(b);
				}
				continue;
			}

			const uint32_t nextBatch = atPass < m_passBatch.size() ? m_passBatch[atPass] : 0;
			const bool crossQueue = prevBatch != nextBatch
				&& m_plan.batches[prevBatch].queue != m_plan.batches[nextBatch].queue;

			if (!crossQueue) {
				if (isImage) {
					GraphImageBarrier b;
					b.image = static_cast<Image*>(owner);
					b.oldLayout = prev.layout;
					b.newLayout = next.layout;
					b.srcStage = prev.stage;
					b.srcAccess = prev.access;
					b.dstStage = next.stage;
					b.dstAccess = next.access;
					setRange(b);
					m_barriers[atPass].images.push_back(b);
				}
				else {
					GraphBufferBarrier b;
					b.buffer = static_cast<Buffer*>(owner);
					b.srcStage = prev.stage;
					b.srcAccess = prev.access;
					b.dstStage = next.stage;
					b.dstAccess = next.access;
					m_barriers[atPass].buffers.push_back(b);
				}
				continue;
			}

			const uint32_t prevFamily = m_graphicDevice ? m_graphicDevice->QueueFamily(m_plan.batches[prevBatch].queue)
				: rhi::kQueueFamilyIgnored;
			const uint32_t nextFamily = m_graphicDevice ? m_graphicDevice->QueueFamily(m_plan.batches[nextBatch].queue)
				: rhi::kQueueFamilyIgnored;
			const bool familyTransfer = prevFamily != nextFamily;

			BarrierBatch& release = m_plan.batches[prevBatch].releases;
			BarrierBatch& acquire = m_barriers[atPass];

			if (isImage) {
				GraphImageBarrier b;
				b.image = static_cast<Image*>(owner);
				b.oldLayout = prev.layout;
				b.newLayout = prev.layout;
				b.srcStage = prev.stage;
				b.srcAccess = prev.access;
				b.dstStage = rhi::PipelineStage::AllCommands;
				b.dstAccess = rhi::Access::None;
				if (familyTransfer) {
					b.srcQueueFamily = prevFamily;
					b.dstQueueFamily = nextFamily;
				}
				setRange(b);
				release.images.push_back(b);

				GraphImageBarrier a = b;
				a.newLayout = next.layout;
				a.srcStage = next.stage;
				a.srcAccess = rhi::Access::None;
				a.dstStage = next.stage;
				a.dstAccess = next.access;
				acquire.images.push_back(a);
			}
			else {
				GraphBufferBarrier b;
				b.buffer = static_cast<Buffer*>(owner);
				b.srcStage = prev.stage;
				b.srcAccess = prev.access;
				b.dstStage = rhi::PipelineStage::AllCommands;
				b.dstAccess = rhi::Access::None;
				if (familyTransfer) {
					b.srcQueueFamily = prevFamily;
					b.dstQueueFamily = nextFamily;
				}
				release.buffers.push_back(b);

				GraphBufferBarrier a = b;
				a.srcStage = next.stage;
				a.srcAccess = rhi::Access::None;
				a.dstStage = next.stage;
				a.dstAccess = next.access;
				acquire.buffers.push_back(a);
			}

			uint32_t spIndex = static_cast<uint32_t>(-1);
			for (uint32_t k = 0; k < m_plan.syncPoints.size(); ++k) {
				if (m_plan.syncPoints[k].producerBatch == prevBatch
					&& m_plan.syncPoints[k].consumerBatch == nextBatch) {
					spIndex = k;
					break;
				}
			}
			if (spIndex == static_cast<uint32_t>(-1)) {
				m_plan.syncPoints.push_back(SyncPoint{ prevBatch, nextBatch });
				spIndex = static_cast<uint32_t>(m_plan.syncPoints.size() - 1);
				m_plan.batches[prevBatch].signals.push_back(spIndex);
				m_plan.batches[nextBatch].waits.push_back(spIndex);
			}
			m_plan.syncPoints[spIndex].consumerStage =
				m_plan.syncPoints[spIndex].consumerStage | next.stage;
		}
	};

	auto buildBlockChain = [&](const std::vector<Resource*>& members) {
		std::vector<BarrierUsePoint> chain;
		for (uint32_t passId : submitSeq) {
			Pass* p = GetPass(passId);
			if (p == nullptr)
				continue;
			const PassTopology& topo = p->get_topology();
			const ResourceUse* readUse = nullptr;
			const ResourceUse* writeUse = nullptr;
			for (Resource* m : members) {
				const uint32_t id = m->get_id();
				for (const auto& use : topo.writes)
					if (use.resourceId == id) { writeUse = &use; break; }
				if (writeUse != nullptr)
					break;
				if (readUse == nullptr)
					for (const auto& use : topo.reads)
						if (use.resourceId == id) { readUse = &use; break; }
			}
			if (writeUse == nullptr && readUse == nullptr)
				continue;
			UsageSync merged = writeUse != nullptr
				? UsageSyncOf(writeUse->usage, p->get_type(), writeUse->stages)
				: UsageSync{};
			if (readUse != nullptr) {
				const UsageSync r = UsageSyncOf(readUse->usage, p->get_type(), readUse->stages);
				merged.stage = merged.stage | r.stage;
				merged.access = merged.access | r.access;
				if (writeUse == nullptr)
					merged.layout = r.layout;
				else if (merged.layout != r.layout)
					merged.layout = rhi::ImageLayout::General;
			}
			if (readUse != nullptr && readUse->usage == Usage::MuxPrepare) {
				merged.stage = merged.stage | rhi::PipelineStage::AllCommands;
				merged.access = merged.access | rhi::Access::MemoryRead;
			}
			chain.push_back(BarrierUsePoint{ passId, merged, writeUse != nullptr, p->get_type() });
		}
		return chain;
	};

	std::map<uint32_t, std::vector<Resource*>> blocks;
	for (auto& holder : resourcePool) {
		Resource* res = holder.get();
		if (res != nullptr)
			blocks[res->m_topo.memoryBlockId].push_back(res);
	}

	for (auto& entry : blocks) {
		const uint32_t blockId = entry.first;
		const std::vector<Resource*>& members = entry.second;

		Resource* owner = nullptr;
		for (Resource* m : members)
			if (m->get_id() == blockId) { owner = m; break; }
		if (owner == nullptr)
			owner = members.front();
		const bool isImage = owner->get_type() == Resource::Image;
		if (muxOutputIds.count(blockId) != 0)
			continue;
		const bool realized = isImage ? static_cast<Image*>(owner)->get_rhi() != nullptr
			: static_cast<Buffer*>(owner)->get_rhi() != nullptr;
		if (!realized)
			continue;

		if (!isImage) {
			auto chain = buildBlockChain(members);
			emitChain(owner, false, chain, nullptr);
			continue;
		}

		std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> layerIntervals;
		for (const auto& a : m_imageAccessRanges) {
			if (a.blockId != blockId)
				continue;
			const uint32_t mipEnd = a.range.base.mipLevel + a.range.mipLevelCount;
			for (uint32_t mip = a.range.base.mipLevel; mip < mipEnd; ++mip)
				layerIntervals[mip].push_back(
					{ a.range.base.layer, a.range.base.layer + a.range.layerCount });
		}
		if (layerIntervals.empty())
			continue;

		std::vector<Partition> partitions;
		for (auto& kv : layerIntervals) {
			auto list = kv.second;
			std::sort(list.begin(), list.end());
			std::vector<std::pair<uint32_t, uint32_t>> merged;
			for (const auto& iv : list) {
				if (!merged.empty() && iv.first <= merged.back().second)
					merged.back().second = std::max(merged.back().second, iv.second);
				else
					merged.push_back(iv);
			}
			for (const auto& iv : merged)
				partitions.push_back(Partition{ kv.first, iv.first, iv.second - iv.first });
		}

		auto intersects = [](const rhi::ImageSubresourceRange& r, uint32_t mip,
			uint32_t layerStart, uint32_t layerCount) {
			const uint32_t mipEnd = r.base.mipLevel + r.mipLevelCount;
			const uint32_t layerEnd = r.base.layer + r.layerCount;
			return mip >= r.base.mipLevel && mip < mipEnd
				&& layerStart < layerEnd && r.base.layer < layerStart + layerCount;
		};

		for (const Partition& part : partitions) {
			std::vector<BarrierUsePoint> chain;
			for (uint32_t passId : submitSeq) {
				Pass* p = GetPass(passId);
				if (p == nullptr)
					continue;
				const PassTopology& topo = p->get_topology();
				bool touched = false;
				bool write = false;
				bool layoutKnown = false;
				rhi::ImageLayout layout = rhi::ImageLayout::Undefined;
				UsageSync merged{};
				std::set<uint32_t> touchedMembers;

				for (Resource* m : members) {
					const uint32_t id = m->get_id();
					auto it = accessByPassRes.find({ passId, id });
					if (it == accessByPassRes.end())
						continue;
					bool touches = false;
					for (const auto& r : it->second)
						if (intersects(r, part.mip, part.layerStart, part.layerCount)) {
							touches = true;
							break;
						}
					if (!touches)
						continue;
					touched = true;
					touchedMembers.insert(id);

					auto applySync = [&](const UsageSync& s) {
						merged.stage = merged.stage | s.stage;
						merged.access = merged.access | s.access;
						if (!layoutKnown) { layout = s.layout; layoutKnown = true; }
						else if (layout != s.layout) layout = rhi::ImageLayout::General;
					};

					for (const auto& use : topo.reads) {
						if (use.resourceId != id)
							continue;
						UsageSync s = UsageSyncOf(use.usage, p->get_type(), use.stages);
						if (use.usage == Usage::MuxPrepare)
							s.layout = rhi::ImageLayout::General;
						applySync(s);
					}
					for (const auto& use : topo.writes) {
						if (use.resourceId != id)
							continue;
						applySync(UsageSyncOf(use.usage, p->get_type(), use.stages));
						write = true;
					}

					auto proxyIt = proxyByPassRes.find({ passId, id });
					if (proxyIt != proxyByPassRes.end()) {
						for (const auto& pu : proxyIt->second) {
							if (!intersects(pu.range, part.mip, part.layerStart, part.layerCount))
								continue;
							applySync(UsageSyncOf(pu.usage, p->get_type(), pu.stages));
							if (pu.write)
								write = true;
						}
					}
				}

				if (!touched)
					continue;
				const rhi::ImageLayout passLayout = layoutKnown ? layout : rhi::ImageLayout::Undefined;
				for (uint32_t id : touchedMembers)
					m_passImageLayouts[{ passId, id }] = passLayout;
				merged.layout = passLayout;
				chain.push_back(BarrierUsePoint{ passId, merged, write, p->get_type() });
			}
			const rhi::ImageSubresourceRange partRange{ { part.mip, part.layerStart }, 1, part.layerCount };
			emitChain(owner, true, chain, &partRange);
		}
	}

	for (const auto& edge : m_orderEdges) {
		const uint32_t fromPass = edge.first;
		const uint32_t toPass = edge.second;
		const uint32_t fromBatch = m_passBatch[fromPass];
		const uint32_t toBatch = m_passBatch[toPass];
		if (fromBatch == toBatch)
			continue;
		const rhi::QueueClass fromQueue = passPool[fromPass]->get_topology().queue;
		const rhi::QueueClass toQueue = passPool[toPass]->get_topology().queue;
		if (fromQueue == toQueue)
			continue;

		uint32_t spIndex = static_cast<uint32_t>(-1);
		for (uint32_t k = 0; k < m_plan.syncPoints.size(); ++k) {
			if (m_plan.syncPoints[k].producerBatch == fromBatch
				&& m_plan.syncPoints[k].consumerBatch == toBatch) {
				spIndex = k;
				break;
			}
		}
		if (spIndex == static_cast<uint32_t>(-1)) {
			m_plan.syncPoints.push_back(SyncPoint{ fromBatch, toBatch });
			spIndex = static_cast<uint32_t>(m_plan.syncPoints.size() - 1);
			m_plan.batches[fromBatch].signals.push_back(spIndex);
			m_plan.batches[toBatch].waits.push_back(spIndex);
		}
		m_plan.syncPoints[spIndex].consumerStage =
			m_plan.syncPoints[spIndex].consumerStage | rhi::PipelineStage::AllCommands;
	}

	ReportAccessRangeMismatches();
}

#pragma endregion

void RenderGraph::RecordGraph(rhi::CommandList& cmd) {
	if (m_plan.batches.size() > 1) {
		std::cerr << "RenderGraph: 计划含 " << m_plan.batches.size()
			<< " 个队列批，请改用 RecordBatch（RecordGraph 仅支持单批）" << std::endl;
		return;
	}
	if (m_plan.batches.empty())
		return;
	RecordBatch(cmd, 0);
	m_firstFramePending = false;
}

static void ResolveBarrierBatch(const RenderGraph::BarrierBatch& src,
	std::vector<rhi::ImageBarrier>& images, std::vector<rhi::BufferBarrier>& buffers)
{
	images.clear();
	buffers.clear();
	images.reserve(src.images.size());
	buffers.reserve(src.buffers.size());
	for (const RenderGraph::GraphImageBarrier& g : src.images) {
		const rhi::Image* handle = g.image != nullptr ? g.image->get_rhi() : nullptr;
		if (handle == nullptr)
			continue;
		const ImageDesc& d = g.image->get_desc();
		rhi::ImageBarrier b;
		b.image = const_cast<rhi::Image*>(handle);
		const uint32_t maxMip = std::max(1u, d.mipLevels);
		const uint32_t maxLayer = std::max(1u, d.arrayLayers);
		ImageSubresourceRange range {};
		if (g.range.has_value()) {
			range = *g.range;
			if (!rhi::NormalizeImageSubresourceRange(range, maxMip, maxLayer)) {
				std::cerr << "[RenderGraph] 屏障子资源范围越界，已跳过: " << g.image->get_name() << std::endl;
				continue;
			}
		}
		else {
			range = g.image->get_barrier_range();
			range.base.mipLevel = std::min(range.base.mipLevel, maxMip - 1);
			range.base.layer = std::min(range.base.layer, maxLayer - 1);
			if (range.mipLevelCount == 0) range.mipLevelCount = maxMip - range.base.mipLevel;
			if (range.layerCount == 0) range.layerCount = maxLayer - range.base.layer;
			range.mipLevelCount = std::min(range.mipLevelCount, maxMip - range.base.mipLevel);
			range.layerCount = std::min(range.layerCount, maxLayer - range.base.layer);
		}
		b.range = range;
		b.oldLayout = g.oldLayout;
		b.newLayout = g.newLayout;
		b.srcStage = g.srcStage;
		b.dstStage = g.dstStage;
		b.srcAccess = g.srcAccess;
		b.dstAccess = g.dstAccess;
		b.srcQueueFamily = g.srcQueueFamily;
		b.dstQueueFamily = g.dstQueueFamily;
		images.push_back(b);
	}
	for (const RenderGraph::GraphBufferBarrier& g : src.buffers) {
		const rhi::Buffer* handle = g.buffer != nullptr ? g.buffer->get_rhi() : nullptr;
		if (handle == nullptr)
			continue;
		rhi::BufferBarrier b;
		b.buffer = const_cast<rhi::Buffer*>(handle);
		b.srcStage = g.srcStage;
		b.dstStage = g.dstStage;
		b.srcAccess = g.srcAccess;
		b.dstAccess = g.dstAccess;
		b.srcQueueFamily = g.srcQueueFamily;
		b.dstQueueFamily = g.dstQueueFamily;
		buffers.push_back(b);
	}
}

void RenderGraph::RecordBatch(rhi::CommandList& cmd, uint32_t batchIndex) {
	if (batchIndex >= m_plan.batches.size())
		return;
	const QueueBatch& batch = m_plan.batches[batchIndex];
	std::vector<rhi::ImageBarrier> images;
	std::vector<rhi::BufferBarrier> buffers;
	auto emit = [&](const BarrierBatch& src) {
		if (src.images.empty() && src.buffers.empty())
			return;
		ResolveBarrierBatch(src, images, buffers);
		if (!images.empty() || !buffers.empty())
			cmd.Barrier(images, buffers);
	};
	for (uint32_t i = batch.begin; i < batch.end; ++i) {
		const uint32_t passId = submitSeq[i];
		Pass* pass = GetPass(passId);
		if (pass == nullptr)
			continue;
		const std::map<uint32_t, BarrierBatch>& frameHeads =
			m_firstFramePending ? m_firstFrameHeads : m_crossFrameHeads;
		auto head = frameHeads.find(passId);
		if (head != frameHeads.end())
			emit(head->second);
		auto it = m_barriers.find(passId);
		if (it != m_barriers.end())
			emit(it->second);
		RecordPass(cmd, pass);
	}
	emit(batch.releases);
}

void RenderGraph::SubmitGraph(const FrameSync& sync) {
	if (m_plan.batches.empty() || m_graphicDevice == nullptr)
		return;

	WaitHostPasses();

	uint32_t presentBatch = static_cast<uint32_t>(-1);
	if (sync.acquireWait != nullptr || sync.presentReady != nullptr) {
		for (auto& p : passPool) {
			if (p != nullptr && p->get_type() == Pass::Present) {
				presentBatch = m_passBatch[p->get_id()];
				break;
			}
		}
		if (presentBatch == static_cast<uint32_t>(-1))
			std::cerr << "RenderGraph: FrameSync 需要图中存在 Present 节点" << std::endl;
	}

	m_hostSignalValue.clear();
	for (uint32_t passId : m_hostPassOrder)
		m_hostSignalValue[passId] = ++m_hostTimelineValue;
	m_batchHostWait.assign(m_plan.batches.size(), 0);
	for (uint32_t b = 0; b < m_batchHostPasses.size(); ++b)
		for (uint32_t passId : m_batchHostPasses[b])
			m_batchHostWait[b] = std::max(m_batchHostWait[b], m_hostSignalValue[passId]);

	if (!m_hostPassOrder.empty() && !m_hostTimeline)
		m_hostTimeline = m_graphicDevice->CreateTimelineSemaphore(0);

	const bool timelineSync = m_graphicDevice->QueueTimeline(rhi::QueueClass::Graphics) != nullptr;
	const uint32_t submitSlot = m_currentSlot < m_frameSlots.size() ? m_currentSlot : 0;
	FrameSlotState& slotState = m_frameSlots[submitSlot];
	if (!timelineSync) {
		while (slotState.crossQueueSemaphores.size() < m_plan.syncPoints.size())
			slotState.crossQueueSemaphores.push_back(m_graphicDevice->CreateSemaphore());
	}

	m_batchTimelineValue.assign(m_plan.batches.size(), 0);

	for (uint32_t b = 0; b < m_plan.batches.size(); ++b) {
		const QueueBatch& batch = m_plan.batches[b];
		if (!m_batchHasGpu[b]) {
			if (m_hostTimeline)
				LaunchHostWorkers(b);
			else if (!m_hostPassOrder.empty())
				RunHostPassesInline(b);
			continue;
		}

		rhi::CommandList cmd = m_graphicDevice->BeginCommandList(batch.queue);
		RecordBatch(cmd, b);

		if (!m_hostTimeline && !m_hostPassOrder.empty())
			RunHostPassesInline(b);

		std::vector<rhi::SemaphoreWait> waits;
		for (uint32_t sp : batch.waits) {
			const SyncPoint& point = m_plan.syncPoints[sp];
			const uint32_t producer = point.producerBatch;
			const rhi::PipelineStage waitStage = point.consumerStage != rhi::PipelineStage::None
				? point.consumerStage : rhi::PipelineStage::AllCommands;
			if (timelineSync && producer < m_batchTimelineValue.size() && m_batchTimelineValue[producer] != 0) {
				rhi::Semaphore* timeline = m_graphicDevice->QueueTimeline(m_plan.batches[producer].queue);
				if (timeline != nullptr) {
					waits.push_back(rhi::SemaphoreWait{ timeline, waitStage,
						m_batchTimelineValue[producer] });
					continue;
				}
			}
			if (sp < slotState.crossQueueSemaphores.size() && slotState.crossQueueSemaphores[sp])
				waits.push_back(rhi::SemaphoreWait{ slotState.crossQueueSemaphores[sp].get(), waitStage });
		}
		if (m_hostTimeline && m_batchHostWait[b] != 0) {
			const rhi::PipelineStage hostWaitStage =
				b < m_batchHostStage.size() && m_batchHostStage[b] != rhi::PipelineStage::None
				? m_batchHostStage[b] : rhi::PipelineStage::AllCommands;
			waits.push_back(rhi::SemaphoreWait{ m_hostTimeline.get(), hostWaitStage, m_batchHostWait[b] });
		}

		std::vector<rhi::SemaphoreSignal> signals;
		if (!timelineSync) {
			for (uint32_t sp : batch.signals) {
				if (sp < slotState.crossQueueSemaphores.size() && slotState.crossQueueSemaphores[sp])
					signals.push_back(rhi::SemaphoreSignal{ slotState.crossQueueSemaphores[sp].get(), rhi::PipelineStage::AllCommands });
			}
		}
		if (b == presentBatch) {
			if (sync.acquireWait != nullptr)
				waits.push_back(rhi::SemaphoreWait{ sync.acquireWait, rhi::PipelineStage::ColorAttachmentOut });
			if (sync.presentReady != nullptr)
				signals.push_back(rhi::SemaphoreSignal{ sync.presentReady, rhi::PipelineStage::AllCommands });
		}

		rhi::SubmitInfo info;
		info.queue = batch.queue;
		info.commandLists = { &cmd, 1 };
		info.waits = waits;
		info.signals = signals;
		m_batchTimelineValue[b] = m_graphicDevice->Submit(info);

		if (m_hostTimeline)
			LaunchHostWorkers(b);
	}
	m_firstFramePending = false;
}

void RenderGraph::WaitHostPasses() {
	for (auto& t : m_hostThreads) {
		if (t.joinable())
			t.join();
	}
	m_hostThreads.clear();
}

void RenderGraph::RetargetBorrowedImage(const std::string& name, const rhi::Image* external) {
	if (external == nullptr)
		return;
	Resource* res = nullptr;
	for (auto& r : resourcePool)
		if (r != nullptr && r->get_name() == name) { res = r.get(); break; }
	if (res == nullptr || res->get_type() != Resource::Image)
		HandleError(ErrorCode::ResourceNotRegistered);
	auto* img = static_cast<Image*>(res);
	if (!img->is_borrowed())
		HandleError(ErrorCode::ResourceTypeDescMismatch);
	if (img->mExternal == external)
		return;

	const rhi::ImageCreateInfo& d = external->Desc();
	rhi::ImageSubresourceRange viewRange{ img->get_view(), 1, 1 };
	rhi::ImageSubresourceRange barrierRange = img->get_barrier_range();
	if (!rhi::NormalizeImageSubresourceRange(viewRange, d.mipLevels, d.arrayLayers)
		|| !rhi::NormalizeImageSubresourceRange(barrierRange, d.mipLevels, d.arrayLayers)
		|| !rhi::ImageSubresourceRangeContains(barrierRange, viewRange)) {
		std::cerr << "RenderGraph: RetargetBorrowedImage \"" << name
			<< "\" 的 view/barrierRange 超出新图像范围，已拒绝换绑" << std::endl;
		return;
	}

	img->mExternal = external;
	img->mDesc.width = d.width;
	img->mDesc.height = d.height;
	img->mDesc.depth = d.depth;
	img->mDesc.arrayLayers = d.arrayLayers;
	img->mDesc.mipLevels = d.mipLevels;
	img->mDesc.samples = d.samples;
	img->mDesc.format = d.format;
	img->mDesc.viewType = d.viewType;
}

void RenderGraph::RetargetBorrowedBuffer(const std::string& name, const rhi::Buffer* external) {
	if (external == nullptr)
		return;
	Resource* res = nullptr;
	for (auto& r : resourcePool)
		if (r != nullptr && r->get_name() == name) { res = r.get(); break; }
	if (res == nullptr || res->get_type() != Resource::Buffer)
		HandleError(ErrorCode::ResourceNotRegistered);
	auto* buf = static_cast<Buffer*>(res);
	if (!buf->is_borrowed())
		HandleError(ErrorCode::ResourceTypeDescMismatch);
	if (buf->mExternal == external)
		return;

	const rhi::BufferCreateInfo& d = external->Desc();
	const BufferDesc& od = buf->get_desc();
	if (od.size != d.size || od.usage != d.usage || od.hostVisible != d.hostVisible) {
		std::cerr << "RenderGraph: RetargetBorrowedBuffer \"" << name
			<< "\" 的描述与候选不一致，已拒绝换绑" << std::endl;
		return;
	}
	buf->mExternal = external;
	buf->mDesc.size = d.size;
	buf->mDesc.usage = d.usage;
	buf->mDesc.hostVisible = d.hostVisible;
}

void RenderGraph::RealizeAllResources() {
	for (auto& r : resourcePool)
		RealizeResource(r.get());
}

void RenderGraph::RealizeAllPasses() {
	for (auto& p : passPool)
		RealizePass(p.get());
}

void RenderGraph::RealizeAll() {
	ResolveAliasGroups();
	EnsureFrameSlots(FrameSlotCount());
	for (auto& slot : m_frameSlots)
		slot.passSets.resize(passPool.size());
	RealizeAllResources();
	ApplyMuxSelection(0);
	RealizeAllPasses();
	CompileBarriers();
}

Resource* RenderGraph::GetResource(uint32_t resourceId) const {
	if (resourceId >= resourcePool.size())
		return nullptr;
	return resourcePool[resourceId].get();
}

Pass* RenderGraph::GetPass(uint32_t passId) const {
	if (passId >= passPool.size())
		return nullptr;
	return passPool[passId].get();
}

#pragma endregion
