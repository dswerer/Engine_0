#include "RenderGraphAbstracted.h"
#include "RenderGraphNodes.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <stack>
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

// 手动分桶别名的拒绝原因：只用于日志（整组降级为非别名，进程继续运行，不新增 fatal 错误码）
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

	// 解析：回填 resourceId，并把 Auto usage 推断为具体值
	for (auto& att : desc.attachments) {
		att.resourceId = resolve(att.resourceName);
		if (att.usage == Usage::Auto) {
			att.usage = IsDepthFormat(formatOf(att.resourceName))
				? Usage::DepthStencilAttachment : Usage::ColorAttachment;
		}
	}
	for (auto& sb : desc.shaderBindings) {
		sb.resourceId = resolve(sb.resourceName);
		if (sb.usage == Usage::Auto)
			sb.usage = (typeOf(sb.resourceName) == Resource::Buffer)
				? Usage::UniformRead : Usage::SampledRead; // 按资源类型推断默认用途
	}

	// 顶点/索引流：回填资源身份，并确认是 Buffer 节点（固定功能输入，不是描述符绑定）
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

	// 校验
	ValidateAttachments(read, write, desc);
	ValidateShaderBindings(read, write, desc.shaderBindings);
	ValidateDrawBindings(read, desc);
	ValidateBindingConflict(desc);
}

void RenderGraph::ResolveComputePassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	ComputePassDesc& desc)
{
	if (desc.shader.stage != rhi::ShaderStage::Compute)
		throw ErrorCode::PassTypeDescMismatch; // 计算节点只接受 compute 阶段

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

	// 计算侧没有附件，Auto 按资源类型推断读用途（storage 读/写需显式声明，与写集校验联动）
	for (auto& sb : desc.shaderBindings) {
		sb.resourceId = resolve(sb.resourceName);
		if (sb.usage == Usage::Auto)
			sb.usage = (typeOf(sb.resourceName) == Resource::Buffer)
				? Usage::UniformRead : Usage::SampledRead;
	}
	ValidateShaderBindings(read, write, desc.shaderBindings);

	// 间接派发：参数 Buffer 须已注册、类型正确，并登记在读集（拓扑边才能把它排到正确位置）
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
			// 拷贝/blit 都要求同格式；拷贝还要求等尺寸（缩放只能走 blit）
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
	// 两端即拓扑边：源在读集、目的在写集，barrier compiler 才有据可依
	if (read.count(*desc.sourceId) == 0)
		throw ErrorCode::TransferReadSetMismatch;
	if (write.count(*desc.destId) == 0)
		throw ErrorCode::TransferWriteSetMismatch;
}

void RenderGraph::ResolvePresentPassDesc(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	const PresentPassDesc& desc)
{
	(void)desc;
	// 呈现只转换一个 presentable image 的布局：恰好一条读边，且不能声明写边
	if (read.size() != 1 || !write.empty())
		throw ErrorCode::PresentReadSetMismatch;
	// 资源池此时尚未实例化，类型从注册暂存表按 id 核对
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

	// 颜色槽须稠密 0..n-1：Realize 派生管线格式签名按 slot 就位，缺口会留下格式空洞
	if (!colorSlots.empty() && *colorSlots.rbegin() + 1 != colorSlots.size())
		throw ErrorCode::ColorSlotNotDense;
}

void RenderGraph::ValidateShaderBindings(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	const std::vector<ShaderBinding>& bindings)
{
	// set/binding 已改由 SPIR-V 反射分配（RealizePass 回填），注册期不再校验
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
	// 资源类型已在 ResolvePassDesc 校验（必须是 Buffer）；这里只管拓扑一致：
	// 顶点/索引流是渲染期读，资源须在 read 集，否则 barrier 顺序与读写边无据可依
	for (const auto& vs : desc.vertexStreams) {
		if (read.count(*vs.resourceId) == 0)
			throw ErrorCode::DrawReadSetMismatch;
	}
	if (desc.indexStream.has_value() && read.count(*desc.indexStream->resourceId) == 0)
		throw ErrorCode::DrawReadSetMismatch;

	// 缺省布局（两向量均空）隐含单流 binding 0：多于一条流无法表达，提前拦下
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

void RenderGraph::RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write, Pass::Type type, PassDesc desc, rhi::QueueClass queue) {
	if (type == Pass::None) HandleError(ErrorCode::PassTypeUnspecified);
	if (!tmp_passes.has_value()) {
		tmp_passes.emplace();
	}
	reg_tmp_pass rtp;
	rtp.id = ++this->pass_top_id;
	rtp.to_read_name = read;
	rtp.to_write_name = write;
	rtp.type = type;
	rtp.queue = queue;
	rtp.desc = std::move(desc);
	tmp_passes.value()[name] = rtp;
}

void RenderGraph::RegisterResource(std::string name, std::set<std::string> reader, std::string writer,Resource::Type type,ResourceDesc desc) {
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
	tmp_resources.value()[name] = rtr;
}

void RenderGraph::RegisterResource(std::string name, std::set<std::string> reader, std::string writer,
	const rhi::Image* external, ImageSubresource view, rhi::ImageLayout initialLayout) {
	if (external == nullptr) HandleError(ErrorCode::ResourceNotRegistered); // 借用路径必须给出有效句柄
	if (!tmp_resources.has_value()) {
		tmp_resources.emplace();
	}
	reg_tmp_resource rtr;
	rtr.id = ++this->resource_top_id;
	rtr.readers_name = reader;
	rtr.writer_name = writer;
	rtr.type = Resource::Image;
	rtr.view = view;
	rtr.externalImage = external;
	rtr.initialLayout = initialLayout;
	tmp_resources.value()[name] = rtr;
}

