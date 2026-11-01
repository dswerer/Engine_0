// Engine_0.cpp: 定义应用程序的入口点。
//

#include "Engine_0.h"
#include "RenderGraphAbstracted.h"
#include "RenderGraphNodes.h"
#include "rhi/RHI.h"
// stb_image：测试贴图上载。实现只在本 TU 展开（避免新增编译单元）
#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image/stb_image.h"
// 取 Win32 原生句柄以创建 Vulkan 表面（GLFW 不建 GL 上下文）
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
// windows.h 的 CreateSemaphore 宏会展开为 CreateSemaphoreA/W，与 rhi::Device::CreateSemaphore 冲突
#undef CreateSemaphore

#include <iostream>
#include <cstring>

using namespace std;
using namespace dEngine;

int main()
{
	// 冒烟测试：初始化 RHI 设备与交换链（隐藏窗口，不渲染）
	if (!glfwInit()) {
		cerr << "glfwInit 失败\n";
		return 1;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* window = glfwCreateWindow(1280, 720, "Engine_0 冒烟测试", nullptr, nullptr);

	rhi::DeviceDesc deviceDesc;
	deviceDesc.enableValidation = true;
	if (window) {
		deviceDesc.nativeWindowHandle = glfwGetWin32Window(window);
		deviceDesc.width = 1280;
		deviceDesc.height = 720;
	}

	if (auto device = rhi::Device::Create(deviceDesc)) {
		const uint32_t api = device->ApiVersion();
		cout << "Device: " << device->DeviceName()
		     << " (Vulkan " << (api >> 22) << "." << ((api >> 12) & 0x3FF) << ")\n";

		// 渲染图：注册、解析、实例化。graph 嵌套在设备作用域内——句柄归节点、随节点 RAII 先于 device 析构
		RenderGraph graph(device.get());

		// shader 目录（glslc 离线产物；未定义说明环境无 glslc，图内管线创建将因空路径失败）
#ifdef ENGINE_SHADER_DIR
		const std::string shaderDir = ENGINE_SHADER_DIR;
#else
		const std::string shaderDir;
#endif
#ifdef ENGINE_ASSET_DIR
		const std::string assetDir = ENGINE_ASSET_DIR;
#else
		const std::string assetDir;
#endif

		// GBuffer 的 RenderPassDesc：一个颜色附件(slot0) + 一个深度附件
		RenderPassDesc gbufferDesc;
		{
			AttachmentBinding colorAtt;
			colorAtt.resourceName = "Color";
			colorAtt.slot = 0;
			colorAtt.load = LoadOp::Clear;
			colorAtt.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
			gbufferDesc.attachments.push_back(colorAtt);

			AttachmentBinding depthAtt;
			depthAtt.resourceName = "Depth";
			depthAtt.usage = Usage::DepthStencilAttachment;
			depthAtt.load = LoadOp::Clear;
			depthAtt.clear.value = DepthStencilValue{ 1.0f, 0 };
			gbufferDesc.attachments.push_back(depthAtt);

			// 管线状态：只写附件不读资源，用无绑定的纯色片元；格式签名/描述符布局由图派生
			gbufferDesc.pipeline.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/solid.frag.spv" },
			};
			gbufferDesc.pipeline.cullMode = CullMode::None; // 与既有冒烟一致
		}

		// ToneMap 的 RenderPassDesc：Backbuffer 附件 + 采样 Blurred
		RenderPassDesc tonemapDesc;
		{
			AttachmentBinding backAtt;
			backAtt.resourceName = "Backbuffer";
			backAtt.slot = 0;
			backAtt.load = LoadOp::Clear;
			backAtt.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
			tonemapDesc.attachments.push_back(backAtt);

			ShaderBinding sb;
			sb.resourceName = "Blurred"; // 须与 textured.frag 的采样变量名一致（按名对账约定）
			tonemapDesc.shaderBindings.push_back(sb);

			// 管线状态：set/binding 由 RHI 反射 textured.frag 得出，图按名对账回填
			tonemapDesc.pipeline.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/textured.frag.spv" },
			};
			tonemapDesc.pipeline.cullMode = CullMode::None;
		}

		// Blur 的 ComputePassDesc：采样 Color、storage 写 Blurred（blur.comp 为 8x8 工作组）
		ComputePassDesc blurDesc;
		{
			ShaderBinding in;
			in.resourceName = "Color";
			in.usage = Usage::SampledRead;
			blurDesc.shaderBindings.push_back(in);

			ShaderBinding out;
			out.resourceName = "Blurred";
			out.usage = Usage::StorageWrite;
			blurDesc.shaderBindings.push_back(out);

			blurDesc.shader = { rhi::ShaderStage::Compute, shaderDir + "/blur.comp.spv" };
			blurDesc.groupCountX = 1920 / 8;
			blurDesc.groupCountY = 1080 / 8;
		}

		// 示例图：GBuffer -> Blur -> ToneMap
		graph.RegisterPass("GBuffer", {},               {"Color", "Depth"}, Pass::Render, gbufferDesc);
		graph.RegisterPass("Blur",    {"Color"},        {"Blurred"},        Pass::Compute, blurDesc);
		graph.RegisterPass("ToneMap", {"Blurred"},      {"Backbuffer"},     Pass::Render, tonemapDesc);

		// 资源用 Pass 名声明写者/读者（单写者模型）
		ImageDesc colorDesc;  colorDesc.width = 1920; colorDesc.height = 1080; colorDesc.format = ImageFormat::RGBA16F;
		ImageDesc depthDesc;  depthDesc.width = 1920; depthDesc.height = 1080; depthDesc.format = ImageFormat::D32F;
		ImageDesc blurredDesc; blurredDesc.width = 1920; blurredDesc.height = 1080; blurredDesc.format = ImageFormat::RGBA16F;
		ImageDesc backDesc;   backDesc.width = 1920;  backDesc.height = 1080; backDesc.format = ImageFormat::RGBA8;

		graph.RegisterResource("Color",      {"Blur"},    "GBuffer", Resource::Image, colorDesc);
		graph.RegisterResource("Depth",      {},          "GBuffer", Resource::Image, depthDesc);
		graph.RegisterResource("Blurred",    {"ToneMap"}, "Blur",    Resource::Image, blurredDesc);
		graph.RegisterResource("Backbuffer", {},          "ToneMap", Resource::Image, backDesc);

		graph.ProcessRegister();
		graph.GenerateSubmitSequence();
		graph.PrintGraph();

		// 实例化冒烟：usage 全图派生 + 后端 Image/Pipeline 创建（句柄归节点，get_rhi 取用）
		graph.RealizeAll();
		{
			uint32_t realizedImages = 0, realizedPipelines = 0;
			for (uint32_t i = 0; graph.GetResource(i) != nullptr; ++i) {
				Resource* res = graph.GetResource(i);
				if (res->get_type() == Resource::Image && static_cast<Image*>(res)->get_rhi())
					++realizedImages;
			}
			for (uint32_t i = 0; graph.GetPass(i) != nullptr; ++i) {
				Pass* p = graph.GetPass(i);
				if (p->get_type() == Pass::Render && static_cast<RenderPass*>(p)->get_rhi())
					++realizedPipelines;
			}
			cout << "RenderGraph realize ok: " << realizedImages << " images, "
			     << realizedPipelines << " pipelines\n";
		}

		rhi::ImageCreateInfo imageInfo;
		imageInfo.width = 1920; imageInfo.height = 1080;
		imageInfo.format = rhi::ImageFormat::RGBA16F;
		imageInfo.usage  = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::Sampled;
		if (auto image = device->CreateImage(imageInfo)) {
			cout << "Image ok: " << image->Desc().width << "x" << image->Desc().height << "\n";
		}

		rhi::BufferCreateInfo bufferInfo;
		bufferInfo.size = 4096;
		bufferInfo.usage = rhi::BufferUsage::Uniform;
		bufferInfo.hostVisible = true;
		if (auto buffer = device->CreateBuffer(bufferInfo)) {
			if (void* mapped = buffer->Map()) {
				*static_cast<uint32_t*>(mapped) = 0x2A;
				cout << "Buffer ok: " << buffer->Desc().size << " bytes, mapped\n";
			}
		}

		// 交换链：创建 → 取一帧 → 改窗口尺寸触发失配 → 验证自动重建后恢复获取
		if (auto swapchain = device->CreateSwapchain()) {
			cout << "Swapchain ok: " << swapchain->Width() << "x" << swapchain->Height() << "\n";

			rhi::Swapchain::Frame frame = swapchain->AcquireNextImage();
			cout << "Acquire #1: acquired=" << frame.acquired
			     << " index=" << frame.imageIndex
			     << " backbuffer=" << (frame.backbuffer ? "borrowed-ok" : "null") << "\n";

			// 尺寸变化使表面与交换链不再匹配：期望 #2 返回 acquired=false 并自动重建
			glfwShowWindow(window);
			glfwSetWindowSize(window, 640, 480);
			glfwPollEvents();
			frame = swapchain->AcquireNextImage();
			cout << "Acquire #2 (resized): acquired=" << frame.acquired
			     << " index=" << frame.imageIndex << "\n";

			frame = swapchain->AcquireNextImage();
			cout << "Acquire #3: acquired=" << frame.acquired
			     << " size=" << swapchain->Width() << "x" << swapchain->Height() << "\n";
		}

		// 多队列冒烟：报告各族索引 + 空 cmd 经 semaphore 从 compute 队列串到 graphics 队列（无资源共享，不需 QFOT）
		{
			const uint32_t gf = device->QueueFamily(rhi::QueueClass::Graphics);
			const uint32_t cf = device->QueueFamily(rhi::QueueClass::AsyncCompute);
			const uint32_t tf = device->QueueFamily(rhi::QueueClass::Transfer);
			cout << "Queues: graphics=f" << gf << " compute=f" << cf
			     << (cf != gf ? "(dedicated)" : "(fallback)")
			     << " transfer=f" << tf << (tf != gf ? "(dedicated)" : "(fallback)") << "\n";

			if (auto sem = device->CreateSemaphore()) {
				rhi::CommandList computeList = device->BeginCommandList(rhi::QueueClass::AsyncCompute);
				rhi::SemaphoreSignal signal{ sem.get(), rhi::PipelineStage::AllCommands };
				rhi::SubmitInfo computeSubmit;
				computeSubmit.queue = rhi::QueueClass::AsyncCompute;
				computeSubmit.commandLists = { &computeList, 1 };
				computeSubmit.signals = { &signal, 1 };
				device->Submit(computeSubmit);

				rhi::CommandList gfxList = device->BeginCommandList();
				rhi::SemaphoreWait wait{ sem.get(), rhi::PipelineStage::AllCommands };
				rhi::SubmitInfo gfxSubmit;
				gfxSubmit.commandLists = { &gfxList, 1 };
				gfxSubmit.waits = { &wait, 1 };
				device->Submit(gfxSubmit);
				device->WaitForFrame();
				cout << "Multi-queue ok: compute(f" << cf << ") -> graphics(f" << gf << ") via semaphore\n";
			}
		}

#ifdef ENGINE_SHADER_DIR
		// 管线与描述符冒烟：shader 由 CMake 调 glslc 离线编译到 ENGINE_SHADER_DIR（shaderDir 已在图注册前定义）

		// A：全屏三角形 + 采样纹理（combined image sampler 路径）
		rhi::ImageCreateInfo texInfo;
		texInfo.width = 128; texInfo.height = 128;
		texInfo.format = rhi::ImageFormat::RGBA8;
		texInfo.usage  = rhi::ImageUsage::Sampled;
		if (auto tex = device->CreateImage(texInfo)) {
			rhi::GraphicsPipelineDesc psoA;
			psoA.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/textured.frag.spv" },
			};
			psoA.colorFormats[0] = rhi::ImageFormat::RGBA8;
			psoA.colorAttachmentCount = 1;
			psoA.depthFormat = rhi::ImageFormat::None; // 无深度附件，管线内部自动关闭深度测试
			psoA.cullMode = rhi::CullMode::None;
			// 描述符布局由 RHI 反射 textured.frag 得出（无需手填 bindings）

			if (auto pipeline = device->CreatePipeline(psoA)) {
				const rhi::DescriptorSetLayout* layout = pipeline->SetLayout(0);
				cout << "Pipeline A ok: set0-bindings=" << (layout ? layout->Bindings().size() : 0) << "\n";
				if (layout) {
					if (auto set = device->AllocateDescriptorSet(*layout)) {
						set->Write(0, *tex, rhi::ImageSubresource{ 0, 0 });
						cout << "DescriptorSet A ok: sampled image\n";
					}
				}
			}
		}

		// B：全屏三角形 + uniform 缓冲（buffer 描述符路径）
		rhi::BufferCreateInfo uboInfo;
		uboInfo.size = 256;
		uboInfo.usage = rhi::BufferUsage::Uniform;
		uboInfo.hostVisible = true;
		if (auto ubo = device->CreateBuffer(uboInfo)) {
			rhi::GraphicsPipelineDesc psoB;
			psoB.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/ubo.frag.spv" },
			};
			psoB.colorFormats[0] = rhi::ImageFormat::RGBA8;
			psoB.colorAttachmentCount = 1;
			psoB.depthFormat = rhi::ImageFormat::None;
			psoB.cullMode = rhi::CullMode::None;
			// 描述符布局由 RHI 反射 ubo.frag 得出（无需手填 bindings）

			if (auto pipeline = device->CreatePipeline(psoB)) {
				const rhi::DescriptorSetLayout* layout = pipeline->SetLayout(0);
				cout << "Pipeline B ok: set0-bindings=" << (layout ? layout->Bindings().size() : 0) << "\n";
				if (layout) {
					if (auto set = device->AllocateDescriptorSet(*layout)) {
						set->Write(0, *ubo, 0, ubo->Desc().size);
						cout << "DescriptorSet B ok: uniform buffer\n";
					}
				}
			}
		}

		// 完整渲染循环冒烟：Acquire -> 录制(barrier / dynamic rendering / draw) -> Submit -> Present -> WaitForFrame
		if (auto swapchain2 = device->CreateSwapchain()) {
			// 计算管线冒烟：Color -> 3x3 模糊 -> Blurred（storage 写），结果随后由图形管线采样
			rhi::ImageCreateInfo colorInfo;
			colorInfo.width = 64; colorInfo.height = 64;
			colorInfo.format = rhi::ImageFormat::RGBA16F;
			colorInfo.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst; // ClearColorImage 要求 TRANSFER_DST
			auto colorTex = device->CreateImage(colorInfo);

			rhi::ImageCreateInfo blurInfo;
			blurInfo.width = 64; blurInfo.height = 64;
			blurInfo.format = rhi::ImageFormat::RGBA16F;
			blurInfo.usage = rhi::ImageUsage::Storage | rhi::ImageUsage::Sampled; // 计算写 + 图形采样
			auto blurTex = device->CreateImage(blurInfo);

			rhi::ComputePipelineDesc computeDesc;
			computeDesc.shader = { rhi::ShaderStage::Compute, shaderDir + "/blur.comp.spv" };
			auto computePipeline = device->CreateComputePipeline(computeDesc);
			auto computeSet = (computePipeline && computePipeline->SetLayout(0))
				? device->AllocateDescriptorSet(*computePipeline->SetLayout(0)) : nullptr;
			if (colorTex && blurTex && computePipeline && computeSet) {
				computeSet->Write(0, *colorTex, rhi::ImageSubresource{ 0, 0 });
				computeSet->Write(1, *blurTex, rhi::ImageSubresource{ 0, 0 });
				cout << "Compute pipeline ok: set0-bindings="
					<< computePipeline->SetLayout(0)->Bindings().size() << "\n";

				// 与帧渲染同队列：barrier 足够表达依赖，CPU 侧用 WaitForFrame 等派发完成
				rhi::CommandList computeCmd = device->BeginCommandList();

				// Color: Undefined -> General（ClearColorImage 与 compute 采样都要求 GENERAL）
				rhi::ImageBarrier colorToGeneral;
				colorToGeneral.image = colorTex.get();
				colorToGeneral.oldLayout = rhi::ImageLayout::Undefined;
				colorToGeneral.newLayout = rhi::ImageLayout::General;
				colorToGeneral.dstStage = rhi::PipelineStage::Transfer;
				colorToGeneral.dstAccess = rhi::Access::TransferWrite;
				computeCmd.Barrier({ &colorToGeneral, 1 }, {});

				const float clearColor[4] = { 0.85f, 0.45f, 0.20f, 1.0f };
				computeCmd.ClearColor(*colorTex, rhi::ImageSubresource{ 0, 0 }, clearColor,
					rhi::ImageLayout::General);

				// Color: General -> ShaderReadOnly（计算采样读）；Blurred: Undefined -> General（storage 写）
				rhi::ImageBarrier colorToRead;
				colorToRead.image = colorTex.get();
				colorToRead.oldLayout = rhi::ImageLayout::General;
				colorToRead.newLayout = rhi::ImageLayout::ShaderReadOnly;
				colorToRead.srcStage = rhi::PipelineStage::Transfer;
				colorToRead.srcAccess = rhi::Access::TransferWrite;
				colorToRead.dstStage = rhi::PipelineStage::ComputeShader;
				colorToRead.dstAccess = rhi::Access::ShaderRead;

				rhi::ImageBarrier blurToGeneral;
				blurToGeneral.image = blurTex.get();
				blurToGeneral.oldLayout = rhi::ImageLayout::Undefined;
				blurToGeneral.newLayout = rhi::ImageLayout::General;
				blurToGeneral.dstStage = rhi::PipelineStage::ComputeShader;
				blurToGeneral.dstAccess = rhi::Access::ShaderWrite;
				computeCmd.Barrier({ &colorToRead, 1 }, {});
				computeCmd.Barrier({ &blurToGeneral, 1 }, {});

				computeCmd.BindPipeline(*computePipeline);
				computeCmd.BindDescriptorSet(0, *computeSet);
				computeCmd.Dispatch(64 / 8, 64 / 8, 1); // 8x8 工作组覆盖 64x64

				// Blurred: General -> ShaderReadOnly（随后由图形管线采样）
				rhi::ImageBarrier blurToRead;
				blurToRead.image = blurTex.get();
				blurToRead.oldLayout = rhi::ImageLayout::General;
				blurToRead.newLayout = rhi::ImageLayout::ShaderReadOnly;
				blurToRead.srcStage = rhi::PipelineStage::ComputeShader;
				blurToRead.srcAccess = rhi::Access::ShaderWrite;
				blurToRead.dstStage = rhi::PipelineStage::FragmentShader;
				blurToRead.dstAccess = rhi::Access::ShaderRead;
				computeCmd.Barrier({ &blurToRead, 1 }, {});

				rhi::SubmitInfo computeSubmit;
				computeSubmit.commandLists = { &computeCmd, 1 };
				device->Submit(computeSubmit);
				device->WaitForFrame();
				cout << "Compute dispatch ok: 8x8 groups -> storage image\n";
			}

			// 图侧计算冒烟：同一 blur.comp 改走 RenderGraph —— 两节点均借用外部 image
			// （输入 "Color" = 上一段 blurTex，输出 "Blurred" = 外部自建、用法由外部负责），
			// Pass 之前的 barrier 由 compiler 按使用链推导，RecordGraph 自动注入
			rhi::ImageCreateInfo graphOutInfo;
			graphOutInfo.width = 64; graphOutInfo.height = 64;
			graphOutInfo.format = rhi::ImageFormat::RGBA16F;
			graphOutInfo.usage = rhi::ImageUsage::Storage | rhi::ImageUsage::Sampled; // 借用节点自管用法
			auto graphOut = device->CreateImage(graphOutInfo);
			if (blurTex && graphOut) {
				RenderGraph computeGraph(device.get());

				ComputePassDesc graphBlur;
				{
					ShaderBinding in;
					in.resourceName = "Color";
					in.usage = Usage::SampledRead;
					graphBlur.shaderBindings.push_back(in);

					ShaderBinding out;
					out.resourceName = "Blurred";
					out.usage = Usage::StorageWrite;
					graphBlur.shaderBindings.push_back(out);

					graphBlur.shader = { rhi::ShaderStage::Compute, shaderDir + "/blur.comp.spv" };
					graphBlur.groupCountX = 64 / 8;
					graphBlur.groupCountY = 64 / 8;
				}

				computeGraph.RegisterPass("GraphBlur", { "Color", "Params" }, { "Blurred" }, Pass::Compute, graphBlur);
				// 借用图声明首态：上一段派发后 Color 已在 ShaderReadOnly（链首因此无需转换）
				computeGraph.RegisterResource("Color", { "GraphBlur" }, "", blurTex.get(), {},
					rhi::ImageLayout::ShaderReadOnly);
				computeGraph.RegisterResource("Blurred", {}, "GraphBlur", graphOut.get());

				// Buffer 节点（Own）：图按 BufferDesc 自建，用法显式给出
				BufferDesc paramDesc;
				paramDesc.size = 256;
				paramDesc.usage = rhi::BufferUsage::Uniform;
				paramDesc.hostVisible = true;
				computeGraph.RegisterResource("Params", { "GraphBlur" }, "", Resource::Buffer, paramDesc);

				computeGraph.ProcessRegister();
				computeGraph.GenerateSubmitSequence();
				computeGraph.RealizeAll();

				uint32_t realizedBuffers = 0;
				for (uint32_t i = 0; computeGraph.GetResource(i) != nullptr; ++i) {
					Resource* res = computeGraph.GetResource(i);
					if (res->get_type() == Resource::Buffer && static_cast<Buffer*>(res)->get_rhi())
						++realizedBuffers;
				}
				cout << "Graph buffer ok: " << realizedBuffers << " realized\n";

				rhi::CommandList graphCmd = device->BeginCommandList();

				// barrier（Color 保持 ShaderReadOnly / Blurred Undefined->General）由 compiler 注入
				computeGraph.RecordGraph(graphCmd); // 按 submitSeq 绑定管线/描述符集并 Dispatch

				// Blurred: General -> ShaderReadOnly（随后由图形管线采样）
				rhi::ImageBarrier outToRead;
				outToRead.image = graphOut.get();
				outToRead.oldLayout = rhi::ImageLayout::General;
				outToRead.newLayout = rhi::ImageLayout::ShaderReadOnly;
				outToRead.srcStage = rhi::PipelineStage::ComputeShader;
				outToRead.srcAccess = rhi::Access::ShaderWrite;
				outToRead.dstStage = rhi::PipelineStage::FragmentShader;
				outToRead.dstAccess = rhi::Access::ShaderRead;
				graphCmd.Barrier({ &outToRead, 1 }, {});

				rhi::SubmitInfo graphSubmit;
				graphSubmit.commandLists = { &graphCmd, 1 };
				device->Submit(graphSubmit);
				device->WaitForFrame();
				cout << "Graph compute ok: 1 compute pass -> borrowed storage image\n";
			}

			// 图侧 uniform buffer 绑定冒烟：ubo.frag 的变量名 "ubo" 与图资源按名对账，
			// 资源类型×usage 推出 UniformBuffer，Realize 时按 offset/range 写入描述符
			{
				RenderGraph uboGraph(device.get());

				RenderPassDesc tintDesc;
				{
					AttachmentBinding att;
					att.resourceName = "Out";
					att.slot = 0;
					att.load = LoadOp::Clear;
					att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
					tintDesc.attachments.push_back(att);

					ShaderBinding ub;
					ub.resourceName = "ubo"; // 须与 ubo.frag 的 uniform 变量名一致
					ub.usage = Usage::UniformRead;
					tintDesc.shaderBindings.push_back(ub);

					tintDesc.pipeline.shaders = {
						{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
						{ rhi::ShaderStage::Fragment, shaderDir + "/ubo.frag.spv" },
					};
					tintDesc.pipeline.cullMode = CullMode::None;
				}

				uboGraph.RegisterPass("Tint", { "ubo" }, { "Out" }, Pass::Render, tintDesc);

				BufferDesc uboNode;
				uboNode.size = 256;
				uboNode.usage = rhi::BufferUsage::Uniform;
				uboNode.hostVisible = true;
				uboGraph.RegisterResource("ubo", { "Tint" }, "", Resource::Buffer, uboNode);

				ImageDesc outNode;
				outNode.width = 64; outNode.height = 64; outNode.format = ImageFormat::RGBA8;
				uboGraph.RegisterResource("Out", {}, "Tint", Resource::Image, outNode);

				uboGraph.ProcessRegister();
				uboGraph.GenerateSubmitSequence();
				uboGraph.RealizeAll();
				cout << "Graph uniform binding ok: reflected UniformBuffer + descriptor write\n";
			}

			// 图侧渲染冒烟：顶点/索引流（VertexStreamBinding / IndexStreamBinding）+ DrawSource。
			// 两个 Pass 各写一张目标图，分别走 Direct indexed 与 Indirect indexed 绘制：
			// 资源身份在注册期按名声明，布局在 PipelineDesc，数量在录制前经 SetDraw 给出
			{
				// 顶点格式 (vec2 pos, vec3 color)，stride 20 —— 布局与 mesh.vert 的 location 对应
				const float vertices[] = {
					-0.6f, -0.6f, 1.0f, 0.0f, 0.0f,
					 0.6f, -0.6f, 0.0f, 1.0f, 0.0f,
					 0.6f,  0.6f, 1.0f, 1.0f, 0.0f,
					-0.6f,  0.6f, 0.2f, 0.4f, 1.0f,
				};
				rhi::BufferCreateInfo vbInfo;
				vbInfo.size = sizeof(vertices);
				vbInfo.usage = rhi::BufferUsage::Vertex;
				vbInfo.hostVisible = true;
				auto vb = device->CreateBuffer(vbInfo);
				if (vb) std::memcpy(vb->Map(), vertices, sizeof(vertices));

				const uint32_t indices[] = { 0, 1, 2, 0, 2, 3 };
				rhi::BufferCreateInfo ibInfo;
				ibInfo.size = sizeof(indices);
				ibInfo.usage = rhi::BufferUsage::Index;
				ibInfo.hostVisible = true;
				auto ib = device->CreateBuffer(ibInfo);
				if (ib) std::memcpy(ib->Map(), indices, sizeof(indices));

				// 间接绘制参数（VkDrawIndexedIndirectCommand：indexCount/instanceCount/firstIndex/vertexOffset/firstInstance）
				VkDrawIndexedIndirectCommand drawCmd{};
				drawCmd.indexCount = 6;
				drawCmd.instanceCount = 1;
				rhi::BufferCreateInfo argsInfo;
				argsInfo.size = sizeof(drawCmd);
				argsInfo.usage = rhi::BufferUsage::Indirect;
				argsInfo.hostVisible = true;
				auto args = device->CreateBuffer(argsInfo);
				if (args) std::memcpy(args->Map(), &drawCmd, sizeof(drawCmd));

				rhi::ImageCreateInfo outInfo;
				outInfo.width = 64; outInfo.height = 64;
				outInfo.format = rhi::ImageFormat::RGBA8;
				outInfo.usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::Sampled;
				auto outA = device->CreateImage(outInfo);
				auto outB = device->CreateImage(outInfo);

				if (vb && ib && args && outA && outB) {
					RenderGraph drawGraph(device.get());

					RenderPassDesc meshPass;
					{
						AttachmentBinding att;
						att.resourceName = "OutA"; // 间接 Pass 复制后仅改目标名
						att.slot = 0;
						att.load = LoadOp::Clear;
						att.clear.value = std::array<float, 4>{ 0.05f, 0.05f, 0.08f, 1.0f };
						meshPass.attachments.push_back(att);

						meshPass.pipeline.shaders = {
							{ rhi::ShaderStage::Vertex,   shaderDir + "/mesh.vert.spv" },
							{ rhi::ShaderStage::Fragment, shaderDir + "/mesh.frag.spv" },
						};
						meshPass.pipeline.topology = PrimitiveTopology::TriangleList;
						// 顶点布局留空：由 RHI 反射 mesh.vert 的 location 0(vec2)/1(vec3) 派生
						// 单流交错布局（stride 20、offset 0/8），与上面的顶点数据排布一致
						meshPass.pipeline.depthTestEnable = false; // 无深度附件
						meshPass.pipeline.cullMode = CullMode::None;

						meshPass.vertexStreams.push_back(VertexStreamBinding{ "Vertices" });
						meshPass.indexStream = IndexStreamBinding{ "Indices", {}, 0, IndexType::UInt32 };
					}
					RenderPassDesc indirectPass = meshPass;
					indirectPass.attachments[0].resourceName = "OutB";

					drawGraph.RegisterPass("DrawDirect",   { "Vertices", "Indices" }, { "OutA" }, Pass::Render, meshPass);
					drawGraph.RegisterPass("DrawIndirect", { "Vertices", "Indices" }, { "OutB" }, Pass::Render, indirectPass);

					// 借用外部创建的顶点/索引/参数缓冲与目标图（用法与生命周期由外部负责）
					drawGraph.RegisterResource("Vertices", { "DrawDirect", "DrawIndirect" }, "", vb.get());
					drawGraph.RegisterResource("Indices",  { "DrawDirect", "DrawIndirect" }, "", ib.get());
					drawGraph.RegisterResource("DrawArgs", { "DrawIndirect" }, "", args.get());
					drawGraph.RegisterResource("OutA", {}, "DrawDirect",   outA.get());
					drawGraph.RegisterResource("OutB", {}, "DrawIndirect", outB.get());

					drawGraph.ProcessRegister();
					drawGraph.GenerateSubmitSequence();
					drawGraph.RealizeAll();

					// 绘制数量是执行参数：录制前按节点名设置（Direct 走数值，Indirect 走参数 buffer）
					for (uint32_t i = 0; drawGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = drawGraph.GetPass(i);
						if (p->get_type() != Pass::Render) continue;
						auto* rp = static_cast<RenderPass*>(p);
						if (p->get_name() == "DrawDirect") {
							DrawSource draw;
							draw.kind = DrawSource::Direct;
							draw.count = 6;  // 索引数
							draw.vertexOffset = 0;
							draw.instanceCount = 1;
							rp->SetDraw(draw);
						}
						else if (p->get_name() == "DrawIndirect") {
							DrawSource draw;
							draw.kind = DrawSource::Indirect;
							draw.indirectBuffer = "DrawArgs";
							draw.indirectOffset = 0;
							draw.count = 1;  // 参数条目数
							rp->SetDraw(draw);
						}
					}

					rhi::CommandList drawCmdList = device->BeginCommandList();

					// 目标图的首态转换（Undefined -> ColorAttachment）由 compiler 在各自 Pass 前注入
					drawGraph.RecordGraph(drawCmdList); // 两个渲染 Pass：BeginRendering/绑定/绘制/EndRendering

					rhi::SubmitInfo drawSubmit;
					drawSubmit.commandLists = { &drawCmdList, 1 };
					device->Submit(drawSubmit);
					device->WaitForFrame();
					cout << "Graph render ok: direct-indexed + indirect-indexed draws\n";
				}
			}

			// 纹理加载冒烟：assets/skyview.jpg -> Host(CPU 解码写 staging) -> Transfer(拷入图像) -> 采样绘制。
			// 三者都是图节点；barrier 由 compiler 按使用链自动推导（含 HOST -> TRANSFER 的内存依赖）
			{
				int texW = 0, texH = 0, texChannels = 0;
				const std::string texturePath = assetDir + "/skyview.jpg";
				stbi_uc* pixels = stbi_load(texturePath.c_str(), &texW, &texH, &texChannels, STBI_rgb_alpha);
				if (pixels == nullptr) {
					cerr << "纹理加载失败: " << texturePath << "\n";
				}
				else {
					const uint64_t stagingBytes = static_cast<uint64_t>(texW) * texH * 4;

					RenderGraph texGraph(device.get());

					// Host 节点：只声明写 staging；回调在录制时执行（Realize 后拿到句柄再装配）
					texGraph.RegisterPass("UploadHost", {}, { "Staging" }, Pass::Host, HostPassDesc{});

					TransferPassDesc uploadDesc;
					uploadDesc.srcBuffer = "Staging";
					uploadDesc.dstImage = "Texture";
					texGraph.RegisterPass("Upload", { "Staging" }, { "Texture" }, Pass::Transfer, uploadDesc);

					RenderPassDesc sampleDesc;
					{
						AttachmentBinding att;
						att.resourceName = "Out";
						att.slot = 0;
						att.load = LoadOp::Clear;
						att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
						sampleDesc.attachments.push_back(att);

						ShaderBinding texBinding;
						texBinding.resourceName = "Texture"; // 与 texture.frag 的变量名一致（按名对账）
						sampleDesc.shaderBindings.push_back(texBinding);

						sampleDesc.pipeline.shaders = {
							{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
							{ rhi::ShaderStage::Fragment, shaderDir + "/texture.frag.spv" },
						};
						sampleDesc.pipeline.cullMode = CullMode::None;
					}
					texGraph.RegisterPass("Sample", { "Texture" }, { "Out" }, Pass::Render, sampleDesc);

					BufferDesc stagingDesc;
					stagingDesc.size = stagingBytes;
					stagingDesc.usage = BufferUsage::TransferSrc; // CopyBufferToImage 的源
					stagingDesc.hostVisible = true;
					texGraph.RegisterResource("Staging", { "Upload" }, "UploadHost", Resource::Buffer, stagingDesc);

					ImageDesc texNodeDesc;
					texNodeDesc.width = static_cast<uint32_t>(texW);
					texNodeDesc.height = static_cast<uint32_t>(texH);
					texNodeDesc.format = ImageFormat::RGBA8;
					texGraph.RegisterResource("Texture", { "Sample" }, "Upload", Resource::Image, texNodeDesc);

					ImageDesc outNodeDesc;
					outNodeDesc.width = 64; outNodeDesc.height = 64;
					outNodeDesc.format = ImageFormat::RGBA8;
					texGraph.RegisterResource("Out", {}, "Sample", Resource::Image, outNodeDesc);

					texGraph.ProcessRegister();
					texGraph.GenerateSubmitSequence();
					texGraph.RealizeAll();

					// Realize 后取 staging 句柄与 Host 节点：装配 CPU 侧上传回调（执行参数）
					rhi::Buffer* staging = nullptr; // Map 非 const：宿主机写得用可写句柄
					HostPass* uploadHost = nullptr;
					for (uint32_t i = 0; texGraph.GetResource(i) != nullptr; ++i) {
						Resource* res = texGraph.GetResource(i);
						if (res->get_type() == Resource::Buffer && res->get_name() == "Staging")
							staging = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					}
					for (uint32_t i = 0; texGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = texGraph.GetPass(i);
						if (p->get_type() == Pass::Host)
							uploadHost = static_cast<HostPass*>(p);
					}

					if (staging != nullptr && uploadHost != nullptr) {
						uploadHost->SetCallback([staging, pixels, stagingBytes]() {
							std::memcpy(staging->Map(), pixels, static_cast<size_t>(stagingBytes));
						});

						rhi::CommandList texCmd = device->BeginCommandList();
						texGraph.RecordGraph(texCmd); // Host 回调 + HOST/TRANSFER barrier + 拷贝 + 采样绘制
						rhi::SubmitInfo texSubmit;
						texSubmit.commandLists = { &texCmd, 1 };
						device->Submit(texSubmit);
						device->WaitForFrame();
						cout << "Texture upload ok: " << texturePath << " " << texW << "x" << texH
						     << " ch=" << texChannels << " -> Host -> Transfer -> Sample\n";
					}
					else {
						cerr << "纹理冒烟失败: staging 或 Host 节点未就绪\n";
					}
					stbi_image_free(pixels);
				}
			}

			rhi::GraphicsPipelineDesc pso;
			pso.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/textured.frag.spv" },
			};
			pso.colorFormats[0] = swapchain2->Format(); // 与 backbuffer 视图格式一致
			pso.colorAttachmentCount = 1;
			pso.depthFormat = rhi::ImageFormat::None;
			pso.cullMode = rhi::CullMode::None;
			// 描述符布局由 RHI 反射得出（无需手填 bindings）

			auto pipeline2 = device->CreatePipeline(pso);
			auto set2 = (pipeline2 && pipeline2->SetLayout(0))
				? device->AllocateDescriptorSet(*pipeline2->SetLayout(0)) : nullptr;
			if (graphOut && pipeline2 && set2) {
				set2->Write(0, *graphOut, rhi::ImageSubresource{ 0, 0 }); // 采样图侧计算产物

				uint32_t presented = 0;
				for (uint32_t attempts = 0; attempts < 8 && presented < 3; ++attempts) {
					rhi::Swapchain::Frame frame = swapchain2->AcquireNextImage();
					if (!frame.acquired) continue; // 交换链重建窗口，跳过本帧重试

					rhi::CommandList cmd = device->BeginCommandList();

					// backbuffer：acquire 后按 Undefined 处理，转 ColorAttachment
					rhi::ImageBarrier toRender;
					toRender.image = frame.backbuffer;
					toRender.oldLayout = rhi::ImageLayout::Undefined;
					toRender.newLayout = rhi::ImageLayout::ColorAttachment;
					toRender.dstStage = rhi::PipelineStage::ColorAttachmentOut;
					toRender.dstAccess = rhi::Access::ColorAttachmentRead | rhi::Access::ColorAttachmentWrite;
					cmd.Barrier({ &toRender, 1 }, {});

					rhi::RenderingAttachment color;
					color.image = frame.backbuffer;
					color.layout = rhi::ImageLayout::ColorAttachment;
					color.load = rhi::LoadOp::Clear;
					color.store = rhi::StoreOp::Store;
					color.clear.value = std::array<float, 4>{ 0.10f, 0.20f, 0.35f, 1.0f };

					rhi::RenderingInfo rendering;
					rendering.colors = &color;
					rendering.colorCount = 1;
					rendering.renderArea.width = swapchain2->Width();
					rendering.renderArea.height = swapchain2->Height();
					cmd.BeginRendering(rendering);
					cmd.BindPipeline(*pipeline2);
					cmd.BindDescriptorSet(0, *set2);
					cmd.SetViewport(rhi::Viewport{ 0.0f, 0.0f,
						static_cast<float>(swapchain2->Width()),
						static_cast<float>(swapchain2->Height()), 0.0f, 1.0f });
					cmd.Draw(3); // 全屏三角形
					cmd.EndRendering();

					// backbuffer：渲染完成转 PresentSrc
					rhi::ImageBarrier toPresent;
					toPresent.image = frame.backbuffer;
					toPresent.oldLayout = rhi::ImageLayout::ColorAttachment;
					toPresent.newLayout = rhi::ImageLayout::PresentSrc;
					toPresent.srcStage = rhi::PipelineStage::ColorAttachmentOut;
					toPresent.srcAccess = rhi::Access::ColorAttachmentWrite;
					toPresent.dstStage = rhi::PipelineStage::AllCommands;
					toPresent.dstAccess = rhi::Access::MemoryRead;
					cmd.Barrier({ &toPresent, 1 }, {});

					// waits 等 acquire 完成；signals 产出渲染完成信号量（Swapchain 按 image 管理）
					rhi::SemaphoreWait wait{ frame.ready, rhi::PipelineStage::ColorAttachmentOut };
					rhi::SemaphoreSignal signal{ frame.presentReady, rhi::PipelineStage::AllCommands };
					rhi::SubmitInfo submit;
					submit.commandLists = { &cmd, 1 };
					submit.waits = { &wait, 1 };
					submit.signals = { &signal, 1 };
					device->Submit(submit);

					swapchain2->Present(); // 内部等当帧 image 的渲染完成信号量
					device->WaitForFrame();
					++presented;
				}
				cout << "Frame loop ok: " << presented << " frames presented\n";
			}
		}
#endif
	}

	if (window) glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}