#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "RenderGraphAbstracted.h"
#include "RenderGraphNodes.h"
#include "AssetResolver.h"
#include "ShaderLibrary.h"
#include "rhi/RHI.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
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
constexpr float    kCubeHalfSize = 1.0f;

struct Options {
	bool     smoke = false;
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

struct CubeVertex {
	glm::vec3 position;
	glm::vec3 normal;
};

// 单位立方体：每面 4 个顶点，法线逐面显式给出
void BuildCube(vector<CubeVertex>& vertices, vector<uint32_t>& indices) {
	struct Face {
		glm::vec3 normal;
		glm::vec3 u;
		glm::vec3 v;
	};
	const Face faces[6] = {
		{ { 1, 0, 0}, { 0, 0,-1}, { 0, 1, 0} },
		{ {-1, 0, 0}, { 0, 0, 1}, { 0, 1, 0} },
		{ { 0, 1, 0}, { 1, 0, 0}, { 0, 0,-1} },
		{ { 0,-1, 0}, { 1, 0, 0}, { 0, 0, 1} },
		{ { 0, 0, 1}, { 1, 0, 0}, { 0, 1, 0} },
		{ { 0, 0,-1}, {-1, 0, 0}, { 0, 1, 0} },
	};
	vertices.clear();
	indices.clear();
	const float s = kCubeHalfSize;
	for (int f = 0; f < 6; ++f) {
		const glm::vec3 base = faces[f].normal * s;
		const glm::vec3 u = faces[f].u * s;
		const glm::vec3 v = faces[f].v * s;
		const glm::vec3 corners[4] = {
			base - u - v, base + u - v, base + u + v, base - u + v
		};
		const uint32_t first = static_cast<uint32_t>(vertices.size());
		for (int c = 0; c < 4; ++c)
			vertices.push_back(CubeVertex{ corners[c], faces[f].normal });
		indices.push_back(first + 0);
		indices.push_back(first + 1);
		indices.push_back(first + 2);
		indices.push_back(first + 0);
		indices.push_back(first + 2);
		indices.push_back(first + 3);
	}
}

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

bool BuildFrameGraph(RenderGraph& graph,
	const rhi::Buffer* vertexBuffer, const rhi::Buffer* indexBuffer,
	const rhi::Image* envTexture, const rhi::Image* backbuffer, bool smoke) {
	BufferDesc uboDesc;
	uboDesc.size = sizeof(SceneUniforms);
	uboDesc.usage = rhi::BufferUsage::Uniform;
	uboDesc.hostVisible = true;
	graph.RegisterResource("SceneUniforms", { "Cube", "Composite" }, "UpdateScene",
		Resource::Buffer, uboDesc);

	graph.RegisterResource("CubeVB", { "Cube" }, "", vertexBuffer);
	graph.RegisterResource("CubeIB", { "Cube" }, "", indexBuffer);

	graph.RegisterResource("EnvTexture", { "Cube", "Composite" }, "", envTexture, {},
		rhi::ImageLayout::ShaderReadOnly, rhi::ImageFormat::None,
		rhi::ImageSubresourceRange{}, true);

	ImageDesc sceneDesc;
	sceneDesc.width = kWidth;
	sceneDesc.height = kHeight;
	sceneDesc.format = ImageFormat::RGBA8;
	graph.RegisterResource("SceneColor", { "Composite" }, "Cube", Resource::Image, sceneDesc);

	ImageDesc depthDesc;
	depthDesc.width = kWidth;
	depthDesc.height = kHeight;
	depthDesc.format = ImageFormat::D32F;
	graph.RegisterResource("Depth", {}, "Cube", Resource::Image, depthDesc);

	if (smoke) {
		ImageDesc backDesc;
		backDesc.width = kWidth;
		backDesc.height = kHeight;
		backDesc.format = ImageFormat::RGBA8;
		graph.RegisterResource("Backbuffer", { "Capture" }, "Composite", Resource::Image, backDesc);
	}
	else {
		graph.RegisterResource("Backbuffer", { "Present" }, "Composite", backbuffer, {},
			rhi::ImageLayout::Undefined);
	}

	graph.RegisterPass("UpdateScene", {}, { "SceneUniforms" }, Pass::Host, HostPassDesc{});

	rhi::SamplerDesc envSampler = rhi::SamplerDesc::Linear(rhi::SamplerAddressMode::Repeat);
	envSampler.addressV = rhi::SamplerAddressMode::ClampToEdge;
	envSampler.addressW = rhi::SamplerAddressMode::ClampToEdge;

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
		cubeDesc.pipeline.cullMode = CullMode::None;
		cubeDesc.pipeline.depthStencil.depthTestEnable = true;
		cubeDesc.pipeline.depthStencil.depthWriteEnable = true;
		cubeDesc.pipeline.depthStencil.depthCompareOp = CompareOp::LessEqual;

		cubeDesc.shaderBindings = {
			ShaderBinding::Uniform("SceneUniforms"),
			ShaderBinding::Sampled("EnvTexture", envSampler) };
		cubeDesc.vertexStreams.push_back(VertexStreamBinding{ "CubeVB", {}, 0 });
		cubeDesc.indexStream = IndexStreamBinding{ "CubeIB", {}, 0, IndexType::UInt32 };
	}
	graph.RegisterPass("Cube", { "SceneUniforms", "EnvTexture", "CubeVB", "CubeIB" },
		{ "SceneColor", "Depth" }, Pass::Render, cubeDesc);

