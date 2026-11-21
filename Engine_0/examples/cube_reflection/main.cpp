#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "RenderGraphAbstracted.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphNodes.h"
#include "ContentResolver.h"
#include "ShaderLibrary.h"
#include "rhi/RHI.h"
#include "MeshDrawData.h"
#include "CubeMesh.h"

#include "stb_image/stb_image.h"
#include "stb_image/stb_image_write.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#undef CreateSemaphore

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef CUBE_REFLECTION_SHADER_DIR
#define CUBE_REFLECTION_SHADER_DIR "cube_reflection_shaders"
#endif
#ifndef CUBE_REFLECTION_ASSET_DIR
#define CUBE_REFLECTION_ASSET_DIR "../../assets"
#endif

using namespace dEngine;
using namespace std;

namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr float    kFovY = 45.0f;

struct Options {
	bool     smoke = false;
	bool     limitFrames = false;
	uint32_t frames = 30;
};

struct SceneUniforms {
	glm::mat4 viewProj;
	glm::mat4 model;
	glm::vec4 camPosition;
	glm::vec4 camRight;
	glm::vec4 camUp;
	glm::vec4 camForward;
	glm::vec4 resolution; // x,y = 像素尺寸；z = tan(fov/2)
};
static_assert(sizeof(SceneUniforms) == 208, "SceneUniforms 必须与 GLSL std140 布局一致");

// 用一次性 Host + Transfer 通道把全景图上传为 ShaderReadOnly 采样纹理
unique_ptr<rhi::Image> UploadEnvironment(rhi::Device& device, const string& path) {
	int texW = 0, texH = 0, texChannels = 0;
	stbi_uc* pixels = stbi_load(path.c_str(), &texW, &texH, &texChannels, STBI_rgb_alpha);
	if (pixels == nullptr) {
		cerr << "环境贴图加载失败: " << path << "\n";
		return nullptr;
	}
	const uint64_t bytes = static_cast<uint64_t>(texW) * static_cast<uint64_t>(texH) * 4ull;

	rhi::ImageCreateInfo envInfo;
	envInfo.width = static_cast<uint32_t>(texW);
	envInfo.height = static_cast<uint32_t>(texH);
	envInfo.format = rhi::ImageFormat::RGBA8;
	envInfo.usage = rhi::ImageUsage::Sampled;
	auto env = device.UploadImage(pixels, bytes, envInfo);
	stbi_image_free(pixels);
	if (env == nullptr)
		return nullptr;

	cout << "Environment texture uploaded: " << path << " " << texW << "x" << texH
	     << " ch=" << texChannels << "\n";
	return env;
}

