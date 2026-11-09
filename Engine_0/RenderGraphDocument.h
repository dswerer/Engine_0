#pragma once
#include "RenderGraphAbstracted.h"

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

	class RenderGraphDocument {
	public:
		// 外部依赖：文档不保存指针与回调，加载时由宿主注入（按图资源名查找）
		struct ExternalBindings {
			std::function<const rhi::Image*(const std::string& name)>  image;
			std::function<const rhi::Buffer*(const std::string& name)> buffer;
			std::map<std::string, std::function<void(HostContext&)>>   hostCallbacks;
		};

		// 抓取声明快照；外部资源只记标记，不记指针
		static RenderGraphDocument Capture(const RenderGraphBuilder& builder);

		bool Save(const std::string& path, std::string* error = nullptr) const;
		static std::optional<RenderGraphDocument> Load(const std::string& path,
			std::string* error = nullptr);

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
		};
		struct PassRecord {
			std::string       name;
			Pass::Type        type = Pass::None;
			rhi::QueueClass   queue = rhi::QueueClass::Graphics;
			std::set<std::string> reads;
			std::set<std::string> writes;
			PassDesc          desc;
		};

		std::vector<ResourceRecord>          m_resources;
		std::vector<PassRecord>              m_passes;
		std::vector<BuilderEdge>             m_edges;
		std::vector<std::vector<std::string>> m_aliasGroups;
	};
}