	RenderPassDesc compositeDesc;
	{
		AttachmentBinding color;
		color.resourceName = "Backbuffer";
		color.slot = 0;
		color.load = LoadOp::DontCare;
		color.store = StoreOp::Store;
		compositeDesc.attachments.push_back(color);

		compositeDesc.pipeline.shaderRefs = { "fullscreen.vert", "composite.frag" };
		compositeDesc.pipeline.cullMode = CullMode::None;
		compositeDesc.pipeline.depthStencil.depthTestEnable = false;

		compositeDesc.shaderBindings = {
			ShaderBinding::Uniform("SceneUniforms"),
			ShaderBinding::Sampled("SceneColor", rhi::SamplerDesc::Nearest()),
			ShaderBinding::Sampled("EnvTexture", envSampler) };
	}
	graph.RegisterPass("Composite", { "SceneUniforms", "SceneColor", "EnvTexture" },
		{ "Backbuffer" }, Pass::Render, compositeDesc);

	if (smoke)
		graph.RegisterPass("Capture", { "Backbuffer" }, {}, Pass::Host, HostPassDesc{});
	else
		graph.RegisterPass("Present", { "Backbuffer" }, {}, Pass::Present, PresentPassDesc{});

	graph.ProcessRegister();
	graph.GenerateSubmitSequence();
	graph.RealizeAll();

	for (uint32_t i = 0; graph.GetPass(i) != nullptr; ++i) {
		Pass* p = graph.GetPass(i);
		if (p->get_type() != Pass::Render)
			continue;
		DrawSource draw;
		draw.kind = DrawSource::Direct;
		if (p->get_name() == "Cube") {
			draw.count = 36;
			draw.first = 0;
			draw.vertexOffset = 0;
			draw.instanceCount = 1;
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
			if (i + 1 < argc) {
				const int n = std::atoi(argv[i + 1]);
				if (n > 0) {
					options.frames = static_cast<uint32_t>(n);
					++i;
				}
			}
		}
	}

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
		window = glfwCreateWindow(kWidth, kHeight, "Engine_0 cube reflection", nullptr, nullptr);
		if (window == nullptr) {
			cerr << "窗口创建失败\n";
			glfwTerminate();
			return 1;
		}
	}

	rhi::DeviceDesc deviceDesc;
	deviceDesc.enableValidation = true;
	deviceDesc.maxFramesInFlight = 2;
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

	AssetResolver assets;
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

	vector<CubeVertex> vertices;
	vector<uint32_t> indices;
	BuildCube(vertices, indices);

	auto vertexBuffer = device->UploadBuffer(vertices.data(),
		static_cast<uint64_t>(vertices.size() * sizeof(CubeVertex)), rhi::BufferUsage::Vertex,
		rhi::PipelineStage::VertexInput, rhi::Access::VertexAttributeRead);
	if (!vertexBuffer) {
		cerr << "顶点缓冲创建失败\n";
		return 1;
	}
	auto indexBuffer = device->UploadBuffer(indices.data(),
		static_cast<uint64_t>(indices.size() * sizeof(uint32_t)), rhi::BufferUsage::Index,
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
	uint32_t renderedFrames = 0;

	using Clock = std::chrono::steady_clock;
	const auto startTime = Clock::now();

	while (true) {
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
			if (!BuildFrameGraph(*graph, vertexBuffer.get(), indexBuffer.get(),
				envTexture.get(), currentFrame.has_value() ? currentFrame->backbuffer : nullptr,
				options.smoke)) {
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
					}
					ctx.SignalGpu();
				});
			}
			graphReady = true;
		}
		else if (currentFrame.has_value()) {
			graph->RetargetBorrowedImage("Backbuffer", currentFrame->backbuffer);
		}

		const float now = std::chrono::duration<float>(Clock::now() - startTime).count();

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

	if (options.smoke) {
		if (!captured || capturedPixels.empty()) {
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

			std::filesystem::create_directories("out");
			const string outputPath = "out/cube_reflection_smoke.png";
			stbi_write_png(outputPath.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
				4, capturedPixels.data(), static_cast<int>(kWidth) * 4);

			cout << "Smoke image: min=" << minLuma << " max=" << maxLuma
			     << " mean=" << meanLuma << "\n";
			cout << "Cube reflection smoke ok: wrote " << outputPath << "\n";
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
	return 0;
}
