#pragma once
#include "RenderGraphAbstracted.h"

namespace dEngine {
	class RenderPass : public Pass {
	public:
		RenderPass(PassTopology&& topo, RenderPassDesc desc)
			: Pass(std::move(topo)), mDesc(std::move(desc)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Render; }
		const RenderPassDesc& get_desc() const { return mDesc; }
		// 实例化产物（Realize 填充；execute 阶段经此取后端对象，未实例化为 nullptr）
		const rhi::Pipeline* get_rhi() const { return m_rhiHandle.get(); }
		// 绘制数量来源：执行参数，每帧由宿主设置（culling/LOD/indirect 都在这里变）
		void SetDraw(const DrawSource& draw) { mDraw = draw; }
		const DrawSource& get_draw() const { return mDraw; }
		// push constant 运行期覆盖（须与 pipeline.pushConstantSize 匹配）
		void SetPushConstants(const void* data, uint32_t size);
		const std::vector<uint8_t>& get_push_constants() const { return mPushConstants; }
	private:
		friend RenderGraph;
		RenderPassDesc mDesc;
		DrawSource mDraw {};
		std::vector<uint8_t> mPushConstants;
		std::unique_ptr<rhi::Pipeline> m_rhiHandle;
	};

	// 计算节点：desc 提供 shader/工作组数，句柄与描述符集由 Realize 填充
	class ComputePass : public Pass {
	public:
		ComputePass(PassTopology&& topo, ComputePassDesc desc)
			: Pass(std::move(topo)), mDesc(std::move(desc)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Compute; }
		const ComputePassDesc& get_desc() const { return mDesc; }
		const rhi::Pipeline* get_rhi() const { return m_rhiHandle.get(); }
		// push constant 运行期覆盖（须与 pushConstantSize 匹配）
		void SetPushConstants(const void* data, uint32_t size);
		const std::vector<uint8_t>& get_push_constants() const { return mPushConstants; }
	private:
		friend RenderGraph;
		ComputePassDesc mDesc;
		std::vector<uint8_t> mPushConstants;
		std::unique_ptr<rhi::Pipeline> m_rhiHandle;
	};

	// Host 节点：纯 CPU 回调（解码/上传等），不创建后端对象、不录命令。
	class HostPass : public Pass {
	public:
		HostPass(PassTopology&& topo, HostPassDesc desc)
			: Pass(std::move(topo)), mDesc(std::move(desc)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Host; }
		const HostPassDesc& get_desc() const { return mDesc; }
		// 回调是执行参数（每帧可换）：设置后覆盖 desc 中的声明
		void SetCallback(std::function<void(HostContext&)> callback) { mCallback = std::move(callback); }
		const std::function<void(HostContext&)>& get_callback() const {
			return mCallback ? mCallback : mDesc.callback;
		}
	private:
		HostPassDesc mDesc;
		std::function<void(HostContext&)> mCallback;
	};

	// 传输节点：单次 buffer -> image 拷贝（上传路径的 GPU 侧），无管线/描述符
	class TransferPass : public Pass {
	public:
		TransferPass(PassTopology&& topo, TransferPassDesc desc)
			: Pass(std::move(topo)), mDesc(std::move(desc)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Transfer; }
		const TransferPassDesc& get_desc() const { return mDesc; }
	private:
		TransferPassDesc mDesc;
	};

	// 呈现节点：只承载布局转换到 PresentSrc，无管线/描述符/命令。
	class PresentPass : public Pass {
	public:
		PresentPass(PassTopology&& topo, PresentPassDesc desc)
			: Pass(std::move(topo)), mDesc(std::move(desc)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Present; }
		const PresentPassDesc& get_desc() const { return mDesc; }
	private:
		PresentPassDesc mDesc;
	};

	// Mux 节点：零命令选择点，选择在 BeginFrame 的主机侧 retarget 完成。
	class MuxPass : public Pass {
	public:
		MuxPass(PassTopology&& topo, MuxPassDesc desc)
			: Pass(std::move(topo)), mDesc(std::move(desc)) {
		}
		Pass::Type get_type() const override { return Pass::Type::Mux; }
		const MuxPassDesc& get_desc() const { return mDesc; }
	private:
		MuxPassDesc mDesc;
	};

