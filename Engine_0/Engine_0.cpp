#include "Engine_0.h"
#include "RenderGraphAbstracted.h"
#include "RenderGraphNodes.h"
#include "rhi/RHI.h"
#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image/stb_image.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#undef CreateSemaphore

#include <iostream>
#include <cstring>

using namespace std;
using namespace dEngine;

int main()
{
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

		RenderGraph graph(device.get());

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

			gbufferDesc.pipeline.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/solid.frag.spv" },
			};
			gbufferDesc.pipeline.cullMode = CullMode::None;
		}

		RenderPassDesc tonemapDesc;
		{
			AttachmentBinding backAtt;
			backAtt.resourceName = "Backbuffer";
			backAtt.slot = 0;
			backAtt.load = LoadOp::Clear;
			backAtt.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
			tonemapDesc.attachments.push_back(backAtt);

			ShaderBinding sb;
			sb.resourceName = "Blurred";
			tonemapDesc.shaderBindings.push_back(sb);

			tonemapDesc.pipeline.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/textured.frag.spv" },
			};
			tonemapDesc.pipeline.cullMode = CullMode::None;
		}

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

		graph.RegisterPass("GBuffer", {},               {"Color", "Depth"}, Pass::Render, gbufferDesc);
		graph.RegisterPass("Blur",    {"Color"},        {"Blurred"},        Pass::Compute, blurDesc);
		graph.RegisterPass("ToneMap", {"Blurred"},      {"Backbuffer"},     Pass::Render, tonemapDesc);

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

		if (auto swapchain = device->CreateSwapchain()) {
			cout << "Swapchain ok: " << swapchain->Width() << "x" << swapchain->Height() << "\n";

			rhi::Swapchain::Frame frame = swapchain->AcquireNextImage();
			cout << "Acquire #1: acquired=" << frame.acquired
			     << " index=" << frame.imageIndex
			     << " backbuffer=" << (frame.backbuffer ? "borrowed-ok" : "null") << "\n";

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

		{
			const uint32_t gf = device->QueueFamily(rhi::QueueClass::Graphics);
			const uint32_t cf = device->QueueFamily(rhi::QueueClass::AsyncCompute);
			const uint32_t tf = device->QueueFamily(rhi::QueueClass::Transfer);
			cout << "Queues: graphics=f" << gf << " compute=f" << cf
			     << (cf != gf ? "(dedicated)" : "(fallback)")
			     << " transfer=f" << tf << (tf != gf ? "(dedicated)" : "(fallback)")
			     << " timeline=" << (device->TimelineSemaphoresSupported() ? "yes" : "no") << "\n";

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

			if (auto timeline = device->CreateTimelineSemaphore()) {
				rhi::CommandList gfxWaitsHost = device->BeginCommandList();
				rhi::SemaphoreWait hostWait{ timeline.get(), rhi::PipelineStage::AllCommands, 1 };
				rhi::SemaphoreSignal gpuSignal{ timeline.get(), rhi::PipelineStage::AllCommands, 2 };
				rhi::SubmitInfo gfxSubmit;
				gfxSubmit.commandLists = { &gfxWaitsHost, 1 };
				gfxSubmit.waits = { &hostWait, 1 };
				gfxSubmit.signals = { &gpuSignal, 1 };
				device->Submit(gfxSubmit);

				rhi::CommandList computeWaitsGpu = device->BeginCommandList(rhi::QueueClass::AsyncCompute);
				rhi::SemaphoreWait gpuWait{ timeline.get(), rhi::PipelineStage::AllCommands, 2 };
				rhi::SubmitInfo computeSubmit;
				computeSubmit.queue = rhi::QueueClass::AsyncCompute;
				computeSubmit.commandLists = { &computeWaitsGpu, 1 };
				computeSubmit.waits = { &gpuWait, 1 };
				const std::uint64_t computeValue = device->Submit(computeSubmit);

				const bool signaled = timeline->Signal(1);

				rhi::Semaphore* computeTimeline = device->QueueTimeline(rhi::QueueClass::AsyncCompute);
				const bool gpuWaited = computeTimeline != nullptr && computeTimeline->Wait(computeValue);
				device->WaitForFrame();
				cout << "Timeline ok: host signal 1 -> graphics -> compute"
				     << ", gpu wait(" << computeValue << ")=" << (gpuWaited ? "ok" : "fail")
				     << ", final value=" << timeline->Value()
				     << ", host wait(2)=" << (timeline->Wait(2) ? "ok" : "timeout")
				     << (signaled ? "" : " (host signal failed)") << "\n";
			}
		}

#ifdef ENGINE_SHADER_DIR

		{
			if (!device->SupportsTessellation() || !device->SupportsGeometryShaders()) {
				cout << "Tess/Geom pipeline skipped: unsupported by device\n";
			}
			else {
				const float patchVerts[] = {
					-0.8f, -0.8f, 1.0f, 0.0f, 0.0f,
					 0.8f, -0.8f, 0.0f, 1.0f, 0.0f,
					 0.0f,  0.8f, 0.0f, 0.0f, 1.0f,
				};
				rhi::BufferCreateInfo patchInfo;
				patchInfo.size = sizeof(patchVerts);
				patchInfo.usage = rhi::BufferUsage::Vertex;
				patchInfo.hostVisible = true;
				auto patchVB = device->CreateBuffer(patchInfo);
				if (patchVB)
					std::memcpy(patchVB->Map(), patchVerts, sizeof(patchVerts));

				rhi::ImageCreateInfo tessInfo;
				tessInfo.width = 64; tessInfo.height = 64;
				tessInfo.format = rhi::ImageFormat::RGBA8;
				tessInfo.usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::TransferSrc;
				auto tessOut = device->CreateImage(tessInfo);

				bool ok = false;
				if (patchVB && tessOut) {
					RenderGraph tessGraph(device.get());

					RenderPassDesc tessDesc;
					{
						AttachmentBinding att;
						att.resourceName = "TessOut";
						att.slot = 0;
						att.load = LoadOp::Clear;
						att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
						tessDesc.attachments.push_back(att);

						tessDesc.pipeline.shaders = {
							{ rhi::ShaderStage::Vertex,      shaderDir + "/tess.vert.spv" },
							{ rhi::ShaderStage::TessControl, shaderDir + "/tess.tesc.spv" },
							{ rhi::ShaderStage::TessEval,    shaderDir + "/tess.tese.spv" },
							{ rhi::ShaderStage::Geometry,    shaderDir + "/tess.geom.spv" },
							{ rhi::ShaderStage::Fragment,    shaderDir + "/tess.frag.spv" },
						};
						tessDesc.pipeline.topology = PrimitiveTopology::PatchList;
						tessDesc.pipeline.tessPatchControlPoints = 3;
						tessDesc.pipeline.cullMode = CullMode::None;
						tessDesc.pipeline.depthStencil.depthTestEnable = false;
						tessDesc.vertexStreams.push_back(VertexStreamBinding{ "PatchVerts" });
					}

					tessGraph.RegisterPass("TessPass", { "PatchVerts" }, { "TessOut" },
						Pass::Render, tessDesc);
					tessGraph.RegisterResource("PatchVerts", { "TessPass" }, "", patchVB.get());
					tessGraph.RegisterResource("TessOut", {}, "TessPass", tessOut.get());

					tessGraph.ProcessRegister();
					tessGraph.GenerateSubmitSequence();
					tessGraph.RealizeAll();

					for (uint32_t i = 0; tessGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = tessGraph.GetPass(i);
						if (p->get_type() != Pass::Render) continue;
						DrawSource draw;
						draw.kind = DrawSource::Direct;
						draw.count = 3;
						draw.instanceCount = 1;
						static_cast<RenderPass*>(p)->SetDraw(draw);
					}

					tessGraph.SubmitGraph();
					device->WaitForFrame();

					RenderGraph tessRead(device.get());
					TransferPassDesc tessBack;
					tessBack.kind = TransferKind::ImageToBuffer;
					tessBack.sourceName = "Frame";
					tessBack.destName = "Pixels";
					tessRead.RegisterPass("Back", { "Frame" }, { "Pixels" }, Pass::Transfer,
						tessBack, {}, rhi::QueueClass::Transfer);
					tessRead.RegisterResource("Frame", { "Back" }, "", tessOut.get(), {},
						rhi::ImageLayout::ColorAttachment);

					BufferDesc tessPixelsDesc;
					tessPixelsDesc.size = 64 * 64 * 4;
					tessPixelsDesc.usage = BufferUsage::TransferDst;
					tessPixelsDesc.hostVisible = true;
					tessRead.RegisterResource("Pixels", {}, "Back", Resource::Buffer, tessPixelsDesc);
					tessRead.ProcessRegister();
					tessRead.GenerateSubmitSequence();
					tessRead.RealizeAll();

					rhi::Buffer* tessPixels = nullptr;
					for (uint32_t i = 0; tessRead.GetResource(i) != nullptr; ++i) {
						Resource* res = tessRead.GetResource(i);
						if (res->get_type() == Resource::Buffer && res->get_name() == "Pixels")
							tessPixels = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					}
					if (tessPixels != nullptr) {
						tessRead.SubmitGraph();
						device->WaitForFrame();
						tessPixels->Invalidate();
						const unsigned char* px = static_cast<const unsigned char*>(tessPixels->Map());
						const size_t center = (32u * 64u + 32u) * 4u;
						ok = px != nullptr && px[center + 3] == 255
							&& (px[center] != 0 || px[center + 1] != 0 || px[center + 2] != 0);
					}
				}
				cout << "Tess/Geom pipeline " << (ok ? "ok" : "FAILED")
				     << ": tess=" << (device->SupportsTessellation() ? "yes" : "no")
				     << ", geom=" << (device->SupportsGeometryShaders() ? "yes" : "no")
				     << ", patchControlPoints=3\n";
			}
		}

		{
			rhi::ImageCreateInfo pcImageInfo;
			pcImageInfo.width = 64; pcImageInfo.height = 64;
			pcImageInfo.format = rhi::ImageFormat::RGBA8;
			pcImageInfo.usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::TransferSrc;
			auto pcOut = device->CreateImage(pcImageInfo);

			bool ok = false;
			if (pcOut) {
				RenderGraph pcGraph(device.get());
				RenderPassDesc pcDesc;
				{
					AttachmentBinding att;
					att.resourceName = "PcOut";
					att.slot = 0;
					att.load = LoadOp::Clear;
					att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
					pcDesc.attachments.push_back(att);

					pcDesc.pipeline.shaders = {
						{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
						{ rhi::ShaderStage::Fragment, shaderDir + "/push_constant.frag.spv" },
					};
					pcDesc.pipeline.cullMode = CullMode::None;
					pcDesc.pipeline.depthStencil.depthTestEnable = false;
					pcDesc.pipeline.pushConstantSize = sizeof(float) * 4;

					const std::array<float, 4> color{ 1.0f, 0.0f, 0.0f, 1.0f };
					pcDesc.pushConstantData.assign(
						reinterpret_cast<const uint8_t*>(color.data()),
						reinterpret_cast<const uint8_t*>(color.data()) + sizeof(color));
				}

				pcGraph.RegisterPass("PushConstant", {}, { "PcOut" }, Pass::Render, pcDesc);
				pcGraph.RegisterResource("PcOut", {}, "PushConstant", pcOut.get());
				pcGraph.ProcessRegister();
				pcGraph.GenerateSubmitSequence();
				pcGraph.RealizeAll();

				for (uint32_t i = 0; pcGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = pcGraph.GetPass(i);
					if (p->get_type() != Pass::Render) continue;
					DrawSource draw;
					draw.count = 3;
					static_cast<RenderPass*>(p)->SetDraw(draw);
				}

				pcGraph.SubmitGraph();
				device->WaitForFrame();

				RenderGraph pcRead(device.get());
				TransferPassDesc backDesc;
				backDesc.kind = TransferKind::ImageToBuffer;
				backDesc.sourceName = "Frame";
				backDesc.destName = "Pixels";
				pcRead.RegisterPass("Back", { "Frame" }, { "Pixels" }, Pass::Transfer,
					backDesc, {}, rhi::QueueClass::Transfer);
				pcRead.RegisterResource("Frame", { "Back" }, "", pcOut.get(), {},
					rhi::ImageLayout::ColorAttachment);

				BufferDesc pixelsDesc;
				pixelsDesc.size = 64 * 64 * 4;
				pixelsDesc.usage = rhi::BufferUsage::TransferDst;
				pixelsDesc.hostVisible = true;
				pcRead.RegisterResource("Pixels", {}, "Back", Resource::Buffer, pixelsDesc);
				pcRead.ProcessRegister();
				pcRead.GenerateSubmitSequence();
				pcRead.RealizeAll();

				rhi::Buffer* pixels = nullptr;
				for (uint32_t i = 0; pcRead.GetResource(i) != nullptr; ++i) {
					Resource* res = pcRead.GetResource(i);
					if (res->get_type() == Resource::Buffer && res->get_name() == "Pixels")
						pixels = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
				}
				if (pixels != nullptr) {
					pcRead.SubmitGraph();
					device->WaitForFrame();
					pixels->Invalidate();
					const unsigned char* px = static_cast<const unsigned char*>(pixels->Map());
					ok = px != nullptr && px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255;
				}
			}
			cout << "Push constant " << (ok ? "ok" : "FAILED")
			     << ": push-constant fragment color\n";
		}

		rhi::ImageCreateInfo texInfo;
		texInfo.width = 128; texInfo.height = 128;
		texInfo.format = rhi::ImageFormat::RGBA8;
		texInfo.usage  = rhi::ImageUsage::Sampled;
		if (auto tex = device->CreateImage(texInfo)) {
			rhi::ImageViewDesc viewDesc;
			viewDesc.range = ImageSubresourceRange{ {0, 0}, 0, 0 };
			viewDesc.format = rhi::ImageFormat::BGRA8;
			const rhi::ImageView& fullView = tex->CreateView(viewDesc);
			cout << "Ranged view ok: handle=" << (fullView.NativeHandle() != nullptr) << "\n";

			rhi::GraphicsPipelineDesc psoA;
			psoA.shaders = {
				{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
				{ rhi::ShaderStage::Fragment, shaderDir + "/textured.frag.spv" },
			};
			psoA.colorFormats[0] = rhi::ImageFormat::RGBA8;
			psoA.colorAttachmentCount = 1;
			psoA.depthFormat = rhi::ImageFormat::None;
			psoA.cullMode = rhi::CullMode::None;

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

		if (auto swapchain2 = device->CreateSwapchain()) {
			rhi::ImageCreateInfo colorInfo;
			colorInfo.width = 64; colorInfo.height = 64;
			colorInfo.format = rhi::ImageFormat::RGBA16F;
			colorInfo.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
			auto colorTex = device->CreateImage(colorInfo);

			rhi::ImageCreateInfo blurInfo;
			blurInfo.width = 64; blurInfo.height = 64;
			blurInfo.format = rhi::ImageFormat::RGBA16F;
			blurInfo.usage = rhi::ImageUsage::Storage | rhi::ImageUsage::Sampled;
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

				rhi::CommandList computeCmd = device->BeginCommandList();

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
				computeCmd.Dispatch(64 / 8, 64 / 8, 1);

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

			rhi::ImageCreateInfo graphOutInfo;
			graphOutInfo.width = 64; graphOutInfo.height = 64;
			graphOutInfo.format = rhi::ImageFormat::RGBA16F;
			graphOutInfo.usage = rhi::ImageUsage::Storage | rhi::ImageUsage::Sampled;
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
				computeGraph.RegisterResource("Color", { "GraphBlur" }, "", blurTex.get(), {},
					rhi::ImageLayout::ShaderReadOnly);
				computeGraph.RegisterResource("Blurred", {}, "GraphBlur", graphOut.get());

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

				computeGraph.RecordGraph(graphCmd);

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
					ub.resourceName = "ubo";
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

			{
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
				outInfo.usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::Sampled
					| rhi::ImageUsage::TransferSrc;
				auto outA = device->CreateImage(outInfo);
				auto outB = device->CreateImage(outInfo);

				if (vb && ib && args && outA && outB) {
					RenderGraph drawGraph(device.get());

					RenderPassDesc meshPass;
					{
						AttachmentBinding att;
						att.resourceName = "OutA";
						att.slot = 0;
						att.load = LoadOp::Clear;
						att.clear.value = std::array<float, 4>{ 0.05f, 0.05f, 0.08f, 1.0f };
						meshPass.attachments.push_back(att);

						meshPass.pipeline.shaders = {
							{ rhi::ShaderStage::Vertex,   shaderDir + "/mesh.vert.spv" },
							{ rhi::ShaderStage::Fragment, shaderDir + "/mesh.frag.spv" },
						};
						meshPass.pipeline.topology = PrimitiveTopology::TriangleList;
						meshPass.pipeline.depthStencil.depthTestEnable = false;
						meshPass.pipeline.cullMode = CullMode::None;

						meshPass.vertexStreams.push_back(VertexStreamBinding{ "Vertices" });
						meshPass.indexStream = IndexStreamBinding{ "Indices", {}, 0, IndexType::UInt32 };
					}
					RenderPassDesc indirectPass = meshPass;
					indirectPass.attachments[0].resourceName = "OutB";

					drawGraph.RegisterPass("DrawDirect",   { "Vertices", "Indices" }, { "OutA" }, Pass::Render, meshPass);
					drawGraph.RegisterPass("DrawIndirect", { "Vertices", "Indices" }, { "OutB" }, Pass::Render, indirectPass);

					drawGraph.RegisterResource("Vertices", { "DrawDirect", "DrawIndirect" }, "", vb.get());
					drawGraph.RegisterResource("Indices",  { "DrawDirect", "DrawIndirect" }, "", ib.get());
					drawGraph.RegisterResource("DrawArgs", { "DrawIndirect" }, "", args.get());
					drawGraph.RegisterResource("OutA", {}, "DrawDirect",   outA.get());
					drawGraph.RegisterResource("OutB", {}, "DrawIndirect", outB.get());

					drawGraph.ProcessRegister();
					drawGraph.GenerateSubmitSequence();
					drawGraph.RealizeAll();

					for (uint32_t i = 0; drawGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = drawGraph.GetPass(i);
						if (p->get_type() != Pass::Render) continue;
						auto* rp = static_cast<RenderPass*>(p);
						if (p->get_name() == "DrawDirect") {
							DrawSource draw;
							draw.kind = DrawSource::Direct;
							draw.count = 6;
							draw.vertexOffset = 0;
							draw.instanceCount = 1;
							rp->SetDraw(draw);
						}
						else if (p->get_name() == "DrawIndirect") {
							DrawSource draw;
							draw.kind = DrawSource::Indirect;
							draw.indirectBuffer = "DrawArgs";
							draw.indirectOffset = 0;
							draw.count = 1;
							rp->SetDraw(draw);
						}
					}

					rhi::CommandList drawCmdList = device->BeginCommandList();

					drawGraph.RecordGraph(drawCmdList);

					rhi::SubmitInfo drawSubmit;
					drawSubmit.commandLists = { &drawCmdList, 1 };
					device->Submit(drawSubmit);
					device->WaitForFrame();
					cout << "Graph render ok: direct-indexed + indirect-indexed draws\n";

					{
						RenderGraph readbackGraph(device.get());

						TransferPassDesc backDesc;
						backDesc.kind = TransferKind::ImageToBuffer;
						backDesc.sourceName = "Frame";
						backDesc.destName = "Pixels";
						readbackGraph.RegisterPass("Back", { "Frame" }, { "Pixels" }, Pass::Transfer,
							backDesc, {}, rhi::QueueClass::Transfer);
						readbackGraph.RegisterPass("Consume", { "Pixels" }, {}, Pass::Host, HostPassDesc{});

						readbackGraph.RegisterResource("Frame", { "Back" }, "", outA.get(), {},
							rhi::ImageLayout::ColorAttachment);

						BufferDesc pixelsDesc;
						pixelsDesc.size = 64 * 64 * 4;
						pixelsDesc.usage = BufferUsage::TransferDst;
						pixelsDesc.hostVisible = true;
						readbackGraph.RegisterResource("Pixels", { "Consume" }, "Back", Resource::Buffer, pixelsDesc);

						readbackGraph.ProcessRegister();
						readbackGraph.GenerateSubmitSequence();
						readbackGraph.RealizeAll();

						HostPass* consume = nullptr;
						for (uint32_t i = 0; readbackGraph.GetPass(i) != nullptr; ++i) {
							Pass* p = readbackGraph.GetPass(i);
							if (p->get_type() == Pass::Host) consume = static_cast<HostPass*>(p);
						}

						unsigned char first[4] = {};
						if (consume != nullptr) {
							consume->SetCallback([&first](HostContext& ctx) {
								ctx.WaitForGpu();
								const rhi::Buffer* source = ctx.ReadBuffer("Pixels");
								if (source != nullptr && source->Map() != nullptr)
									std::memcpy(first, source->Map(), sizeof(first));
								ctx.SignalGpu();
							});
							readbackGraph.SubmitGraph();
							readbackGraph.WaitHostPasses();
							device->WaitForFrame();

							auto closeTo = [](unsigned char v, int expect) {
								const int d = int(v) - expect;
								return d >= -2 && d <= 2;
							};
							const bool ok = closeTo(first[0], 13) && closeTo(first[1], 13) && closeTo(first[2], 20)
								&& first[3] == 255;
							cout << "GPU->Host readback " << (ok ? "ok" : "FAILED") << ": rgba8="
							     << int(first[0]) << "," << int(first[1]) << ","
							     << int(first[2]) << "," << int(first[3]) << "\n";
						}
					}
				}
			}

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

					texGraph.RegisterPass("UploadHost", {}, { "Staging" }, Pass::Host, HostPassDesc{});

					TransferPassDesc uploadDesc;
					uploadDesc.kind = TransferKind::BufferToImage;
					uploadDesc.sourceName = "Staging";
					uploadDesc.destName = "Texture";
					texGraph.RegisterPass("Upload", { "Staging" }, { "Texture" }, Pass::Transfer, uploadDesc, {}, rhi::QueueClass::Transfer);

					RenderPassDesc sampleDesc;
					{
						AttachmentBinding att;
						att.resourceName = "Out";
						att.slot = 0;
						att.load = LoadOp::Clear;
						att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
						sampleDesc.attachments.push_back(att);

						ShaderBinding texBinding;
						texBinding.resourceName = "Texture";
						rhi::SamplerDesc sampler;
						sampler.magFilter = rhi::Filter::Nearest;
						sampler.minFilter = rhi::Filter::Nearest;
						sampler.mipmapMode = rhi::SamplerMipmapMode::Nearest;
						sampler.addressU = rhi::SamplerAddressMode::ClampToEdge;
						sampler.addressV = rhi::SamplerAddressMode::ClampToEdge;
						texBinding.sampler = sampler;
						sampleDesc.shaderBindings.push_back(texBinding);

						sampleDesc.pipeline.shaders = {
							{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
							{ rhi::ShaderStage::Fragment, shaderDir + "/texture.frag.spv" },
						};
						sampleDesc.pipeline.cullMode = CullMode::None;
					}
					texGraph.RegisterPass("Sample", { "Texture" }, { "Out" }, Pass::Render, sampleDesc);

					texGraph.RegisterPass("VerifyHost", { "Staging" }, {}, Pass::Host, HostPassDesc{});

					BufferDesc stagingDesc;
					stagingDesc.size = stagingBytes;
					stagingDesc.usage = BufferUsage::TransferSrc;
					stagingDesc.hostVisible = true;
					texGraph.RegisterResource("Staging", { "Upload", "VerifyHost" }, "UploadHost", Resource::Buffer, stagingDesc);

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

					rhi::Buffer* staging = nullptr;
					HostPass* uploadHost = nullptr;
					HostPass* verifyHost = nullptr;
					for (uint32_t i = 0; texGraph.GetResource(i) != nullptr; ++i) {
						Resource* res = texGraph.GetResource(i);
						if (res->get_type() == Resource::Buffer && res->get_name() == "Staging")
							staging = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					}
					for (uint32_t i = 0; texGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = texGraph.GetPass(i);
						if (p->get_type() != Pass::Host)
							continue;
						if (p->get_name() == "UploadHost") uploadHost = static_cast<HostPass*>(p);
						else if (p->get_name() == "VerifyHost") verifyHost = static_cast<HostPass*>(p);
					}

					if (staging != nullptr && uploadHost != nullptr && verifyHost != nullptr) {
						uploadHost->SetCallback([pixels, stagingBytes](HostContext& ctx) {
							ctx.WaitForGpu();
							rhi::Buffer* target = ctx.WriteBuffer("Staging");
							if (target != nullptr)
								std::memcpy(target->Map(), pixels, static_cast<size_t>(stagingBytes));
							ctx.SignalGpu();
						});
						verifyHost->SetCallback([](HostContext& ctx) {
							ctx.WaitForGpu();
							const rhi::Buffer* source = ctx.ReadBuffer("Staging");
							if (source != nullptr && source->Map() != nullptr) {
								const unsigned char* bytes = static_cast<const unsigned char*>(source->Map());
								(void)bytes[0];
							}
							ctx.SignalGpu();
						});

						texGraph.SubmitGraph();
						texGraph.WaitHostPasses();
						device->WaitForFrame();

						const RenderGraph::SubmitPlan& plan = texGraph.GetSubmitPlan();
						cout << "Texture upload ok: " << texturePath << " " << texW << "x" << texH
						     << " ch=" << texChannels << " -> Host -> Transfer -> Sample (host verify)"
						     << " (" << plan.batches.size() << " batches, "
						     << plan.syncPoints.size() << " sync points)\n";
					}
					else {
						cerr << "纹理冒烟失败: staging 或 Host 节点未就绪\n";
					}
					stbi_image_free(pixels);
				}
			}

			{
				const uint64_t mip0Bytes = 64ull * 64ull * 4ull;
				const uint64_t mip1Bytes = 32ull * 32ull * 4ull;

				RenderGraph regionGraph(device.get());

				BufferDesc srcDesc;
				srcDesc.size = mip0Bytes + mip1Bytes;
				srcDesc.usage = BufferUsage::TransferSrc;
				srcDesc.hostVisible = true;
				BufferDesc dstDesc;
				dstDesc.size = mip0Bytes + mip1Bytes;
				dstDesc.usage = BufferUsage::TransferDst;
				dstDesc.hostVisible = true;

				ImageDesc texDesc;
				texDesc.width = 64; texDesc.height = 64;
				texDesc.mipLevels = 2;
				texDesc.format = ImageFormat::RGBA8;

				ImageDesc clearTexDesc;
				clearTexDesc.width = 8; clearTexDesc.height = 8;
				clearTexDesc.format = ImageFormat::RGBA8;

				BufferDesc clearBufDesc;
				clearBufDesc.size = 8 * 8 * 4;
				clearBufDesc.usage = BufferUsage::TransferDst;
				clearBufDesc.hostVisible = true;

				regionGraph.RegisterResource("Src", { "Upload" }, "", Resource::Buffer, srcDesc);
				regionGraph.RegisterResource("Dst", {}, "Read", Resource::Buffer, dstDesc);
				regionGraph.RegisterResource("Tex", { "Read" }, "Upload", Resource::Image, texDesc);
				regionGraph.RegisterResource("ClearTex", { "ClearRead" }, "Clear", Resource::Image, clearTexDesc);
				regionGraph.RegisterResource("ClearBuf", {}, "ClearRead", Resource::Buffer, clearBufDesc);

				rhi::BufferImageCopyRegion mip0Region;
				mip0Region.subresource = rhi::ImageSubresourceRange{ { 0, 0 }, 1, 1 };
				mip0Region.imageExtentWidth = 64;
				mip0Region.imageExtentHeight = 64;
				mip0Region.imageExtentDepth = 1;
				rhi::BufferImageCopyRegion mip1Region;
				mip1Region.bufferOffset = mip0Bytes;
				mip1Region.subresource = rhi::ImageSubresourceRange{ { 1, 0 }, 1, 1 };
				mip1Region.imageExtentWidth = 32;
				mip1Region.imageExtentHeight = 32;
				mip1Region.imageExtentDepth = 1;

				TransferPassDesc uploadDesc;
				uploadDesc.kind = TransferKind::BufferToImage;
				uploadDesc.sourceName = "Src";
				uploadDesc.destName = "Tex";
				uploadDesc.bufferImageRegions = { mip0Region, mip1Region };

				TransferPassDesc readDesc;
				readDesc.kind = TransferKind::ImageToBuffer;
				readDesc.sourceName = "Tex";
				readDesc.destName = "Dst";
				readDesc.bufferImageRegions = { mip0Region, mip1Region };

				rhi::BufferImageCopyRegion clearRegion;
				clearRegion.subresource = rhi::ImageSubresourceRange{ { 0, 0 }, 1, 1 };
				clearRegion.imageExtentWidth = 8;
				clearRegion.imageExtentHeight = 8;
				clearRegion.imageExtentDepth = 1;

				TransferPassDesc clearDesc;
				clearDesc.kind = TransferKind::ClearImage;
				clearDesc.destName = "ClearTex";
				clearDesc.clearRange = rhi::ImageSubresourceRange{ {}, 0, 0 };
				clearDesc.clearValue.value = std::array<float, 4>{ 0.0f, 0.0f, 1.0f, 1.0f };

				TransferPassDesc clearReadDesc;
				clearReadDesc.kind = TransferKind::ImageToBuffer;
				clearReadDesc.sourceName = "ClearTex";
				clearReadDesc.destName = "ClearBuf";
				clearReadDesc.bufferImageRegions = { clearRegion };

				regionGraph.RegisterPass("Upload", { "Src" }, { "Tex" }, Pass::Transfer, uploadDesc);
				regionGraph.RegisterPass("Read", { "Tex" }, { "Dst" }, Pass::Transfer, readDesc);
				regionGraph.RegisterPass("Clear", {}, { "ClearTex" }, Pass::Transfer, clearDesc);
				regionGraph.RegisterPass("ClearRead", { "ClearTex" }, { "ClearBuf" }, Pass::Transfer, clearReadDesc);

				regionGraph.ProcessRegister();
				regionGraph.GenerateSubmitSequence();
				regionGraph.RealizeAll();

				rhi::Buffer* srcBuf = nullptr;
				rhi::Buffer* dstBuf = nullptr;
				rhi::Buffer* clearBuf = nullptr;
				for (uint32_t i = 0; regionGraph.GetResource(i) != nullptr; ++i) {
					Resource* res = regionGraph.GetResource(i);
					if (res->get_type() != Resource::Buffer)
						continue;
					if (res->get_name() == "Src")
						srcBuf = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					else if (res->get_name() == "Dst")
						dstBuf = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					else if (res->get_name() == "ClearBuf")
						clearBuf = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
				}

				bool ok = false;
				if (srcBuf != nullptr && dstBuf != nullptr && clearBuf != nullptr) {
					unsigned char* src = static_cast<unsigned char*>(srcBuf->Map());
					std::memset(src, 0, static_cast<size_t>(mip0Bytes + mip1Bytes));
					for (uint64_t i = 0; i < mip0Bytes; i += 4) {
						src[i] = 255;
						src[i + 3] = 255;
					}
					for (uint64_t i = mip0Bytes; i < mip0Bytes + mip1Bytes; i += 4) {
						src[i + 1] = 255;
						src[i + 3] = 255;
					}
					srcBuf->Flush();

					regionGraph.SubmitGraph();
					device->WaitForFrame();

					dstBuf->Invalidate();
					clearBuf->Invalidate();
					const unsigned char* dst = static_cast<const unsigned char*>(dstBuf->Map());
					const unsigned char* clr = static_cast<const unsigned char*>(clearBuf->Map());
					ok = dst[0] == 255 && dst[1] == 0 && dst[2] == 0 && dst[3] == 255
						&& dst[mip0Bytes] == 0 && dst[mip0Bytes + 1] == 255
						&& dst[mip0Bytes + 2] == 0 && dst[mip0Bytes + 3] == 255
						&& clr[2] == 255 && clr[3] == 255;
				}
				cout << "Transfer regions + multi-mip partitions " << (ok ? "ok" : "FAILED")
				     << ": mip0=red mip1=green clear=blue\n";
			}

			{
				RenderGraph mipClearGraph(device.get());

				ImageDesc mipDesc;
				mipDesc.width = 64; mipDesc.height = 64;
				mipDesc.mipLevels = 2;
				mipDesc.format = ImageFormat::RGBA8;
				BufferDesc outDesc;
				outDesc.size = 32 * 32 * 4;
				outDesc.usage = BufferUsage::TransferDst;
				outDesc.hostVisible = true;

				mipClearGraph.RegisterResource("MipClear", { "ReadMip1" }, "ClearMip1",
					Resource::Image, mipDesc);
				mipClearGraph.RegisterResource("MipOut", {}, "ReadMip1", Resource::Buffer, outDesc);

				rhi::BufferImageCopyRegion readRegion;
				readRegion.subresource = rhi::ImageSubresourceRange{ { 1, 0 }, 1, 1 };
				readRegion.imageExtentWidth = 32;
				readRegion.imageExtentHeight = 32;
				readRegion.imageExtentDepth = 1;

				TransferPassDesc clearDesc;
				clearDesc.kind = TransferKind::ClearImage;
				clearDesc.destName = "MipClear";
				clearDesc.clearRange = rhi::ImageSubresourceRange{ { 1, 0 }, 1, 1 };
				clearDesc.clearValue.value = std::array<float, 4>{ 0.0f, 0.0f, 1.0f, 1.0f };

				TransferPassDesc readDesc;
				readDesc.kind = TransferKind::ImageToBuffer;
				readDesc.sourceName = "MipClear";
				readDesc.destName = "MipOut";
				readDesc.bufferImageRegions = { readRegion };

				mipClearGraph.RegisterPass("ClearMip1", {}, { "MipClear" }, Pass::Transfer, clearDesc);
				mipClearGraph.RegisterPass("ReadMip1", { "MipClear" }, { "MipOut" },
					Pass::Transfer, readDesc);
				mipClearGraph.ProcessRegister();
				mipClearGraph.GenerateSubmitSequence();
				mipClearGraph.RealizeAll();

				rhi::Buffer* outBuf = nullptr;
				for (uint32_t i = 0; mipClearGraph.GetResource(i) != nullptr; ++i) {
					Resource* res = mipClearGraph.GetResource(i);
					if (res->get_type() == Resource::Buffer && res->get_name() == "MipOut")
						outBuf = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
				}

				bool ok = false;
				if (outBuf != nullptr) {
					mipClearGraph.SubmitGraph();
					device->WaitForFrame();
					outBuf->Invalidate();
					const unsigned char* data = static_cast<const unsigned char*>(outBuf->Map());
					ok = data != nullptr && data[0] == 0 && data[1] == 0
						&& data[2] == 255 && data[3] == 255;
				}
				cout << "Per-mip clear " << (ok ? "ok" : "FAILED") << ": mip1=blue\n";
			}

			{
				RenderGraph hostMipGraph(device.get());

				ImageDesc hostDesc;
				hostDesc.width = 64; hostDesc.height = 64;
				hostDesc.mipLevels = 2;
				hostDesc.format = ImageFormat::RGBA8;
				BufferDesc srcDesc;
				srcDesc.size = 32 * 32 * 4;
				srcDesc.usage = BufferUsage::TransferSrc;
				srcDesc.hostVisible = true;

				hostMipGraph.RegisterResource("SrcMip", { "UploadMip" }, "",
					Resource::Buffer, srcDesc);
				hostMipGraph.RegisterResource("HostTex", { "ConsumeHost" }, "UploadMip",
					Resource::Image, hostDesc, rhi::ImageSubresource{ 1, 0 });

				rhi::BufferImageCopyRegion region;
				region.subresource = rhi::ImageSubresourceRange{ { 1, 0 }, 1, 1 };
				region.imageExtentWidth = 32;
				region.imageExtentHeight = 32;
				region.imageExtentDepth = 1;

				TransferPassDesc uploadDesc;
				uploadDesc.kind = TransferKind::BufferToImage;
				uploadDesc.sourceName = "SrcMip";
				uploadDesc.destName = "HostTex";
				uploadDesc.bufferImageRegions = { region };

				hostMipGraph.RegisterPass("UploadMip", { "SrcMip" }, { "HostTex" },
					Pass::Transfer, uploadDesc);
				hostMipGraph.RegisterPass("ConsumeHost", { "HostTex" }, {},
					Pass::Host, HostPassDesc{});
				hostMipGraph.ProcessRegister();
				hostMipGraph.GenerateSubmitSequence();
				hostMipGraph.RealizeAll();

				rhi::Buffer* srcBuf = nullptr;
				for (uint32_t i = 0; hostMipGraph.GetResource(i) != nullptr; ++i) {
					Resource* res = hostMipGraph.GetResource(i);
					if (res->get_type() == Resource::Buffer && res->get_name() == "SrcMip")
						srcBuf = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
				}
				HostPass* consume = nullptr;
				for (uint32_t i = 0; hostMipGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = hostMipGraph.GetPass(i);
					if (p->get_type() == Pass::Host)
						consume = static_cast<HostPass*>(p);
				}

				bool ok = false;
				if (srcBuf != nullptr && consume != nullptr) {
					unsigned char* pixels = static_cast<unsigned char*>(srcBuf->Map());
					std::memset(pixels, 0, 32 * 32 * 4);
					for (uint64_t i = 0; i < 32 * 32 * 4; i += 4) {
						pixels[i + 1] = 255;
						pixels[i + 3] = 255;
					}
					srcBuf->Flush();

					consume->SetCallback([&ok](HostContext& ctx) {
						ctx.WaitForGpu();
						const unsigned char* data = static_cast<const unsigned char*>(
							ctx.ReadImage("HostTex"));
						if (data != nullptr)
							ok = data[0] == 0 && data[1] == 255 && data[2] == 0 && data[3] == 255;
						ctx.SignalGpu();
					});
					hostMipGraph.SubmitGraph();
					hostMipGraph.WaitHostPasses();
					device->WaitForFrame();
				}
				cout << "Host staging mip1 " << (ok ? "ok" : "FAILED") << ": mip1=green\n";
			}

			{
				RenderGraph layerGraph(device.get());

				ImageDesc layerDesc;
				layerDesc.width = 16; layerDesc.height = 16;
				layerDesc.arrayLayers = 2;
				layerDesc.format = ImageFormat::RGBA8;

				BufferDesc layerSrcDesc;
				layerSrcDesc.size = 16 * 16 * 4;
				layerSrcDesc.usage = BufferUsage::TransferSrc;
				layerSrcDesc.hostVisible = true;
				BufferDesc layerDstDesc;
				layerDstDesc.size = 16 * 16 * 4;
				layerDstDesc.usage = BufferUsage::TransferDst;
				layerDstDesc.hostVisible = true;

				layerGraph.RegisterResource("LayerSrc", { "UploadLayer" }, "",
					Resource::Buffer, layerSrcDesc);
				layerGraph.RegisterResource("LayerDst", {}, "ReadLayer",
					Resource::Buffer, layerDstDesc);
				layerGraph.RegisterResource("LayerTex", { "ReadLayer" }, "UploadLayer",
					Resource::Image, layerDesc);

				rhi::BufferImageCopyRegion layerRegion;
				layerRegion.subresource = rhi::ImageSubresourceRange{ { 0, 1 }, 1, 1 };
				layerRegion.imageExtentWidth = 16;
				layerRegion.imageExtentHeight = 16;
				layerRegion.imageExtentDepth = 1;

				TransferPassDesc layerUpDesc;
				layerUpDesc.kind = TransferKind::BufferToImage;
				layerUpDesc.sourceName = "LayerSrc";
				layerUpDesc.destName = "LayerTex";
				layerUpDesc.bufferImageRegions = { layerRegion };

				TransferPassDesc layerReadDesc;
				layerReadDesc.kind = TransferKind::ImageToBuffer;
				layerReadDesc.sourceName = "LayerTex";
				layerReadDesc.destName = "LayerDst";
				layerReadDesc.bufferImageRegions = { layerRegion };

				layerGraph.RegisterPass("UploadLayer", { "LayerSrc" }, { "LayerTex" },
					Pass::Transfer, layerUpDesc);
				layerGraph.RegisterPass("ReadLayer", { "LayerTex" }, { "LayerDst" },
					Pass::Transfer, layerReadDesc);
				layerGraph.ProcessRegister();
				layerGraph.GenerateSubmitSequence();
				layerGraph.RealizeAll();

				rhi::Buffer* layerSrc = nullptr;
				rhi::Buffer* layerDst = nullptr;
				for (uint32_t i = 0; layerGraph.GetResource(i) != nullptr; ++i) {
					Resource* res = layerGraph.GetResource(i);
					if (res->get_type() != Resource::Buffer)
						continue;
					if (res->get_name() == "LayerSrc")
						layerSrc = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					else if (res->get_name() == "LayerDst")
						layerDst = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
				}

				bool ok = false;
				if (layerSrc != nullptr && layerDst != nullptr) {
					unsigned char* pixels = static_cast<unsigned char*>(layerSrc->Map());
					std::memset(pixels, 0, 16 * 16 * 4);
					for (uint64_t i = 0; i < 16 * 16 * 4; i += 4) {
						pixels[i + 1] = 255;
						pixels[i + 3] = 255;
					}
					layerSrc->Flush();

					layerGraph.SubmitGraph();
					device->WaitForFrame();
					layerDst->Invalidate();
					const unsigned char* data = static_cast<const unsigned char*>(layerDst->Map());
					ok = data != nullptr && data[0] == 0 && data[1] == 255
						&& data[2] == 0 && data[3] == 255;
				}
				cout << "Layer region copy " << (ok ? "ok" : "FAILED") << ": layer1=green\n";
			}

			{
				if (!device->SupportsBindlessTextures()) {
					cout << "Static texture heap: unsupported by device, skipped\n";
				}
				else {
					rhi::ImageCreateInfo texInfo;
					texInfo.width = 1; texInfo.height = 1;
					texInfo.format = rhi::ImageFormat::RGBA8;
					texInfo.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
					auto texA = device->CreateImage(texInfo);
					auto texB = device->CreateImage(texInfo);

					rhi::ImageCreateInfo outInfo;
					outInfo.width = 64; outInfo.height = 64;
					outInfo.format = rhi::ImageFormat::RGBA8;
					outInfo.usage = rhi::ImageUsage::ColorAttachment
						| rhi::ImageUsage::TransferSrc;
					auto heapOut = device->CreateImage(outInfo);

					if (texA && texB && heapOut) {
						rhi::CommandList initCmd = device->BeginCommandList();
						rhi::ImageBarrier toDst[2];
						for (int i = 0; i < 2; ++i) {
							toDst[i].image = i == 0 ? texA.get() : texB.get();
							toDst[i].oldLayout = rhi::ImageLayout::Undefined;
							toDst[i].newLayout = rhi::ImageLayout::TransferDst;
							toDst[i].dstStage = rhi::PipelineStage::Transfer;
							toDst[i].dstAccess = rhi::Access::TransferWrite;
						}
						rhi::ImageBarrier toRead[2];
						for (int i = 0; i < 2; ++i) {
							toRead[i].image = i == 0 ? texA.get() : texB.get();
							toRead[i].oldLayout = rhi::ImageLayout::TransferDst;
							toRead[i].newLayout = rhi::ImageLayout::ShaderReadOnly;
							toRead[i].srcStage = rhi::PipelineStage::Transfer;
							toRead[i].srcAccess = rhi::Access::TransferWrite;
							toRead[i].dstStage = rhi::PipelineStage::FragmentShader;
							toRead[i].dstAccess = rhi::Access::ShaderRead;
						}
						initCmd.Barrier({ &toDst[0], 2 }, {});
						const float red[4]  = { 1.0f, 0.0f, 0.0f, 1.0f };
						const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
						initCmd.ClearColor(*texA, rhi::ImageSubresource{ 0, 0 }, red,
							rhi::ImageLayout::TransferDst);
						initCmd.ClearColor(*texB, rhi::ImageSubresource{ 0, 0 }, blue,
							rhi::ImageLayout::TransferDst);
						initCmd.Barrier({ &toRead[0], 2 }, {});
						rhi::SubmitInfo initSubmit;
						initSubmit.commandLists = { &initCmd, 1 };
						device->Submit(initSubmit);
						device->WaitForFrame();

						RenderGraph heapGraph(device.get());

						const uint32_t slotA = heapGraph.RegisterStaticTexture("StaticA", *texA);
						const uint32_t slotB = heapGraph.RegisterStaticTexture("StaticB", *texB);

						RenderPassDesc heapDesc;
						{
							AttachmentBinding att;
							att.resourceName = "Out";
							att.slot = 0;
							att.load = LoadOp::Clear;
							att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
							heapDesc.attachments.push_back(att);

							heapDesc.pipeline.shaders = {
								{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
								{ rhi::ShaderStage::Fragment, shaderDir + "/static_texture.frag.spv" },
							};
							heapDesc.pipeline.cullMode = CullMode::None;
							heapDesc.pipeline.depthStencil.depthTestEnable = false;
							heapDesc.bindStaticHeap = true;
						}
						heapGraph.RegisterPass("DrawHeap", {}, { "Out" }, Pass::Render, heapDesc);
						heapGraph.RegisterResource("Out", {}, "DrawHeap", heapOut.get());

						heapGraph.ProcessRegister();
						heapGraph.GenerateSubmitSequence();
						heapGraph.RealizeAll();

						for (uint32_t i = 0; heapGraph.GetPass(i) != nullptr; ++i) {
							Pass* p = heapGraph.GetPass(i);
							if (p->get_type() == Pass::Render) {
								DrawSource draw;
								draw.count = 3;
								static_cast<RenderPass*>(p)->SetDraw(draw);
							}
						}

						rhi::CommandList heapCmd = device->BeginCommandList();
						heapGraph.RecordGraph(heapCmd);
						rhi::SubmitInfo heapSubmit;
						heapSubmit.commandLists = { &heapCmd, 1 };
						device->Submit(heapSubmit);
						device->WaitForFrame();
						cout << "Static texture heap bind ok: slots=" << slotA << "," << slotB
						     << " bindless=" << (device->SupportsBindlessTextures() ? "yes" : "no") << "\n";

						{
							RenderGraph readbackGraph(device.get());
							TransferPassDesc backDesc;
							backDesc.kind = TransferKind::ImageToBuffer;
							backDesc.sourceName = "Frame";
							backDesc.destName = "Pixels";
							readbackGraph.RegisterPass("Back", { "Frame" }, { "Pixels" }, Pass::Transfer,
								backDesc, {}, rhi::QueueClass::Transfer);
							readbackGraph.RegisterPass("Consume", { "Pixels" }, {}, Pass::Host, HostPassDesc{});
							readbackGraph.RegisterResource("Frame", { "Back" }, "", heapOut.get(), {},
								rhi::ImageLayout::ColorAttachment);

							BufferDesc pixelsDesc;
							pixelsDesc.size = 64 * 64 * 4;
							pixelsDesc.usage = BufferUsage::TransferDst;
							pixelsDesc.hostVisible = true;
							readbackGraph.RegisterResource("Pixels", { "Consume" }, "Back", Resource::Buffer, pixelsDesc);

							readbackGraph.ProcessRegister();
							readbackGraph.GenerateSubmitSequence();
							readbackGraph.RealizeAll();

							HostPass* consume = nullptr;
							for (uint32_t i = 0; readbackGraph.GetPass(i) != nullptr; ++i) {
								Pass* p = readbackGraph.GetPass(i);
								if (p->get_type() == Pass::Host) consume = static_cast<HostPass*>(p);
							}
							unsigned char pixel[4] = {};
							if (consume != nullptr) {
								consume->SetCallback([&pixel](HostContext& ctx) {
									ctx.WaitForGpu();
									const rhi::Buffer* source = ctx.ReadBuffer("Pixels");
									if (source != nullptr && source->Map() != nullptr)
										std::memcpy(pixel, source->Map(), sizeof(pixel));
									ctx.SignalGpu();
								});
								readbackGraph.SubmitGraph();
								readbackGraph.WaitHostPasses();
								device->WaitForFrame();

								auto closeEnough = [](unsigned char v, int expect) {
									const int d = int(v) - expect;
									return d >= -2 && d <= 2;
								};
								const bool ok = closeEnough(pixel[0], 128) && closeEnough(pixel[1], 0)
									&& closeEnough(pixel[2], 128) && pixel[3] == 255;
								cout << "Static texture heap readback " << (ok ? "ok" : "FAILED")
								     << ": rgba8=" << int(pixel[0]) << "," << int(pixel[1]) << ","
								     << int(pixel[2]) << "," << int(pixel[3]) << "\n";
							}
						}
					}
				}
			}

			{
				RenderGraph stateGraph(device.get());

				RenderPassDesc dualDesc;
				{
					AttachmentBinding attA;
					attA.resourceName = "ColorA";
					attA.slot = 0;
					attA.load = LoadOp::Clear;
					attA.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 1.0f, 1.0f };
					dualDesc.attachments.push_back(attA);

					AttachmentBinding attB;
					attB.resourceName = "ColorB";
					attB.slot = 1;
					attB.load = LoadOp::Clear;
					attB.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
					dualDesc.attachments.push_back(attB);

					dualDesc.pipeline.shaders = {
						{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
						{ rhi::ShaderStage::Fragment, shaderDir + "/dual_output.frag.spv" },
					};
					dualDesc.pipeline.cullMode = CullMode::None;
					dualDesc.pipeline.depthStencil.depthTestEnable = false;

					rhi::ColorAttachmentState blendState;
					blendState.enableBlend = true;
					blendState.srcColor = rhi::BlendFactor::SrcAlpha;
					blendState.dstColor = rhi::BlendFactor::OneMinusSrcAlpha;
					blendState.srcAlpha = rhi::BlendFactor::One;
					blendState.dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha;
					blendState.writeMask = rhi::ColorWriteMask::All;

					rhi::ColorAttachmentState maskState;
					maskState.writeMask = rhi::ColorWriteMask::R;
					dualDesc.pipeline.colorStates = { blendState, maskState };
				}
				stateGraph.RegisterPass("Dual", {}, { "ColorA", "ColorB" }, Pass::Render, dualDesc);
				stateGraph.RegisterPass("VerifyDual", { "ColorA", "ColorB" }, {}, Pass::Host, HostPassDesc{});

				ImageDesc colorADesc;
				colorADesc.width = 64; colorADesc.height = 64;
				colorADesc.format = ImageFormat::RGBA8;
				stateGraph.RegisterResource("ColorA", { "VerifyDual" }, "Dual", Resource::Image, colorADesc);
				stateGraph.RegisterResource("ColorB", { "VerifyDual" }, "Dual", Resource::Image, colorADesc);

				stateGraph.ProcessRegister();
				stateGraph.GenerateSubmitSequence();
				stateGraph.RealizeAll();

				for (uint32_t i = 0; stateGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = stateGraph.GetPass(i);
					if (p->get_type() != Pass::Render) continue;
					DrawSource draw;
					draw.count = 3;
					static_cast<RenderPass*>(p)->SetDraw(draw);
				}

				HostPass* verifyDual = nullptr;
				for (uint32_t i = 0; stateGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = stateGraph.GetPass(i);
					if (p->get_type() == Pass::Host) verifyDual = static_cast<HostPass*>(p);
				}

				bool stateOk = false;
				if (verifyDual != nullptr) {
					verifyDual->SetCallback([&stateOk](HostContext& ctx) {
						ctx.WaitForGpu();
						const unsigned char* a = static_cast<const unsigned char*>(ctx.ReadImage("ColorA"));
						const unsigned char* b = static_cast<const unsigned char*>(ctx.ReadImage("ColorB"));
						if (a != nullptr && b != nullptr) {
							auto closeEnough = [](unsigned char v, int expect) {
								const int d = int(v) - expect;
								return d >= -2 && d <= 2;
							};
							stateOk = closeEnough(a[0], 128) && closeEnough(a[1], 0)
								&& closeEnough(a[2], 128) && a[3] == 255
								&& b[0] == 255 && b[1] == 0 && b[2] == 0 && b[3] == 255;
						}
						ctx.SignalGpu();
					});
					stateGraph.SubmitGraph();
					stateGraph.WaitHostPasses();
					device->WaitForFrame();
					cout << "Per-attachment state " << (stateOk ? "ok" : "FAILED")
					     << ": blend+writeMask\n";
				}
			}

			{
				RenderGraph sameQueueGraph(device.get());
				ComputePassDesc emptyCompute;
				emptyCompute.shader = { rhi::ShaderStage::Compute, "" };
				RenderGraph::PassOrder headFirst;
				headFirst.after = { "Head" };
				sameQueueGraph.RegisterPass("Tail", {}, {}, Pass::Compute, emptyCompute,
					headFirst, rhi::QueueClass::Graphics);
				sameQueueGraph.RegisterPass("Head", {}, {}, Pass::Compute, emptyCompute,
					{}, rhi::QueueClass::Graphics);
				sameQueueGraph.ProcessRegister();
				sameQueueGraph.GenerateSubmitSequence();
				sameQueueGraph.CompileBarriers();

				auto idOf = [](RenderGraph& g, const std::string& name) -> uint32_t {
					for (uint32_t i = 0; g.GetPass(i) != nullptr; ++i)
						if (g.GetPass(i)->get_name() == name) return i;
					return UINT32_MAX;
				};
				auto seqPos = [](RenderGraph& g, uint32_t id) -> int {
					const std::vector<uint32_t>& seq = g.GetSubmitSequence();
					for (size_t i = 0; i < seq.size(); ++i)
						if (seq[i] == id) return static_cast<int>(i);
					return -1;
				};
				const uint32_t headId = idOf(sameQueueGraph, "Head");
				const uint32_t tailId = idOf(sameQueueGraph, "Tail");
				const bool orderOk = seqPos(sameQueueGraph, headId) >= 0
					&& seqPos(sameQueueGraph, tailId) >= 0
					&& seqPos(sameQueueGraph, headId) < seqPos(sameQueueGraph, tailId);
				const bool sameQueueOk = sameQueueGraph.GetSubmitPlan().batches.size() == 1
					&& sameQueueGraph.GetSubmitPlan().syncPoints.empty();
				cout << "Order constraint same-queue " << ((orderOk && sameQueueOk) ? "ok" : "FAILED")
				     << ": Head -> Tail, syncPoints=" << sameQueueGraph.GetSubmitPlan().syncPoints.size()
				     << "\n";
			}
			{
				RenderGraph crossQueueGraph(device.get());
				ComputePassDesc emptyCompute;
				emptyCompute.shader = { rhi::ShaderStage::Compute, "" };
				RenderGraph::PassOrder computeFirst;
				computeFirst.after = { "ComputeStage" };
				crossQueueGraph.RegisterPass("ComputeStage", {}, {}, Pass::Compute, emptyCompute,
					{}, rhi::QueueClass::AsyncCompute);
				crossQueueGraph.RegisterPass("GraphicsStage", {}, {}, Pass::Compute, emptyCompute,
					computeFirst, rhi::QueueClass::Graphics);
				crossQueueGraph.ProcessRegister();
				crossQueueGraph.GenerateSubmitSequence();
				crossQueueGraph.CompileBarriers();

				const auto& plan = crossQueueGraph.GetSubmitPlan();
				bool noMemoryBarrier = true;
				for (const auto& batch : plan.batches)
					if (!batch.releases.images.empty() || !batch.releases.buffers.empty())
						noMemoryBarrier = false;
				const bool crossOk = plan.batches.size() == 2 && plan.syncPoints.size() == 1
					&& noMemoryBarrier;
				cout << "Order constraint cross-queue " << (crossOk ? "ok" : "FAILED")
				     << ": batches=" << plan.batches.size()
				     << ", syncPoints=" << plan.syncPoints.size()
				     << ", memoryBarrier=" << (noMemoryBarrier ? "none" : "present") << "\n";
			}

			{
				rhi::ImageCreateInfo muxTexInfo;
				muxTexInfo.width = 64; muxTexInfo.height = 64;
				muxTexInfo.format = rhi::ImageFormat::RGBA8;
				muxTexInfo.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
				auto muxTexA = device->CreateImage(muxTexInfo);
				auto muxTexB = device->CreateImage(muxTexInfo);

				if (muxTexA && muxTexB) {
					rhi::CommandList muxInit = device->BeginCommandList();
					rhi::ImageBarrier muxToDst[2];
					rhi::ImageBarrier muxToRead[2];
					for (int i = 0; i < 2; ++i) {
						muxToDst[i].image = i == 0 ? muxTexA.get() : muxTexB.get();
						muxToDst[i].oldLayout = rhi::ImageLayout::Undefined;
						muxToDst[i].newLayout = rhi::ImageLayout::TransferDst;
						muxToDst[i].dstStage = rhi::PipelineStage::Transfer;
						muxToDst[i].dstAccess = rhi::Access::TransferWrite;

						muxToRead[i].image = i == 0 ? muxTexA.get() : muxTexB.get();
						muxToRead[i].oldLayout = rhi::ImageLayout::TransferDst;
						muxToRead[i].newLayout = rhi::ImageLayout::ShaderReadOnly;
						muxToRead[i].srcStage = rhi::PipelineStage::Transfer;
						muxToRead[i].srcAccess = rhi::Access::TransferWrite;
						muxToRead[i].dstStage = rhi::PipelineStage::FragmentShader;
						muxToRead[i].dstAccess = rhi::Access::ShaderRead;
					}
					muxInit.Barrier({ &muxToDst[0], 2 }, {});
					const float muxRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
					const float muxBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
					muxInit.ClearColor(*muxTexA, rhi::ImageSubresource{ 0, 0 }, muxRed,
						rhi::ImageLayout::TransferDst);
					muxInit.ClearColor(*muxTexB, rhi::ImageSubresource{ 0, 0 }, muxBlue,
						rhi::ImageLayout::TransferDst);
					muxInit.Barrier({ &muxToRead[0], 2 }, {});
					rhi::SubmitInfo muxInitSubmit;
					muxInitSubmit.commandLists = { &muxInit, 1 };
					device->Submit(muxInitSubmit);
					device->WaitForFrame();

					RenderGraph muxGraph(device.get());
					ImageDesc muxOutDesc;
					muxOutDesc.width = 64; muxOutDesc.height = 64;
					muxOutDesc.format = ImageFormat::RGBA8;
					muxGraph.RegisterMuxOutput("MuxOut", { "SampleMux" }, "SelectMux", muxOutDesc);
					muxGraph.RegisterResource("MuxA", { "SelectMux" }, "", muxTexA.get(), {},
						rhi::ImageLayout::ShaderReadOnly);
					muxGraph.RegisterResource("MuxB", { "SelectMux" }, "", muxTexB.get(), {},
						rhi::ImageLayout::ShaderReadOnly);

					MuxPassDesc muxDesc;
					muxDesc.alternatives = { "MuxA", "MuxB" };
					muxGraph.RegisterPass("SelectMux", { "MuxA", "MuxB" }, { "MuxOut" }, Pass::Mux, muxDesc);

					RenderPassDesc muxSampleDesc;
					{
						AttachmentBinding att;
						att.resourceName = "Out";
						att.slot = 0;
						att.load = LoadOp::Clear;
						att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
						muxSampleDesc.attachments.push_back(att);

						ShaderBinding sb;
						sb.resourceName = "MuxOut";
						muxSampleDesc.shaderBindings.push_back(sb);
						muxSampleDesc.pipeline.shaders = {
							{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
							{ rhi::ShaderStage::Fragment, shaderDir + "/mux_sample.frag.spv" },
						};
						muxSampleDesc.pipeline.cullMode = CullMode::None;
						muxSampleDesc.pipeline.depthStencil.depthTestEnable = false;
					}
					muxGraph.RegisterPass("SampleMux", { "MuxOut" }, { "Out" }, Pass::Render, muxSampleDesc);
					muxGraph.RegisterPass("VerifyMux", { "Out" }, {}, Pass::Host, HostPassDesc{});

					ImageDesc muxOutImageDesc;
					muxOutImageDesc.width = 64; muxOutImageDesc.height = 64;
					muxOutImageDesc.format = ImageFormat::RGBA8;
					muxGraph.RegisterResource("Out", { "VerifyMux" }, "SampleMux", Resource::Image,
						muxOutImageDesc);

					muxGraph.ProcessRegister();
					muxGraph.GenerateSubmitSequence();
					muxGraph.RealizeAll();

					for (uint32_t i = 0; muxGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = muxGraph.GetPass(i);
						if (p->get_type() != Pass::Render) continue;
						DrawSource draw;
						draw.count = 3;
						static_cast<RenderPass*>(p)->SetDraw(draw);
					}

					unsigned char muxPixel[4] = {};
					HostPass* verifyMux = nullptr;
					for (uint32_t i = 0; muxGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = muxGraph.GetPass(i);
						if (p->get_type() == Pass::Host) verifyMux = static_cast<HostPass*>(p);
					}
					if (verifyMux != nullptr) {
						verifyMux->SetCallback([&muxPixel](HostContext& ctx) {
							ctx.WaitForGpu();
							const unsigned char* px = static_cast<const unsigned char*>(ctx.ReadImage("Out"));
							if (px != nullptr)
								std::memcpy(muxPixel, px, sizeof(muxPixel));
							ctx.SignalGpu();
						});

						const uint32_t slot0 = device->BeginFrame();
						muxGraph.SetMuxSelection("SelectMux", slot0, 0);
						muxGraph.BeginFrame(slot0);
						muxGraph.SubmitGraph();
						muxGraph.WaitHostPasses();
						device->WaitForFrame();
						const bool muxRedOk = muxPixel[0] == 255 && muxPixel[1] == 0
							&& muxPixel[2] == 0 && muxPixel[3] == 255;

						const uint32_t slot1 = device->BeginFrame();
						muxGraph.SetMuxSelection("SelectMux", slot1, 1);
						muxGraph.BeginFrame(slot1);
						muxGraph.SubmitGraph();
						muxGraph.WaitHostPasses();
						device->WaitForFrame();
						const bool muxBlueOk = muxPixel[0] == 0 && muxPixel[1] == 0
							&& muxPixel[2] == 255 && muxPixel[3] == 255;

						cout << "Runtime mux " << ((muxRedOk && muxBlueOk) ? "ok" : "FAILED")
						     << ": first(red)=" << (muxRedOk ? "ok" : "FAILED")
						     << ", second(blue)=" << (muxBlueOk ? "ok" : "FAILED")
						     << ", last rgba8=" << int(muxPixel[0]) << "," << int(muxPixel[1])
						     << "," << int(muxPixel[2]) << "," << int(muxPixel[3]) << "\n";
					}
				}
			}

			{
				rhi::ImageCreateInfo candInfo;
				candInfo.width = 64; candInfo.height = 64;
				candInfo.format = rhi::ImageFormat::RGBA16F;
				candInfo.usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc
					| rhi::ImageUsage::TransferDst;
				auto candA = device->CreateImage(candInfo);
				auto candB = device->CreateImage(candInfo);

				rhi::ImageCreateInfo colorInfo;
				colorInfo.width = 64; colorInfo.height = 64;
				colorInfo.format = rhi::ImageFormat::RGBA16F;
				colorInfo.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
				auto colorImg = device->CreateImage(colorInfo);

				bool ok = false;
				if (candA && candB && colorImg) {
					rhi::CommandList init = device->BeginCommandList();
					rhi::ImageBarrier toDst;
					toDst.image = colorImg.get();
					toDst.oldLayout = rhi::ImageLayout::Undefined;
					toDst.newLayout = rhi::ImageLayout::TransferDst;
					toDst.dstStage = rhi::PipelineStage::Transfer;
					toDst.dstAccess = rhi::Access::TransferWrite;
					init.Barrier({ &toDst, 1 }, {});
					rhi::ImageBarrier candToDst[2];
					for (int i = 0; i < 2; ++i) {
						candToDst[i].image = i == 0 ? candA.get() : candB.get();
						candToDst[i].oldLayout = rhi::ImageLayout::Undefined;
						candToDst[i].newLayout = rhi::ImageLayout::TransferDst;
						candToDst[i].dstStage = rhi::PipelineStage::Transfer;
						candToDst[i].dstAccess = rhi::Access::TransferWrite;
					}
					init.Barrier({ &candToDst[0], 2 }, {});
					const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
					const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
					init.ClearColor(*candA, rhi::ImageSubresource{ 0, 0 }, red,
						rhi::ImageLayout::TransferDst);
					init.ClearColor(*candB, rhi::ImageSubresource{ 0, 0 }, blue,
						rhi::ImageLayout::TransferDst);
					init.ClearColor(*colorImg, rhi::ImageSubresource{ 0, 0 }, red,
						rhi::ImageLayout::TransferDst);
					rhi::ImageBarrier toRead;
					toRead.image = colorImg.get();
					toRead.oldLayout = rhi::ImageLayout::TransferDst;
					toRead.newLayout = rhi::ImageLayout::ShaderReadOnly;
					toRead.srcStage = rhi::PipelineStage::Transfer;
					toRead.srcAccess = rhi::Access::TransferWrite;
					toRead.dstStage = rhi::PipelineStage::ComputeShader;
					toRead.dstAccess = rhi::Access::ShaderRead;
					init.Barrier({ &toRead, 1 }, {});
					rhi::SubmitInfo initSubmit;
					initSubmit.commandLists = { &init, 1 };
					device->Submit(initSubmit);
					device->WaitForFrame();

					RenderGraph storageMuxGraph(device.get());

					ImageDesc storeOutDesc;
					storeOutDesc.width = 64; storeOutDesc.height = 64;
					storeOutDesc.format = ImageFormat::RGBA16F;
					storageMuxGraph.RegisterMuxOutput("Blurred", { "BlurMux" },
						"SelectStore", storeOutDesc);
					storageMuxGraph.RegisterResource("StoreA", { "SelectStore" }, "",
						candA.get(), {}, rhi::ImageLayout::TransferDst, rhi::ImageFormat::None,
						rhi::ImageSubresourceRange{ {}, 0, 0 }, true);
					storageMuxGraph.RegisterResource("StoreB", { "SelectStore" }, "",
						candB.get(), {}, rhi::ImageLayout::TransferDst, rhi::ImageFormat::None,
						rhi::ImageSubresourceRange{ {}, 0, 0 }, true);
					storageMuxGraph.RegisterResource("Color", { "BlurMux" }, "",
						colorImg.get(), {}, rhi::ImageLayout::ShaderReadOnly);

					MuxPassDesc storeMuxDesc;
					storeMuxDesc.alternatives = { "StoreA", "StoreB" };
					storageMuxGraph.RegisterPass("SelectStore", { "StoreA", "StoreB" },
						{ "Blurred" }, Pass::Mux, storeMuxDesc);

					ComputePassDesc blurMuxDesc;
					blurMuxDesc.shader = { rhi::ShaderStage::Compute, shaderDir + "/blur.comp.spv" };
					blurMuxDesc.groupCountX = 1;
					blurMuxDesc.groupCountY = 1;
					ShaderBinding colorBinding;
					colorBinding.resourceName = "Color";
					ShaderBinding storeBinding;
					storeBinding.resourceName = "Blurred";
					storeBinding.usage = Usage::StorageWrite;
					blurMuxDesc.shaderBindings = { colorBinding, storeBinding };
					storageMuxGraph.RegisterPass("BlurMux", { "Color" }, { "Blurred" },
						Pass::Compute, blurMuxDesc);

					BufferDesc storePixelsDesc;
					storePixelsDesc.size = 64 * 64 * 8;
					storePixelsDesc.usage = BufferUsage::TransferDst;
					storePixelsDesc.hostVisible = true;
					storageMuxGraph.RegisterResource("StorePixels", {}, "ReadStore",
						Resource::Buffer, storePixelsDesc);

					rhi::BufferImageCopyRegion storeRegion;
					storeRegion.subresource = rhi::ImageSubresourceRange{ { 0, 0 }, 1, 1 };
					storeRegion.imageExtentWidth = 64;
					storeRegion.imageExtentHeight = 64;
					storeRegion.imageExtentDepth = 1;

					TransferPassDesc storeReadDesc;
					storeReadDesc.kind = TransferKind::ImageToBuffer;
					storeReadDesc.sourceName = "Blurred";
					storeReadDesc.destName = "StorePixels";
					storeReadDesc.bufferImageRegions = { storeRegion };

					RenderGraph::PassOrder storeReadOrder;
					storeReadOrder.after = { "SelectStore", "BlurMux" };
					storageMuxGraph.RegisterPass("ReadStore", { "Blurred" }, { "StorePixels" },
						Pass::Transfer, storeReadDesc, storeReadOrder);

					storageMuxGraph.ProcessRegister();
					storageMuxGraph.GenerateSubmitSequence();
					storageMuxGraph.RealizeAll();

					rhi::Buffer* pixels = nullptr;
					for (uint32_t i = 0; storageMuxGraph.GetResource(i) != nullptr; ++i) {
						Resource* res = storageMuxGraph.GetResource(i);
						if (res->get_type() == Resource::Buffer && res->get_name() == "StorePixels")
							pixels = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					}

					if (pixels != nullptr) {
						const size_t corner = (63u * 64u + 63u) * 4u;

						const uint32_t slot0 = device->BeginFrame();
						storageMuxGraph.SetMuxSelection("SelectStore", slot0, 0);
						storageMuxGraph.BeginFrame(slot0);
						storageMuxGraph.SubmitGraph();
						device->WaitForFrame();
						pixels->Invalidate();
						const uint16_t* px = static_cast<const uint16_t*>(pixels->Map());
						const bool redOk = px != nullptr && px[corner] == 0x3C00
							&& px[corner + 1] == 0 && px[corner + 2] == 0
							&& px[corner + 3] == 0x3C00;

						const uint32_t slot1 = device->BeginFrame();
						storageMuxGraph.SetMuxSelection("SelectStore", slot1, 1);
						storageMuxGraph.BeginFrame(slot1);
						storageMuxGraph.SubmitGraph();
						device->WaitForFrame();
						pixels->Invalidate();
						px = static_cast<const uint16_t*>(pixels->Map());
						const bool blueOk = px != nullptr && px[corner] == 0
							&& px[corner + 1] == 0 && px[corner + 2] == 0x3C00
							&& px[corner + 3] == 0x3C00;

						ok = redOk && blueOk;
					}
					cout << "Mux storage output " << (ok ? "ok" : "FAILED")
					     << ": red->blue switch\n";
					}
				}

			{
				rhi::ImageCreateInfo candInfo;
				candInfo.width = 64; candInfo.height = 64;
				candInfo.format = rhi::ImageFormat::RGBA8;
				candInfo.usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::TransferSrc
					| rhi::ImageUsage::TransferDst;
				auto candA = device->CreateImage(candInfo);
				auto candB = device->CreateImage(candInfo);

				rhi::CommandList attachInit = device->BeginCommandList();
				rhi::ImageBarrier attachToDst[2];
				for (int i = 0; i < 2; ++i) {
					attachToDst[i].image = i == 0 ? candA.get() : candB.get();
					attachToDst[i].oldLayout = rhi::ImageLayout::Undefined;
					attachToDst[i].newLayout = rhi::ImageLayout::TransferDst;
					attachToDst[i].dstStage = rhi::PipelineStage::Transfer;
					attachToDst[i].dstAccess = rhi::Access::TransferWrite;
				}
				attachInit.Barrier({ &attachToDst[0], 2 }, {});
				const float attachRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
				const float attachBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
				attachInit.ClearColor(*candA, rhi::ImageSubresource{ 0, 0 }, attachRed,
					rhi::ImageLayout::TransferDst);
				attachInit.ClearColor(*candB, rhi::ImageSubresource{ 0, 0 }, attachBlue,
					rhi::ImageLayout::TransferDst);
				rhi::SubmitInfo attachInitSubmit;
				attachInitSubmit.commandLists = { &attachInit, 1 };
				device->Submit(attachInitSubmit);
				device->WaitForFrame();

				bool ok = false;
				if (candA && candB) {
					RenderGraph attachMuxGraph(device.get());

					ImageDesc attachOutDesc;
					attachOutDesc.width = 64; attachOutDesc.height = 64;
					attachOutDesc.format = ImageFormat::RGBA8;
					attachMuxGraph.RegisterMuxOutput("AttachOut", {}, "SelectAttach", attachOutDesc);
					attachMuxGraph.RegisterResource("AttachA", { "SelectAttach" }, "",
						candA.get(), {}, rhi::ImageLayout::TransferDst, rhi::ImageFormat::None,
						rhi::ImageSubresourceRange{ {}, 0, 0 }, true);
					attachMuxGraph.RegisterResource("AttachB", { "SelectAttach" }, "",
						candB.get(), {}, rhi::ImageLayout::TransferDst, rhi::ImageFormat::None,
						rhi::ImageSubresourceRange{ {}, 0, 0 }, true);

					MuxPassDesc attachMuxDesc;
					attachMuxDesc.alternatives = { "AttachA", "AttachB" };
					attachMuxGraph.RegisterPass("SelectAttach", { "AttachA", "AttachB" },
						{ "AttachOut" }, Pass::Mux, attachMuxDesc);

					RenderPassDesc fillDesc;
					{
						AttachmentBinding att;
						att.resourceName = "AttachOut";
						att.slot = 0;
						att.load = LoadOp::Load;
						att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
						fillDesc.attachments.push_back(att);
						fillDesc.pipeline.shaders = {
							{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
							{ rhi::ShaderStage::Fragment, shaderDir + "/solid.frag.spv" },
						};
						fillDesc.pipeline.cullMode = CullMode::None;
						fillDesc.pipeline.depthStencil.depthTestEnable = false;
					}
					attachMuxGraph.RegisterPass("FillMux", {}, { "AttachOut" },
						Pass::Render, fillDesc);

					BufferDesc attachPixelsDesc;
					attachPixelsDesc.size = 64 * 64 * 4;
					attachPixelsDesc.usage = BufferUsage::TransferDst;
					attachPixelsDesc.hostVisible = true;
					attachMuxGraph.RegisterResource("AttachPixels", {}, "ReadAttach",
						Resource::Buffer, attachPixelsDesc);

					rhi::BufferImageCopyRegion readRegion;
					readRegion.subresource = rhi::ImageSubresourceRange{ { 0, 0 }, 1, 1 };
					readRegion.imageExtentWidth = 64;
					readRegion.imageExtentHeight = 64;
					readRegion.imageExtentDepth = 1;

					TransferPassDesc attachReadDesc;
					attachReadDesc.kind = TransferKind::ImageToBuffer;
					attachReadDesc.sourceName = "AttachOut";
					attachReadDesc.destName = "AttachPixels";
					attachReadDesc.bufferImageRegions = { readRegion };

					RenderGraph::PassOrder readOrder;
					readOrder.after = { "SelectAttach", "FillMux" };
					attachMuxGraph.RegisterPass("ReadAttach", { "AttachOut" }, { "AttachPixels" },
						Pass::Transfer, attachReadDesc, readOrder);

					attachMuxGraph.ProcessRegister();
					attachMuxGraph.GenerateSubmitSequence();
					attachMuxGraph.RealizeAll();

					for (uint32_t i = 0; attachMuxGraph.GetPass(i) != nullptr; ++i) {
						Pass* p = attachMuxGraph.GetPass(i);
						if (p->get_type() != Pass::Render)
							continue;
						DrawSource draw;
						draw.count = 0;
						static_cast<RenderPass*>(p)->SetDraw(draw);
					}

					rhi::Buffer* pixels = nullptr;
					for (uint32_t i = 0; attachMuxGraph.GetResource(i) != nullptr; ++i) {
						Resource* res = attachMuxGraph.GetResource(i);
						if (res->get_type() == Resource::Buffer && res->get_name() == "AttachPixels")
							pixels = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
					}

					if (pixels != nullptr) {
						const uint32_t slot0 = device->BeginFrame();
						attachMuxGraph.SetMuxSelection("SelectAttach", slot0, 0);
						attachMuxGraph.BeginFrame(slot0);
						attachMuxGraph.SubmitGraph();
						device->WaitForFrame();
						pixels->Invalidate();
						const unsigned char* px = static_cast<const unsigned char*>(pixels->Map());
						const bool redOk = px != nullptr && px[0] == 255 && px[1] == 0
							&& px[2] == 0 && px[3] == 255;

						const uint32_t slot1 = device->BeginFrame();
						attachMuxGraph.SetMuxSelection("SelectAttach", slot1, 1);
						attachMuxGraph.BeginFrame(slot1);
						attachMuxGraph.SubmitGraph();
						device->WaitForFrame();
						pixels->Invalidate();
						px = static_cast<const unsigned char*>(pixels->Map());
						const bool blueOk = px != nullptr && px[0] == 0 && px[1] == 0
							&& px[2] == 255 && px[3] == 255;
						ok = redOk && blueOk;
					}
				}
					cout << "Mux attachment output " << (ok ? "ok" : "FAILED")
					     << ": red->blue switch\n";
				}

			{
				rhi::BufferCreateInfo uboInfo;
				uboInfo.size = 256;
				uboInfo.usage = rhi::BufferUsage::Uniform;
				uboInfo.hostVisible = true;
				auto uboA = device->CreateBuffer(uboInfo);
				auto uboB = device->CreateBuffer(uboInfo);
				if (uboA && uboB) {
					const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
					const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
					std::memcpy(uboA->Map(), red, sizeof(red));
					std::memcpy(uboB->Map(), blue, sizeof(blue));
					uboA->Flush();
					uboB->Flush();
				}

				RenderGraph bufferMuxGraph(device.get());
				BufferDesc uboDesc;
				uboDesc.size = 256;
				uboDesc.usage = rhi::BufferUsage::Uniform;
				uboDesc.hostVisible = true;
				bufferMuxGraph.RegisterMuxOutput("ubo", { "Tint" }, "SelectUbo", uboDesc);
				bufferMuxGraph.RegisterResource("UboA", { "SelectUbo" }, "", uboA.get());
				bufferMuxGraph.RegisterResource("UboB", { "SelectUbo" }, "", uboB.get());

				MuxPassDesc uboMuxDesc;
				uboMuxDesc.alternatives = { "UboA", "UboB" };
				bufferMuxGraph.RegisterPass("SelectUbo", { "UboA", "UboB" }, { "ubo" },
					Pass::Mux, uboMuxDesc);

				RenderPassDesc uboTintDesc;
				{
					AttachmentBinding att;
					att.resourceName = "Out";
					att.slot = 0;
					att.load = LoadOp::Clear;
					att.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
					uboTintDesc.attachments.push_back(att);

					ShaderBinding ub;
					ub.resourceName = "ubo";
					ub.usage = Usage::UniformRead;
					uboTintDesc.shaderBindings.push_back(ub);
					uboTintDesc.pipeline.shaders = {
						{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
						{ rhi::ShaderStage::Fragment, shaderDir + "/ubo.frag.spv" },
					};
					uboTintDesc.pipeline.cullMode = CullMode::None;
					uboTintDesc.pipeline.depthStencil.depthTestEnable = false;
				}
				bufferMuxGraph.RegisterPass("Tint", { "ubo" }, { "Out" }, Pass::Render, uboTintDesc);
				bufferMuxGraph.RegisterPass("VerifyUbo", { "Out" }, {}, Pass::Host, HostPassDesc{});

				ImageDesc uboOutDesc;
				uboOutDesc.width = 64; uboOutDesc.height = 64;
				uboOutDesc.format = ImageFormat::RGBA8;
				bufferMuxGraph.RegisterResource("Out", { "VerifyUbo" }, "Tint",
					Resource::Image, uboOutDesc);

				bufferMuxGraph.ProcessRegister();
				bufferMuxGraph.GenerateSubmitSequence();
				bufferMuxGraph.RealizeAll();

				for (uint32_t i = 0; bufferMuxGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = bufferMuxGraph.GetPass(i);
					if (p->get_type() != Pass::Render)
						continue;
					DrawSource draw;
					draw.count = 3;
					static_cast<RenderPass*>(p)->SetDraw(draw);
				}

				unsigned char uboPixel[4] = {};
				HostPass* verifyUbo = nullptr;
				for (uint32_t i = 0; bufferMuxGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = bufferMuxGraph.GetPass(i);
					if (p->get_type() == Pass::Host)
						verifyUbo = static_cast<HostPass*>(p);
				}

				bool ok = false;
				if (verifyUbo != nullptr) {
					verifyUbo->SetCallback([&uboPixel](HostContext& ctx) {
						ctx.WaitForGpu();
						const unsigned char* px = static_cast<const unsigned char*>(
							ctx.ReadImage("Out"));
						if (px != nullptr)
							std::memcpy(uboPixel, px, sizeof(uboPixel));
						ctx.SignalGpu();
					});

					const uint32_t slot0 = device->BeginFrame();
					bufferMuxGraph.SetMuxSelection("SelectUbo", slot0, 0);
					bufferMuxGraph.BeginFrame(slot0);
					bufferMuxGraph.SubmitGraph();
					bufferMuxGraph.WaitHostPasses();
					device->WaitForFrame();
					const bool redOk = uboPixel[0] == 255 && uboPixel[1] == 0
						&& uboPixel[2] == 0 && uboPixel[3] == 255;

					const uint32_t slot1 = device->BeginFrame();
					bufferMuxGraph.SetMuxSelection("SelectUbo", slot1, 1);
					bufferMuxGraph.BeginFrame(slot1);
					bufferMuxGraph.SubmitGraph();
					bufferMuxGraph.WaitHostPasses();
					device->WaitForFrame();
					const bool blueOk = uboPixel[0] == 0 && uboPixel[1] == 0
						&& uboPixel[2] == 255 && uboPixel[3] == 255;
					ok = redOk && blueOk;
				}
				cout << "Mux buffer output " << (ok ? "ok" : "FAILED")
				     << ": red->blue switch\n";
			}

			{
				ComputePassDesc emptyCompute;
				emptyCompute.shader = { rhi::ShaderStage::Compute, "" };
				BufferDesc dataDesc;
				dataDesc.size = 4;
				dataDesc.usage = BufferUsage::Storage;
				dataDesc.hostVisible = true;

				RenderGraph liveGraph(device.get());
				liveGraph.RegisterPass("Writer", {}, { "Data" }, Pass::Compute, emptyCompute);
				liveGraph.RegisterPass("Reader", { "Data" }, {}, Pass::Host, HostPassDesc{});
				liveGraph.RegisterResource("Data", { "Reader" }, "Writer", Resource::Buffer, dataDesc);
				liveGraph.ProcessRegister();

				uint32_t writerId = UINT32_MAX;
				for (uint32_t i = 0; liveGraph.GetPass(i) != nullptr; ++i)
					if (liveGraph.GetPass(i)->get_name() == "Writer") writerId = i;
				liveGraph.SetPassActive("Writer", false);
				const bool liveRejected = liveGraph.GetPass(writerId)->is_active();

				RenderGraph deadGraph(device.get());
				deadGraph.RegisterPass("Solo", {}, { "Lonely" }, Pass::Compute, emptyCompute);
				deadGraph.RegisterResource("Lonely", {}, "Solo", Resource::Buffer, dataDesc);
				deadGraph.ProcessRegister();
				uint32_t soloId = UINT32_MAX;
				for (uint32_t i = 0; deadGraph.GetPass(i) != nullptr; ++i)
					if (deadGraph.GetPass(i)->get_name() == "Solo") soloId = i;
				deadGraph.SetPassActive("Solo", false);
				const bool deadGated = !deadGraph.GetPass(soloId)->is_active();

				cout << "Execution gate " << ((liveRejected && deadGated) ? "ok" : "FAILED")
				     << ": live producer kept active=" << (liveRejected ? "yes" : "no")
				     << ", dead pass gated=" << (deadGated ? "yes" : "no") << "\n";
			}

			{
				rhi::ImageCreateInfo mfImageInfo;
				mfImageInfo.width = 64; mfImageInfo.height = 64;
				mfImageInfo.format = rhi::ImageFormat::RGBA8;
				mfImageInfo.usage = rhi::ImageUsage::TransferDst | rhi::ImageUsage::TransferSrc;
				auto mfImageA = device->CreateImage(mfImageInfo);
				auto mfImageB = device->CreateImage(mfImageInfo);

				const uint64_t mfBytes = 64ull * 64ull * 4ull;
				rhi::BufferCreateInfo mfBufferInfo;
				mfBufferInfo.size = mfBytes;
				mfBufferInfo.usage = rhi::BufferUsage::TransferDst;
				mfBufferInfo.hostVisible = true;
				auto mfReadA = device->CreateBuffer(mfBufferInfo);
				auto mfReadB = device->CreateBuffer(mfBufferInfo);

				if (mfImageA && mfImageB && mfReadA && mfReadB) {
					uint32_t mfSlotA = device->BeginFrame();
					rhi::CommandList mfCmdA = device->BeginCommandList();
					rhi::ImageBarrier mfToDstA;
					mfToDstA.image = mfImageA.get();
					mfToDstA.oldLayout = rhi::ImageLayout::Undefined;
					mfToDstA.newLayout = rhi::ImageLayout::TransferDst;
					mfToDstA.dstStage = rhi::PipelineStage::Transfer;
					mfToDstA.dstAccess = rhi::Access::TransferWrite;
					rhi::ImageBarrier mfToSrcA;
					mfToSrcA.image = mfImageA.get();
					mfToSrcA.oldLayout = rhi::ImageLayout::TransferDst;
					mfToSrcA.newLayout = rhi::ImageLayout::TransferSrc;
					mfToSrcA.srcStage = rhi::PipelineStage::Transfer;
					mfToSrcA.srcAccess = rhi::Access::TransferWrite;
					mfToSrcA.dstStage = rhi::PipelineStage::Transfer;
					mfToSrcA.dstAccess = rhi::Access::TransferRead;
					mfCmdA.Barrier({ &mfToDstA, 1 }, {});
					const float mfRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
					mfCmdA.ClearColor(*mfImageA, rhi::ImageSubresource{ 0, 0 }, mfRed,
						rhi::ImageLayout::TransferDst);
					mfCmdA.Barrier({ &mfToSrcA, 1 }, {});
					mfCmdA.CopyImageToBuffer(*mfImageA, *mfReadA, rhi::ImageLayout::TransferSrc);
					rhi::SubmitInfo mfSubmitA;
					mfSubmitA.commandLists = { &mfCmdA, 1 };
					device->Submit(mfSubmitA);

					uint32_t mfSlotB = device->BeginFrame();
					rhi::CommandList mfCmdB = device->BeginCommandList();
					rhi::ImageBarrier mfToDstB;
					mfToDstB.image = mfImageB.get();
					mfToDstB.oldLayout = rhi::ImageLayout::Undefined;
					mfToDstB.newLayout = rhi::ImageLayout::TransferDst;
					mfToDstB.dstStage = rhi::PipelineStage::Transfer;
					mfToDstB.dstAccess = rhi::Access::TransferWrite;
					rhi::ImageBarrier mfToSrcB;
					mfToSrcB.image = mfImageB.get();
					mfToSrcB.oldLayout = rhi::ImageLayout::TransferDst;
					mfToSrcB.newLayout = rhi::ImageLayout::TransferSrc;
					mfToSrcB.srcStage = rhi::PipelineStage::Transfer;
					mfToSrcB.srcAccess = rhi::Access::TransferWrite;
					mfToSrcB.dstStage = rhi::PipelineStage::Transfer;
					mfToSrcB.dstAccess = rhi::Access::TransferRead;
					mfCmdB.Barrier({ &mfToDstB, 1 }, {});
					const float mfBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
					mfCmdB.ClearColor(*mfImageB, rhi::ImageSubresource{ 0, 0 }, mfBlue,
						rhi::ImageLayout::TransferDst);
					mfCmdB.Barrier({ &mfToSrcB, 1 }, {});
					mfCmdB.CopyImageToBuffer(*mfImageB, *mfReadB, rhi::ImageLayout::TransferSrc);
					rhi::SubmitInfo mfSubmitB;
					mfSubmitB.commandLists = { &mfCmdB, 1 };
					device->Submit(mfSubmitB);

					device->WaitAllFrames();

					mfReadA->Invalidate();
					mfReadB->Invalidate();
					const unsigned char* pxA = static_cast<const unsigned char*>(mfReadA->Map());
					const unsigned char* pxB = static_cast<const unsigned char*>(mfReadB->Map());
					const bool mfAOk = pxA && pxA[0] == 255 && pxA[1] == 0 && pxA[2] == 0;
					const bool mfBOk = pxB && pxB[0] == 0 && pxB[1] == 0 && pxB[2] == 255;
					cout << "Multi-frame in flight " << ((mfAOk && mfBOk) ? "ok" : "FAILED")
					     << ": slots=" << mfSlotA << "," << mfSlotB
					     << " (no intermediate wait)\n";
				}
			}

			{
				RenderGraph featureGraph(device.get());

				constexpr uint32_t kSrcSize = 64;
				constexpr uint32_t kSmallSize = 32;

				featureGraph.RegisterPass("FillHost", {}, { "Color", "Args" }, Pass::Host, HostPassDesc{});

				ComputePassDesc blurDesc;
				{
					ShaderBinding colorBinding;
					colorBinding.resourceName = "Color";
					ShaderBinding blurredBinding;
					blurredBinding.resourceName = "Blurred";
					blurredBinding.usage = Usage::StorageWrite;
					blurDesc.shaderBindings = { colorBinding, blurredBinding };
					blurDesc.shader = { rhi::ShaderStage::Compute, shaderDir + "/blur.comp.spv" };
					blurDesc.indirectBuffer = "Args";
				}
				featureGraph.RegisterPass("Blur", { "Color", "Args" }, { "Blurred" }, Pass::Compute, blurDesc);

				TransferPassDesc blitDesc;
				blitDesc.kind = TransferKind::ImageToImage;
				blitDesc.sourceName = "Blurred";
				blitDesc.destName = "Small";
				blitDesc.blit = true;
				blitDesc.linearFilter = false;
				featureGraph.RegisterPass("Blit", { "Blurred" }, { "Small" }, Pass::Transfer, blitDesc);

				TransferPassDesc copyDesc;
				copyDesc.kind = TransferKind::ImageToImage;
				copyDesc.sourceName = "Small";
				copyDesc.destName = "Copied";
				copyDesc.blit = false;
				featureGraph.RegisterPass("Copy", { "Small" }, { "Copied" }, Pass::Transfer, copyDesc);

				featureGraph.RegisterPass("VerifyHost", { "Copied" }, {}, Pass::Host, HostPassDesc{});

				ImageDesc colorDesc;
				colorDesc.width = kSrcSize; colorDesc.height = kSrcSize;
				colorDesc.format = ImageFormat::RGBA16F;
				featureGraph.RegisterResource("Color", { "Blur" }, "FillHost", Resource::Image, colorDesc);

				ImageDesc blurredDesc;
				blurredDesc.width = kSrcSize; blurredDesc.height = kSrcSize;
				blurredDesc.format = ImageFormat::RGBA16F;
				featureGraph.RegisterResource("Blurred", { "Blit" }, "Blur", Resource::Image, blurredDesc);

				ImageDesc smallDesc;
				smallDesc.width = kSmallSize; smallDesc.height = kSmallSize;
				smallDesc.format = ImageFormat::RGBA16F;
				featureGraph.RegisterResource("Small", { "Copy" }, "Blit", Resource::Image, smallDesc);

				ImageDesc copiedDesc;
				copiedDesc.width = kSmallSize; copiedDesc.height = kSmallSize;
				copiedDesc.format = ImageFormat::RGBA16F;
				featureGraph.RegisterResource("Copied", { "VerifyHost" }, "Copy", Resource::Image, copiedDesc);

				BufferDesc argsDesc;
				argsDesc.size = 3 * sizeof(uint32_t);
				argsDesc.usage = BufferUsage::Indirect;
				argsDesc.hostVisible = true;
				featureGraph.RegisterResource("Args", { "Blur" }, "FillHost", Resource::Buffer, argsDesc);

				featureGraph.ProcessRegister();
				featureGraph.GenerateSubmitSequence();
				featureGraph.RealizeAll();

				HostPass* fillHost = nullptr;
				HostPass* verifyHost = nullptr;
				for (uint32_t i = 0; featureGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = featureGraph.GetPass(i);
					if (p->get_type() != Pass::Host) continue;
					if (p->get_name() == "FillHost") fillHost = static_cast<HostPass*>(p);
					else if (p->get_name() == "VerifyHost") verifyHost = static_cast<HostPass*>(p);
				}

				bool featureOk = false;
				if (fillHost != nullptr && verifyHost != nullptr) {
					fillHost->SetCallback([](HostContext& ctx) {
						ctx.WaitForGpu();
						if (void* color = ctx.WriteImage("Color")) {
							const uint64_t count = uint64_t(kSrcSize) * kSrcSize;
							struct Half4 { uint16_t r, g, b, a; };
							Half4* px = static_cast<Half4*>(color);
							for (uint64_t i = 0; i < count; ++i)
								px[i] = Half4{ 0x3400u, 0x3800u, 0x3C00u, 0x3C00u };
						}
						if (rhi::Buffer* args = ctx.WriteBuffer("Args")) {
							uint32_t* cmd = static_cast<uint32_t*>(args->Map());
							cmd[0] = kSrcSize / 8;
							cmd[1] = kSrcSize / 8;
							cmd[2] = 1;
						}
						ctx.SignalGpu();
					});
					verifyHost->SetCallback([&featureOk](HostContext& ctx) {
						ctx.WaitForGpu();
						const unsigned short* px = static_cast<const unsigned short*>(ctx.ReadImage("Copied"));
						if (px != nullptr) {
							featureOk = px[0] == 0x3400u && px[1] == 0x3800u && px[2] == 0x3C00u && px[3] == 0x3C00u;
						}
						ctx.SignalGpu();
					});

					featureGraph.SubmitGraph();
					featureGraph.WaitHostPasses();
					device->WaitForFrame();

					const RenderGraph::SubmitPlan& plan = featureGraph.GetSubmitPlan();
					cout << "Host image / ImageToImage / DispatchIndirect " << (featureOk ? "ok" : "FAILED")
					     << ": WriteImage -> Blur(indirect " << kSrcSize / 8 << "x" << kSrcSize / 8
					     << ") -> Blit(" << kSrcSize << "->" << kSmallSize << ") -> Copy -> ReadImage"
					     << " (" << plan.batches.size() << " batches)\n";
				}
				else {
					cerr << "特性冒烟失败: Host 节点未就绪\n";
				}
			}

			{
				RenderGraph aliasGraph(device.get());

				BufferDesc aliasSrcDesc;
				aliasSrcDesc.size = 16;
				aliasSrcDesc.usage = rhi::BufferUsage::TransferSrc;
				aliasSrcDesc.hostVisible = true;

				ImageDesc aliasImageDesc;
				aliasImageDesc.width = 64; aliasImageDesc.height = 64;
				aliasImageDesc.format = ImageFormat::RGBA8;

				TransferPassDesc toA;
				toA.kind = TransferKind::BufferToImage;
				toA.sourceName = "SrcA";
				toA.destName = "AliasA";
				aliasGraph.RegisterPass("UseA", { "SrcA" }, { "AliasA" }, Pass::Transfer,
					toA, {}, rhi::QueueClass::Transfer);

				TransferPassDesc toB = toA;
				toB.sourceName = "SrcB";
				toB.destName = "AliasB";
				aliasGraph.RegisterPass("UseB", { "SrcB" }, { "AliasB" }, Pass::Transfer,
					toB, {}, rhi::QueueClass::AsyncCompute);

				aliasGraph.RegisterResource("SrcA", { "UseA" }, "", Resource::Buffer, aliasSrcDesc);
				aliasGraph.RegisterResource("AliasA", {}, "UseA", Resource::Image, aliasImageDesc);
				aliasGraph.RegisterResource("SrcB", { "UseB" }, "", Resource::Buffer, aliasSrcDesc);
				aliasGraph.RegisterResource("AliasB", {}, "UseB", Resource::Image, aliasImageDesc);

				aliasGraph.ProcessRegister();
				aliasGraph.GenerateSubmitSequence();
				aliasGraph.AliasResources({ "AliasA", "AliasB" });
				aliasGraph.RealizeAll();

				const rhi::Image* aliasA = nullptr;
				const rhi::Image* aliasB = nullptr;
				for (uint32_t i = 0; aliasGraph.GetResource(i) != nullptr; ++i) {
					Resource* res = aliasGraph.GetResource(i);
					if (res->get_name() == "AliasA") aliasA = static_cast<Image*>(res)->get_rhi();
					else if (res->get_name() == "AliasB") aliasB = static_cast<Image*>(res)->get_rhi();
				}
				cout << "Alias shared handle: "
					<< (aliasA != nullptr && aliasA == aliasB ? "ok" : "FAILED")
					<< " (" << aliasGraph.GetSubmitPlan().batches.size() << " batches)\n";
			}
			{
				RenderGraph rejectGraph(device.get());

				BufferDesc srcDesc;
				srcDesc.size = 16;
				srcDesc.usage = rhi::BufferUsage::TransferSrc;
				srcDesc.hostVisible = true;

				ImageDesc smallDesc;
				smallDesc.width = 32; smallDesc.height = 32; smallDesc.format = ImageFormat::RGBA8;
				ImageDesc largeDesc;
				largeDesc.width = 64; largeDesc.height = 64; largeDesc.format = ImageFormat::RGBA8;

				TransferPassDesc toSmall;
				toSmall.kind = TransferKind::BufferToImage;
				toSmall.sourceName = "SrcSmall";
				toSmall.destName = "Small";
				rejectGraph.RegisterPass("UseSmall", { "SrcSmall" }, { "Small" }, Pass::Transfer, toSmall);

				TransferPassDesc toLarge = toSmall;
				toLarge.sourceName = "SrcLarge";
				toLarge.destName = "Large";
				rejectGraph.RegisterPass("UseLarge", { "SrcLarge" }, { "Large" }, Pass::Transfer, toLarge);

				rejectGraph.RegisterResource("SrcSmall", { "UseSmall" }, "", Resource::Buffer, srcDesc);
				rejectGraph.RegisterResource("Small", {}, "UseSmall", Resource::Image, smallDesc);
				rejectGraph.RegisterResource("SrcLarge", { "UseLarge" }, "", Resource::Buffer, srcDesc);
				rejectGraph.RegisterResource("Large", {}, "UseLarge", Resource::Image, largeDesc);

				rejectGraph.ProcessRegister();
				rejectGraph.GenerateSubmitSequence();
				rejectGraph.AliasResources({ "Small", "Large" });
				rejectGraph.RealizeAll();

				const rhi::Image* smallImg = nullptr;
				const rhi::Image* largeImg = nullptr;
				for (uint32_t i = 0; rejectGraph.GetResource(i) != nullptr; ++i) {
					Resource* res = rejectGraph.GetResource(i);
					if (res->get_name() == "Small") smallImg = static_cast<Image*>(res)->get_rhi();
					else if (res->get_name() == "Large") largeImg = static_cast<Image*>(res)->get_rhi();
				}
				cout << "Alias degraded handles: "
					<< (smallImg != nullptr && largeImg != nullptr && smallImg != largeImg
						? "ok (independent)" : "FAILED") << "\n";
			}

			{
				RenderGraph persistGraph(device.get());

				BufferDesc stagingDesc;
				stagingDesc.size = 64 * 64 * 4;
				stagingDesc.usage = rhi::BufferUsage::TransferSrc;
				stagingDesc.hostVisible = true;
				stagingDesc.crossFrame = true;

				ImageDesc persistDesc;
				persistDesc.width = 64; persistDesc.height = 64;
				persistDesc.format = ImageFormat::RGBA8;
				persistDesc.crossFrame = true;

				persistGraph.RegisterPass("Fill", {}, { "Staging" }, Pass::Host, HostPassDesc{});
				TransferPassDesc upload;
				upload.kind = TransferKind::BufferToImage;
				upload.sourceName = "Staging";
				upload.destName = "Persist";
				persistGraph.RegisterPass("Upload", { "Staging" }, { "Persist" }, Pass::Transfer,
					upload, {}, rhi::QueueClass::Transfer);

				persistGraph.RegisterResource("Staging", { "Upload" }, "Fill", Resource::Buffer, stagingDesc);
				persistGraph.RegisterResource("Persist", {}, "Upload", Resource::Image, persistDesc);

				persistGraph.ProcessRegister();
				persistGraph.GenerateSubmitSequence();
				persistGraph.RealizeAll();

				for (uint32_t i = 0; persistGraph.GetPass(i) != nullptr; ++i) {
					Pass* p = persistGraph.GetPass(i);
					if (p->get_type() == Pass::Host) {
						static_cast<HostPass*>(p)->SetCallback([](HostContext& ctx) {
							ctx.WaitForGpu();
							rhi::Buffer* staging = ctx.WriteBuffer("Staging");
							if (staging != nullptr && staging->Map() != nullptr)
								std::memset(staging->Map(), 0x3C, 64 * 64 * 4);
							ctx.SignalGpu();
						});
					}
				}

				persistGraph.SubmitGraph();
				persistGraph.WaitHostPasses();
				device->WaitForFrame();
				persistGraph.SubmitGraph();
				persistGraph.WaitHostPasses();
				device->WaitForFrame();
				cout << "Cross-frame resubmit ok: 2 frames\n";
			}

			if (graphOut) {
				std::optional<RenderGraph> frameGraph;

				uint32_t presented = 0;
				for (uint32_t attempts = 0; attempts < 8 && presented < 3; ++attempts) {
					rhi::Swapchain::Frame frame = swapchain2->AcquireNextImage();
					if (!frame.acquired) continue;

					if (!frameGraph) {
						frameGraph.emplace(device.get());

						RenderPassDesc compositeDesc;
						{
							AttachmentBinding att;
							att.resourceName = "Backbuffer";
							att.slot = 0;
							att.load = LoadOp::Clear;
							att.clear.value = std::array<float, 4>{ 0.10f, 0.20f, 0.35f, 1.0f };
							compositeDesc.attachments.push_back(att);

							ShaderBinding sampled;
							sampled.resourceName = "Blurred";
							compositeDesc.shaderBindings.push_back(sampled);

							compositeDesc.pipeline.shaders = {
								{ rhi::ShaderStage::Vertex,   shaderDir + "/fullscreen.vert.spv" },
								{ rhi::ShaderStage::Fragment, shaderDir + "/textured.frag.spv" },
							};
							compositeDesc.pipeline.cullMode = CullMode::None;
						}

						frameGraph->RegisterPass("Composite", { "Blurred" }, { "Backbuffer" }, Pass::Render, compositeDesc);
						frameGraph->RegisterPass("Present", { "Backbuffer" }, {}, Pass::Present, PresentPassDesc{});

						frameGraph->RegisterResource("Backbuffer", { "Present" }, "Composite",
							frame.backbuffer, {}, rhi::ImageLayout::Undefined);
						frameGraph->RegisterResource("Blurred", { "Composite" }, "",
							graphOut.get(), {}, rhi::ImageLayout::ShaderReadOnly);

						frameGraph->ProcessRegister();
						frameGraph->GenerateSubmitSequence();
						frameGraph->RealizeAll();

						for (uint32_t i = 0; frameGraph->GetPass(i) != nullptr; ++i) {
							Pass* p = frameGraph->GetPass(i);
							if (p->get_type() != Pass::Render) continue;
							DrawSource draw;
							draw.kind = DrawSource::Direct;
							draw.count = 3;
							draw.instanceCount = 1;
							static_cast<RenderPass*>(p)->SetDraw(draw);
						}
					}
					else {
						frameGraph->RetargetBorrowedImage("Backbuffer", frame.backbuffer);
					}

					frameGraph->SubmitGraph(RenderGraph::FrameSync{ frame.ready, frame.presentReady });
					frameGraph->WaitHostPasses();
					swapchain2->Present();
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