bool BuildFrameGraph(RenderGraph& graph, const Mesh& cubeMesh,
	const vector<MeshShaderInput>& cubeInputs, const MeshDrawData& cubeDraw,
	const rhi::Buffer* vertexBuffer, const rhi::Buffer* indexBuffer,
	const rhi::Image* envTexture, const rhi::Image* backbuffer, bool smoke, bool useHeap) {
	RenderGraphBuilder builder;

	BufferDesc uboDesc;
	uboDesc.size = sizeof(SceneUniforms);
	uboDesc.usage = rhi::BufferUsage::Uniform;
	uboDesc.hostVisible = true;
	builder.DeclareBufferResource("SceneUniforms", uboDesc);

	builder.DeclareBufferResource("CubeVB", vertexBuffer);
	if (cubeDraw.indexType) builder.DeclareBufferResource("CubeIB", indexBuffer);

	ImageOptions envOptions;
	envOptions.initialLayout = rhi::ImageLayout::ShaderReadOnly;
	envOptions.crossFrame = true;
	builder.DeclareImageResource("EnvTexture", envTexture, envOptions);

	ImageDesc sceneDesc;
	sceneDesc.width = kWidth;
	sceneDesc.height = kHeight;
	sceneDesc.format = ImageFormat::RGBA8;
	builder.DeclareImageResource("SceneColor", sceneDesc);

	ImageDesc depthDesc;
	depthDesc.width = kWidth;
	depthDesc.height = kHeight;
	depthDesc.format = ImageFormat::D32F;
	builder.DeclareImageResource("Depth", depthDesc);

	if (smoke) {
		ImageDesc backDesc;
		backDesc.width = kWidth;
		backDesc.height = kHeight;
		backDesc.format = ImageFormat::RGBA8;
		builder.DeclareImageResource("Backbuffer", backDesc);
	}
	else {
		ImageOptions backOptions;
		backOptions.initialLayout = rhi::ImageLayout::Undefined;
		builder.DeclareImageResource("Backbuffer", backbuffer, backOptions);
	}

	builder.DeclarePass("UpdateScene", {}, { "SceneUniforms" }, Pass::Host, HostPassDesc{});

	rhi::SamplerDesc envSampler = rhi::SamplerDesc::Linear(rhi::SamplerAddressMode::Repeat);
	envSampler.addressV = rhi::SamplerAddressMode::ClampToEdge;
	envSampler.addressW = rhi::SamplerAddressMode::ClampToEdge;

	// 环境贴图进入静态纹理堆（slot 0），Composite 通过绑定数组采样
	if (useHeap && envTexture != nullptr)
		builder.DeclareStaticTexture("EnvTexture", envTexture, {},
			rhi::ImageFormat::None, envSampler, "skyview");

	RenderPassDesc cubeDesc;
	{
		AttachmentBinding color;
		color.resourceName = "SceneColor";
		color.slot = 0;
		color.load = LoadOp::Clear;
		color.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f };
		cubeDesc.attachments.push_back(color);

		AttachmentBinding depth;
		depth.resourceName = "Depth";
		depth.usage = Usage::DepthStencilAttachment;
		depth.load = LoadOp::Clear;
		depth.clear.value = DepthStencilValue{ 1.0f, 0 };
		depth.store = StoreOp::DontCare;
		cubeDesc.attachments.push_back(depth);

		cubeDesc.pipeline.shaderRefs = { "cube.vert", "cube.frag" };
		cubeDesc.pipeline.vertexBindings = cubeDraw.vertexBindings;
		cubeDesc.pipeline.vertexAttributes = cubeDraw.vertexAttributes;
		cubeDesc.pipeline.topology = cubeDraw.topology;
		cubeDesc.pipeline.tessPatchControlPoints = cubeDraw.patchControlPoints;
		cubeDesc.pipeline.cullMode = CullMode::None;
		cubeDesc.pipeline.depthStencil.depthTestEnable = true;
		cubeDesc.pipeline.depthStencil.depthWriteEnable = true;
		cubeDesc.pipeline.depthStencil.depthCompareOp = CompareOp::LessEqual;

		string pipelineError;
		if (!ValidateMeshPipeline(cubeMesh, 0, cubeInputs, cubeDesc.pipeline, &pipelineError)) {
			cerr << pipelineError << '\n';
			return false;
		}
		cout << "Mesh pipeline compatibility=PASS\n";

		cubeDesc.shaderBindings = {
			ShaderBinding::Uniform("SceneUniforms"),
			ShaderBinding::Sampled("EnvTexture", envSampler) };
		cubeDesc.vertexStreams.push_back(VertexStreamBinding{ "CubeVB", {}, 0 });
		if (cubeDraw.indexType)
			cubeDesc.indexStream = IndexStreamBinding{ "CubeIB", {}, 0, *cubeDraw.indexType };
	}
	// 顶点和索引缓冲的读依赖由绑定描述导出。
	builder.DeclarePass("Cube", Pass::Render, cubeDesc);

	RenderPassDesc compositeDesc;
	{
		AttachmentBinding color;
		color.resourceName = "Backbuffer";
		color.slot = 0;
		color.load = LoadOp::DontCare;
		color.store = StoreOp::Store;
		compositeDesc.attachments.push_back(color);

		compositeDesc.pipeline.shaderRefs = useHeap
			? std::vector<std::string>{ "fullscreen.vert", "composite_heap.frag" }
			: std::vector<std::string>{ "fullscreen.vert", "composite.frag" };
		compositeDesc.pipeline.cullMode = CullMode::None;
		compositeDesc.pipeline.depthStencil.depthTestEnable = false;
		compositeDesc.bindStaticHeap = useHeap;

		compositeDesc.shaderBindings = {
			ShaderBinding::Uniform("SceneUniforms"),
			ShaderBinding::Sampled("SceneColor", rhi::SamplerDesc::Nearest()) };
		if (!useHeap)
			compositeDesc.shaderBindings.push_back(ShaderBinding::Sampled("EnvTexture", envSampler));
	}
	// 走静态堆时，EnvTexture 的访问不在图的拓扑里（堆状态由声明保证 ShaderReadOnly），
	// 不能把它留在读集中，否则 Usage::Auto 会把它转到 General，与堆描述符布局冲突。
	std::set<std::string> compositeReads = { "SceneUniforms", "SceneColor" };
	if (!useHeap)
		compositeReads.insert("EnvTexture");
	builder.DeclarePass("Composite", compositeReads,
		{ "Backbuffer" }, Pass::Render, compositeDesc);

	if (smoke)
		builder.DeclarePass("Capture", { "Backbuffer", "SceneColor" }, {}, Pass::Host, HostPassDesc{});
	else
		builder.DeclarePass("Present", { "Backbuffer" }, {}, Pass::Present, PresentPassDesc{});

	LowerResult lower = builder.Lower();
	if (!lower.ok) {
		for (const auto& diagnostic : lower.diagnostics)
			cerr << "RenderGraphBuilder: " << diagnostic.message << "\n";
		return false;
	}
	builder.Build(graph);
	graph.ProcessRegister();
	graph.GenerateSubmitSequence();
	graph.RealizeAll();

	if (useHeap && envTexture != nullptr) {
		// 重复注册同名条目会返回已有 slot，用于确认 Build 已完成堆注册
		const uint32_t envSlot = graph.RegisterStaticTexture("EnvTexture", *envTexture);
		cout << "Static texture heap ok: slot=" << envSlot << "\n";
	}

	for (uint32_t i = 0; graph.GetPass(i) != nullptr; ++i) {
		Pass* p = graph.GetPass(i);
		if (p->get_type() != Pass::Render)
			continue;
		DrawSource draw;
		draw.kind = DrawSource::Direct;
		if (p->get_name() == "Cube") {
			draw = cubeDraw.draw;
		}
		else {
			draw.count = 3;
			draw.instanceCount = 1;
		}
		static_cast<RenderPass*>(p)->SetDraw(draw);
	}
	return true;
}

} // namespace

