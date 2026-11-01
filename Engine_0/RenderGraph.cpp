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
	case Usage::TransferRead:           return "TransferRead";
	case Usage::TransferWrite:          return "TransferWrite";
	case Usage::HostWrite:              return "HostWrite";
	case Usage::PresentSrc:             return "PresentSrc";
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
		std::cerr << "RenderGraph: Transfer 节点的源须为 Buffer、目的须为 Image" << std::endl; break;
	case ErrorCode::TransferReadSetMismatch:
		std::cerr << "RenderGraph: Transfer 的源 Buffer 应在 read 集" << std::endl; break;
	case ErrorCode::TransferWriteSetMismatch:
		std::cerr << "RenderGraph: Transfer 的目的 Image 应在 write 集" << std::endl; break;
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

	desc.srcBufferId = resolve(desc.srcBuffer);
	desc.dstImageId = resolve(desc.dstImage);
	if (typeOf(desc.srcBuffer) != Resource::Buffer || typeOf(desc.dstImage) != Resource::Image)
		throw ErrorCode::TransferBindingTypeMismatch;
	// 两端即拓扑边：源在读集、目的在写集，barrier compiler 才有据可依
	if (read.count(*desc.srcBufferId) == 0)
		throw ErrorCode::TransferReadSetMismatch;
	if (write.count(*desc.dstImageId) == 0)
		throw ErrorCode::TransferWriteSetMismatch;
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

		// 组拓扑：读/写边从解析后的 desc 回填 usage（无 desc 的 pass 边暂为 Auto，compiler 阶段保守处理）
		PassTopology topo;
		topo.id = id;
		topo.name = name;
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
			}
			else if (auto* tpd = std::get_if<TransferPassDesc>(&desc)) {
				// 源 buffer 读、目的 image 写；与 ResolveTransferPassDesc 的校验互为印证
				if (tpd->srcBufferId && *tpd->srcBufferId == resId) return Usage::TransferRead;
				if (tpd->dstImageId && *tpd->dstImageId == resId) return Usage::TransferWrite;
			}
			else if (std::get_if<HostPassDesc>(&desc)) {
				// Host 节点纯 CPU：写边是宿主机写；读边无对应的 Host 读语义，保守留给 Auto
				return isWrite ? Usage::HostWrite : Usage::Auto;
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
		resourcePool[id] = Resource::AutoAllocator(std::move(topo), type, std::move(resource.second.desc),
			resource.second.view, resource.second.externalImage, resource.second.externalBuffer);
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
		case Pass::Render:   return "Render";
		case Pass::Compute:  return "Compute";
		case Pass::Host:     return "Host";
		case Pass::Transfer: return "Transfer";
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
	case Usage::IndexRead:              return rhi::ImageUsage::None;    // 仅 Buffer 有意义，永不进入图像创建位
	case Usage::ResolveSrc:             return rhi::ImageUsage::ColorAttachment;
	case Usage::ResolveDst:             return rhi::ImageUsage::ColorAttachment;
	case Usage::TransferRead:           return rhi::ImageUsage::TransferSrc;  // 仅 Buffer 有意义，图像为误用时保留能力位
	case Usage::TransferWrite:          return rhi::ImageUsage::TransferDst;
	case Usage::HostWrite:              return rhi::ImageUsage::None;         // 仅 Buffer 有意义（宿主机写）
	case Usage::PresentSrc:             return rhi::ImageUsage::None;         // 呈现不需要创建位
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
	case Resource::Buffer: {
		auto* buf = static_cast<Buffer*>(resource);
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
		// 纯 CPU 节点 / 拷贝通道：不创建管线与描述符集，Realize 为空操作
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
		cmd.Dispatch(d.groupCountX, d.groupCountY, d.groupCountZ);
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
	case Pass::Host: {
		// 纯 CPU：执行宿主机回调（不录命令）。回调在提交前完成，HOST 阶段的 barrier 保证可见性
		auto* hp = static_cast<HostPass*>(pass);
		const std::function<void()>& cb = hp->get_callback();
		if (cb) cb();
		break;
	}
	case Pass::Transfer: {
		// 拷贝通道：图的 barrier 已把目的图转到 TransferDst，这里按该布局直译
		auto* tp = static_cast<TransferPass*>(pass);
		const TransferPassDesc& d = tp->get_desc();
		Resource* srcRes = GetResource(*d.srcBufferId);
		Resource* dstRes = GetResource(*d.dstImageId);
		if (srcRes == nullptr || srcRes->get_type() != Resource::Buffer
			|| dstRes == nullptr || dstRes->get_type() != Resource::Image)
			HandleError(ErrorCode::ResourceNotRegistered);
		RealizeResource(srcRes); // 幂等
		RealizeResource(dstRes);
		const rhi::Buffer* src = static_cast<Buffer*>(srcRes)->get_rhi();
		rhi::Image* dst = const_cast<rhi::Image*>(static_cast<Image*>(dstRes)->get_rhi());
		if (src == nullptr || dst == nullptr)
			HandleError(ErrorCode::ResourceNotRegistered);
		cmd.CopyBufferToImage(*src, *dst, rhi::ImageLayout::TransferDst);
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

void RenderGraph::CompileBarriers() {
	m_barriers.clear();
	if (submitSeq.empty())
		return;

	struct UsePoint {
		uint32_t     passId = 0;
		Usage        usage = Usage::Auto;
		bool         write = false;
		Pass::Type   passType = Pass::None;
	};

	for (auto& holder : resourcePool) {
		Resource* res = holder.get();
		if (res == nullptr)
			continue;
		const uint32_t resId = res->get_id();
		const bool isImage = res->get_type() == Resource::Image;
		const rhi::Image* image = isImage ? static_cast<Image*>(res)->get_rhi() : nullptr;
		const rhi::Buffer* buffer = isImage ? nullptr : static_cast<Buffer*>(res)->get_rhi();
		if ((isImage && image == nullptr) || (!isImage && buffer == nullptr))
			continue; // 未实例化：无句柄可同步

		// 链：submitSeq 序下的每次使用；同一 Pass 对同一资源的多条边合并为一条（写优先）
		std::vector<UsePoint> chain;
		for (uint32_t passId : submitSeq) {
			Pass* p = GetPass(passId);
			if (p == nullptr)
				continue;
			const PassTopology& topo = p->get_topology();
			const ResourceUse* readUse = nullptr;
			const ResourceUse* writeUse = nullptr;
			for (const auto& use : topo.reads)
				if (use.resourceId == resId) readUse = &use;
			for (const auto& use : topo.writes)
				if (use.resourceId == resId) writeUse = &use;
			if (writeUse != nullptr)
				chain.push_back(UsePoint{ passId, writeUse->usage, true, p->get_type() });
			else if (readUse != nullptr)
				chain.push_back(UsePoint{ passId, readUse->usage, false, p->get_type() });
		}
		if (chain.empty())
			continue;

		// 图像子资源范围：布局按图像整体跟踪（覆盖全部 mip/层，与拷贝区域一致）
		rhi::ImageSubresourceRange range;
		if (isImage) {
			const ImageDesc& d = static_cast<Image*>(res)->get_desc();
			range.mipLevelCount = std::max(1u, d.mipLevels);
			range.layerCount = std::max(1u, d.arrayLayers);
		}

		const UsageSync first = UsageSyncOf(chain.front().usage, chain.front().passType);

		// 链首：图像从既有布局转到首次访问布局（借用资源可显式声明 initialLayout）
		const rhi::ImageLayout initLayout = res->get_topology().initialLayout;
		if (isImage && initLayout != first.layout) {
			rhi::ImageBarrier b;
			b.image = const_cast<rhi::Image*>(image);
			b.range = range;
			b.oldLayout = initLayout;
			b.newLayout = first.layout;
			b.srcStage = rhi::PipelineStage::None; // 无前驱：仅做布局转换
			b.srcAccess = rhi::Access::None;
			b.dstStage = first.stage;
			b.dstAccess = first.access;
			m_barriers[chain.front().passId].images.push_back(b);
		}

		// 相邻 prev/next 对：前驱写、后继写或图像布局改变即需一道 barrier，注入后继 Pass 之前
		for (size_t i = 0; i + 1 < chain.size(); ++i) {
			const UsageSync prev = UsageSyncOf(chain[i].usage, chain[i].passType);
			const UsageSync next = UsageSyncOf(chain[i + 1].usage, chain[i + 1].passType);
			const bool need = chain[i].write || chain[i + 1].write
				|| (isImage && prev.layout != next.layout);
			if (!need)
				continue;
			const uint32_t atPass = chain[i + 1].passId;
			if (isImage) {
				rhi::ImageBarrier b;
				b.image = const_cast<rhi::Image*>(image);
				b.range = range;
				b.oldLayout = prev.layout;
				b.newLayout = next.layout;
				b.srcStage = prev.stage;
				b.srcAccess = prev.access;
				b.dstStage = next.stage;
				b.dstAccess = next.access;
				m_barriers[atPass].images.push_back(b);
			}
			else {
				rhi::BufferBarrier b;
				b.buffer = const_cast<rhi::Buffer*>(buffer);
				b.srcStage = prev.stage;
				b.srcAccess = prev.access;
				b.dstStage = next.stage;
				b.dstAccess = next.access;
				m_barriers[atPass].buffers.push_back(b);
			}
		}
	}
}

#pragma endregion

void RenderGraph::RecordGraph(rhi::CommandList& cmd) {
	for (uint32_t passId : submitSeq) {
		Pass* pass = GetPass(passId);
		if (pass == nullptr)
			continue;
		// 先发该 Pass 的 barrier 批（同 Pass 的图像/buffer 已合批），再录命令
		auto it = m_barriers.find(passId);
		if (it != m_barriers.end())
			cmd.Barrier(it->second.images, it->second.buffers);
		RecordPass(cmd, pass);
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
	CompileBarriers(); // 句柄就绪后推导 barrier 批（RecordGraph 在每 Pass 前注入）
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