	// 图像节点：一个节点 = 一个视图身份，可自建（Own）或借用（Borrow）。
	class Image : public Resource {
	public:
		// Own：view 为本节点视图（默认首 mip 首层），count=0 表示整图。
		Image(ResourceTopology&& topo, ImageDesc desc, ImageSubresource view,
			rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			ImageSubresourceRange barrierRange = ImageSubresourceRange{ {}, 0, 0 })
			: Resource(std::move(topo)), mDesc(std::move(desc)), mView(view),
			  mViewFormat(viewFormat), mBarrierRange(barrierRange) {
		}
		// Borrow：不拥有底层 image，描述信息由外部 image 回填
		Image(ResourceTopology&& topo, const rhi::Image* external, ImageSubresource view,
			rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			ImageSubresourceRange barrierRange = ImageSubresourceRange{ {}, 0, 0 });
		// Borrow-later：注册时只有 desc（如 mux 输出），句柄由 BeginFrame 的 retarget 填入
		Image(ResourceTopology&& topo, ImageDesc desc, bool borrowedLater, ImageSubresource view,
			rhi::ImageFormat viewFormat = rhi::ImageFormat::None,
			ImageSubresourceRange barrierRange = ImageSubresourceRange{ {}, 0, 0 });

		Resource::Type get_type() const override { return Resource::Type::Image; }
		const ImageDesc& get_desc() const { return mDesc; }
		// 本节点代表的子资源视图：绑定时作为起点，数组绑定按 layer 递增展开
		ImageSubresource get_view() const { return mView; }
		rhi::ImageFormat get_view_format() const { return mViewFormat; }
		// 同步范围：count 为 0 时补齐到图像末尾；默认整图，收窄后 barrier 只转换该区间
		ImageSubresourceRange get_barrier_range() const { return mBarrierRange; }
		// 借用节点（含 Borrow-later）：不创建、不拥有；Borrow-later 的句柄在 retarget 前为空
		bool is_borrowed() const { return mBorrowed; }
		// 实例化产物（Own 为自建句柄；Borrow 直接返回外部句柄；未实例化为 nullptr）
		const rhi::Image* get_rhi() const { return mExternal ? mExternal : m_rhiHandle.get(); }
	private:
		friend RenderGraph;
		ImageDesc mDesc;
		ImageSubresource mView {};
		rhi::ImageFormat mViewFormat = rhi::ImageFormat::None;
		ImageSubresourceRange mBarrierRange { {}, 0, 0 };
		std::unique_ptr<rhi::Image> m_rhiHandle;
		const rhi::Image*          mExternal = nullptr;
		bool                       mBorrowed = false;
	};

	// 缓冲节点：语义与 Image 同构，可自建（Own）或借用（Borrow）。
	class Buffer : public Resource {
	public:
		// Own：按 BufferDesc 在 Realize 时创建
		Buffer(ResourceTopology&& topo, BufferDesc desc)
			: Resource(std::move(topo)), mDesc(std::move(desc)) {
		}
		// Borrow：不拥有底层 buffer，描述信息由外部 buffer 回填
		Buffer(ResourceTopology&& topo, const rhi::Buffer* external);
		// Borrow-later：注册时只有描述，句柄由运行期 retarget 填入
		Buffer(ResourceTopology&& topo, BufferDesc desc, bool borrowedLater)
			: Resource(std::move(topo)), mDesc(std::move(desc)), mBorrowed(borrowedLater) {
		}

		Resource::Type get_type() const override { return Resource::Type::Buffer; }
		const BufferDesc& get_desc() const { return mDesc; }
		bool is_borrowed() const { return mBorrowed; }
		// 实例化产物（Own 为自建句柄；Borrow 直接返回外部句柄；未实例化为 nullptr）
		const rhi::Buffer* get_rhi() const { return mExternal ? mExternal : m_rhiHandle.get(); }
	private:
		friend RenderGraph;
		BufferDesc mDesc;
		std::unique_ptr<rhi::Buffer> m_rhiHandle;
		const rhi::Buffer*           mExternal = nullptr;
		bool                         mBorrowed = false;
	};
}
