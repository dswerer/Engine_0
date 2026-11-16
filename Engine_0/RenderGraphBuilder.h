#pragma once
#include "RenderGraphAbstracted.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// 前端（Builder）：逻辑声明、版本化 lowering、模块编译与实例化。
namespace dEngine {

	enum class BuilderSeverity { Warning, Error };

	enum class BuilderDiagKind {
		ImpliedEdgeAdded,        // 资源依赖新增了用户未声明的边
		Contradiction,           // 用户拓扑成环 / 与资源依赖矛盾
		WriterOrderUndetermined, // 同一资源的多个写者未被用户拓扑定序
		ReadBeforeProducer,      // 读者排在生产者之前且资源没有可读初态
		SamePassReadWrite,       // 同一 pass 内同一逻辑名既读又写
		DoubleBufferNotImplemented, // 同 pass 读写但未声明 inPlace：语义为双缓冲，暂未实现
		InPlaceNotReadWrite,     // inPlace 集中的资源必须同时出现在读写集
		InPlaceTopologyRequired, // 模块内 in-place pass 缺少显式前驱/后继
		InPlaceQueueMismatch,    // in-place 资源被不同队列的 pass 使用
		VersionAliasConflict,    // 版本别名组的寿命重叠或同 pass 触达多个成员
		DuplicateName,
		UnknownName,
		EmptyName,
		MuxMultiWriter,
		UnboundExternal        // 外部/资产键资源没有可用句柄
	};

	struct BuilderEdge {
		std::string from;
		std::string to;
		bool operator<(const BuilderEdge& o) const {
			return std::tie(from, to) < std::tie(o.from, o.to);
		}
		bool operator==(const BuilderEdge& o) const {
			return from == o.from && to == o.to;
		}
	};

	struct BuilderDiagnostic {
		BuilderSeverity          severity = BuilderSeverity::Error;
		BuilderDiagKind          kind = BuilderDiagKind::Contradiction;
		std::string              message;
		std::optional<BuilderEdge> edge;
	};

	struct BuilderTopologyReport {
		std::vector<BuilderEdge> declared;     // 用户声明的顺序边
		std::vector<BuilderEdge> impliedAdded; // case 1：资源依赖补出的边（带警告）
		std::vector<BuilderEdge> orderingOnly; // case 3：纯顺序边，不改资源依赖
		std::vector<BuilderEdge> conflicts;    // case 2：矛盾边
	};

	struct BuilderVersionInfo {
		std::string resource;
		uint32_t    version = 0;
		std::string producer;      // 空 = 导入版本
		bool        aliasPrevious = false;
	};

	struct LoweredResource {
		ResourceRef        ref;
		ResourceDesc       desc;
		ImageSubresource   view {};
		rhi::ImageFormat   viewFormat = rhi::ImageFormat::None;
		ImageSubresourceRange barrierRange { {}, 0, 0 };
		rhi::ImageLayout   initialLayout = rhi::ImageLayout::Undefined;
		bool               crossFrame = false;
		bool               external = false;
		const rhi::Image*  externalImage = nullptr;
		const rhi::Buffer* externalBuffer = nullptr;
		bool               borrowedLater = false;
	};

	struct LoweredPass {
		std::string       name;
		Pass::Type        type = Pass::None;
		PassDesc          desc;
		rhi::QueueClass   queue = rhi::QueueClass::Graphics;
		std::set<ResourceRef> reads;
		std::set<ResourceRef> writes;
		// 本 pass 内逻辑名 -> 版本；同一名不允许同时出现在读/写集，故单个版本即可
		std::map<std::string, uint32_t> bindingVersions;
	};

	struct LoweredAliasGroup {
		std::vector<ResourceRef> members;
		bool mustAlias = true;
	};

	struct LoweredGraph {
		std::vector<LoweredResource>    resources;
		std::vector<LoweredPass>        passes;
		std::vector<LoweredAliasGroup>  aliasGroups;
		std::vector<BuilderEdge>        orderEdges; // 用户边 + 别名钉住边
	};

	struct LowerResult {
		bool                           ok = false;
		BuilderTopologyReport          topology;
		std::vector<BuilderDiagnostic> diagnostics;
		std::vector<BuilderVersionInfo> versions;
		LoweredGraph                   graph;
	};

	// ---- 模块：逻辑声明的可复用封装，端口是已声明资源的对外视图 ----

	enum class ModulePortKind { Input, Output, Inout };

	struct ModulePort {
		std::string    name;   // 模块内部逻辑名（必须先 DeclareResource）
		ModulePortKind kind = ModulePortKind::Input;
	};

	struct ModulePortInfo {
		ModulePort     port;
		Resource::Type type = Resource::None;
		ResourceDesc   desc;
		ImageSubresource view {};
		rhi::ImageFormat viewFormat = rhi::ImageFormat::None;
		ImageSubresourceRange barrierRange { {}, 0, 0 };
		bool           crossFrame = false;
	};

	struct ModuleBinding {
		std::string port;
		std::string parentResource;   // 父 Builder 已声明的逻辑名
	};