int main(int argc, char** argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--smoke") == 0) {
			options.smoke = true;
			options.limitFrames = true;
			if (i + 1 < argc) {
				const int n = std::atoi(argv[i + 1]);
				if (n > 0) {
					options.frames = static_cast<uint32_t>(n);
					++i;
				}
			}
		}
		else if (std::strcmp(argv[i], "--frames") == 0) {
			if (i + 1 >= argc || std::atoi(argv[i + 1]) <= 0) {
				cerr << "--frames 需要正整数\n";
				return 2;
			}
			options.frames = static_cast<uint32_t>(std::atoi(argv[++i]));
			options.limitFrames = true;
		}
	}

	Mesh cubeMesh = examples::CreateCubeMesh();
	MeshDrawData cubeDraw;
	string meshError;
	const vector<MeshShaderInput> cubeInputs = {
		{VertexSemantic::Position, 0, 0, rhi::VertexFormat::Float3},
		{VertexSemantic::Normal, 0, 1, rhi::VertexFormat::Float3}
	};
	if (!cubeMesh.RecalculateBounds(&meshError)
		|| !BuildMeshDrawData(cubeMesh, 0, cubeInputs, cubeDraw, &meshError)) {
		cerr << meshError << '\n';
		return 1;
	}
	cout << "Mesh: " << cubeMesh.name << " vertices=" << cubeMesh.vertexCount
	     << " indices=" << cubeMesh.indices->IndexCount() << " submeshes=" << cubeMesh.submeshes.size()
	     << " sourceBinding=" << cubeDraw.sourceBindings[0] << " -> gpuBinding=0"
	     << " draw.count=" << cubeDraw.draw.count << " Validate=PASS\n";

	const string shaderDir = CUBE_REFLECTION_SHADER_DIR;
	const string assetDir = CUBE_REFLECTION_ASSET_DIR;

	GLFWwindow* window = nullptr;
	bool glfwInitialized = false;
	if (!options.smoke) {
		if (!glfwInit()) {
			cerr << "glfwInit 失败\n";
			return 1;
		}
		glfwInitialized = true;
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
		const string title = "Engine_0 Mesh: " + cubeMesh.name + " | " + to_string(cubeMesh.vertexCount)
			+ " vertices | " + to_string(cubeMesh.indices->IndexCount()) + " indices";
		window = glfwCreateWindow(kWidth, kHeight, title.c_str(), nullptr, nullptr);
		if (window == nullptr) {
			cerr << "窗口创建失败\n";
			glfwTerminate();
			return 1;
		}
	}

	rhi::DeviceDesc deviceDesc;
	deviceDesc.enableValidation = true;
	deviceDesc.maxFramesInFlight = 2;
	if (std::getenv("ENGINE_FORCE_FIXED_HEAP") != nullptr)
		deviceDesc.forceFixedArrayHeap = true;
	if (window != nullptr) {
		deviceDesc.nativeWindowHandle = glfwGetWin32Window(window);
		deviceDesc.width = kWidth;
		deviceDesc.height = kHeight;
	}

	auto device = rhi::Device::Create(deviceDesc);
	if (!device) {
		cerr << "Vulkan 设备创建失败\n";
		if (window) glfwDestroyWindow(window);
		if (glfwInitialized) glfwTerminate();
		return 1;
	}
	cout << "Device: " << device->DeviceName() << "\n";
	cout << "Static heap tier: "
	     << (device->GetStaticHeapTier() == rhi::StaticHeapTier::DescriptorIndexing
	         ? "bindless" : "fixed-array") << "\n";
	const bool useHeap = device->GetStaticHeapTier() != rhi::StaticHeapTier::None;

	ContentResolver assets;
	assets.Mount("shaders", shaderDir);
	assets.Mount("assets", assetDir);
	ShaderLibrary shaders(assets);
	{
		std::vector<std::string> shaderWarnings;
		if (!shaders.LoadManifest("shaders/shaders.manifest.json", &shaderWarnings)) {
			for (const auto& warning : shaderWarnings)
				cerr << "[ShaderLibrary] " << warning << "\n";
			cerr << "着色器清单加载失败\n";
			if (window) glfwDestroyWindow(window);
			if (glfwInitialized) glfwTerminate();
			return 1;
		}
	}

	auto envTexture = UploadEnvironment(*device, assetDir + "/skyview.jpg");
	if (!envTexture) {
		if (window) glfwDestroyWindow(window);
		if (glfwInitialized) glfwTerminate();
		return 1;
	}

	const auto& vertexBytes = cubeMesh.FindVertexStream(cubeDraw.sourceBindings[0])->bytes;
	auto vertexBuffer = device->UploadBuffer(vertexBytes.data(),
		static_cast<uint64_t>(vertexBytes.size()), rhi::BufferUsage::Vertex,
		rhi::PipelineStage::VertexInput, rhi::Access::VertexAttributeRead);
	if (!vertexBuffer) {
		cerr << "顶点缓冲创建失败\n";
		return 1;
	}
	auto indexBuffer = device->UploadBuffer(cubeMesh.indices->bytes.data(),
		static_cast<uint64_t>(cubeMesh.indices->bytes.size()), rhi::BufferUsage::Index,
		rhi::PipelineStage::VertexInput, rhi::Access::IndexRead);
	if (!indexBuffer) {
		cerr << "索引缓冲创建失败\n";
		return 1;
	}

	// 俯视相机：从斜上方看向原点，画面中同时可见顶面与两个侧面
	const glm::vec3 eye(0.0f, 3.3f, 2.0f);
	const glm::vec3 target(0.0f, 0.0f, 0.0f);
	const glm::vec3 worldUp(0.0f, 0.0f, 1.0f);
	const glm::vec3 forward = glm::normalize(target - eye);
	const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
	const glm::vec3 up = glm::cross(right, forward);

	glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(kFovY),
		static_cast<float>(kWidth) / static_cast<float>(kHeight), 0.1f, 100.0f);
	// Vulkan NDC y 轴朝下：翻转投影 Y，与 gl_FragCoord 的像素坐标对齐
	proj[1][1] *= -1.0f;
	const glm::mat4 view = glm::lookAt(eye, target, worldUp);
	const glm::mat4 viewProj = proj * view;

	if (window != nullptr) {
		glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int, int action, int) {
			if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
				glfwSetWindowShouldClose(w, GLFW_TRUE);
		});
	}

	optional<RenderGraph> graph;
	optional<rhi::Swapchain::Frame> currentFrame;
	auto swapchain = window ? device->CreateSwapchain() : nullptr;

	bool graphReady = false;
	bool captureRequested = false;
	bool captured = false;
	vector<unsigned char> capturedPixels;
	vector<unsigned char> capturedScenePixels;
	uint32_t renderedFrames = 0;

	using Clock = std::chrono::steady_clock;
	const auto startTime = Clock::now();

	while (true) {
		if (options.limitFrames && renderedFrames >= options.frames) break;
		if (window != nullptr) {
			if (glfwWindowShouldClose(window))
				break;
			glfwPollEvents();
			rhi::Swapchain::Frame frame = swapchain->AcquireNextImage();
			if (!frame.acquired)
				continue;
			currentFrame = frame;
		}
		else if (renderedFrames >= options.frames) {
			break;
		}

		if (!graphReady) {
			graph.emplace(device.get(), &shaders);
			if (!BuildFrameGraph(*graph, cubeMesh, cubeInputs, cubeDraw, vertexBuffer.get(), indexBuffer.get(),
				envTexture.get(), currentFrame.has_value() ? currentFrame->backbuffer : nullptr,
				options.smoke, useHeap)) {
				return 1;
			}

			if (options.smoke) {
				HostPass* capturePass = static_cast<HostPass*>(graph->FindPass("Capture"));
				capturePass->SetCallback([&](HostContext& ctx) {
					ctx.WaitForGpu();
					if (captureRequested) {
						if (void* px = ctx.ReadImage("Backbuffer")) {
							capturedPixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
							std::memcpy(capturedPixels.data(), px, capturedPixels.size());
							captured = true;
						}
						if (void* px = ctx.ReadImage("SceneColor")) {
							capturedScenePixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
							std::memcpy(capturedScenePixels.data(), px, capturedScenePixels.size());
						}
					}
					ctx.SignalGpu();
				});
			}
			graphReady = true;
		}
		else if (currentFrame.has_value()) {
			graph->RetargetBorrowedImage("Backbuffer", currentFrame->backbuffer);
		}

		const float now = options.smoke ? 0.75f : std::chrono::duration<float>(Clock::now() - startTime).count();

		glm::mat4 model = glm::rotate(glm::mat4(1.0f), now * 0.8f, glm::vec3(0.0f, 1.0f, 0.0f));
		model = glm::rotate(model, now * 0.45f, glm::vec3(1.0f, 0.0f, 0.0f));

		SceneUniforms uniforms;
		uniforms.viewProj = viewProj;
		uniforms.model = model;
		uniforms.camPosition = glm::vec4(eye, 1.0f);
		uniforms.camRight = glm::vec4(right, 0.0f);
		uniforms.camUp = glm::vec4(up, 0.0f);
		uniforms.camForward = glm::vec4(forward, 0.0f);
		uniforms.resolution = glm::vec4(static_cast<float>(kWidth), static_cast<float>(kHeight),
			std::tan(glm::radians(kFovY) * 0.5f), 0.0f);

		HostPass* update = static_cast<HostPass*>(graph->FindPass("UpdateScene"));
		update->SetCallback([uniforms](HostContext& ctx) {
			ctx.WaitForGpu();
			if (rhi::Buffer* b = ctx.WriteBuffer("SceneUniforms"))
				std::memcpy(b->Map(), &uniforms, sizeof(uniforms));
			ctx.SignalGpu();
		});

		if (options.smoke)
			captureRequested = (renderedFrames + 1 == options.frames);

		const uint32_t slot = device->BeginFrame();
		graph->BeginFrame(slot);

		if (currentFrame.has_value()) {
			graph->SubmitGraph(RenderGraph::FrameSync{
				currentFrame->ready, currentFrame->presentReady });
		}
		else {
			graph->SubmitGraph();
		}

		graph->WaitHostPasses();

		if (window != nullptr && currentFrame.has_value()) {
			swapchain->Present();
			currentFrame.reset();
		}
		device->WaitForFrame();

		++renderedFrames;
	}

	bool smokeOk = !options.smoke;
	if (options.smoke) {
		if (!captured || capturedPixels.empty() || capturedScenePixels.empty()) {
			cerr << "Cube reflection smoke FAILED: 未捕获到输出图像\n";
		}
		else {
			int minLuma = 255;
			int maxLuma = 0;
			double sumLuma = 0.0;
			for (size_t i = 0; i + 3 < capturedPixels.size(); i += 4) {
				const int luma = (int(capturedPixels[i]) + int(capturedPixels[i + 1])
					+ int(capturedPixels[i + 2])) / 3;
				minLuma = std::min(minLuma, luma);
				maxLuma = std::max(maxLuma, luma);
				sumLuma += luma;
			}
			const double meanLuma = sumLuma / (static_cast<double>(kWidth) * kHeight);
			size_t cubePixels = 0;
			for (size_t i = 3; i < capturedScenePixels.size(); i += 4)
				if (capturedScenePixels[i] > 127) ++cubePixels;

			std::filesystem::create_directories("out");
			const string outputPath = "out/cube_reflection_smoke.png";
			const bool written = stbi_write_png(outputPath.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
				4, capturedPixels.data(), static_cast<int>(kWidth) * 4) != 0;
			smokeOk = written && maxLuma > minLuma && cubePixels > 0
				&& cubePixels < static_cast<size_t>(kWidth) * kHeight;

			cout << "Smoke image: min=" << minLuma << " max=" << maxLuma
			     << " mean=" << meanLuma << "\n";
			cout << "Mesh coverage pixels=" << cubePixels << '/' << kWidth * kHeight << '\n';
			cout << "Cube reflection smoke " << (smokeOk ? "PASS" : "FAIL") << ": " << outputPath << "\n";
		}
	}

	if (window != nullptr) {
		device->WaitIdle();
		if (swapchain) swapchain.reset();
		glfwDestroyWindow(window);
		window = nullptr;
	}
	device->WaitIdle();
	if (glfwInitialized) glfwTerminate();
	cout << "Rendered frames=" << renderedFrames << '\n';
	return smokeOk ? 0 : 1;
}
