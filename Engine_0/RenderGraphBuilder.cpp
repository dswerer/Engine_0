#include "RenderGraphBuilder.h"
#include "RenderGraphCompare.h"
#include "AssetLoader.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <queue>
#include <utility>

namespace dEngine {

static const char* QueueClassName(rhi::QueueClass queue) {
	switch (queue) {
	case rhi::QueueClass::Graphics:     return "Graphics";
	case rhi::QueueClass::AsyncCompute: return "AsyncCompute";
	case rhi::QueueClass::Transfer:     return "Transfer";
	}
	return "Unknown";
}

// ================= RenderGraphBuilder：逻辑声明 + 版本化 lowering =================

static bool BuilderReachable(const std::vector<std::vector<uint32_t>>& adj,
	uint32_t from, uint32_t to) {
	if (from == to) return true;
	std::vector<bool> seen(adj.size(), false);
	std::vector<uint32_t> stack{ from };
	seen[from] = true;
	while (!stack.empty()) {
		const uint32_t u = stack.back();
		stack.pop_back();
		for (uint32_t v : adj[u]) {
			if (v == to) return true;
			if (!seen[v]) { seen[v] = true; stack.push_back(v); }
		}
	}
	return false;
}

static bool BuilderFindCycle(const std::vector<std::vector<uint32_t>>& adj,
	std::vector<uint32_t>& cycle) {
	const size_t n = adj.size();
	std::vector<int> color(n, 0);
	std::vector<uint32_t> path;
	bool found = false;
	std::function<bool(uint32_t)> dfs = [&](uint32_t u) -> bool {
		color[u] = 1;
		path.push_back(u);
		for (uint32_t v : adj[u]) {
			if (color[v] == 1) {
				cycle.clear();
				auto it = std::find(path.begin(), path.end(), v);
				cycle.assign(it, path.end());
				cycle.push_back(v);
				return true;
			}
			if (color[v] == 0 && dfs(v)) return true;
		}
		color[u] = 2;
		path.pop_back();
		return false;
	};
	for (uint32_t u = 0; u < n && !found; ++u)
		if (color[u] == 0 && dfs(u)) found = true;
	return found;
}

static bool BuilderWritesWithLoad(const PassDesc& desc, const std::string& name) {
	if (auto* rp = std::get_if<RenderPassDesc>(&desc)) {
		for (const auto& att : rp->attachments)
			if (att.resourceName == name && att.load == LoadOp::Load)
				return true;
	}
	return false;
}

RenderGraphBuilder::Topology RenderGraphBuilder::Topology::Sequence(std::vector<std::string> order) {
	Topology t;
	for (size_t i = 1; i < order.size(); ++i)
		t.edges.push_back(BuilderEdge{ order[i - 1], order[i] });
	return t;
}

RenderGraphBuilder::Topology& RenderGraphBuilder::Topology::Then(const std::string& from, const std::string& to) {
	edges.push_back(BuilderEdge{ from, to });
	return *this;
}

RenderGraphBuilder::Topology& RenderGraphBuilder::Topology::ThenAll(
	const std::vector<std::string>& from, const std::vector<std::string>& to) {
	for (const auto& a : from)
		for (const auto& b : to)
			if (!a.empty() && !b.empty() && a != b)
				edges.push_back(BuilderEdge{ a, b });
	return *this;
}

void RenderGraphBuilder::DeclareExternalImage(std::string name, const rhi::Image* image,
	std::string asset, ImageOptions options) {
	ResourceDecl d;
	d.name = std::move(name);
	d.type = Resource::Image;
	d.view = options.view;
	d.viewFormat = options.viewFormat;
	d.barrierRange = options.barrierRange;
	d.initialLayout = options.initialLayout;
	d.crossFrame = options.crossFrame;
	d.asset = std::move(asset);
	d.external = true;
	d.externalImage = image;
	m_resources.push_back(std::move(d));
}

void RenderGraphBuilder::DeclareExternalBuffer(std::string name, const rhi::Buffer* buffer,
	std::string asset, BufferOptions options) {
	ResourceDecl d;
	d.name = std::move(name);
	d.type = Resource::Buffer;
	d.crossFrame = options.crossFrame;
	d.asset = std::move(asset);
	d.external = true;
	d.externalBuffer = buffer;
	m_resources.push_back(std::move(d));
}

void RenderGraphBuilder::DeclareImageResource(std::string name, ImageDesc desc,
	ImageOptions options) {
	ResourceDecl d;
	d.name = std::move(name);
	d.type = Resource::Image;
	d.crossFrame = options.crossFrame || desc.crossFrame;
	d.desc = std::move(desc);
	d.view = options.view;
	d.viewFormat = options.viewFormat;
	d.barrierRange = options.barrierRange;
	d.initialLayout = options.initialLayout;
	m_resources.push_back(std::move(d));
}

void RenderGraphBuilder::DeclareBufferResource(std::string name, BufferDesc desc,
	BufferOptions options) {
	ResourceDecl d;
	d.name = std::move(name);
	d.type = Resource::Buffer;
	d.crossFrame = options.crossFrame || desc.crossFrame;
	d.desc = std::move(desc);
	m_resources.push_back(std::move(d));
}

void RenderGraphBuilder::DeclareImageResource(std::string name, const rhi::Image* external,
	ImageOptions options, std::string assetKey) {
	DeclareExternalImage(std::move(name), external, std::move(assetKey), options);
}

void RenderGraphBuilder::DeclareBufferResource(std::string name, const rhi::Buffer* external,
	BufferOptions options, std::string assetKey) {
	DeclareExternalBuffer(std::move(name), external, std::move(assetKey), options);
}

bool RenderGraphBuilder::DeclareImageResource(std::string name, std::string assetKey,
	ImageOptions options, std::string* error) {
	const rhi::Image* image = nullptr;
	if (m_loader != nullptr) {
		std::string localError;
		image = m_loader->AcquireImage(assetKey, &localError);
		if (image == nullptr) {
			if (error) *error = localError;
			return false;
		}
	}
	DeclareExternalImage(std::move(name), image, std::move(assetKey), options);
	return true;
}

bool RenderGraphBuilder::DeclareBufferResource(std::string name, std::string assetKey,
	BufferOptions options, std::string* error) {
	const rhi::Buffer* buffer = nullptr;
	if (m_loader != nullptr) {
		std::string localError;
		buffer = m_loader->AcquireBuffer(assetKey, &localError);
		if (buffer == nullptr) {
			if (error) *error = localError;
			return false;
		}
	}
	DeclareExternalBuffer(std::move(name), buffer, std::move(assetKey), options);
	return true;
}

void RenderGraphBuilder::DeclareStaticTexture(std::string name, const rhi::Image* image,
	ImageSubresource view, rhi::ImageFormat viewFormat,
	std::optional<rhi::SamplerDesc> sampler, std::string asset) {
	StaticTextureDecl decl;
	decl.name = std::move(name);
	decl.image = image;
	decl.view = view;
	decl.viewFormat = viewFormat;
	decl.sampler = std::move(sampler);
	decl.asset = std::move(asset);
	m_staticTextures.push_back(std::move(decl));
}

// 从 PassDesc 收集 StorageReadWrite 绑定，自动填充 inPlace 集
static std::set<std::string> CollectInPlaceBindings(const PassDesc& desc) {
	std::set<std::string> out;
	auto add = [&](const std::vector<ShaderBinding>& bindings) {
		for (const auto& b : bindings) {
			if (b.usage != Usage::StorageReadWrite)
				continue;
			const std::string& n = b.graphResourceName.empty() ? b.resourceName : b.graphResourceName;
			if (!n.empty()) out.insert(n);
		}
	};
	if (const auto* rp = std::get_if<RenderPassDesc>(&desc))
		add(rp->shaderBindings);
	else if (const auto* cp = std::get_if<ComputePassDesc>(&desc))
		add(cp->shaderBindings);
	return out;
}

void RenderGraphBuilder::DeclarePass(std::string name, std::set<std::string> reads,
	std::set<std::string> writes, Pass::Type type, PassDesc desc,
	std::optional<rhi::QueueClass> queue) {
	auto inPlace = CollectInPlaceBindings(desc);
	DeclarePass(std::move(name), std::move(reads), std::move(writes),
		std::move(inPlace), type, std::move(desc), queue);
}

void RenderGraphBuilder::DeclarePass(std::string name, std::set<std::string> reads,
	std::set<std::string> writes, std::set<std::string> inPlace, Pass::Type type, PassDesc desc,
	std::optional<rhi::QueueClass> queue) {
	PassDecl p;
	p.name = std::move(name);
	p.reads = std::move(reads);
	p.writes = std::move(writes);
	p.inPlace = std::move(inPlace);
	p.type = type;
	p.desc = std::move(desc);
	for (const auto& n : CollectInPlaceBindings(p.desc))
		p.inPlace.insert(n);
	p.queue = queue.value_or(DefaultQueueForPass(type, p.desc));
	m_passes.push_back(std::move(p));
}

// 从 PassDesc 推导读写集：与后端各 *ReadSetMismatch/*WriteSetMismatch 校验同一套分类
static bool DerivePassIo(Pass::Type type, const PassDesc& desc,
	std::set<std::string>& reads, std::set<std::string>& writes) {
	auto addShaderBinding = [&](const ShaderBinding& b) {
		const std::string& n = b.graphResourceName.empty() ? b.resourceName : b.graphResourceName;
		if (n.empty())
			return;
		if (b.usage == Usage::StorageReadWrite) {
			reads.insert(n);
			writes.insert(n);
		}
		else {
			(b.usage == Usage::StorageWrite ? writes : reads).insert(n);
		}
	};
	switch (type) {
	case Pass::Render: {
		const auto* d = std::get_if<RenderPassDesc>(&desc);
		if (d == nullptr)
			return false;
		for (const auto& att : d->attachments) {
			if (att.resourceName.empty())
				continue;
			(att.usage == Usage::ResolveSrc ? reads : writes).insert(att.resourceName);
		}
		for (const auto& b : d->shaderBindings)
			addShaderBinding(b);
		for (const auto& vs : d->vertexStreams) {
			if (!vs.resourceName.empty())
				reads.insert(vs.resourceName);
		}
		if (d->indexStream.has_value() && !d->indexStream->resourceName.empty())
			reads.insert(d->indexStream->resourceName);
		return true;
	}
	case Pass::Compute: {
		const auto* d = std::get_if<ComputePassDesc>(&desc);
		if (d == nullptr)
			return false;
		for (const auto& b : d->shaderBindings)
			addShaderBinding(b);
		if (!d->indirectBuffer.empty())
			reads.insert(d->indirectBuffer);
		return true;
	}
	case Pass::Transfer: {
		const auto* d = std::get_if<TransferPassDesc>(&desc);
		if (d == nullptr)
			return false;
		// ClearImage 只有写端
		if (d->kind != TransferKind::ClearImage && !d->sourceName.empty())
			reads.insert(d->sourceName);
		if (!d->destName.empty())
			writes.insert(d->destName);
		return true;
	}
	default:
		return false;
	}
}

// 就地读写声明：资源在该 pass 的 inPlace 集中

void RenderGraphBuilder::DeclarePass(std::string name, Pass::Type type, PassDesc desc,
	std::optional<rhi::QueueClass> queue) {
	PassDecl p;
	p.name = std::move(name);
	p.type = type;
	p.queue = queue.value_or(DefaultQueueForPass(type, desc));
	if (!DerivePassIo(type, desc, p.reads, p.writes))
		RenderGraph::HandleError(RenderGraph::ErrorCode::PassIoNotDerivable);
	p.desc = std::move(desc);
	p.inPlace = CollectInPlaceBindings(p.desc);
	m_passes.push_back(std::move(p));
}

void RenderGraphBuilder::DeclareTopology(Topology topology) {
	for (const auto& e : topology.edges)
		m_edges.push_back(e);
}

void RenderGraphBuilder::AliasLogicalResources(std::vector<std::string> names) {
	m_aliasGroups.push_back(std::move(names));
}

bool RenderGraphBuilder::SameDecl(const ResourceDecl& a, const ResourceDecl& b) {
	if (a.type != b.type) return false;
	if (!MergeResourceDescEqual(a.desc, b.desc)) return false;
	if (!MergeSubresourceEqual(a.view, b.view)) return false;
	if (a.viewFormat != b.viewFormat) return false;
	if (!MergeRangeEqual(a.barrierRange, b.barrierRange)) return false;
	if (a.initialLayout != b.initialLayout) return false;
	if (a.crossFrame != b.crossFrame) return false;
	if (a.external != b.external) return false;
	if (a.externalImage != b.externalImage) return false;
	if (a.externalBuffer != b.externalBuffer) return false;
	if (a.borrowedLater != b.borrowedLater) return false;
	return true;
}

void RenderGraphBuilder::RewritePassBindings(PassDesc& desc,
	const std::map<std::string, std::string>& nameMap) {
	auto mapped = [&](const std::string& n) -> std::string {
		auto it = nameMap.find(n);
		return it == nameMap.end() ? n : it->second;
	};
	auto mapShader = [&](ShaderBinding& sb) {
		const std::string graph = sb.graphResourceName.empty()
			? sb.resourceName : sb.graphResourceName;
		auto it = nameMap.find(graph);
		if (it != nameMap.end())
			sb.graphResourceName = it->second;
	};

	if (auto* rp = std::get_if<RenderPassDesc>(&desc)) {
		for (auto& att : rp->attachments)
			att.resourceName = mapped(att.resourceName);
		for (auto& sb : rp->shaderBindings)
			mapShader(sb);
		for (auto& vs : rp->vertexStreams)
			vs.resourceName = mapped(vs.resourceName);
		if (rp->indexStream.has_value())
			rp->indexStream->resourceName = mapped(rp->indexStream->resourceName);
	}
	else if (auto* cp = std::get_if<ComputePassDesc>(&desc)) {
		for (auto& sb : cp->shaderBindings)
			mapShader(sb);
		if (!cp->indirectBuffer.empty())
			cp->indirectBuffer = mapped(cp->indirectBuffer);
	}
	else if (auto* tp = std::get_if<TransferPassDesc>(&desc)) {
		if (!tp->sourceName.empty())
			tp->sourceName = mapped(tp->sourceName);
		if (!tp->destName.empty())
			tp->destName = mapped(tp->destName);
	}
	else if (auto* mp = std::get_if<MuxPassDesc>(&desc)) {
		for (auto& alt : mp->alternatives)
			alt = mapped(alt);
	}
}

bool RenderGraphBuilder::ImportDeclarations(const RenderGraphBuilder& other, const ImportOptions& options) {
	if (&other == this) {
		std::cerr << "RenderGraphBuilder: 不能导入自身的声明" << std::endl;
		return false;
	}
	const std::string prefix = options.passPrefix;

	// ---- 预检，失败不改动本 Builder ----
	for (const ResourceDecl& r : other.m_resources) {
		if (r.name.empty()) {
			std::cerr << "RenderGraphBuilder: 导入的资源名为空" << std::endl;
			return false;
		}
		const auto it = std::find_if(m_resources.begin(), m_resources.end(),
			[&r](const ResourceDecl& x) { return x.name == r.name; });
		if (it == m_resources.end())
			continue;
		if (!options.dedupIdenticalResources) {
			std::cerr << "RenderGraphBuilder: 资源名重复且未开启 dedup："
				<< r.name << std::endl;
			return false;
		}
		if (!SameDecl(*it, r)) {
			std::cerr << "RenderGraphBuilder: 同名资源声明不一致：" << r.name << std::endl;
			return false;
		}
	}

	std::set<std::string> passNames;
	for (const PassDecl& p : m_passes)
		passNames.insert(p.name);
	for (const PassDecl& p : other.m_passes) {
		if (p.name.empty()) {
			std::cerr << "RenderGraphBuilder: 导入的 Pass 名为空" << std::endl;
			return false;
		}
		const std::string newName = prefix + p.name;
		if (!passNames.insert(newName).second) {
			std::cerr << "RenderGraphBuilder: 导入后 Pass 名冲突：" << newName
				<< "（可通过 ImportOptions::passPrefix 消除）" << std::endl;
			return false;
		}
	}

	// ---- 提交：资源同名合一，Pass 名加前缀，顺序边与别名组一并合并 ----
	for (const ResourceDecl& r : other.m_resources) {
		const auto it = std::find_if(m_resources.begin(), m_resources.end(),
			[&r](const ResourceDecl& x) { return x.name == r.name; });
		if (it == m_resources.end())
			m_resources.push_back(r);
	}
	for (const PassDecl& p : other.m_passes) {
		PassDecl np = p;
		np.name = prefix + p.name;
		m_passes.push_back(std::move(np));
	}
	for (const BuilderEdge& e : other.m_edges)
		m_edges.push_back(BuilderEdge{ prefix + e.from, prefix + e.to });
	for (const auto& group : other.m_aliasGroups)
		m_aliasGroups.push_back(group);
	return true;
}

bool RenderGraphBuilder::CompileModule(std::string name, std::vector<ModulePort> ports,
	RenderGraphModule* out, std::vector<std::string>* errors) const {
	if (out == nullptr)
		return false;

	bool bad = false;
	auto addErr = [&](std::string msg) {
		bad = true;
		if (errors != nullptr) errors->push_back(std::move(msg));
		else std::cerr << "RenderGraphBuilder: " << msg << std::endl;
	};

	if (name.empty()) addErr("模块名为空");

	std::set<std::string> portNames;
	for (const ModulePort& p : ports) {
		if (p.name.empty()) addErr("模块端口名为空");
		else if (!portNames.insert(p.name).second) addErr("模块端口名重复：" + p.name);
	}

	std::map<std::string, size_t> resIndex, passIndex;
	for (size_t i = 0; i < m_resources.size(); ++i) {
		const ResourceDecl& r = m_resources[i];
		if (r.name.empty()) { addErr("模块资源名为空"); continue; }
		if (!resIndex.emplace(r.name, i).second)
			addErr("模块资源名重复：" + r.name);
	}
	for (size_t i = 0; i < m_passes.size(); ++i) {
		const PassDecl& p = m_passes[i];
		if (p.name.empty()) { addErr("模块 Pass 名为空"); continue; }
		else if (!passIndex.emplace(p.name, i).second)
			addErr("模块 Pass 名重复：" + p.name);
		if (p.type == Pass::None) addErr("模块 Pass 类型未指定：" + p.name);
		else if (p.type == Pass::Present) addErr("模块内不允许 Present：" + p.name);
		else if (p.type == Pass::Mux) addErr("模块内不允许 Mux：" + p.name);
	}

	for (const PassDecl& p : m_passes) {
		for (const auto& n : p.reads)
			if (resIndex.find(n) == resIndex.end())
				addErr("pass \"" + p.name + "\" 读取未声明资源：" + n);
		for (const auto& n : p.writes)
			if (resIndex.find(n) == resIndex.end())
				addErr("pass \"" + p.name + "\" 写入未声明资源：" + n);
		for (const auto& n : p.inPlace)
			if (p.reads.find(n) == p.reads.end() || p.writes.find(n) == p.writes.end())
				addErr("inPlace 资源必须同时在读写集：" + n);
		for (const auto& n : p.writes)
			if (p.reads.find(n) != p.reads.end() && p.inPlace.find(n) == p.inPlace.end())
				addErr("同 Pass 读写资源未声明 inPlace（双缓冲尚未实现）：" + n);
	}

	// in-place 资源跨队列需要 ownership transfer；首版直接拒绝
	{
		std::set<std::string> inPlaceResources;
		for (const PassDecl& p : m_passes)
			inPlaceResources.insert(p.inPlace.begin(), p.inPlace.end());
		std::map<std::string, std::pair<rhi::QueueClass, std::string>> queues;
		for (const PassDecl& p : m_passes) {
			auto check = [&](const std::string& n) {
				if (inPlaceResources.find(n) == inPlaceResources.end())
					return;
				auto it = queues.find(n);
				if (it == queues.end())
					queues.emplace(n, std::make_pair(p.queue, p.name));
				else if (it->second.first != p.queue)
					addErr("in-place 资源 \"" + n + "\" 同时被 pass \"" + it->second.second
						+ "\"（" + QueueClassName(it->second.first) + "）与 \"" + p.name
						+ "\"（" + QueueClassName(p.queue)
						+ "）使用；跨队列需要 queue family ownership transfer，当前不支持");
			};
			for (const auto& n : p.reads) check(n);
			for (const auto& n : p.writes) check(n);
		}
	}

	std::vector<ModulePortInfo> infos;
	for (const ModulePort& p : ports) {
		auto it = resIndex.find(p.name);
		if (it == resIndex.end()) {
			addErr("端口未在模块中声明：" + p.name);
			continue;
		}
		const ResourceDecl& rd = m_resources[it->second];
		if (rd.IsExternal())
			addErr("端口不能使用借用资源声明：" + p.name);
		bool referenced = false;
		for (const PassDecl& pass : m_passes) {
			if (pass.reads.find(p.name) != pass.reads.end()
				|| pass.writes.find(p.name) != pass.writes.end()) {
				referenced = true;
				break;
			}
		}
		if (!referenced)
			addErr("端口未被任何 pass 引用：" + p.name);
		for (const auto& group : m_aliasGroups)
			if (std::find(group.begin(), group.end(), p.name) != group.end())
				addErr("端口不能进入模块级别名组：" + p.name);

		ModulePortInfo info;
		info.port = p;
		info.type = rd.type;
		info.desc = rd.desc;
		info.view = rd.view;
		info.viewFormat = rd.viewFormat;
		info.barrierRange = rd.barrierRange;
		info.crossFrame = rd.crossFrame;
		infos.push_back(std::move(info));
	}

	// 固定引用：模块自带的外部资源（非端口），由模块初始化注入句柄，父级不参与
	std::vector<ModuleFixedResource> fixed;
	for (const ResourceDecl& rd : m_resources) {
		if (portNames.find(rd.name) != portNames.end())
			continue; // 端口已单独校验，且禁止借用声明
		if (!rd.IsExternal())
			continue;
		if (rd.borrowedLater)
			addErr("模块内不支持 Borrow-later 资源：" + rd.name);
		for (const auto& group : m_aliasGroups)
			if (std::find(group.begin(), group.end(), rd.name) != group.end())
				addErr("固定引用不能进入模块级别名组：" + rd.name);

		ModuleFixedResource fr;
		fr.name = rd.name;
		fr.type = rd.type;
		fr.desc = rd.desc;
		fr.view = rd.view;
		fr.viewFormat = rd.viewFormat;
		fr.barrierRange = rd.barrierRange;
		fr.initialLayout = rd.initialLayout;
		fr.crossFrame = rd.crossFrame;
		fr.asset = rd.asset;
		fr.image = rd.externalImage;
		fr.buffer = rd.externalBuffer;
		fixed.push_back(std::move(fr));
	}

	// 声明边 + 资源数据边合成模块内部图
	const size_t n = m_passes.size();
	std::vector<std::vector<uint32_t>> declAdj(n);
	auto pushAdj = [](std::vector<std::vector<uint32_t>>& a, uint32_t u, uint32_t v) {
		if (std::find(a[u].begin(), a[u].end(), v) == a[u].end())
			a[u].push_back(v);
	};
	for (const BuilderEdge& e : m_edges) {
		auto ia = passIndex.find(e.from);
		auto ib = passIndex.find(e.to);
		if (ia == passIndex.end() || ib == passIndex.end()) {
			addErr("顺序边引用未声明的 pass：" + e.from + " -> " + e.to);
			continue;
		}
		const uint32_t a = static_cast<uint32_t>(ia->second);
		const uint32_t b = static_cast<uint32_t>(ib->second);
		if (a == b) {
			addErr("模块内禁止 Pass 自环：" + e.from);
			continue;
		}
		pushAdj(declAdj, a, b);
	}

	std::set<std::pair<uint32_t, uint32_t>> edges;
	for (uint32_t u = 0; u < n; ++u)
		for (uint32_t v : declAdj[u])
			edges.insert({ u, v });

	// in-place pass 的顺序必须由显式拓扑给出；首/末 pass 允许缺一侧
	{
		std::set<uint32_t> explicitIn, explicitOut;
		for (uint32_t u = 0; u < n; ++u)
			for (uint32_t v : declAdj[u]) {
				explicitOut.insert(u);
				explicitIn.insert(v);
			}
		for (uint32_t i = 0; i < n; ++i) {
			if (m_passes[i].inPlace.empty())
				continue;
			if (i > 0 && explicitIn.find(i) == explicitIn.end())
				addErr("模块内 in-place pass 必须显式声明前驱：" + m_passes[i].name);
			if (i + 1 < n && explicitOut.find(i) == explicitOut.end())
				addErr("模块内 in-place pass 必须显式声明后继：" + m_passes[i].name);
		}
	}

	// 资源数据边：按 pass 的读写集推导 writer -> readers
	std::map<std::string, std::vector<uint32_t>> writersByRes, readersByRes;
	for (size_t i = 0; i < m_passes.size(); ++i) {
		for (const auto& w : m_passes[i].writes)
			writersByRes[w].push_back(static_cast<uint32_t>(i));
		for (const auto& r : m_passes[i].reads)
			readersByRes[r].push_back(static_cast<uint32_t>(i));
	}
	std::set<std::string> inPlaceResources;
	for (const PassDecl& p : m_passes)
		inPlaceResources.insert(p.inPlace.begin(), p.inPlace.end());
	for (const auto& kv : writersByRes) {
		if (inPlaceResources.find(kv.first) != inPlaceResources.end())
			continue; // in-place 资源的顺序完全由显式拓扑决定
		auto rit = readersByRes.find(kv.first);
		if (rit == readersByRes.end())
			continue;
		for (uint32_t writer : kv.second)
			for (uint32_t reader : rit->second)
				if (writer != reader)
					edges.insert({ writer, reader });
	}
	std::vector<std::vector<uint32_t>> adj(n);
	std::vector<uint32_t> indeg(n, 0), outdeg(n, 0);
	for (const auto& e : edges) {
		adj[e.first].push_back(e.second);
		++indeg[e.second];
		++outdeg[e.first];
	}

	// 成环检测
	bool cycled = false;
	{
		std::vector<uint32_t> deg = indeg;
		std::queue<uint32_t> q;
		for (uint32_t i = 0; i < n; ++i)
			if (deg[i] == 0) q.push(i);
		uint32_t visited = 0;
		while (!q.empty()) {
			const uint32_t u = q.front();
			q.pop();
			++visited;
			for (uint32_t v : adj[u])
				if (--deg[v] == 0) q.push(v);
		}
		if (visited != n) {
			cycled = true;
			addErr("模块内部拓扑/数据依赖成环");
		}
	}

	// 模块内多写者必须被声明拓扑定序
	for (const auto& kv : writersByRes) {
		const std::vector<uint32_t>& ws = kv.second;
		for (size_t i = 0; i < ws.size(); ++i) {
			for (size_t j = i + 1; j < ws.size(); ++j) {
				if (!BuilderReachable(declAdj, ws[i], ws[j])
					&& !BuilderReachable(declAdj, ws[j], ws[i])) {
					addErr("模块内资源 \"" + kv.first + "\" 的写者 \""
						+ m_passes[ws[i]].name + "\" 与 \"" + m_passes[ws[j]].name
						+ "\" 未在拓扑中定序");
				}
			}
		}
	}

	// 入口/出口：入度 / 出度为 0。成环时所有节点都可能非零度，
	// 此时的“无入口/出口”只是环的连带结果，不再重复报错
	std::vector<std::string> entries, exits;
	for (uint32_t i = 0; i < n; ++i) {
		if (indeg[i] == 0) entries.push_back(m_passes[i].name);
		if (outdeg[i] == 0) exits.push_back(m_passes[i].name);
	}
	if (!cycled && (entries.empty() || exits.empty()))
		addErr("模块没有入口或出口");

	if (bad)
		return false;

	out->mName = std::move(name);
	out->mDecl = *this;
	out->mPorts = std::move(infos);
	out->mFixed = std::move(fixed);
	out->mEntryNames = std::move(entries);
	out->mExitNames = std::move(exits);
	return true;
}

// ---- 模块初始化：把固定引用名解析成具体句柄（整体校验后一次性应用） ----

bool RenderGraphModule::Initialize(const std::vector<ModuleFixedBinding>& bindings,
	std::vector<std::string>* errors) {
	bool bad = false;
	auto addErr = [&](std::string msg) {
		bad = true;
		if (errors != nullptr) errors->push_back(std::move(msg));
		else std::cerr << "RenderGraphModule: " << msg << std::endl;
	};

	std::map<std::string, const ModuleFixedBinding*> byName;
	for (const auto& b : bindings) {
		if (!byName.emplace(b.name, &b).second)
			addErr("固定引用重复初始化：" + b.name);
		if (b.image != nullptr && b.buffer != nullptr)
			addErr("固定引用同时提供 image 与 buffer：" + b.name);
		if (b.image == nullptr && b.buffer == nullptr)
			addErr("固定引用未提供句柄：" + b.name);
	}

	for (const auto& kv : byName) {
		const ModuleFixedResource* fr = nullptr;
		for (const auto& f : mFixed)
			if (f.name == kv.first) { fr = &f; break; }
		if (fr == nullptr) {
			addErr("不是模块固定引用：" + kv.first);
			continue;
		}
		if (kv.second->image != nullptr && fr->type != Resource::Image)
			addErr("固定引用 \"" + kv.first + "\" 是 Buffer，不能用 Image 初始化");
		if (kv.second->buffer != nullptr && fr->type != Resource::Buffer)
			addErr("固定引用 \"" + kv.first + "\" 是 Image，不能用 Buffer 初始化");
	}

	if (bad)
		return false;

	for (auto& f : mFixed) {
		auto it = byName.find(f.name);
		if (it == byName.end())
			continue;
		f.image = it->second->image;
		f.buffer = it->second->buffer;
		for (auto& r : mDecl.m_resources) {
			if (r.name != f.name)
				continue;
			r.external = true;
			r.externalImage = f.image;
			r.externalBuffer = f.buffer;
		}
	}
	return true;
}

bool RenderGraphModule::IsInitialized() const {
	for (const auto& f : mFixed)
		if (!f.Bound()) return false;
	return true;
}

std::vector<std::string> RenderGraphModule::UnboundFixedResources() const {
	std::vector<std::string> out;
	for (const auto& f : mFixed)
		if (!f.Bound()) out.push_back(f.name);
	return out;
}

std::optional<ModuleInstance> RenderGraphBuilder::Instantiate(
	const RenderGraphModule& module, std::string tag,
	const std::vector<ModuleBinding>& bindings, std::vector<std::string>* errors) {
	bool bad = false;
	auto addErr = [&](std::string msg) {
		bad = true;
		if (errors != nullptr) errors->push_back(std::move(msg));
		else std::cerr << "RenderGraphBuilder: " << msg << std::endl;
	};

	if (module.mName.empty()) addErr("模块尚未编译");
	if (tag.empty()) addErr("模块实例 tag 为空");
	else if (tag.find('/') != std::string::npos) addErr("模块实例 tag 不能包含 '/'");
	if (m_instanceTags.find(tag) != m_instanceTags.end())
		addErr("模块实例 tag 重复：" + tag);

	std::set<std::string> portNames;
	for (const auto& info : module.mPorts)
		portNames.insert(info.port.name);

	std::map<std::string, std::string> portToParent;
	for (const auto& b : bindings) {
		if (portNames.find(b.port) == portNames.end()) {
			addErr("未知端口绑定：" + b.port);
			continue;
		}
		if (b.parentResource.empty()) {
			addErr("端口 \"" + b.port + "\" 未指定父资源");
			continue;
		}
		if (!portToParent.emplace(b.port, b.parentResource).second)
			addErr("端口重复绑定：" + b.port);
	}
	for (const auto& info : module.mPorts)
		if (portToParent.find(info.port.name) == portToParent.end())
			addErr("端口未绑定：" + info.port.name);

	std::map<std::string, const ResourceDecl*> parentRes;
	for (const auto& r : m_resources)
		if (parentRes.find(r.name) == parentRes.end())
			parentRes.emplace(r.name, &r);

	for (const auto& info : module.mPorts) {
		auto bit = portToParent.find(info.port.name);
		if (bit == portToParent.end())
			continue;
		auto pit = parentRes.find(bit->second);
		if (pit == parentRes.end()) {
			addErr("绑定父资源未声明：" + bit->second);
			continue;
		}
		const ResourceDecl& pd = *pit->second;
		if (pd.type != info.type)
			addErr("端口 \"" + info.port.name + "\" 与父资源 \"" + bit->second + "\" 类型不一致");
		// 外部父资源没有图侧描述，句柄即权威；只校验类型与 crossFrame
		if (!pd.IsExternal()) {
			if (!MergeResourceDescEqual(pd.desc, info.desc))
				addErr("端口 \"" + info.port.name + "\" 与父资源 \"" + bit->second + "\" 描述不一致");
			if (!MergeSubresourceEqual(pd.view, info.view)
				|| pd.viewFormat != info.viewFormat
				|| !MergeRangeEqual(pd.barrierRange, info.barrierRange))
				addErr("端口 \"" + info.port.name + "\" 与父资源 \"" + bit->second + "\" 视图/同步范围不一致");
		}
		if (info.crossFrame && !pd.crossFrame)
			addErr("端口 \"" + info.port.name + "\" 要求 crossFrame，父资源不满足");
	}

	// ---- 固定引用：模块自带资源，按身份收敛到父级唯一声明（不做 tag 作用域化） ----
	std::map<std::string, std::string> fixedToParent;
	{
		std::set<std::string> referenced;
		for (const auto& p : module.mDecl.m_passes) {
			referenced.insert(p.reads.begin(), p.reads.end());
			referenced.insert(p.writes.begin(), p.writes.end());
		}

		auto toDecl = [](const ModuleFixedResource& f) {
			ResourceDecl d;
			d.name = f.name;
			d.type = f.type;
			d.desc = f.desc;
			d.view = f.view;
			d.viewFormat = f.viewFormat;
			d.barrierRange = f.barrierRange;
			d.initialLayout = f.initialLayout;
			d.crossFrame = f.crossFrame;
			d.asset = f.asset;
			d.external = true;
			d.externalImage = f.image;
			d.externalBuffer = f.buffer;
			return d;
		};
		auto sameSource = [](const ResourceDecl& a, const ResourceDecl& b) {
			if (a.type != b.type) return false;
			if (!a.asset.empty() && a.asset == b.asset) return true;
			if (a.externalImage != nullptr || b.externalImage != nullptr)
				return a.externalImage != nullptr && a.externalImage == b.externalImage;
			if (a.externalBuffer != nullptr || b.externalBuffer != nullptr)
				return a.externalBuffer != nullptr && a.externalBuffer == b.externalBuffer;
			return false;
		};
		// 固定引用始终是借用资源：句柄与宿主布局状态即权威，父级声明优先，
		// 不要求 desc/view/initialLayout 与模块定义逐字段相等（与端口校验一致）
		auto converge = [&](const ModuleFixedResource& fixed, const ResourceDecl& host,
			const std::string& hostLabel) {
			if (fixed.crossFrame && !host.crossFrame)
				addErr("固定引用 \"" + fixed.name + "\" 要求 crossFrame，但" + hostLabel
					+ " \"" + host.name + "\" 不满足");
			else
				fixedToParent[fixed.name] = host.name;
		};

		std::vector<std::pair<std::string, ResourceDecl>> pending;
		for (const auto& fr : module.mFixed) {
			if (referenced.find(fr.name) == referenced.end())
				continue; // 未被任何 pass 使用，不进入父图
			if (!fr.Bound()) {
				addErr("模块固定引用未初始化：" + fr.name);
				continue;
			}
			ResourceDecl fd = toDecl(fr);

			bool matched = false;
			for (const auto& r : m_resources) {
				if (!r.IsExternal() || !sameSource(r, fd)) continue;
				converge(fr, r, "父级资源");
				matched = true;
				break;
			}
			if (matched) continue;

			for (const auto& p : pending) {
				if (!sameSource(p.second, fd)) continue;
				converge(fr, p.second, "已收敛的固定引用");
				matched = true;
				break;
			}
			if (matched) continue;

			const std::string candidate = module.mName + "/" + fr.name;
			bool taken = false;
			for (const auto& r : m_resources)
				if (r.name == candidate) { taken = true; break; }
			for (const auto& p : pending)
				if (p.first == candidate) { taken = true; break; }
			if (taken) {
				addErr("固定引用资源名冲突：" + candidate);
				continue;
			}

			fixedToParent[fr.name] = candidate;
			fd.name = candidate;
			pending.emplace_back(candidate, std::move(fd));
		}

		if (bad)
			return std::nullopt;

		for (auto& p : pending)
			m_resources.push_back(std::move(p.second));
	}

	// 名称映射：端口 -> 父资源；固定引用 -> 收敛后的父级资源；内部资源/Pass -> tag/name
	std::map<std::string, std::string> nameMap;
	for (const auto& info : module.mPorts) {
		auto bit = portToParent.find(info.port.name);
		if (bit != portToParent.end())
			nameMap[info.port.name] = bit->second;
	}
	for (const auto& kv : fixedToParent)
		nameMap[kv.first] = kv.second;
	for (const auto& r : module.mDecl.m_resources) {
		if (portNames.find(r.name) != portNames.end())
			continue;
		if (r.IsExternal())
			continue; // 固定引用已在父级收敛
		nameMap[r.name] = tag + "/" + r.name;
	}

	std::set<std::string> existingRes, existingPass;
	for (const auto& r : m_resources)
		existingRes.insert(r.name);
	for (const auto& p : m_passes)
		existingPass.insert(p.name);
	for (const auto& r : module.mDecl.m_resources) {
		if (portNames.find(r.name) != portNames.end())
			continue;
		if (r.IsExternal())
			continue;
		const std::string nn = nameMap[r.name];
		if (!existingRes.insert(nn).second)
			addErr("实例资源名冲突：" + nn);
	}
	for (const auto& p : module.mDecl.m_passes) {
		const std::string nn = tag + "/" + p.name;
		if (!existingPass.insert(nn).second)
			addErr("实例 Pass 名冲突：" + nn);
	}

	for (const auto& p : module.mDecl.m_passes) {
		for (const auto& n : p.reads)
			if (nameMap.find(n) == nameMap.end())
				addErr("模块 pass \"" + p.name + "\" 读取未知资源：" + n);
		for (const auto& n : p.writes)
			if (nameMap.find(n) == nameMap.end())
				addErr("模块 pass \"" + p.name + "\" 写入未知资源：" + n);
	}

	if (bad)
		return std::nullopt;

	for (const auto& r : module.mDecl.m_resources) {
		if (portNames.find(r.name) != portNames.end())
			continue;
		if (r.IsExternal())
			continue; // 固定引用已在父级收敛，不复制
		ResourceDecl nr = r;
		nr.name = nameMap.at(r.name);
		m_resources.push_back(std::move(nr));
	}
	for (const auto& p : module.mDecl.m_passes) {
		PassDecl np = p;
		np.name = tag + "/" + p.name;
		std::set<std::string> reads, writes, inPlace;
		for (const auto& n : p.reads)
			reads.insert(nameMap.at(n));
		for (const auto& n : p.writes)
			writes.insert(nameMap.at(n));
		for (const auto& n : p.inPlace)
			inPlace.insert(nameMap.at(n));
		np.reads = std::move(reads);
		np.writes = std::move(writes);
		np.inPlace = std::move(inPlace);
		RewritePassBindings(np.desc, nameMap);
		m_passes.push_back(std::move(np));
	}
	for (const auto& e : module.mDecl.m_edges)
		m_edges.push_back(BuilderEdge{ tag + "/" + e.from, tag + "/" + e.to });
	for (const auto& g : module.mDecl.m_aliasGroups) {
		std::vector<std::string> ng;
		ng.reserve(g.size());
		for (const auto& n : g) {
			auto it = nameMap.find(n);
			if (it != nameMap.end())
				ng.push_back(it->second);
		}
		m_aliasGroups.push_back(std::move(ng));
	}

	m_instanceTags.insert(tag);

	ModuleInstance inst;
	inst.tag = tag;
	for (const auto& e : module.mEntryNames)
		inst.Entries.push_back(tag + "/" + e);
	for (const auto& e : module.mExitNames)
		inst.Exits.push_back(tag + "/" + e);
	return inst;
}

LowerResult RenderGraphBuilder::Lower() const {
	LowerResult out;
	uint32_t errorCount = 0;

	auto addDiag = [&](BuilderSeverity sev, BuilderDiagKind kind, std::string msg,
		std::optional<BuilderEdge> edge = std::nullopt) {
		BuilderDiagnostic d;
		d.severity = sev;
		d.kind = kind;
		d.message = std::move(msg);
		d.edge = std::move(edge);
		if (sev == BuilderSeverity::Error) ++errorCount;
		out.diagnostics.push_back(std::move(d));
	};

	// ---- 名称与引用校验 ----
	std::map<std::string, size_t> resMap, passMap;
	for (size_t i = 0; i < m_resources.size(); ++i) {
		const ResourceDecl& r = m_resources[i];
		if (r.name.empty()) { addDiag(BuilderSeverity::Error, BuilderDiagKind::EmptyName, "资源名为空"); continue; }
		if (resMap.find(r.name) != resMap.end()) {
			addDiag(BuilderSeverity::Error, BuilderDiagKind::DuplicateName, "资源名重复：" + r.name);
			continue;
		}
		resMap.emplace(r.name, i);
		if (r.IsExternal() && r.externalImage == nullptr && r.externalBuffer == nullptr)
			addDiag(BuilderSeverity::Error, BuilderDiagKind::UnboundExternal,
				"外部资源未绑定（缺少句柄或资产解析）: " + r.name);
	}
	for (size_t i = 0; i < m_passes.size(); ++i) {
		const PassDecl& p = m_passes[i];
		if (p.name.empty()) { addDiag(BuilderSeverity::Error, BuilderDiagKind::EmptyName, "Pass 名为空"); continue; }
		if (passMap.find(p.name) != passMap.end()) {
			addDiag(BuilderSeverity::Error, BuilderDiagKind::DuplicateName, "Pass 名重复：" + p.name);
			continue;
		}
		passMap.emplace(p.name, i);
	}
	for (const auto& p : m_passes) {
		for (const auto& n : p.reads)
			if (resMap.find(n) == resMap.end())
				addDiag(BuilderSeverity::Error, BuilderDiagKind::UnknownName,
					"pass \"" + p.name + "\" 读取未声明资源：" + n);
		for (const auto& n : p.writes)
			if (resMap.find(n) == resMap.end())
				addDiag(BuilderSeverity::Error, BuilderDiagKind::UnknownName,
					"pass \"" + p.name + "\" 写入未声明资源：" + n);
	}

	// ---- in-place 声明、双缓冲意图、写者收集、Mux 多写 ----
	std::map<std::string, std::vector<uint32_t>> writers;
	std::set<std::string> muxOutputs;
	std::set<std::string> inPlaceResources;
	for (const PassDecl& p : m_passes)
		inPlaceResources.insert(p.inPlace.begin(), p.inPlace.end());

	std::map<std::string, std::pair<rhi::QueueClass, std::string>> inPlaceQueue;
	for (size_t i = 0; i < m_passes.size(); ++i) {
		const PassDecl& p = m_passes[i];
		for (const auto& n : p.inPlace) {
			if (p.reads.find(n) == p.reads.end() || p.writes.find(n) == p.writes.end())
				addDiag(BuilderSeverity::Error, BuilderDiagKind::InPlaceNotReadWrite,
					"pass \"" + p.name + "\" 的 inPlace 资源 \"" + n
					+ "\" 必须同时出现在 reads 和 writes");
		}
		for (const auto& n : p.writes) {
			if (p.reads.find(n) != p.reads.end() && p.inPlace.find(n) == p.inPlace.end())
				addDiag(BuilderSeverity::Error, BuilderDiagKind::DoubleBufferNotImplemented,
					"pass \"" + p.name + "\" 同时读写资源 \"" + n
					+ "\" 但未声明 inPlace；原地读写请显式声明，否则需要跨帧双缓冲（尚未实现）");
			writers[n].push_back(static_cast<uint32_t>(i));
			if (p.type == Pass::Mux)
				muxOutputs.insert(n);
		}
	}
	// in-place 资源一旦跨队列，需要 queue family ownership transfer；首版直接拒绝
	for (const PassDecl& p : m_passes) {
		auto checkQueue = [&](const std::string& n) {
			if (inPlaceResources.find(n) == inPlaceResources.end())
				return;
			auto it = inPlaceQueue.find(n);
			if (it == inPlaceQueue.end())
				inPlaceQueue.emplace(n, std::make_pair(p.queue, p.name));
			else if (it->second.first != p.queue)
				addDiag(BuilderSeverity::Error, BuilderDiagKind::InPlaceQueueMismatch,
					"in-place 资源 \"" + n + "\" 同时被 pass \"" + it->second.second
					+ "\"（" + QueueClassName(it->second.first) + "）与 \"" + p.name
					+ "\"（" + QueueClassName(p.queue)
					+ "）使用；跨队列需要 queue family ownership transfer，当前不支持");
		};
		for (const auto& n : p.reads) checkQueue(n);
		for (const auto& n : p.writes) checkQueue(n);
	}
	for (const auto& name : muxOutputs) {
		if (writers[name].size() > 1)
			addDiag(BuilderSeverity::Error, BuilderDiagKind::MuxMultiWriter,
				"Mux 输出不允许存在多个写者：" + name);
	}

	// ---- 用户拓扑边 ----
	std::set<BuilderEdge> userEdges;
	for (const auto& e : m_edges) {
		if (passMap.find(e.from) == passMap.end() || passMap.find(e.to) == passMap.end()) {
			addDiag(BuilderSeverity::Error, BuilderDiagKind::UnknownName,
				"顺序边引用未声明的 pass：" + e.from + " -> " + e.to, e);
			continue;
		}
		if (e.from == e.to) {
			addDiag(BuilderSeverity::Error, BuilderDiagKind::Contradiction,
				"顺序边自引用：" + e.from, e);
			out.topology.conflicts.push_back(e);
			continue;
		}
		userEdges.insert(e);
	}
	out.topology.declared.assign(userEdges.begin(), userEdges.end());

	const size_t n = m_passes.size();
	std::vector<std::vector<uint32_t>> userAdj(n);
	std::vector<uint32_t> indeg(n, 0);
	for (const auto& e : userEdges) {
		const uint32_t a = static_cast<uint32_t>(passMap.at(e.from));
		const uint32_t b = static_cast<uint32_t>(passMap.at(e.to));
		auto& adj = userAdj[a];
		if (std::find(adj.begin(), adj.end(), b) == adj.end()) {
			adj.push_back(b);
			++indeg[b];
		}
	}

	// ---- 多写者必须被用户拓扑定序 ----
	for (const auto& kv : writers) {
		const std::vector<uint32_t>& ws = kv.second;
		for (size_t i = 0; i < ws.size(); ++i) {
			for (size_t j = i + 1; j < ws.size(); ++j) {
				if (!BuilderReachable(userAdj, ws[i], ws[j])
					&& !BuilderReachable(userAdj, ws[j], ws[i])) {
					addDiag(BuilderSeverity::Error, BuilderDiagKind::WriterOrderUndetermined,
						"资源 \"" + kv.first + "\" 的写者 \"" + m_passes[ws[i]].name
						+ "\" 与 \"" + m_passes[ws[j]].name + "\" 未在拓扑中定序");
				}
			}
		}
	}

	if (errorCount > 0) return out;

	// ---- 用户拓扑排序（同层按声明顺序）----
	std::vector<uint32_t> order;
	{
		std::vector<uint32_t> deg = indeg;
		std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> pq;
		for (uint32_t i = 0; i < n; ++i)
			if (deg[i] == 0) pq.push(i);
		while (!pq.empty()) {
			const uint32_t u = pq.top();
			pq.pop();
			order.push_back(u);
			for (uint32_t v : userAdj[u])
				if (--deg[v] == 0) pq.push(v);
		}
	}
	if (order.size() != n) {
		std::vector<uint32_t> cycle;
		if (BuilderFindCycle(userAdj, cycle)) {
			std::string path;
			for (size_t i = 0; i < cycle.size(); ++i) {
				if (i) path += " -> ";
				path += m_passes[cycle[i]].name;
			}
			addDiag(BuilderSeverity::Error, BuilderDiagKind::Contradiction, "用户拓扑成环：" + path);
			for (size_t i = 1; i < cycle.size(); ++i)
				out.topology.conflicts.push_back(BuilderEdge{
					m_passes[cycle[i - 1]].name, m_passes[cycle[i]].name });
		}
		else {
			addDiag(BuilderSeverity::Error, BuilderDiagKind::Contradiction, "用户拓扑成环");
		}
		return out;
	}

	// ---- 版本分配：读当前版本，写下一版本 ----
	struct ResState { uint32_t current = 0; uint32_t next = 1; };
	std::map<std::string, ResState> state;
	struct VerRecord {
		std::string resource;
		uint32_t    version = 0;
		std::string producer;
		std::vector<std::string> readers;
		bool        used = false;
		uint32_t    firstUse = static_cast<uint32_t>(-1);
		uint32_t    lastUse = 0;
	};
	std::map<ResourceRef, VerRecord> recs;
	auto ensureVersion = [&](const std::string& name, uint32_t v) -> ResourceRef {
		ResourceRef ref{ name, v };
		auto it = recs.find(ref);
		if (it == recs.end()) {
			VerRecord r;
			r.resource = name;
			r.version = v;
			recs.emplace(ref, std::move(r));
		}
		return ref;
	};
	auto touch = [&](const ResourceRef& ref, uint32_t index) {
		VerRecord& r = recs.at(ref);
		r.used = true;
		r.firstUse = std::min(r.firstUse, index);
		r.lastUse = std::max(r.lastUse, index);
	};

	// 就地读写：记录 (资源名, 写入版本)，稍后与前一版本共享内存
	std::vector<std::pair<std::string, uint32_t>> inPlaceWrites;

	std::vector<LoweredPass> lowered;
	lowered.reserve(order.size());
	for (uint32_t idx = 0; idx < order.size(); ++idx) {
		const PassDecl& pd = m_passes[order[idx]];
		LoweredPass lp;
		lp.name = pd.name;
		lp.type = pd.type;
		lp.desc = pd.desc;
		lp.queue = pd.queue;

		// 先分配写版本：就地资源随后读到的就是本次写入的版本
		for (const auto& name : pd.writes) {
			const uint32_t v = state[name].next++;
			state[name].current = v;
			const ResourceRef ref = ensureVersion(name, v);
			VerRecord& rec = recs.at(ref);
			rec.producer = pd.name;
			touch(ref, idx);
			lp.writes.insert(ref);
			lp.bindingVersions[name] = v;

			if (pd.inPlace.find(name) != pd.inPlace.end())
				inPlaceWrites.emplace_back(name, v);
		}

		for (const auto& name : pd.reads) {
			const uint32_t v = state[name].current;
			const ResourceRef ref = ensureVersion(name, v);
			recs.at(ref).readers.push_back(pd.name);
			touch(ref, idx);
			lp.reads.insert(ref);
			lp.bindingVersions[name] = v;

			const ResourceDecl& rd = m_resources[resMap.at(name)];
			const bool hasInitial = rd.IsExternal() || rd.crossFrame
				|| rd.initialLayout != rhi::ImageLayout::Undefined;
			if (v == 0 && !hasInitial) {
				addDiag(BuilderSeverity::Error, BuilderDiagKind::ReadBeforeProducer,
					"pass \"" + pd.name + "\" 读取资源 \"" + name
					+ "\" 的导入版本，但该资源没有外部句柄且 initialLayout 为 Undefined");
			}
		}

		lowered.push_back(std::move(lp));
	}

	// ---- 别名计划：Load 原地链、就地读写链、跨帧整链、用户声明桶 ----
	std::vector<std::pair<ResourceRef, ResourceRef>> unions;
	for (const LoweredPass& lp : lowered) {
		for (const ResourceRef& wref : lp.writes) {
			if (!BuilderWritesWithLoad(lp.desc, wref.name))
				continue;
			const ResourceRef prev{ wref.name, wref.version - 1 };
			ensureVersion(prev.name, prev.version);
			unions.emplace_back(prev, wref);
		}
	}
	// 就地读写：写入版本与前一版本共享同一块内存（同一资源的版本链）
	for (const auto& entry : inPlaceWrites) {
		const ResourceRef prev{ entry.first, entry.second - 1 };
		ensureVersion(prev.name, prev.version);
		unions.emplace_back(prev, ResourceRef{ entry.first, entry.second });
	}
	for (const ResourceDecl& rd : m_resources) {
		if (!rd.crossFrame) continue;
		std::vector<ResourceRef> versions;
		for (const auto& kv : recs)
			if (kv.first.name == rd.name)
				versions.push_back(kv.first);
		for (size_t i = 1; i < versions.size(); ++i)
			unions.emplace_back(versions[0], versions[i]);
	}

	for (const auto& group : m_aliasGroups) {
		std::set<std::string> unique;
		const ResourceDecl* first = nullptr;
		bool okGroup = true;
		for (const auto& name : group) {
			auto it = resMap.find(name);
			if (it == resMap.end()) {
				addDiag(BuilderSeverity::Error, BuilderDiagKind::VersionAliasConflict,
					"别名组引用未声明资源：" + name);
				okGroup = false;
				continue;
			}
			unique.insert(name);
			const ResourceDecl& rd = m_resources[it->second];
			if (first == nullptr) first = &rd;
			else if (!SameDecl(*first, rd)) {
				addDiag(BuilderSeverity::Error, BuilderDiagKind::VersionAliasConflict,
					"别名组成员声明不一致：" + name);
				okGroup = false;
			}
		}
		if (unique.size() < 2) {
			addDiag(BuilderSeverity::Error, BuilderDiagKind::VersionAliasConflict,
				"别名组至少需要两个资源");
			okGroup = false;
		}
		if (!okGroup) continue;
		std::vector<ResourceRef> versions;
		for (const auto& kv : recs)
			if (unique.find(kv.first.name) != unique.end())
				versions.push_back(kv.first);
		for (size_t i = 1; i < versions.size(); ++i)
			unions.emplace_back(versions[0], versions[i]);
	}

	// ---- 并查集汇总别名块 ----
	std::vector<ResourceRef> refs;
	for (const auto& kv : recs) refs.push_back(kv.first);
	std::map<ResourceRef, uint32_t> refIndex;
	for (uint32_t i = 0; i < refs.size(); ++i) refIndex.emplace(refs[i], i);
	std::vector<uint32_t> dsuParent(refs.size());
	for (uint32_t i = 0; i < refs.size(); ++i) dsuParent[i] = i;
	std::function<uint32_t(uint32_t)> findRoot = [&](uint32_t x) -> uint32_t {
		while (dsuParent[x] != x) { dsuParent[x] = dsuParent[dsuParent[x]]; x = dsuParent[x]; }
		return x;
	};
	for (const auto& u : unions) {
		auto ia = refIndex.find(u.first);
		auto ib = refIndex.find(u.second);
		if (ia == refIndex.end() || ib == refIndex.end()) continue;
		const uint32_t ra = findRoot(ia->second);
		const uint32_t rb = findRoot(ib->second);
		if (ra != rb) dsuParent[rb] = ra;
	}
	std::map<uint32_t, std::vector<ResourceRef>> groups;
	for (uint32_t i = 0; i < refs.size(); ++i)
		groups[findRoot(i)].push_back(refs[i]);

	// ---- 别名块校验：同 pass 多触达、寿命重叠 ----
	for (auto& kv : groups) {
		const std::vector<ResourceRef>& members = kv.second;
		if (members.size() < 2) continue;
		for (const LoweredPass& lp : lowered) {
			uint32_t touched = 0;
			for (const ResourceRef& m : members)
				if (lp.reads.count(m) || lp.writes.count(m)) ++touched;
			if (touched > 1)
				addDiag(BuilderSeverity::Error, BuilderDiagKind::VersionAliasConflict,
					"pass \"" + lp.name + "\" 同时触达同一别名块的多个版本");
		}
		std::vector<uint32_t> firsts, lasts;
		for (const ResourceRef& m : members) {
			const VerRecord& r = recs.at(m);
			if (!r.used) continue;
			firsts.push_back(r.firstUse);
			lasts.push_back(r.lastUse);
		}
		for (size_t i = 0; i < firsts.size(); ++i)
			for (size_t j = i + 1; j < firsts.size(); ++j)
				if (std::max(firsts[i], firsts[j]) <= std::min(lasts[i], lasts[j]))
					addDiag(BuilderSeverity::Error, BuilderDiagKind::VersionAliasConflict,
						"别名块内版本寿命重叠：" + members.front().name);
	}

	// ---- 输出别名组与版本表 ----
	for (auto& kv : groups) {
		if (kv.second.size() < 2) continue;
		LoweredAliasGroup g;
		g.members = kv.second;
		g.mustAlias = true;
		out.graph.aliasGroups.push_back(std::move(g));
	}
	for (const auto& kv : recs) {
		BuilderVersionInfo vi;
		vi.resource = kv.first.name;
		vi.version = kv.first.version;
		vi.producer = kv.second.producer;
		if (kv.first.version > 0) {
			const ResourceRef prev{ kv.first.name, kv.first.version - 1 };
			auto pit = refIndex.find(prev);
			if (pit != refIndex.end()
				&& findRoot(pit->second) == findRoot(refIndex.at(kv.first)))
				vi.aliasPrevious = true;
		}
		out.versions.push_back(std::move(vi));
	}

	// ---- 顺序边：用户边 + 别名块钉住边 ----
	std::set<BuilderEdge> finalEdges = userEdges;
	for (auto& kv : groups) {
		const std::vector<ResourceRef>& members = kv.second;
		if (members.size() < 2) continue;
		std::vector<const VerRecord*> used;
		for (const ResourceRef& m : members) {
			const VerRecord& r = recs.at(m);
			if (r.used) used.push_back(&r);
		}
		std::sort(used.begin(), used.end(), [](const VerRecord* a, const VerRecord* b) {
			return a->firstUse < b->firstUse;
		});
		std::vector<std::vector<std::string>> levels;
		for (const VerRecord* r : used) {
			if (!r->producer.empty())
				levels.push_back(std::vector<std::string>{ r->producer });
			if (!r->readers.empty())
				levels.push_back(r->readers);
		}
		for (size_t i = 1; i < levels.size(); ++i)
			for (const auto& a : levels[i - 1])
				for (const auto& b : levels[i])
					if (a != b) finalEdges.insert(BuilderEdge{ a, b });
	}
	out.graph.orderEdges.assign(finalEdges.begin(), finalEdges.end());

	// ---- 三情况报告：资源依赖补边 / 纯顺序边 ----
	std::set<BuilderEdge> resEdges;
	for (const auto& kv : recs) {
		const VerRecord& r = kv.second;
		if (r.producer.empty()) continue;
		for (const auto& reader : r.readers)
			if (reader != r.producer)
				resEdges.insert(BuilderEdge{ r.producer, reader });
	}
	std::vector<std::vector<uint32_t>> resAdj(n);
	for (const auto& e : resEdges) {
		if (passMap.find(e.from) == passMap.end() || passMap.find(e.to) == passMap.end())
			continue;
		const uint32_t a = static_cast<uint32_t>(passMap.at(e.from));
		const uint32_t b = static_cast<uint32_t>(passMap.at(e.to));
		auto& adj = resAdj[a];
		if (std::find(adj.begin(), adj.end(), b) == adj.end())
			adj.push_back(b);
	}
	auto reach = [](const std::map<std::string, size_t>& pm,
		const std::vector<std::vector<uint32_t>>& adj,
		const BuilderEdge& e) {
		auto ia = pm.find(e.from);
		auto ib = pm.find(e.to);
		if (ia == pm.end() || ib == pm.end()) return false;
		return BuilderReachable(adj, static_cast<uint32_t>(ia->second),
			static_cast<uint32_t>(ib->second));
	};
	for (const auto& e : resEdges) {
		if (!reach(passMap, userAdj, e)) {
			out.topology.impliedAdded.push_back(e);
			addDiag(BuilderSeverity::Warning, BuilderDiagKind::ImpliedEdgeAdded,
				"资源依赖补出顺序边：" + e.from + " -> " + e.to, e);
		}
	}
	for (const auto& e : userEdges)
		if (!reach(passMap, resAdj, e))
			out.topology.orderingOnly.push_back(e);

	// ---- 依据版本化声明重建资源 ----
	for (const auto& kv : recs) {
		auto ri = resMap.find(kv.first.name);
		if (ri == resMap.end()) continue;
		const ResourceDecl& rd = m_resources[ri->second];
		LoweredResource lr;
		lr.ref = kv.first;
		lr.desc = rd.desc;
		lr.view = rd.view;
		lr.viewFormat = rd.viewFormat;
		lr.barrierRange = rd.barrierRange;
		lr.initialLayout = rd.initialLayout;
		lr.crossFrame = rd.crossFrame;
		lr.external = rd.IsExternal();
		lr.externalImage = rd.externalImage;
		lr.externalBuffer = rd.externalBuffer;
		lr.borrowedLater = rd.borrowedLater;
		out.graph.resources.push_back(std::move(lr));
	}
	out.graph.passes = std::move(lowered);
	out.ok = errorCount == 0;
	return out;
}

bool RenderGraphBuilder::Build(RenderGraph& out, LowerResult* result) const {
	LowerResult r = Lower();
	if (result != nullptr)
		*result = r;
	if (!r.ok)
		return false;

	// 每个版本引用对应的写者 / 读者（pass 名）
	std::map<ResourceRef, std::string> writerOf;
	std::map<ResourceRef, std::set<std::string>> readersOf;
	std::map<std::string, Pass::Type> passType;
	for (const LoweredPass& p : r.graph.passes) {
		passType[p.name] = p.type;
		for (const ResourceRef& w : p.writes)
			writerOf[w] = p.name;
		for (const ResourceRef& rd : p.reads)
			readersOf[rd].insert(p.name);
	}

	for (const LoweredResource& lr : r.graph.resources) {
		std::string writer;
		auto wit = writerOf.find(lr.ref);
		if (wit != writerOf.end())
			writer = wit->second;
		std::set<std::string> readers;
		auto rit = readersOf.find(lr.ref);
		if (rit != readersOf.end())
			readers = rit->second;

		const bool muxOutput = !writer.empty()
			&& passType.find(writer) != passType.end()
			&& passType.at(writer) == Pass::Mux;

		if (lr.externalImage != nullptr) {
			out.RegisterResource(lr.ref.name, readers, writer, lr.externalImage,
				lr.view, lr.initialLayout, lr.viewFormat, lr.barrierRange,
				lr.crossFrame, lr.ref.version);
		}
		else if (lr.externalBuffer != nullptr) {
			out.RegisterResource(lr.ref.name, readers, writer, lr.externalBuffer, lr.ref.version);
		}
		else if (muxOutput) {
			out.RegisterMuxOutput(lr.ref.name, readers, writer, lr.desc, lr.ref.version);
		}
		else {
			Resource::Type type = Resource::None;
			if (std::get_if<ImageDesc>(&lr.desc)) type = Resource::Image;
			else if (std::get_if<BufferDesc>(&lr.desc)) type = Resource::Buffer;
			out.RegisterResource(lr.ref.name, readers, writer, type, lr.desc,
				lr.view, lr.viewFormat, lr.barrierRange,
				lr.initialLayout, lr.crossFrame, lr.ref.version);
		}
	}

	// 顺序边按目标 pass 聚合成 PassOrder.after
	std::map<std::string, std::set<std::string>> after;
	for (const BuilderEdge& e : r.graph.orderEdges)
		after[e.to].insert(e.from);

	for (const LoweredPass& p : r.graph.passes) {
		RenderGraph::PassOrder order;
		auto ait = after.find(p.name);
		if (ait != after.end())
			order.after = ait->second;
		out.RegisterPass(p.name, p.reads, p.writes, p.type, p.desc,
			std::move(order), p.queue, p.bindingVersions);
	}

	for (const LoweredAliasGroup& g : r.graph.aliasGroups)
		out.AliasResourceVersions(g.members);

	// 静态纹理堆：必须在 ProcessRegister / RealizeAll 之前注册
	for (const StaticTextureDecl& decl : m_staticTextures) {
		if (decl.image == nullptr)
			continue;
		out.RegisterStaticTexture(decl.name, *decl.image, decl.view, decl.viewFormat, decl.sampler);
	}

	return true;
}

}