	// 固定引用：模块自带的外部资源，不经过父级端口；实例化时按身份收敛为父级唯一声明
	struct ModuleFixedResource {
		std::string       name;
		Resource::Type    type = Resource::None;
		ResourceDesc      desc;
		ImageSubresource  view {};
		rhi::ImageFormat  viewFormat = rhi::ImageFormat::None;
		ImageSubresourceRange barrierRange { {}, 0, 0 };
		rhi::ImageLayout  initialLayout = rhi::ImageLayout::Undefined;
		bool              crossFrame = false;
		std::string       asset;   // 资产清单键（可为空）
		const rhi::Image*  image = nullptr;
		const rhi::Buffer* buffer = nullptr;
		bool Bound() const { return image != nullptr || buffer != nullptr; }
	};

	// 模块初始化输入：把固定引用名解析成具体句柄
	struct ModuleFixedBinding {
		std::string        name;
		const rhi::Image*  image = nullptr;
		const rhi::Buffer* buffer = nullptr;
	};

	struct ModuleInstance {
		std::string tag;
		std::vector<std::string> Entries;   // 已作用域化
		std::vector<std::string> Exits;
	};

	class RenderGraphModule;
	class AssetLoader;

	// 资源声明的选项：按资源类型拆分，避免 buffer 携带无意义的图像字段
	struct ImageOptions {
		ImageSubresource      view {};
		rhi::ImageFormat      viewFormat = rhi::ImageFormat::None;
		ImageSubresourceRange barrierRange { {}, 0, 0 };
		rhi::ImageLayout      initialLayout = rhi::ImageLayout::Undefined;
		bool                  crossFrame = false;
	};

	struct BufferOptions {
		bool crossFrame = false;
	};

	// 静态纹理堆条目：按声明顺序分配 slot（固定 set 0 / binding 0）
	struct StaticTextureDecl {
		std::string                     name;
		const rhi::Image*               image = nullptr;
		ImageSubresource                view {};
		rhi::ImageFormat                viewFormat = rhi::ImageFormat::None;
		std::optional<rhi::SamplerDesc> sampler;
		std::string                     asset; // 资产清单键（仅用于文档）
	};

	// 多写者前端：只收集逻辑声明；Lower() 是纯函数，不接触 Device。
	class RenderGraphBuilder {
	public:
		struct Topology {
			std::vector<BuilderEdge> edges;
			static Topology Sequence(std::vector<std::string> order); // 相邻成边
			Topology& Then(const std::string& from, const std::string& to);
			// 集合间全连接边：用于把模块实例的 Entries/Exits 接入父拓扑
			Topology& ThenAll(const std::vector<std::string>& from,
				const std::vector<std::string>& to);
		};

		// 前端合并：把另一份逻辑声明并入本 Builder，合并发生在 Lower 之前，不涉及版本。
		struct ImportOptions {
			std::string passPrefix;              // 追加图 Pass 名前缀，避免同名冲突
			bool dedupIdenticalResources = true; // 同名且描述完全一致的资源自动合一
		};

		explicit RenderGraphBuilder(rhi::Device* device = nullptr, AssetLoader* loader = nullptr)
			: m_device(device), m_loader(loader) {}
		// 将 other 的声明并入本 Builder；失败返回 false 且本 Builder 保持原状。
		bool ImportDeclarations(const RenderGraphBuilder& other, const ImportOptions& options = {});
		// 把当前声明编译成模块：端口必须已声明为 owned 资源，且被至少一个 pass 引用
		bool CompileModule(std::string name, std::vector<ModulePort> ports,
			RenderGraphModule* out, std::vector<std::string>* errors = nullptr) const;
		// 在父 Builder 中展开模块实例：端口显式绑定父资源，内部名加 tag 作用域
		std::optional<ModuleInstance> Instantiate(const RenderGraphModule& module,
			std::string tag, const std::vector<ModuleBinding>& bindings,
			std::vector<std::string>* errors = nullptr);

		void DeclareImageResource(std::string name, ImageDesc desc, ImageOptions options = {});
		void DeclareBufferResource(std::string name, BufferDesc desc, BufferOptions options = {});
		// 已绑定句柄；assetKey 仅用于文档记录，可为空
		void DeclareImageResource(std::string name, const rhi::Image* external,
			ImageOptions options = {}, std::string assetKey = {});
		void DeclareBufferResource(std::string name, const rhi::Buffer* external,
			BufferOptions options = {}, std::string assetKey = {});
		// 资产键：有 loader 时现场装载；否则声明为未绑定，等待初始化
		bool DeclareImageResource(std::string name, std::string assetKey,
			ImageOptions options = {}, std::string* error = nullptr);
		bool DeclareBufferResource(std::string name, std::string assetKey,
			BufferOptions options = {}, std::string* error = nullptr);
		// 静态纹理堆：Build 时按声明顺序注册；需设备支持 bindless
		void DeclareStaticTexture(std::string name, const rhi::Image* image,
			ImageSubresource view = {}, rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			std::optional<rhi::SamplerDesc> sampler = std::nullopt, std::string asset = {});

