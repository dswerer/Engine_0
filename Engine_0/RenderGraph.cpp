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
	case ErrorCode::DuplicateSetBinding:
		std::cerr << "RenderGraph: set/binding 重复" << std::endl; break;
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
}

void RenderGraph::ValidateShaderBindings(const std::set<uint32_t>& read, const std::set<uint32_t>& write,
	const RenderPassDesc& desc)
{
	std::set<std::pair<uint32_t, uint32_t>> setBindingKeys;
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

		if (!setBindingKeys.insert({ sb.set, sb.binding }).second)
			throw ErrorCode::DuplicateSetBinding;
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
		resourcePool[id] = Resource::AutoAllocator(std::move(topo), type, std::move(resource.second.desc));
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
				std::cout << "set" << b.set << ".binding" << b.binding << " "
					<< b.resourceName << "(#"
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

