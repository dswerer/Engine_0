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
	}
	return "Unknown";
}

static bool IsDepthFormat(ImageFormat f) {
	return f == ImageFormat::D32F || f == ImageFormat::D24S8 || f == ImageFormat::S8;
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
			sb.usage = Usage::SampledRead;
	}

	// 校验
	ValidateAttachments(read, write, desc);
	ValidateShaderBindings(read, write, desc);
	ValidateBindingConflict(desc);
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
	const RenderPassDesc& desc)
{
	// set/binding 已改由 SPIR-V 反射分配（RealizePass 回填），注册期不再校验
	for (auto& sb : desc.shaderBindings) {
		switch (sb.usage) {
		case Usage::SampledRead:
		case Usage::StorageRead:
		case Usage::StorageWrite:
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

void RenderGraph::RegisterPass(std::string name, std::set<std::string> read, std::set<std::string> write, Pass::Type type, PassDesc desc) {
	if (type == Pass::None) HandleError(ErrorCode::PassTypeUnspecified);
	if (!tmp_passes.has_value()) {
		tmp_passes.emplace();
	}
	reg_tmp_pass rtp;
	rtp.id = ++this->pass_top_id;
	rtp.to_read_name = read;
	rtp.to_write_name = write;
	rtp.type = type;
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
	const rhi::Image* external, ImageSubresource view) {
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
	tmp_resources.value()[name] = rtr;
}

void RenderGraph::ProcessRegister() {
	passPool.resize(tmp_passes.value().size());
	resourcePool.resize(tmp_resources.value().size());

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

		// 组拓扑：读/写边从解析后的 desc 回填 usage（无 desc 的 pass 边暂为 Auto，compiler 阶段保守处理）
		PassTopology topo;
		topo.id = id;
		topo.name = name;
		auto usageOf = [&desc](uint32_t resId) -> Usage {
			if (auto* rpd = std::get_if<RenderPassDesc>(&desc)) {
				for (auto& att : rpd->attachments)
					if (att.resourceId && *att.resourceId == resId) return att.usage;
				for (auto& sb : rpd->shaderBindings)
					if (sb.resourceId && *sb.resourceId == resId) return sb.usage;
			}
			return Usage::Auto;
		};
		for (uint32_t resId : write)
			topo.writes.push_back(ResourceUse{ resId, usageOf(resId) });
		for (uint32_t resId : read)
			topo.reads.push_back(ResourceUse{ resId, usageOf(resId) });

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
		topo.external = (resource.second.externalImage != nullptr); // 借用节点：生命周期不在图内
		resourcePool[id] = Resource::AutoAllocator(std::move(topo), type, std::move(resource.second.desc),
			resource.second.view, resource.second.externalImage);
	}

	tmp_passes.reset();
	tmp_resources.reset();
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

void RenderGraph::PrintGraph() {
	auto PassType = [](Pass::Type t) -> const char* {
		switch (t) {
		case Pass::Render:  return "Render";
		case Pass::Compute: return "Compute";
		default:            return "None";
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
	case Usage::ResolveSrc:             return rhi::ImageUsage::ColorAttachment;
	case Usage::ResolveDst:             return rhi::ImageUsage::ColorAttachment;
	case Usage::Auto:
	default:
		return isWrite ? rhi::ImageUsage::Storage
			: (rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage);
	}
}

rhi::ImageUsage RenderGraph::DeriveImageUsage(uint32_t resourceId) const {
	rhi::ImageUsage bits = rhi::ImageUsage::None;
	for (auto& p : passPool) {
		const PassTopology& topo = p->get_topology();
		for (auto& use : topo.writes)
			if (use.resourceId == resourceId)
				bits = bits | UsageToImageBits(use.usage, true);
		for (auto& use : topo.reads)
			if (use.resourceId == resourceId)
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
		info.usage = DeriveImageUsage(resource->get_id()); // 全图访问并集回填，用户不必手填
		img->m_rhiHandle = m_graphicDevice->CreateImage(info);
		break;
	}
	case Resource::Buffer:
		HandleError(ErrorCode::ResourceTypeDescMismatch); // Buffer 节点尚未落地
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

		// ---- 反射对账：管线布局（RHI 反射产出，携带变量名）× shaderBindings，按「变量名 == 资源名」匹配 ----
		// 反射变量无图资源 = 漏接；图资源未被反射命中 = 声明了 shader 不用的绑定；
		// 命中后按 资源类型×usage 推导期望类型与反射类型比对，回填 set/binding，
		// 并按反射的 set 顺序分配描述符集、写入内容（句柄来自节点，元素数来自反射）
		uint32_t setCount = 0;
		while (rp->m_rhiHandle->SetLayout(setCount) != nullptr)
			++setCount;
		rp->m_sets.resize(setCount);
		for (uint32_t s = 0; s < setCount; ++s) {
			auto set = m_graphicDevice->AllocateDescriptorSet(*rp->m_rhiHandle->SetLayout(s));
			if (!set)
				HandleError(ErrorCode::DescriptorSetAllocationFailed); // 详细原因已由 RHI 打印
			rp->m_sets[s] = std::move(set);
		}

		for (uint32_t s = 0; s < setCount; ++s) {
			for (const auto& db : rp->m_rhiHandle->SetLayout(s)->Bindings()) {
				auto it = std::find_if(rp->mDesc.shaderBindings.begin(), rp->mDesc.shaderBindings.end(),
					[&db](const ShaderBinding& sb) { return sb.resourceName == db.name; });
				if (it == rp->mDesc.shaderBindings.end())
					HandleError(ErrorCode::ShaderVariableUnbound); // 反射变量在图中无同名资源

				Resource* res = GetResource(*it->resourceId);
				if (!res)
					HandleError(ErrorCode::ResourceNotRegistered);
				rhi::ResourceType expected;
				switch (res->get_type()) {
				case Resource::Image:
					expected = (it->usage == Usage::SampledRead)
						? rhi::ResourceType::SampledImage : rhi::ResourceType::StorageImage;
					break;
				case Resource::Buffer:
					if (it->usage == Usage::SampledRead)
						HandleError(ErrorCode::ShaderBindingTypeMismatch); // Buffer 无采样语义
					expected = rhi::ResourceType::StorageBuffer;
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
					rp->m_sets[s]->Write(db.binding, *img->get_rhi(), img->get_view(), db.arrayCount);
					break;
				}
				case Resource::Buffer:
					HandleError(ErrorCode::ResourceTypeDescMismatch); // Buffer 节点尚未落地
				default:
					HandleError(ErrorCode::ResourceTypeUnspecified);
				}
			}
		}
		for (const auto& sb : rp->mDesc.shaderBindings) {
			if (!sb.set.has_value())
				HandleError(ErrorCode::ShaderBindingNameNotFound); // 图声明的资源未被 shader 使用
		}
		break;
	}
	case Pass::Compute:
		break; // ComputePass 无描述（读写边由拓扑承载），管线信息落地前无事可做
	default:
		HandleError(ErrorCode::PassTypeUnspecified);
	}
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
	RealizeAllResources();
	RealizeAllPasses();
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