		// 读写集推导版：Render/Compute/Transfer 从 PassDesc 推导读写集；
		// Host/Present/Mux 无可推导绑定（Mux 输出不在 desc 内），须用显式重载
		void DeclarePass(std::string name, Pass::Type type, PassDesc desc = {},
			std::optional<rhi::QueueClass> queue = std::nullopt);
		void DeclarePass(std::string name, std::set<std::string> reads, std::set<std::string> writes,
			Pass::Type type, PassDesc desc = {},
			std::optional<rhi::QueueClass> queue = std::nullopt);
		// 显式就地读写集：必须同时出现在 reads/writes；不在 inPlace 的同 Pass 读写按双缓冲处理
		void DeclarePass(std::string name, std::set<std::string> reads, std::set<std::string> writes,
			std::set<std::string> inPlace, Pass::Type type, PassDesc desc = {},
			std::optional<rhi::QueueClass> queue = std::nullopt);

		void DeclareTopology(Topology topology);
		// 用户声明的别名组是硬契约：成员必须同类型/同描述/同视图；不满足直接报错，不会静默退回
		void AliasLogicalResources(std::vector<std::string> names);

		LowerResult Lower() const;
		// 把 lowering 结果注册进目标图；只写声明，ProcessRegister/RealizeAll 由调用方完成。
		bool Build(RenderGraph& out, LowerResult* result = nullptr) const;
		rhi::Device* Device() const { return m_device; }

	private:
		friend class RenderGraphDocument;
		friend class RenderGraphModule;
		friend class RenderGraphModuleDocument;

		struct ResourceDecl {
			std::string       name;
			Resource::Type    type = Resource::None;
			ResourceDesc      desc;
			ImageSubresource  view {};
			rhi::ImageFormat  viewFormat = rhi::ImageFormat::None;
			ImageSubresourceRange barrierRange { {}, 0, 0 };
			rhi::ImageLayout  initialLayout = rhi::ImageLayout::Undefined;
			bool              crossFrame = false;
			std::string       asset; // 外部资源的清单键（仅用于文档）
			// 显式标记「引用外部句柄」，与指针是否已绑定解耦（支持先声明后初始化）
			bool              external = false;
			const rhi::Image*  externalImage = nullptr;
			const rhi::Buffer* externalBuffer = nullptr;
			bool              borrowedLater = false;
			bool IsExternal() const {
				return external || externalImage != nullptr || externalBuffer != nullptr || borrowedLater;
			}
		};
		struct PassDecl {
			std::string       name;
			std::set<std::string> reads;
			std::set<std::string> writes;
			std::set<std::string> inPlace; // 必须 ⊆ reads ∩ writes；为空时同 Pass 读写视为双缓冲
			Pass::Type        type = Pass::None;
			PassDesc          desc;
			rhi::QueueClass   queue = rhi::QueueClass::Graphics;
		};
		static bool SameDecl(const ResourceDecl& a, const ResourceDecl& b);
		// 把 PassDesc 内的图侧名字按映射重写；ShaderBinding 只写 graphResourceName
		static void RewritePassBindings(PassDesc& desc,
			const std::map<std::string, std::string>& nameMap);
		void DeclareExternalImage(std::string name, const rhi::Image* image,
			std::string asset, ImageOptions options);
		void DeclareExternalBuffer(std::string name, const rhi::Buffer* buffer,
			std::string asset, BufferOptions options);

		rhi::Device* m_device = nullptr;
		AssetLoader* m_loader = nullptr;
		std::vector<ResourceDecl> m_resources;
		std::vector<PassDecl>     m_passes;
		std::vector<BuilderEdge>  m_edges;
		std::vector<std::vector<std::string>> m_aliasGroups;
		std::vector<StaticTextureDecl>     m_staticTextures;
		std::set<std::string>     m_instanceTags;
	};

	// 模块：只读声明快照 + 端口信息 + 内部入口/出口（内部名）
	class RenderGraphModule {
	public:
		const std::string& Name() const { return mName; }
		const std::vector<ModulePortInfo>& Ports() const { return mPorts; }
		const std::vector<ModuleFixedResource>& FixedResources() const { return mFixed; }
		const std::vector<std::string>& Entries() const { return mEntryNames; }
		const std::vector<std::string>& Exits() const { return mExitNames; }

		// ---- 初始化：注入固定引用句柄（模块功能自身的资源，不由父级提供） ----
		// 整体校验后一次性应用；失败不改动模块。未列出的固定引用保持原状。
		bool Initialize(const std::vector<ModuleFixedBinding>& bindings,
			std::vector<std::string>* errors = nullptr);
		bool IsInitialized() const;
		std::vector<std::string> UnboundFixedResources() const;
	private:
		friend class RenderGraphBuilder;
		friend class RenderGraphModuleDocument;
		std::string                 mName;
		RenderGraphBuilder          mDecl;
		std::vector<ModulePortInfo> mPorts;
		std::vector<ModuleFixedResource> mFixed;
		std::vector<std::string>    mEntryNames;
		std::vector<std::string>    mExitNames;
	};

}