void RenderGraph::RegisterResource(std::string name, std::set<std::string> reader, std::string writer,
	const rhi::Buffer* external) {
	if (external == nullptr) HandleError(ErrorCode::ResourceNotRegistered); // 借用路径必须给出有效句柄
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

// 图像格式的每像素字节数（Host staging 的紧打包尺寸估算；未知格式返回 0）
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

// mip0 全层紧打包尺寸（与 vkCmdCopyImageToBuffer 的缺省 rowLength 一致）
static uint64_t StagingBytesFor(const ImageDesc& d) {
	const uint64_t w = d.width;
	const uint64_t h = d.height == 0 ? 1 : d.height;
	const uint64_t z = d.depth == 0 ? 1 : d.depth;
	const uint64_t layers = d.arrayLayers == 0 ? 1 : d.arrayLayers;
	const uint64_t bpp = BytesPerPixel(d.format);
	return w * h * z * layers * bpp;
}

void RenderGraph::ProcessRegister() {
	passPool.resize(tmp_passes.value().size());
	resourcePool.resize(tmp_resources.value().size());
	m_extraEdges.clear();
	m_hostImageStaging.clear();

	for (auto& pass : tmp_passes.value()) {
		uint32_t id = pass.second.id;
		std::string name = pass.first;
		Pass::Type type = pass.second.type;
		PassDesc desc = std::move(pass.second.desc);

		// 名字 -> 资源 id（未注册的名字跳过并提示）
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

		// 解析 desc（回填 resourceId、Auto 推断 usage）并做声明期校验
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

		// 组拓扑：读/写边从解析后的 desc 回填 usage（无 desc 的 pass 边暂为 Auto，compiler 阶段保守处理）
		PassTopology topo;
		topo.id = id;
		topo.name = name;
		topo.queue = tmp_passes.value().at(name).queue;
		auto usageOf = [&desc](uint32_t resId, bool isWrite) -> Usage {
			if (auto* rpd = std::get_if<RenderPassDesc>(&desc)) {
				for (auto& att : rpd->attachments)
					if (att.resourceId && *att.resourceId == resId) return att.usage;
				for (auto& sb : rpd->shaderBindings)
					if (sb.resourceId && *sb.resourceId == resId) return sb.usage;
				// 固定功能输入通道：读语义同样记入拓扑边，供 compiler 映射 VERTEX_INPUT / INDEX_READ
				for (auto& vs : rpd->vertexStreams)
					if (vs.resourceId && *vs.resourceId == resId) return Usage::VertexInputRead;
				if (rpd->indexStream && rpd->indexStream->resourceId
					&& *rpd->indexStream->resourceId == resId)
					return Usage::IndexRead;
			}
			else if (auto* cpd = std::get_if<ComputePassDesc>(&desc)) {
				for (auto& sb : cpd->shaderBindings)
					if (sb.resourceId && *sb.resourceId == resId) return sb.usage;
				// 间接派发参数：读语义单独成类，barrier 映射到 DRAW_INDIRECT/INDIRECT_COMMAND_READ
				if (cpd->indirectBufferId && *cpd->indirectBufferId == resId) return Usage::IndirectRead;
			}
			else if (auto* tpd = std::get_if<TransferPassDesc>(&desc)) {
				// 源读、目的写，与资源是 Buffer 还是 Image 无关；与 ResolveTransferPassDesc 的校验互为印证
				if (tpd->sourceId && *tpd->sourceId == resId) return Usage::TransferRead;
				if (tpd->destId && *tpd->destId == resId) return Usage::TransferWrite;
			}
			else if (std::get_if<PresentPassDesc>(&desc)) {
				// 读边即呈现收尾：布局转 PresentSrc（barrier compiler 据此产出转换）
				return isWrite ? Usage::Auto : Usage::PresentSrc;
			}
			else if (std::get_if<HostPassDesc>(&desc)) {
				// Host 节点纯 CPU：写边是宿主机写、读边是宿主机读（均由 HOST 阶段表达）
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
			|| resource.second.externalBuffer != nullptr); // 借用节点：生命周期不在图内
		topo.initialLayout = resource.second.initialLayout; // 链首布局（Own 缺省 Undefined）
		// 跨帧声明来自资源描述（借用节点不做帧间持久，其生命周期由外部管理）
		topo.crossFrame = false;
		if (auto* id = std::get_if<ImageDesc>(&resource.second.desc))
			topo.crossFrame = id->crossFrame;
		else if (auto* bd = std::get_if<BufferDesc>(&resource.second.desc))
			topo.crossFrame = bd->crossFrame;
		// 缺省未分桶：内存块即自身；AliasResources 的组在 RealizeAll 前解析并可能改写
		topo.aliasGroup = static_cast<uint32_t>(-1);
		topo.memoryBlockId = id;
		resourcePool[id] = Resource::AutoAllocator(std::move(topo), type, std::move(resource.second.desc),
			resource.second.view, resource.second.externalImage, resource.second.externalBuffer);
	}

	// Host 段的图像读写：把图像换成隐式 staging 中转（必须在拓扑/提交序生成前完成）
	SynthesizeHostImageStaging();

	tmp_passes.reset();
	tmp_resources.reset();
}

void RenderGraph::SynthesizeHostImageStaging()
{
	// 收集「Host 段 × 图像」的读写需求（同一段同一图像合并，读改写共用一个 staging）
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
		const uint64_t bytes = StagingBytesFor(desc);
		if (bytes == 0) {
			std::cerr << "RenderGraph: Host 段 \"" << u.hostName << "\" 的读/写图像 \"" << u.imageName
				<< "\" 尺寸或格式无效，跳过 staging 中转" << std::endl;
			continue;
		}

		// 中转 Buffer：hostVisible + TransferSrc/Dst（GPU 与 CPU 双向搬运）
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

		// 读回：image -> staging（排在 Host 段前，由 Kahn 依赖自然落位）
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
			passPool.push_back(Pass::AutoAllocator(std::move(tp), Pass::Transfer, td));
		}

		// 写回：staging -> image（排在 Host 段后）
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
			passPool.push_back(Pass::AutoAllocator(std::move(tp), Pass::Transfer, td));
		}

		// Host 段拓扑改为只碰 staging：图像布局完全由中转 Pass 承担
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

		// 图像侧的图关系：Host 不再是它的直接读者/writer，由中转 Pass 代替
		auto& imgTopo = imageRes->m_topo;
		imgTopo.readers.erase(u.hostId);
		if (u.read && readTransferId != static_cast<uint32_t>(-1)) {
			// writer 是本 Host 段时（跨帧读改写）不能把读中转登记为读者，否则写中转会反向依赖它；
			// 该情形读中转无入边，按帧首顺序读取上一帧内容
			if (imgTopo.writer != u.hostId)
				imgTopo.readers.insert(readTransferId);
		}
		if (u.write && writeTransferId != static_cast<uint32_t>(-1)) {
			if (imgTopo.writer == u.hostId) {
				// Host 是该图像的生产者：写中转接替 writer，下游读者自然排在其后
				imgTopo.writer = writeTransferId;
			}
			else {
				// 读改写（图像另有生产者）：读中转已排在原生产者后，写中转需显式连到下游读者
				for (uint32_t reader : imgTopo.readers) {
					if (reader == u.hostId || reader == readTransferId)
						continue;
					Pass* rp = GetPass(reader);
					if (rp != nullptr && rp->get_type() == Pass::Host)
						continue; // 其它 Host 段经自己的中转接入，避免与它们的读边成环
					m_extraEdges.emplace_back(writeTransferId, reader);
				}
			}
		}

		// staging 的归属：写优先（GPU 批据此等主机 timeline），读中转/主机段的先后用额外边表达
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
	//获取邻接表
	for (auto& pResource : resourcePool) {
		uint32_t writer = pResource.get()->m_topo.writer;
		if (writer == static_cast<uint32_t>(-1)) {
			continue;//无writer
		}
		for (uint32_t reader : pResource.get()->m_topo.readers) {
			if (reader == writer) {
				throw 0;
				continue;//跳过自环
			}
			auto& edges = AdjList[writer];
			if (std::find(edges.begin(), edges.end(), reader) == edges.end()) {
				edges.push_back(reader);
				indegree[reader]++;
			}
		}
	}
	// 合成节点的额外依赖边（Host 图像 staging 的读中转 -> Host、Host -> 写中转）
	for (const auto& e : m_extraEdges) {
		if (e.first >= AdjList.size() || e.second >= AdjList.size() || e.first == e.second)
			continue;
		auto& edges = AdjList[e.first];
		if (std::find(edges.begin(), edges.end(), e.second) == edges.end()) {
			edges.push_back(e.second);
			indegree[e.second]++;
		}
	}
	//kahn
	std::vector<uint32_t> queue;
	for (size_t i = 0; i < indegree.size(); i++) {
		if (indegree[i] == 0) {
			queue.push_back(i);
			indegree[i] = -1;
		}
	}
	for (size_t i = 0; i < queue.size(); i++) {
		for (uint32_t desc : AdjList[queue[i]]) {
			indegree[desc]--;
			if (indegree[desc] == 0) {
				indegree[desc] = -1;
				queue.push_back(desc);
			}
		}
		submitSeq.push_back(queue[i]);
	}
	for (int deg : indegree) {
		if (deg > 0) {
			throw 1;
		}
	}

}

void RenderGraph::AliasResources(std::vector<std::string> names) {
	m_aliasRequests.push_back(std::move(names));
}

