#pragma once
#include "RenderGraphBuilder.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// 渲染图统一数据格式：保存「声明层」而非运行态。
// 只序列化 RenderGraphBuilder 的逻辑声明（资源 / Pass / 拓扑边 / 别名组），
// 注册期回填字段（resourceId、set/binding、alternativeIds 等）与外部句柄一律不落盘。
namespace dEngine {

	class ContentResolver;

	class RenderGraphDocument {
	public:
		// 外部依赖：文档不保存指针与回调，加载时由宿主注入（按图资源名查找）
		struct ExternalBindings {
			std::function<const rhi::Image*(const std::string& name)>  image;
			std::function<const rhi::Buffer*(const std::string& name)> buffer;
			// 文档记录了 asset 键时的回退解析（可由 AssetCatalog + 宿主装载实现）
			std::function<const rhi::Image*(const std::string& asset)>  imageAsset;
			std::function<const rhi::Buffer*(const std::string& asset)> bufferAsset;
			// 静态纹理堆：按堆名或资产键解析
			std::function<const rhi::Image*(const std::string& name)>  staticImage;
			std::function<const rhi::Image*(const std::string& asset)> staticImageAsset;
			std::map<std::string, std::function<void(HostContext&)>>   hostCallbacks;
		};

		// 抓取声明快照；外部资源只记标记，不记指针
		static RenderGraphDocument Capture(const RenderGraphBuilder& builder);

		bool Save(const std::string& path, std::string* error = nullptr) const;
		static std::optional<RenderGraphDocument> Load(const std::string& path,
			std::string* error = nullptr);
		// 经 ContentResolver 读取逻辑路径（支持 pack: 内存包来源）
		static std::optional<RenderGraphDocument> Load(const ContentResolver& resolver,
			std::string_view logicalPath, std::string* error = nullptr);

		// 只重建声明；Lower/Build 由调用方执行，便于拿到 BuilderDiagnostic
		bool Restore(RenderGraphBuilder& builder, const ExternalBindings& externals = {},
			std::vector<std::string>* errors = nullptr) const;

		// 规范化 JSON（字段顺序固定，便于 diff 与 round-trip 比对）
		std::string ToJson() const;
		static std::optional<RenderGraphDocument> FromJson(std::string_view text,
			std::string* error = nullptr);

		size_t ResourceCount() const { return m_resources.size(); }
		size_t PassCount() const { return m_passes.size(); }
		size_t EdgeCount() const { return m_edges.size(); }
		size_t StaticTextureCount() const { return m_staticTextures.size(); }

	private:
		struct ResourceRecord {
			std::string        name;
			Resource::Type     type = Resource::None;
			ResourceDesc       desc;
			ImageSubresource   view {};
			rhi::ImageFormat   viewFormat = rhi::ImageFormat::None;
			ImageSubresourceRange barrierRange { {}, 0, 0 };
			rhi::ImageLayout   initialLayout = rhi::ImageLayout::Undefined;
			bool               crossFrame = false;
			bool               externalImage = false;
			bool               externalBuffer = false;
			std::string        asset; // 外部资源的清单键（空 = 未记录）
		};
		struct StaticTextureRecord {
			std::string                     name;
			std::string                     asset;
			ImageSubresource                view {};
			rhi::ImageFormat                viewFormat = rhi::ImageFormat::None;
			std::optional<rhi::SamplerDesc> sampler;
		};
		struct PassRecord {
			std::string       name;
			Pass::Type        type = Pass::None;
			rhi::QueueClass   queue = rhi::QueueClass::Graphics;
			std::set<std::string> reads;
			std::set<std::string> writes;
			std::set<std::string> inPlace;
			PassDesc          desc;
		};

		std::vector<ResourceRecord>          m_resources;
		std::vector<PassRecord>              m_passes;
		std::vector<BuilderEdge>             m_edges;
		std::vector<std::vector<std::string>> m_aliasGroups;
		std::vector<StaticTextureRecord>     m_staticTextures;
	};

	// 模块文档：保存「模块声明层」（端口 / 资源 / Pass / 拓扑 / 别名），不保存任何句柄。
	// 固定引用只记 name / asset / 类型，加载时由宿主解析并调用 RenderGraphModule::Initialize。
	class RenderGraphModuleDocument {
	public:
		struct FixedBindings {
			std::function<const rhi::Image*(const std::string& name)>  image;
			std::function<const rhi::Buffer*(const std::string& name)> buffer;
			std::function<const rhi::Image*(const std::string& asset)>  imageAsset;
			std::function<const rhi::Buffer*(const std::string& asset)> bufferAsset;
			std::map<std::string, std::function<void(HostContext&)>>   hostCallbacks;
		};

		// 从已编译模块抓取声明快照（含固定引用元数据，不含指针）
		static RenderGraphModuleDocument Capture(const RenderGraphModule& module);

		bool Save(const std::string& path, std::string* error = nullptr) const;
		static std::optional<RenderGraphModuleDocument> Load(const std::string& path,
			std::string* error = nullptr);
		// 经 ContentResolver 读取逻辑路径（支持 pack: 内存包来源）
		static std::optional<RenderGraphModuleDocument> Load(const ContentResolver& resolver,
			std::string_view logicalPath, std::string* error = nullptr);

		// bindings 为空：只重建未初始化的模块定义（固定引用保持未绑定）
		// bindings 非空：解析全部固定引用 -> CompileModule -> Initialize，任一缺失即失败
		bool Restore(RenderGraphModule& out, const FixedBindings* bindings = nullptr,
			std::vector<std::string>* errors = nullptr) const;

		// 规范化 JSON（字段顺序固定，便于 diff 与 round-trip 比对）
		std::string ToJson() const;
		static std::optional<RenderGraphModuleDocument> FromJson(std::string_view text,
			std::string* error = nullptr);

		const std::string& Name() const { return m_name; }
		size_t ResourceCount() const { return m_resources.size(); }
		size_t PassCount() const { return m_passes.size(); }
		size_t PortCount() const { return m_ports.size(); }
		size_t FixedResourceCount() const;
		size_t EdgeCount() const { return m_edges.size(); }

		// 依赖清单：打包时用于收集 SPIR-V / 资产文件
		std::vector<std::string> ShaderRefs() const;
		std::vector<std::string> AssetKeys() const;

	private:
		struct ResourceRecord {
			std::string        name;
			Resource::Type     type = Resource::None;
			ResourceDesc       desc;
			ImageSubresource   view {};
			rhi::ImageFormat   viewFormat = rhi::ImageFormat::None;
			ImageSubresourceRange barrierRange { {}, 0, 0 };
			rhi::ImageLayout   initialLayout = rhi::ImageLayout::Undefined;
			bool               crossFrame = false;
			bool               externalImage = false;
			bool               externalBuffer = false;
			std::string        asset;
		};
		struct PortRecord {
			std::string    name;
			ModulePortKind kind = ModulePortKind::Input;
		};
		struct PassRecord {
			std::string       name;
			Pass::Type        type = Pass::None;
			rhi::QueueClass   queue = rhi::QueueClass::Graphics;
			std::set<std::string> reads;
			std::set<std::string> writes;
			std::set<std::string> inPlace;
			PassDesc          desc;
		};

		std::string                          m_name;
		std::vector<ResourceRecord>          m_resources;
		std::vector<PortRecord>              m_ports;
		std::vector<PassRecord>              m_passes;
		std::vector<BuilderEdge>             m_edges;
		std::vector<std::vector<std::string>> m_aliasGroups;
	};
}