void RenderGraph::ResolveAliasGroups() {
	if (m_aliasResolved)
		return; // 分桶只解析一次（RealizeAll 先跑，CompileBarriers 再进这里是空操作）
	m_aliasResolved = true;
	if (m_aliasRequests.empty())
		return;

	// 寿命校验按「批」判定是否同时存活，需要批划分（未生成提交序时批为空，只做静态校验）
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

		// ---- 1. 按名解析成员（组内成员必须已注册且尚未加入别的组）----
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

		// ---- 2. 静态校验：别名要求同类型 + 同描述 + 同首态 + 同生命周期类别 + 非借用 ----
		if (reason == AliasRejectReason::None) {
			const Resource* first = members.front();
			for (Resource* r : members) {
				if (r->get_type() != first->get_type()) {
					reason = AliasRejectReason::TypeMismatch; break;
				}
				if (r->m_topo.external) {
					reason = AliasRejectReason::BorrowedResource; break; // 借用句柄不能与图内内存同块
				}
				if (r->m_topo.crossFrame != first->m_topo.crossFrame) {
					reason = AliasRejectReason::CrossFrameMismatch; break;
				}
				if (r->m_topo.initialLayout != first->m_topo.initialLayout) {
					reason = AliasRejectReason::DescMismatch; break; // 共享布局链首必须一致
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
					// Buffer 的创建位/内存类型不参与并集，必须整组一致才能共享句柄
					if (a.size != b.size || a.usage != b.usage || a.hostVisible != b.hostVisible) {
						reason = AliasRejectReason::DescMismatch; break;
					}
				}
			}
		}

		// ---- 3. 寿命校验：同一内存块不能在同一批里同时存活（批级区间两两不相交）----
		// 批内执行顺序不跨命令缓冲保证别名内存的复用边界，故以「批」为最小存活粒度；
		// 不在任何 pass 使用的成员没有寿命，不参与相交判定
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
			degrade(reason); // 整组降级：成员保持 aliasGroup=-1 / memoryBlockId=自身 id
			continue;
		}

		// ---- 4. 通过：整组归入同一内存块，代表取最小 id（Realize 由代表创建句柄，其余借用）----
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
	// 缓冲创建位是位标志：拼成 "Vertex|Index|Uniform" 形式
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
				// set/binding 注册期为空（反射回填在 Realize 后），未回填时打印占位
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

#pragma region Realize // 节点实例化：描述层 -> RHI 后端

void RenderGraph::SetDevice(rhi::Device* graphic) {
	m_graphicDevice = graphic;
}

// usage 边 -> RHI 创建位。Auto 边来自无 desc 的 pass（当前即 ComputePass）：
// 读边保守展开 Sampled|Storage（两种 compute 访问都可能），写边 Storage。
// ResolveSrc/Dst 按渲染期内解析处理：源以颜色附件身份被读，目的被写
static rhi::ImageUsage UsageToImageBits(Usage u, bool isWrite) {
	switch (u) {
	case Usage::ColorAttachment:        return rhi::ImageUsage::ColorAttachment;
	case Usage::DepthStencilAttachment: return rhi::ImageUsage::DepthStencil;
	case Usage::SampledRead:            return rhi::ImageUsage::Sampled;
	case Usage::StorageRead:            return rhi::ImageUsage::Storage;
	case Usage::StorageWrite:           return rhi::ImageUsage::Storage;
	case Usage::UniformRead:            return rhi::ImageUsage::Storage; // 仅 Buffer 有意义；误用于图像时 RealizeShaderSets 会报类型不匹配
	case Usage::VertexInputRead:
	case Usage::IndexRead:
	case Usage::IndirectRead:           return rhi::ImageUsage::None;    // 仅 Buffer 有意义，永不进入图像创建位
	case Usage::ResolveSrc:             return rhi::ImageUsage::ColorAttachment;
	case Usage::ResolveDst:             return rhi::ImageUsage::ColorAttachment;
	case Usage::TransferRead:           return rhi::ImageUsage::TransferSrc;  // 仅 Buffer 有意义，图像为误用时保留能力位
	case Usage::TransferWrite:          return rhi::ImageUsage::TransferDst;
	case Usage::HostWrite:              return rhi::ImageUsage::None;         // 仅 Buffer 有意义（宿主机写）
	case Usage::HostRead:               return rhi::ImageUsage::None;         // 仅 Buffer 有意义（宿主机读）
	case Usage::PresentSrc:             return rhi::ImageUsage::None;         // 呈现不需要创建位
	case Usage::Auto:
	default:
		return isWrite ? rhi::ImageUsage::Storage
			: (rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage);
	}
}

rhi::ImageUsage RenderGraph::DeriveImageUsage(uint32_t memoryBlockId) const {
	// 别名组成员共享同一底层 image，创建位必须是块内所有成员访问的并集
	std::set<uint32_t> memberIds;
	for (auto& r : resourcePool)
		if (r != nullptr && r->m_topo.memoryBlockId == memoryBlockId)
			memberIds.insert(r->get_id());

	rhi::ImageUsage bits = rhi::ImageUsage::None;
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
	}
	return bits;
}

void RenderGraph::RealizeResource(Resource* resource) {
	if (m_graphicDevice == nullptr)
		HandleError(ErrorCode::DeviceNotSet);

	switch (resource->get_type()) {
	case Resource::Image: {
		auto* img = static_cast<Image*>(resource);
		// 别名成员（非内存块代表）：借用代表节点创建的句柄（描述已由组校验保证一致）
		if (img->m_topo.memoryBlockId != img->get_id()) {
			Resource* owner = GetResource(img->m_topo.memoryBlockId);
			if (owner == nullptr || owner->get_type() != Resource::Image)
				HandleError(ErrorCode::ResourceTypeDescMismatch);
			RealizeResource(owner); // 代表先就绪（组校验保证不是借用句柄）
			img->mExternal = static_cast<Image*>(owner)->get_rhi();
			return;
		}
		if (img->is_borrowed())
			return; // 借用节点：句柄与 usage 均由外部负责，图侧不创建、不汇总
		if (img->m_rhiHandle)
			return; // 幂等：已实例化直接返回
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
		info.usage = DeriveImageUsage(img->m_topo.memoryBlockId); // 内存块访问并集回填，用户不必手填
		img->m_rhiHandle = m_graphicDevice->CreateImage(info);
		break;
	}
	case Resource::Buffer: {
		auto* buf = static_cast<Buffer*>(resource);
		// 别名成员：借用代表节点的缓冲句柄
		if (buf->m_topo.memoryBlockId != buf->get_id()) {
			Resource* owner = GetResource(buf->m_topo.memoryBlockId);
			if (owner == nullptr || owner->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceTypeDescMismatch);
			RealizeResource(owner);
			buf->mExternal = static_cast<Buffer*>(owner)->get_rhi();
			return;
		}
		if (buf->is_borrowed())
			return; // 借用节点：句柄由外部创建/管理
		if (buf->m_rhiHandle)
			return; // 幂等：已实例化直接返回
		const BufferDesc& d = buf->get_desc();
		rhi::BufferCreateInfo info;
		info.size = d.size;
		info.usage = d.usage; // 显式创建位（图的 Usage 边尚无 buffer 语义，暂不派生）
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
			return; // 幂等：已实例化直接返回
		const RenderPassDesc& desc = rp->get_desc();

		// ---- 管线状态：来自 PipelineDesc（shader / 固定功能）----
		rhi::GraphicsPipelineDesc info;
		info.shaders           = desc.pipeline.shaders;
		info.depthTestEnable   = desc.pipeline.depthTestEnable;
		info.depthWriteEnable  = desc.pipeline.depthWriteEnable;
		info.depthCompareOp    = desc.pipeline.depthCompareOp;
		info.cullMode          = desc.pipeline.cullMode;
		info.blendEnable       = desc.pipeline.blendEnable;
		info.pushConstantSize  = desc.pipeline.pushConstantSize;
		// 顶点输入布局属于管线状态：拓扑/绑定/属性从 PipelineDesc 原样带入（流序号与 vertexStreams 对齐）
		info.topology         = desc.pipeline.topology;
		info.vertexBindings   = desc.pipeline.vertexBindings;
		info.vertexAttributes = desc.pipeline.vertexAttributes;

		// ---- 附件格式签名：由图从附件派生（图的价值点，用户不必手填）----
		// 颜色按 slot 就位（注册期已校验稠密），深度取 DepthStencil 附件；
		// 格式取节点 ImageDesc——经 RHI 同一映射，与实例化图像的原生格式一致
		for (const auto& att : desc.attachments) {
			Resource* res = GetResource(*att.resourceId);
			if (!res)
				HandleError(ErrorCode::ResourceNotRegistered);
			if (res->get_type() != Resource::Image)
				HandleError(ErrorCode::ResourceTypeDescMismatch); // 附件只能挂 Image 节点
			RealizeResource(res); // 幂等：附件资源先于管线就绪，解除对 RealizeAll 调用顺序的依赖
			if (att.usage == Usage::ColorAttachment) {
				info.colorFormats[att.slot] = static_cast<Image*>(res)->get_desc().format;
				info.colorAttachmentCount = std::max(info.colorAttachmentCount, att.slot + 1);
			}
			else if (att.usage == Usage::DepthStencilAttachment) {
				info.depthFormat = static_cast<Image*>(res)->get_desc().format;
			}
			// ResolveSrc/Dst 渲染期内解析，不进管线格式签名
		}

		rp->m_rhiHandle = m_graphicDevice->CreatePipeline(info);
		if (!rp->m_rhiHandle)
			HandleError(ErrorCode::PipelineCreationFailed); // 详细原因已由 RHI 打印

		RealizeShaderSets(*rp->m_rhiHandle, rp->mDesc.shaderBindings, rp->m_sets);
		break;
	}
	case Pass::Compute: {
		auto* cp = static_cast<ComputePass*>(pass);
		if (cp->m_rhiHandle)
			return; // 幂等

		rhi::ComputePipelineDesc info;
		info.shader = cp->mDesc.shader;
		info.pushConstantSize = cp->mDesc.pushConstantSize;
		cp->m_rhiHandle = m_graphicDevice->CreateComputePipeline(info);
		if (!cp->m_rhiHandle)
			HandleError(ErrorCode::PipelineCreationFailed); // 详细原因已由 RHI 打印

		RealizeShaderSets(*cp->m_rhiHandle, cp->mDesc.shaderBindings, cp->m_sets);
		break;
	}
	case Pass::Host:
	case Pass::Transfer:
	case Pass::Present:
		// 纯 CPU 节点 / 拷贝通道 / 呈现收尾：不创建管线与描述符集，Realize 为空操作
		break;
	default:
		HandleError(ErrorCode::PassTypeUnspecified);
	}
}

void RenderGraph::RealizeShaderSets(rhi::Pipeline& pipeline, std::vector<ShaderBinding>& shaderBindings,
	std::vector<std::unique_ptr<rhi::DescriptorSet>>& sets)
{
	// ---- 反射对账：管线布局（RHI 反射产出，携带变量名）× shaderBindings，按「变量名 == 资源名」匹配 ----
	// 反射变量无图资源 = 漏接；图资源未被反射命中 = 声明了 shader 不用的绑定；
	// 命中后按 资源类型×usage 推导期望类型与反射类型比对，回填 set/binding，
	// 并按反射的 set 顺序分配描述符集、写入内容（句柄来自节点，元素数来自反射）
	uint32_t setCount = 0;
	while (pipeline.SetLayout(setCount) != nullptr)
		++setCount;
	sets.resize(setCount);
	for (uint32_t s = 0; s < setCount; ++s) {
		auto set = m_graphicDevice->AllocateDescriptorSet(*pipeline.SetLayout(s));
		if (!set)
			HandleError(ErrorCode::DescriptorSetAllocationFailed); // 详细原因已由 RHI 打印
		sets[s] = std::move(set);
	}

	for (uint32_t s = 0; s < setCount; ++s) {
		for (const auto& db : pipeline.SetLayout(s)->Bindings()) {
			auto it = std::find_if(shaderBindings.begin(), shaderBindings.end(),
				[&db](const ShaderBinding& sb) { return sb.resourceName == db.name; });
			if (it == shaderBindings.end())
				HandleError(ErrorCode::ShaderVariableUnbound); // 反射变量在图中无同名资源

			Resource* res = GetResource(*it->resourceId);
			if (!res)
				HandleError(ErrorCode::ResourceNotRegistered);
			rhi::ResourceType expected;
			switch (res->get_type()) {
			case Resource::Image:
				if (it->usage == Usage::UniformRead)
					HandleError(ErrorCode::ShaderBindingTypeMismatch); // 图像没有 uniform 语义
				expected = (it->usage == Usage::SampledRead)
					? rhi::ResourceType::SampledImage : rhi::ResourceType::StorageImage;
				break;
			case Resource::Buffer:
				if (it->usage == Usage::SampledRead)
					HandleError(ErrorCode::ShaderBindingTypeMismatch); // Buffer 无采样语义
				expected = (it->usage == Usage::UniformRead)
					? rhi::ResourceType::UniformBuffer : rhi::ResourceType::StorageBuffer;
				break;
			default:
				HandleError(ErrorCode::ResourceTypeUnspecified);
			}
			if (expected != db.type)
				HandleError(ErrorCode::ShaderBindingTypeMismatch); // 如 SampledRead 却反射出 StorageImage
			it->set = db.set;
			it->binding = db.binding;

			// 写绑定内容：图像以节点自带的子资源视图为起点，数组按 layer 递增铺满反射出的长度
			switch (res->get_type()) {
			case Resource::Image: {
				auto* img = static_cast<Image*>(res);
				RealizeResource(res); // 幂等：确保句柄就绪（借用节点为空操作）
				if (!img->get_rhi())
					HandleError(ErrorCode::ResourceNotRegistered);
				sets[s]->Write(db.binding, *img->get_rhi(), img->get_view(), db.arrayCount);
				break;
			}
			case Resource::Buffer: {
				auto* buf = static_cast<Buffer*>(res);
				RealizeResource(res); // 幂等：确保句柄就绪（借用节点为空操作）
				if (!buf->get_rhi())
					HandleError(ErrorCode::ResourceNotRegistered);
				// 窗口按绑定声明；range=0 表示到 buffer 末尾（数组按该 range 步进，元素数取反射长度）
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
			HandleError(ErrorCode::ShaderBindingNameNotFound); // 图声明的资源未被 shader 使用
	}
}

void RenderGraph::RecordPass(rhi::CommandList& cmd, Pass* pass) {
	switch (pass->get_type()) {
	case Pass::Compute: {
		auto* cp = static_cast<ComputePass*>(pass);
		const rhi::Pipeline* pso = cp->get_rhi();
		if (pso == nullptr)
			HandleError(ErrorCode::PipelineCreationFailed); // 未 Realize 就录制
		cmd.BindPipeline(*pso);
		for (uint32_t s = 0; cp->get_set(s) != nullptr; ++s)
			cmd.BindDescriptorSet(s, *cp->get_set(s));
		const ComputePassDesc& d = cp->get_desc();
		// 间接派发：工作组数来自参数 Buffer（须已 Realize，barrier 已把它转到 IndirectRead 可读状态）
		if (!d.indirectBuffer.empty() && d.indirectBufferId.has_value()) {
			Resource* argsRes = GetResource(*d.indirectBufferId);
			if (argsRes == nullptr || argsRes->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(argsRes); // 幂等
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
			HandleError(ErrorCode::PipelineCreationFailed); // 未 Realize 就录制
		const RenderPassDesc& d = rp->get_desc();

		// 附件按 slot 就位（注册期已校验颜色槽稠密）：颜色连续数组，深度单列。
		// layout 由调用方的 barrier 预先转换，这里按用途填约定布局
		std::array<rhi::RenderingAttachment, kMaxColorAttachments> colors {};
		uint32_t colorCount = 0;
		rhi::RenderingAttachment depth {};
		bool hasDepth = false;
		for (const auto& att : d.attachments) {
			Resource* res = GetResource(*att.resourceId);
			if (res == nullptr || res->get_type() != Resource::Image)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(res); // 幂等
			Image* img = static_cast<Image*>(res);
			const rhi::Image* raw = img->get_rhi();
			if (raw == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);

			rhi::RenderingAttachment ra;
			ra.image  = const_cast<rhi::Image*>(raw); // BeginRendering 内只读（建视图）
			ra.view   = img->get_view();
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
			// ResolveSrc/Dst：RHI 的 dynamic rendering 尚无 resolve 附件字段，跳过
		}

		// 渲染区域与视口：autoFromColor 取首个颜色附件尺寸（槽 0 必在；无颜色时退深度附件）
		const rhi::Image* sized = nullptr;
		if (colorCount > 0) sized = colors[0].image;
		else if (hasDepth) sized = depth.image;
		uint32_t width = sized ? sized->Desc().width : 0;
		uint32_t height = sized ? sized->Desc().height : 0;

		rhi::RenderingInfo info;
		info.colors = colors.data();
		info.colorCount = colorCount;
		info.depth = hasDepth ? &depth : nullptr;
		info.renderArea.width = width;
		info.renderArea.height = height;

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
		cmd.SetViewport(vp);
		for (uint32_t s = 0; rp->get_set(s) != nullptr; ++s)
			cmd.BindDescriptorSet(s, *rp->get_set(s));

		// 顶点流：序号即管线的 binding index（布局在 PipelineDesc 的 vertexBindings）
		for (uint32_t i = 0; i < static_cast<uint32_t>(d.vertexStreams.size()); ++i) {
			Resource* res = GetResource(*d.vertexStreams[i].resourceId);
			if (res == nullptr || res->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(res); // 幂等
			const rhi::Buffer* buf = static_cast<Buffer*>(res)->get_rhi();
			if (buf == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.BindVertexBuffer(*buf, d.vertexStreams[i].offset, i);
		}

		// 索引流（可选）：offset/类型是录制参数，不是管线状态
		if (d.indexStream.has_value()) {
			Resource* res = GetResource(*d.indexStream->resourceId);
			if (res == nullptr || res->get_type() != Resource::Buffer)
				HandleError(ErrorCode::ResourceNotRegistered);
			RealizeResource(res); // 幂等
			const rhi::Buffer* buf = static_cast<Buffer*>(res)->get_rhi();
			if (buf == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.BindIndexBuffer(*buf, d.indexStream->offset, d.indexStream->type);
		}

		// 绘制数量来自执行参数：Direct 走 CPU 数值，Indirect 走参数 buffer（有索引流即 indexed indirect）
		const DrawSource& draw = rp->get_draw();
		if (draw.kind == DrawSource::Direct) {
			if (d.indexStream.has_value())
				cmd.DrawIndexed(draw.count, draw.first, draw.vertexOffset, draw.instanceCount);
			else
				cmd.Draw(draw.count, draw.first, draw.instanceCount);
		}
		else {
			// 参数 buffer 是执行期声明（不在拓扑边里），按图资源名解析
			const rhi::Buffer* args = nullptr;
			for (auto& r : resourcePool) {
				if (r->get_name() != draw.indirectBuffer) continue;
				if (r->get_type() != Resource::Buffer)
					HandleError(ErrorCode::ResourceTypeDescMismatch);
				RealizeResource(r.get()); // 幂等
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
		// 纯 CPU：回调由 SubmitGraph 在独立线程执行（同步边界由 HostContext 包裹），录制阶段不产生命令
		break;
	case Pass::Present:
		// 呈现收尾：只有 barrier compiler 注入的 ColorAttachment -> PresentSrc 转换，无命令可录
		break;
	case Pass::Transfer: {
		// 拷贝通道：图的 barrier 已把图像侧转到 TransferSrc/TransferDst，这里按方向直译
		auto* tp = static_cast<TransferPass*>(pass);
		const TransferPassDesc& d = tp->get_desc();
		Resource* srcRes = GetResource(*d.sourceId);
		Resource* dstRes = GetResource(*d.destId);
		if (srcRes == nullptr || dstRes == nullptr)
			HandleError(ErrorCode::ResourceNotRegistered);
		RealizeResource(srcRes); // 幂等
		RealizeResource(dstRes);

		switch (d.kind) {
		case TransferKind::BufferToImage: {
			const rhi::Buffer* src = static_cast<Buffer*>(srcRes)->get_rhi();
			rhi::Image* dst = const_cast<rhi::Image*>(static_cast<Image*>(dstRes)->get_rhi());
			if (src == nullptr || dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.CopyBufferToImage(*src, *dst, rhi::ImageLayout::TransferDst, d.srcOffset);
			break;
		}
		case TransferKind::ImageToBuffer: {
			const rhi::Image* src = static_cast<Image*>(srcRes)->get_rhi();
			const rhi::Buffer* dst = static_cast<Buffer*>(dstRes)->get_rhi();
			if (src == nullptr || dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			cmd.CopyImageToBuffer(*src, *dst, rhi::ImageLayout::TransferSrc, d.dstOffset);
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
			// 两端图像都已是 TransferSrc/TransferDst；拷贝要求等尺寸，blit 允许缩放
			const rhi::Image* src = static_cast<Image*>(srcRes)->get_rhi();
			rhi::Image* dst = const_cast<rhi::Image*>(static_cast<Image*>(dstRes)->get_rhi());
			if (src == nullptr || dst == nullptr)
				HandleError(ErrorCode::ResourceNotRegistered);
			if (d.blit)
				cmd.BlitImage(*src, *dst, rhi::ImageLayout::TransferSrc,
					rhi::ImageLayout::TransferDst, d.linearFilter);
			else
				cmd.CopyImage(*src, *dst, rhi::ImageLayout::TransferSrc, rhi::ImageLayout::TransferDst);
			break;
		}
		}
		break;
	}
	default:
		HandleError(ErrorCode::PassTypeUnspecified);
	}
}

#pragma region BarrierCompiler // 使用链 -> barrier 批（纯函数，无状态机）

namespace {
	// 单次使用的同步语义：stage/access/目标布局全部由「usage × Pass 类型」查表得出
	struct UsageSync {
		rhi::PipelineStage stage = rhi::PipelineStage::None;
		rhi::Access        access = rhi::Access::None;
		rhi::ImageLayout   layout = rhi::ImageLayout::Undefined;
	};

	// shader 类读写的阶段：计算 Pass 走 ComputeShader，其余（渲染/拷贝）按片元阶段
	rhi::PipelineStage ShaderStageFor(Pass::Type type) {
		return type == Pass::Compute ? rhi::PipelineStage::ComputeShader
			: rhi::PipelineStage::FragmentShader;
	}

	UsageSync UsageSyncOf(Usage u, Pass::Type type) {
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
			s.stage = ShaderStageFor(type);
			s.access = rhi::Access::ShaderRead;
			s.layout = rhi::ImageLayout::ShaderReadOnly;
			break;
		case Usage::StorageRead:
			s.stage = ShaderStageFor(type);
			s.access = rhi::Access::ShaderRead;
			s.layout = rhi::ImageLayout::General;
			break;
		case Usage::StorageWrite:
			s.stage = ShaderStageFor(type);
			s.access = rhi::Access::ShaderWrite;
			s.layout = rhi::ImageLayout::General;
			break;
		case Usage::UniformRead:
			s.stage = ShaderStageFor(type);
			s.access = rhi::Access::ShaderRead;
			s.layout = rhi::ImageLayout::General; // 仅 Buffer 有效（图像误用会在 Realize 报类型不匹配）
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
			// 间接派发的参数由 DRAW_INDIRECT 阶段读取（compute 的 vkCmdDispatchIndirect 也归此阶段）
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
		case Usage::Auto:
		default:
			// 无 desc 的 pass（边为 Auto）：保守展开（全阶段 + 全内存访问）
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

	// 队列归属：Host 节点跟随其后首个 GPU pass，末尾的 Host 节点跟随前一个 GPU pass；
	// Host 段始终置于所属批的批首（见下面的切批规则），便于提交时先起回调、GPU 侧直接 wait 主机值
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

	// 依赖感知切分：在每个「有跨队列消费者」的生产 pass 之后切批，让 signal 尽早发出
	// （只切真正有依赖的边；布局变化单独发生的资源不影响切批）。
	// 按「内存块」聚合：别名成员共享内存，块内成员的先后使用同样构成依赖
	std::map<uint32_t, std::vector<Resource*>> blocks;
	for (auto& holder : resourcePool) {
		Resource* res = holder.get();
		if (res != nullptr)
			blocks[res->m_topo.memoryBlockId].push_back(res);
	}
	std::set<uint32_t> cuts; // 下标含义：在该 submitSeq 位置之后切批
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
				const bool need = prevWrite || write; // 有写即构成依赖（读读可并行）
				if (need && queues[prevIndex] != queues[i])
					cuts.insert(prevIndex);
			}
			prevIndex = i;
			prevWrite = write;
		}
	}

	// 批划分：submitSeq 上相邻、同队列、且不在 cuts 处的连续 pass 合成一批
	// （批序恒为拓扑序的子序）。Host 段另起一批（其段内可连续多个 Host 节点），
	// 保证整批的 Host 都在批首、同批的 GPU 命令只需一个主机 timeline 值作为 wait
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

	// passId -> 批下标（跨队列 release/acquire 判定用）
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

	// 批内是否有 GPU 命令：纯 Host 批没有可提交的命令，只起回调
	for (uint32_t b = 0; b < m_plan.batches.size(); ++b) {
		for (uint32_t i = m_plan.batches[b].begin; i < m_plan.batches[b].end; ++i) {
			Pass* p = GetPass(submitSeq[i]);
			if (p != nullptr && p->get_type() != Pass::Host) {
				m_batchHasGpu[b] = true;
				break;
			}
		}
	}

	// Host 段的上游：读资源在 submitSeq 上最近一次使用（含读，不只是 writer）。
	// 只看 writer 不够——GPU 侧最后触及该资源的可能是读者（例如上传批读过 staging）。
	// 别名组成员共享同一内存块，故按 memoryBlockId 记账：块内任一成员的触及都算（否则会漏）
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
				// 同批的 Host 段由同一个 worker 顺序执行，无需等待
				if (upBatch == static_cast<uint32_t>(-1) || upBatch == myBatch)
					continue;
				if (up->get_type() == Pass::Host)
					sync.hostPasses.push_back(up->get_id());
				else
					sync.gpuBatches.push_back(upBatch);
			}
			// 写边同样要等上游：GPU 先写(RAW)/先读(WAR) 时，主机覆写都不能抢跑
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
		// 更新最近使用：本 pass 的全部读写边都记到所属内存块上
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

	// 各 GPU 批 GPU 侧要等的主机值来源：批首的 Host 段，以及批内 Pass 读到的 Host 写者。
	// 别名块内任一成员的 Host 写者都算（同一内存）
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

	// GPU 侧等主机值时的 stage 掩码：只让批内真正与所等 Host 段数据相交的阶段等待，
	// 其余阶段可以先跑。相交含三个方向：GPU 读 Host 写的块(RAW)、GPU 写 Host 读/写过的块(WAR/WAW)。
	// 无相交则退回 AllCommands（等价于不收窄，保持原有保守语义）
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
					const UsageSync us = UsageSyncOf(use.usage, p->get_type());
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
				continue; // 提交失败或该批无 GPU 命令：没有可等的值
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

	// 按本段拓扑边装配「名字 -> 句柄」表：读集给只读句柄、写集给可写句柄；
	// 同时把可见性范围固化（读∪写失效、写回写），worker 之后不回查图，只按这些指针操作
	HostContext::Bindings bindings;
	if (Pass* pass = GetPass(passId)) {
		auto collect = [&](const ResourceUse& use, bool writable) {
			Resource* res = GetResource(use.resourceId);
			if (res == nullptr || res->get_type() != Resource::Buffer)
				return; // 图像不可映射，主机可见的只有 Buffer
			const rhi::Buffer* handle = static_cast<const Buffer*>(res)->get_rhi();
			if (handle == nullptr)
				return;
			if (writable) {
				// 图节点只暴露 const 句柄，但底层对象本身非 const：写路径需要可写视图
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

	// 图像经隐式 staging 中转：把逻辑图像名映射到中转 Buffer 句柄（可见性已在上面按 staging 边固化）
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

	// 上下文在提交线程装配（此时上游批的提交值已记录），worker 只做等待/回调/推进
	std::vector<std::pair<const HostPass*, HostContext>> jobs;
	for (uint32_t i = batch.begin; i < batch.end; ++i) {
		Pass* p = GetPass(submitSeq[i]);
		if (p == nullptr || p->get_type() != Pass::Host)
			continue;
		jobs.emplace_back(static_cast<const HostPass*>(p), MakeHostContext(p->get_id()));
	}
	if (jobs.empty())
		return;

	// 同一批的 Host 段按 submitSeq 顺序在一个 worker 里执行（段间可能有数据接力）
	m_hostThreads.emplace_back([jobs = std::move(jobs)]() mutable {
		for (auto& job : jobs) {
			const std::function<void(HostContext&)>& cb = job.first->get_callback();
			if (cb) cb(job.second);
			// 漏调 SignalGpu 会让下游 GPU 永久等待：回调返回后兜底推进
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

void RenderGraph::CompileBarriers() {
	ResolveAliasGroups(); // 首次进入时解析分桶（RealizeAll 已执行则跳过）
	m_barriers.clear();
	m_firstFrameHeads.clear();
	m_crossFrameHeads.clear();
	if (submitSeq.empty())
		return;
	BuildSubmitPlan(); // 每次编译都重建批划分（同时清空上一轮 release/同步点）

	// 单次使用已固化为 UsageSync（stage/access/目标布局），同 pass 多条边写优先合并
	struct UsePoint {
		uint32_t     passId = 0;
		UsageSync    sync;
		bool         write = false;
		Pass::Type   passType = Pass::None;
	};

	// 外层按「内存块」聚合：别名组成员共享同一底层句柄，布局与可见性必须按块整体跟踪，
	// 否则块内成员之间的复用边界会漏 barrier、布局状态也会分裂
	std::map<uint32_t, std::vector<Resource*>> blocks;
	for (auto& holder : resourcePool) {
		Resource* res = holder.get();
		if (res != nullptr)
			blocks[res->m_topo.memoryBlockId].push_back(res);
	}

	for (auto& entry : blocks) {
		const uint32_t blockId = entry.first;
		const std::vector<Resource*>& members = entry.second;

		// 代表节点：块 id 即最小成员 id；句柄/描述/首态以代表为准（组校验保证成员一致）
		Resource* owner = nullptr;
		for (Resource* m : members)
			if (m->get_id() == blockId) { owner = m; break; }
		if (owner == nullptr)
			owner = members.front();

		const bool isImage = owner->get_type() == Resource::Image;
		// 计划只存节点身份：句柄此处仅用于确认已实例化，真正的取用发生在录制期
		const bool realized = isImage ? static_cast<Image*>(owner)->get_rhi() != nullptr
			: static_cast<Buffer*>(owner)->get_rhi() != nullptr;
		if (!realized)
			continue; // 未实例化：无句柄可同步

		// 链：submitSeq 序下块内成员的每次使用；同一 Pass 内的多条边合并为一条（写优先）
		// （寿命校验已保证同批不会有两个成员同时存活，正常情况下每个 Pass 只贡献一条边）
		std::vector<UsePoint> chain;
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
			if (writeUse != nullptr)
				chain.push_back(UsePoint{ passId, UsageSyncOf(writeUse->usage, p->get_type()), true, p->get_type() });
			else if (readUse != nullptr)
				chain.push_back(UsePoint{ passId, UsageSyncOf(readUse->usage, p->get_type()), false, p->get_type() });
		}
		if (chain.empty())
			continue;

		const UsageSync first = chain.front().sync;
		const rhi::ImageLayout initLayout = owner->get_topology().initialLayout;
		const bool crossFrame = owner->get_topology().crossFrame;

		if (!crossFrame) {
			// 帧内资源：链首从既有布局转到首次访问布局（借用资源可显式声明 initialLayout）
			if (isImage && initLayout != first.layout) {
				GraphImageBarrier b;
				b.image = static_cast<Image*>(owner);
				b.oldLayout = initLayout;
				b.newLayout = first.layout;
				b.srcStage = rhi::PipelineStage::None; // 无前驱：仅做布局转换
				b.srcAccess = rhi::Access::None;
				b.dstStage = first.stage;
				b.dstAccess = first.access;
				m_barriers[chain.front().passId].images.push_back(b);
			}
		}
		else {
			// 跨帧资源：帧首屏障按「上帧末次 -> 本帧首次」闭环，内容不会因 Undefined 被丢弃。
			// 图像取首/末个 GPU 使用做锚点（Host 段不改变布局）；Buffer 允许 Host 访问参与闭环。
			size_t firstIdx = 0;
			size_t lastIdx = chain.size() - 1;
			if (isImage) {
				while (firstIdx < chain.size() && chain[firstIdx].passType == Pass::Host)
					++firstIdx;
				while (lastIdx > firstIdx && chain[lastIdx].passType == Pass::Host)
					--lastIdx;
				if (firstIdx >= chain.size())
					continue; // 纯 Host 访问图像：没有 GPU 布局链可闭环
			}
			const UsageSync firstUse = chain[firstIdx].sync;
			const UsageSync lastUse = chain[lastIdx].sync;
			const bool firstWrite = chain[firstIdx].write;
			const bool lastWrite = chain[lastIdx].write;

			// 首帧：图像从初态转入首次 GPU 布局（Own 初态是 Undefined；借用看声明的 initialLayout）
			if (isImage && initLayout != firstUse.layout) {
				GraphImageBarrier b;
				b.image = static_cast<Image*>(owner);
				b.oldLayout = initLayout;
				b.newLayout = firstUse.layout;
				b.srcStage = rhi::PipelineStage::None; // 首帧没有前驱
				b.srcAccess = rhi::Access::None;
				b.dstStage = firstUse.stage;
				b.dstAccess = firstUse.access;
				m_firstFrameHeads[chain[firstIdx].passId].images.push_back(b);
			}

			// 稳态：上帧末次访问 -> 本帧首次访问。前驱写/后继写需要内存依赖；
			// 图像布局跨帧改变时即使是 WAR 也要转换布局
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

			// 跨帧同步的计划只覆盖帧内批：若上帧末次使用与本帧首次使用在不同队列，
			// 帧间执行顺序必须由外部保证（单帧在飞 / WaitForFrame），否则需要额外跨帧信号量
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
					&& m_plan.batches[lastBatch].queue != m_plan.batches[firstBatch].queue) {
					std::cerr << "RenderGraph: 跨帧资源 \"" << owner->get_name()
						<< "\" 跨队列存活，帧间依赖需外部保证串行（WaitForFrame 或单帧在飞）"
						<< std::endl;
				}
			}
		}

		// 相邻 prev/next 对：前驱写、后继写或图像布局改变即需一道 barrier，注入后继 Pass 之前
		for (size_t i = 0; i + 1 < chain.size(); ++i) {
			const UsageSync prev = chain[i].sync;
			const UsageSync next = chain[i + 1].sync;
			const bool need = chain[i].write || chain[i + 1].write
				|| (isImage && prev.layout != next.layout);
			if (!need)
				continue;
			const uint32_t prevPassId = chain[i].passId;
			const uint32_t atPass = chain[i + 1].passId;

			// 跨队列判定：不同批且队列类别不同。同队列的不同批靠提交顺序已保证执行先后，
			// 只需在生产/消费批各自内部排序（普通 barrier 即可，无需 semaphore）
			const uint32_t prevBatch = prevPassId < m_passBatch.size() ? m_passBatch[prevPassId] : 0;

			// 消费端是 Host 段：纯 Host 批不录 GPU 命令，barrier 只能挂到生产批批尾（releases）。
			// 执行先后由主机 timeline 保证，这里只负责把 device 写变成 host 可读（dstStage=Host/HOST_READ）。
			// Host -> Host 无需 GPU barrier（主机侧串行）。
			if (chain[i + 1].passType == Pass::Host) {
				if (chain[i].passType == Pass::Host)
					continue;
				BarrierBatch& tail = m_plan.batches[prevBatch].releases;
				if (isImage) {
					GraphImageBarrier b;
					b.image = static_cast<Image*>(owner);
					b.oldLayout = prev.layout;
					b.newLayout = prev.layout; // 只做内存可见性，不改布局
					b.srcStage = prev.stage;
					b.srcAccess = prev.access;
					b.dstStage = rhi::PipelineStage::Host;
					b.dstAccess = rhi::Access::HostRead;
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

			// 跨队列：生产批尾发 release（oldLayout==newLayout，布局转换留给消费侧），
			// 消费 Pass 处发 acquire（真正做布局转换 + 内存可见性）；批间再串一个 semaphore 点。
			// release 的 dstStage 保持 AllCommands：该 barrier 在源队列录制，收窄会引入
			// 「目标阶段不被源队列族支持」的约束；跨队列的实际等待由 semaphore 掩码精确表达。
			// acquire 的 srcStage 取消费侧真实 stage（消费队列族必然支持），与 wait 掩码一致。
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
				b.newLayout = prev.layout; // 不转换：权威侧是消费批里的 acquire
				b.srcStage = prev.stage;
				b.srcAccess = prev.access;
				b.dstStage = rhi::PipelineStage::AllCommands;
				b.dstAccess = rhi::Access::None;
				if (familyTransfer) {
					b.srcQueueFamily = prevFamily;
					b.dstQueueFamily = nextFamily;
				}
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

			// 同步点去重：同一 (生产批, 消费批) 只串一个 semaphore，所有跨批边共享；
			// 同时把这些边在消费侧的 stage 并起来，作为该同步点的 wait 掩码
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
			m_plan.syncPoints[spIndex].consumerStage = m_plan.syncPoints[spIndex].consumerStage | next.stage;
		}
	}
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
	m_firstFramePending = false; // 本帧已录制：下一帧走跨帧稳态计划
}

// 图侧 barrier 记录 -> RHI barrier：句柄与子资源范围在录制期解析。
// 计划因此与具体句柄无关——借用图像换绑后计划依然有效，不需要重编译
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
			continue; // 未实例化：跳过（构图阶段已过滤，这里只是防御）
		const ImageDesc& d = g.image->get_desc();
		rhi::ImageBarrier b;
		b.image = const_cast<rhi::Image*>(handle);
		// 布局按图像整体跟踪（覆盖全部 mip/层，与拷贝区域一致）
		b.range.mipLevelCount = std::max(1u, d.mipLevels);
		b.range.layerCount = std::max(1u, d.arrayLayers);
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
	// 转换缓冲：批内复用，避免每道屏障都重新分配
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
		// 帧首屏障（跨帧资源专用）：首帧从初态起步，之后按上帧末态闭环；两套互斥
		const std::map<uint32_t, BarrierBatch>& frameHeads =
			m_firstFramePending ? m_firstFrameHeads : m_crossFrameHeads;
		auto head = frameHeads.find(passId);
		if (head != frameHeads.end())
			emit(head->second);
		// 再发该 Pass 的普通 barrier 批（同批 barrier + 来自跨队列的 acquire），最后录命令
		auto it = m_barriers.find(passId);
		if (it != m_barriers.end())
			emit(it->second);
		RecordPass(cmd, pass);
	}
	// 批尾 release：把本批产出的跨队列资源交给后续队列（布局转换由消费侧 acquire 完成）
	emit(batch.releases);
}

void RenderGraph::SubmitGraph(const FrameSync& sync) {
	if (m_plan.batches.empty() || m_graphicDevice == nullptr)
		return;

	WaitHostPasses(); // 上一帧若漏 join，先收口（worker 只碰本图数据）

	// 外部帧同步的落点：Present 节点所在批（acquire 等待挂批首，presentReady 挂批尾信号）。
	// 没有 Present 节点时同步参数无效（调用方应自行在外部同步）
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

	// 每帧重新分配主机 timeline 值：跨帧单调、绝不复位（timeline 值不可回退）。
	// 同时把「各批 GPU 侧要等的主机值」算出来（批内可能有多段，取最大值）
	m_hostSignalValue.clear();
	for (uint32_t passId : m_hostPassOrder)
		m_hostSignalValue[passId] = ++m_hostTimelineValue;
	m_batchHostWait.assign(m_plan.batches.size(), 0);
	for (uint32_t b = 0; b < m_batchHostPasses.size(); ++b)
		for (uint32_t passId : m_batchHostPasses[b])
			m_batchHostWait[b] = std::max(m_batchHostWait[b], m_hostSignalValue[passId]);

	// 主机 timeline：有 Host 段时惰性创建。设备不支持 timeline 时退化为「提交前同步执行」
	if (!m_hostPassOrder.empty() && !m_hostTimeline)
		m_hostTimeline = m_graphicDevice->CreateTimelineSemaphore(0);

	// 跨队列同步：优先用每队列 GPU timeline（Submit 已自动 signal 该批的值，消费批按值 wait）。
	// 设备不支持 timeline 时退回 binary 信号量（每帧新建；上一帧在 WaitForFrame 后已无在飞使用）
	const bool timelineSync = m_graphicDevice->QueueTimeline(rhi::QueueClass::Graphics) != nullptr;
	m_frameSemaphores.clear();
	if (!timelineSync) {
		for (size_t i = 0; i < m_plan.syncPoints.size(); ++i)
			m_frameSemaphores.push_back(m_graphicDevice->CreateSemaphore());
	}

	m_batchTimelineValue.assign(m_plan.batches.size(), 0);

	for (uint32_t b = 0; b < m_plan.batches.size(); ++b) {
		const QueueBatch& batch = m_plan.batches[b];
		if (!m_batchHasGpu[b]) {
			// 纯 Host 批（末尾）：没有命令可提交，只起回调
			if (m_hostTimeline)
				LaunchHostWorkers(b);
			else if (!m_hostPassOrder.empty())
				RunHostPassesInline(b);
			continue;
		}

		rhi::CommandList cmd = m_graphicDevice->BeginCommandList(batch.queue);
		RecordBatch(cmd, b);

		// 无主机 timeline：下游等不到主机信号，退化为提交前同步执行
		if (!m_hostTimeline && !m_hostPassOrder.empty())
			RunHostPassesInline(b);

		std::vector<rhi::SemaphoreWait> waits;
		for (uint32_t sp : batch.waits) {
			const SyncPoint& point = m_plan.syncPoints[sp];
			const uint32_t producer = point.producerBatch;
			// wait 掩码 = 该同步点消费侧的 stage 并集；空集才退回 AllCommands（保守）
			const rhi::PipelineStage waitStage = point.consumerStage != rhi::PipelineStage::None
				? point.consumerStage : rhi::PipelineStage::AllCommands;
			// timeline 路径：等生产批在该队列 timeline 上推进到的值（生产批先提交，值已记录）
			if (timelineSync && producer < m_batchTimelineValue.size() && m_batchTimelineValue[producer] != 0) {
				rhi::Semaphore* timeline = m_graphicDevice->QueueTimeline(m_plan.batches[producer].queue);
				if (timeline != nullptr) {
					waits.push_back(rhi::SemaphoreWait{ timeline, waitStage,
						m_batchTimelineValue[producer] });
					continue;
				}
			}
			if (sp < m_frameSemaphores.size() && m_frameSemaphores[sp])
				waits.push_back(rhi::SemaphoreWait{ m_frameSemaphores[sp].get(), waitStage });
		}
		// Host→GPU：GPU 侧等批首 Host 段在主机 timeline 上推进到的值。
		// 掩码只含与所等 Host 段数据相交的阶段，其余阶段可以先跑；空集退回 AllCommands
		if (m_hostTimeline && m_batchHostWait[b] != 0) {
			const rhi::PipelineStage hostWaitStage =
				b < m_batchHostStage.size() && m_batchHostStage[b] != rhi::PipelineStage::None
				? m_batchHostStage[b] : rhi::PipelineStage::AllCommands;
			waits.push_back(rhi::SemaphoreWait{ m_hostTimeline.get(), hostWaitStage, m_batchHostWait[b] });
		}

		// binary 回退路径才需要在生产批尾显式 signal；timeline 由 Submit 自动追加
		std::vector<rhi::SemaphoreSignal> signals;
		if (!timelineSync) {
			for (uint32_t sp : batch.signals) {
				if (sp < m_frameSemaphores.size() && m_frameSemaphores[sp])
					signals.push_back(rhi::SemaphoreSignal{ m_frameSemaphores[sp].get(), rhi::PipelineStage::AllCommands });
			}
		}
		// 外部帧同步：acquire 完成前不得写 presentable image；渲染完成时产出 present 信号
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
		// 该批在本队列 GPU timeline 上的值：Host 段的 WaitForGpu 按此等待
		m_batchTimelineValue[b] = m_graphicDevice->Submit(info);

		// 提交已返回（上游批的值已记录），此刻起回调线程可安全取用
		if (m_hostTimeline)
			LaunchHostWorkers(b);
	}
	m_firstFramePending = false; // 首帧已提交：之后录制走跨帧稳态闭环计划
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
		return; // 句柄未变：屏障链依然有效，无需重编译

	// 交换链重建后 backbuffer 指针变化：只更新节点句柄与描述。
	// barrier 计划只存节点身份与同步语义，句柄/子资源范围在录制期解析，因此无需重编译
	img->mExternal = external;
	const rhi::ImageCreateInfo& d = external->Desc();
	img->mDesc.width = d.width;
	img->mDesc.height = d.height;
	img->mDesc.depth = d.depth;
	img->mDesc.arrayLayers = d.arrayLayers;
	img->mDesc.mipLevels = d.mipLevels;
	img->mDesc.samples = d.samples;
	img->mDesc.format = d.format;
	img->mDesc.viewType = d.viewType;
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
	ResolveAliasGroups(); // 分桶校验/降级必须先于实例化：决定谁是内存块代表与 usage 并集
	RealizeAllResources();
	RealizeAllPasses();
	CompileBarriers(); // 内部先 BuildSubmitPlan（批划分），再推导批内/跨批 barrier
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

