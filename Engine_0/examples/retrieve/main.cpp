#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "RenderGraphAbstracted.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphNodes.h"
#include "RenderGraphDocument.h"
#include "ContentResolver.h"
#include "AssetCatalog.h"
#include "AssetPack.h"
#include "ModulePack.h"
#include "ShaderLibrary.h"
#include "rhi/RHI.h"
#include "AssimpMesh.h"
#include "FrameData.h"
#include "Camera.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#undef CreateSemaphore

#include "stb_image/stb_image_write.h"

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef RETRIEVE_SHADER_DIR
#define RETRIEVE_SHADER_DIR "retrieve_shaders"
#endif
#ifndef RETRIEVE_ASSET_DIR
#define RETRIEVE_ASSET_DIR "assets"
#endif
#ifndef RETRIEVE_MODULE_DIR
#define RETRIEVE_MODULE_DIR "modules"
#endif

using namespace dEngine;
using namespace std;

namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kFishCount = 1000;
constexpr uint32_t kMaxFish = 1024;
constexpr uint32_t kWorkGroupSize = 256;
constexpr uint32_t kFishDataFloats = kMaxFish * 7;
constexpr uint32_t kFishDataBytes = kFishDataFloats * sizeof(float);
// std430 中 vec3 数组的元素步长是 16 字节；这里按 vec4 分配，避免越界写入
constexpr uint32_t kOutVelBytes = kMaxFish * sizeof(glm::vec4);
constexpr uint32_t kShadowSize = 4096;
constexpr uint32_t kLutSize = 256;
constexpr uint32_t kSkyAmbientSize = 128;

const string kAssetDir = RETRIEVE_ASSET_DIR;
const string kShaderDir = RETRIEVE_SHADER_DIR;
const string kModuleDir = RETRIEVE_MODULE_DIR;

struct Options {
	bool     smoke = false;
	bool     emitModules = false;
	uint32_t frames = 60;
	float    boidsMinSpeed = 0.0f; // 0 = 完全按原工程；>0 为演示用最低巡游速度
};

struct AppInput {
	Camera* camera = nullptr;
	bool*   sunProc = nullptr;
	bool    firstMouse = true;
	float   lastX = 0.0f;
	float   lastY = 0.0f;
};

glm::mat4 MakeLightSpace(const glm::vec3& lightDir, const glm::vec3& lightPos) {
	glm::vec3 up;
	if (std::abs(lightDir.x) < 0.9f)
		up = glm::normalize(glm::cross(lightDir, glm::vec3(1.0f, 0.0f, 0.0f)));
	else
		up = glm::normalize(glm::cross(lightDir, glm::vec3(0.0f, 1.0f, 0.0f)));
	const float orthoSize = 100.0f;
	glm::mat4 proj = glm::orthoRH_ZO(-orthoSize, orthoSize, -orthoSize, orthoSize,
		-orthoSize, orthoSize);
	glm::mat4 view = glm::lookAt(lightPos, lightPos + lightDir, up);
	return proj * view;
}

std::unique_ptr<rhi::Image> CreateLut(rhi::Device& device, uint32_t size, rhi::ImageFormat format) {
	rhi::ImageCreateInfo info;
	info.width = size;
	info.height = size;
	info.format = format;
	info.usage = rhi::ImageUsage::Storage | rhi::ImageUsage::Sampled;
	return device.CreateImage(info);
}

// ---- 模块定义：大气预计算 ----
RenderGraphModule BuildAtmosphereModuleDefinition(std::vector<std::string>* errors) {
	RenderGraphBuilder builder;

	BufferDesc paramsDesc;
	paramsDesc.size = sizeof(AtmosphereParams);
	paramsDesc.usage = rhi::BufferUsage::Uniform;
	paramsDesc.hostVisible = true;
	builder.DeclareBufferResource("AtmosphereParams", paramsDesc);

	ImageOptions lutOptions;
	lutOptions.initialLayout = rhi::ImageLayout::Undefined;
	const rhi::Image* noImage = nullptr;
	builder.DeclareImageResource("TransmittanceLUT", noImage, lutOptions);
	builder.DeclareImageResource("MultipleScatteringLUT", noImage, lutOptions);
	builder.DeclareImageResource("SkyAmbientLUT", noImage, lutOptions);

	builder.DeclarePass("HostParams", {}, { "AtmosphereParams" }, Pass::Host, HostPassDesc{});

	ComputePassDesc transmittanceDesc;
	transmittanceDesc.shaderRef = "transmittance.comp";
	transmittanceDesc.groupCountX = kLutSize / 4;
	transmittanceDesc.groupCountY = kLutSize / 8;
	transmittanceDesc.shaderBindings = {
		ShaderBinding::Uniform("AtmosphereParams"),
		ShaderBinding::StorageWrite("TransmittanceLUT") };
	builder.DeclarePass("Transmittance", { "AtmosphereParams" }, { "TransmittanceLUT" },
		Pass::Compute, transmittanceDesc);

	ComputePassDesc msDesc;
	msDesc.shaderRef = "multiple_scattering.comp";
	msDesc.groupCountX = kLutSize / 4;
	msDesc.groupCountY = kLutSize / 8;
	msDesc.shaderBindings = {
		ShaderBinding::Uniform("AtmosphereParams"),
		ShaderBinding::Sampled("TransmittanceLUT", rhi::SamplerDesc::Linear()),
		ShaderBinding::StorageWrite("MultipleScatteringLUT") };
	builder.DeclarePass("MultipleScattering", { "AtmosphereParams", "TransmittanceLUT" },
		{ "MultipleScatteringLUT" }, Pass::Compute, msDesc);

	ComputePassDesc saDesc;
	saDesc.shaderRef = "sky_ambient.comp";
	saDesc.groupCountX = kSkyAmbientSize / 4;
	saDesc.groupCountY = kSkyAmbientSize / 8;
	saDesc.shaderBindings = {
		ShaderBinding::Uniform("AtmosphereParams"),
		ShaderBinding::Sampled("TransmittanceLUT", rhi::SamplerDesc::Linear()),
		ShaderBinding::Sampled("MultipleScatteringLUT", rhi::SamplerDesc::Linear()),
		ShaderBinding::StorageWrite("SkyAmbientLUT") };
	builder.DeclarePass("SkyAmbient", { "AtmosphereParams", "TransmittanceLUT", "MultipleScatteringLUT" },
		{ "SkyAmbientLUT" }, Pass::Compute, saDesc);

	RenderGraphModule module;
	builder.CompileModule("AtmospherePrecompute",
		{ { "AtmosphereParams", ModulePortKind::Inout } }, &module, errors);
	return module;
}

// ---- 模块定义：鱼群更新 ----
RenderGraphModule BuildFishUpdateModuleDefinition(std::vector<std::string>* errors) {
	RenderGraphBuilder builder;

	BufferDesc boidsDesc;
	boidsDesc.size = sizeof(BoidsParams);
	boidsDesc.usage = rhi::BufferUsage::Uniform;
	boidsDesc.hostVisible = true;
	builder.DeclareBufferResource("BoidsParams", boidsDesc);

	BufferDesc fishDesc;
	fishDesc.size = kFishDataBytes;
	fishDesc.usage = rhi::BufferUsage::Storage;
	fishDesc.hostVisible = true;
	fishDesc.crossFrame = true;
	builder.DeclareBufferResource("fishes", fishDesc);

	BufferDesc outVelDesc;
	outVelDesc.size = kOutVelBytes;
	outVelDesc.usage = rhi::BufferUsage::Storage;
	outVelDesc.hostVisible = false;
	outVelDesc.crossFrame = true;
	builder.DeclareBufferResource("outvel", outVelDesc);

	ComputePassDesc evalDesc;
	evalDesc.shaderRef = "boids_eval.comp";
	evalDesc.groupCountX = (kFishCount + kWorkGroupSize - 1) / kWorkGroupSize;
	evalDesc.shaderBindings = {
		ShaderBinding::Uniform("BoidsParams"),
		ShaderBinding::StorageRead("fishes"),
		ShaderBinding::StorageWrite("outvel") };
	builder.DeclarePass("BoidsEval", { "BoidsParams", "fishes" }, { "outvel" },
		Pass::Compute, evalDesc);

	ComputePassDesc updateDesc;
	updateDesc.shaderRef = "boids_update.comp";
	updateDesc.groupCountX = (kFishCount + kWorkGroupSize - 1) / kWorkGroupSize;
	updateDesc.shaderBindings = {
		ShaderBinding::Uniform("BoidsParams"),
		ShaderBinding::StorageReadWrite("fishes"),
		ShaderBinding::StorageRead("outvel") };
	builder.DeclarePass("BoidsUpdate", { "BoidsParams", "fishes", "outvel" }, { "fishes" },
		Pass::Compute, updateDesc);

	// in-place pass 的顺序必须显式声明
	builder.DeclareTopology(RenderGraphBuilder::Topology::Sequence({ "BoidsEval", "BoidsUpdate" }));

	RenderGraphModule module;
	builder.CompileModule("FishUpdate",
		{ { "fishes", ModulePortKind::Inout }, { "BoidsParams", ModulePortKind::Input } },
		&module, errors);
	return module;
}

bool EmitModules(const ShaderLibrary& shaders) {
	std::error_code ec;
	std::filesystem::create_directories(kModuleDir, ec);

	std::vector<std::string> errors;
	RenderGraphModule atmosphere = BuildAtmosphereModuleDefinition(&errors);
	RenderGraphModule fish = BuildFishUpdateModuleDefinition(&errors);
	if (atmosphere.Name().empty() || fish.Name().empty()) {
		for (const auto& error : errors)
			cerr << "[Module] " << error << "\n";
		return false;
	}

	// 只产出 .pak：模块文档在内存中生成并直接嵌入包内，不再写 sidecar JSON
	AssetCatalog emptyCatalog(shaders.Resolver());
	ModulePackResult result;
	std::string error;
	if (!BuildModulePackFile(RenderGraphModuleDocument::Capture(atmosphere), shaders,
			emptyCatalog, kModuleDir + "/atmosphere.pak", &result, &error)
		|| !BuildModulePackFile(RenderGraphModuleDocument::Capture(fish), shaders,
			emptyCatalog, kModuleDir + "/fish_update.pak", &result, &error)) {
		cerr << "模块打包失败: " << error << "\n";
		return false;
	}

	cout << "Modules emitted: " << kModuleDir << "\n";
	return true;
}

std::optional<RenderGraphModuleDocument> LoadModuleDocument(const std::string& packPath,
	const std::string& packEntry, const std::string& fallbackPath, std::string* error) {
	const auto pack = AssetPack::LoadFile(packPath, error);
	if (pack) {
		ContentResolver packed;
		packed.SetPack(&*pack);
		if (auto document = RenderGraphModuleDocument::Load(packed, "pack:" + packEntry, error))
			return document;
	}
	if (error) error->clear();
	return RenderGraphModuleDocument::Load(fallbackPath, error);
}

bool PrecomputeAtmosphere(rhi::Device& device, const AtmosphereParams& params,
	std::unique_ptr<rhi::Image>& t2a, std::unique_ptr<rhi::Image>& ms,
	std::unique_ptr<rhi::Image>& sa, std::unique_ptr<rhi::Buffer>& atmoUBO,
	const ShaderLibrary& shaders, const RenderGraphModuleDocument& atmosphereDoc)
{
	t2a = CreateLut(device, kLutSize, rhi::ImageFormat::RGBA16F);
	ms = CreateLut(device, kLutSize, rhi::ImageFormat::RGBA16F);
	sa = CreateLut(device, kSkyAmbientSize, rhi::ImageFormat::RGBA16F);
	if (!t2a || !ms || !sa) {
		cerr << "大气 LUT 图像创建失败\n";
		return false;
	}

	rhi::BufferCreateInfo uboInfo;
	uboInfo.size = sizeof(AtmosphereParams);
	uboInfo.usage = rhi::BufferUsage::Uniform;
	uboInfo.hostVisible = true;
	atmoUBO = device.CreateBuffer(uboInfo);
	if (!atmoUBO) {
		cerr << "AtmosphereParams UBO 创建失败\n";
		return false;
	}

	RenderGraphModule atmosphereModule;
	{
		RenderGraphModuleDocument::FixedBindings bindings;
		bindings.image = [&](const std::string& name) -> const rhi::Image* {
			if (name == "TransmittanceLUT") return t2a.get();
			if (name == "MultipleScatteringLUT") return ms.get();
			if (name == "SkyAmbientLUT") return sa.get();
			return nullptr;
		};
		std::vector<std::string> restoreErrors;
		if (!atmosphereDoc.Restore(atmosphereModule, &bindings, &restoreErrors)) {
			cerr << "大气模块恢复失败";
			if (!restoreErrors.empty()) cerr << ": " << restoreErrors.front();
			cerr << "\n";
			return false;
		}
	}

	RenderGraph graph(&device, &shaders);

	RenderGraphBuilder builder;
	builder.DeclareBufferResource("AtmosphereParams", atmoUBO.get());

	ImageOptions lutOptions;
	lutOptions.initialLayout = rhi::ImageLayout::Undefined;
	builder.DeclareImageResource("TransmittanceLUT", t2a.get(), lutOptions);
	builder.DeclareImageResource("MultipleScatteringLUT", ms.get(), lutOptions);
	builder.DeclareImageResource("SkyAmbientLUT", sa.get(), lutOptions);

	std::vector<std::string> moduleErrors;
	const auto instance = builder.Instantiate(atmosphereModule, "Atmosphere",
		{ { "AtmosphereParams", "AtmosphereParams" } }, &moduleErrors);
	if (!instance) {
		for (const auto& error : moduleErrors)
			cerr << "[Atmosphere] " << error << "\n";
		return false;
	}

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

	HostPass* hostParams = nullptr;
	for (const auto& name : instance->Entries) {
		Pass* pass = graph.FindPass(name);
		if (pass != nullptr && pass->get_type() == Pass::Host) {
			hostParams = static_cast<HostPass*>(pass);
			break;
		}
	}
	if (hostParams == nullptr) {
		cerr << "HostParams 节点缺失\n";
		return false;
	}
	hostParams->SetCallback([params](HostContext& ctx) {
		ctx.WaitForGpu();
		if (rhi::Buffer* b = ctx.WriteBuffer("AtmosphereParams"))
			std::memcpy(b->Map(), &params, sizeof(params));
		ctx.SignalGpu();
	});

	graph.SubmitGraph();
	graph.WaitHostPasses();
	device.WaitForFrame();

	// 预计算结束后把三张 LUT 直接转换到只读布局（RHI 的 TransitionImage 封装）
	rhi::CommandList cmd = device.BeginCommandList();
	const rhi::PipelineStage lutDstStage =
		rhi::PipelineStage::ComputeShader | rhi::PipelineStage::FragmentShader;
	cmd.TransitionImage(*t2a, rhi::ImageLayout::ShaderReadOnly,
		rhi::ImageLayout::ShaderReadOnly, {}, rhi::PipelineStage::ComputeShader,
		rhi::Access::ShaderRead, lutDstStage, rhi::Access::ShaderRead);
	cmd.TransitionImage(*ms, rhi::ImageLayout::ShaderReadOnly,
		rhi::ImageLayout::ShaderReadOnly, {}, rhi::PipelineStage::ComputeShader,
		rhi::Access::ShaderRead, lutDstStage, rhi::Access::ShaderRead);
	cmd.TransitionImage(*sa, rhi::ImageLayout::General,
		rhi::ImageLayout::ShaderReadOnly, {}, rhi::PipelineStage::ComputeShader,
		rhi::Access::ShaderWrite, lutDstStage, rhi::Access::ShaderRead);
	rhi::SubmitInfo submit;
	submit.commandLists = { &cmd, 1 };
	device.Submit(submit);
	device.WaitForFrame();

	cout << "Atmosphere LUT precompute ok\n";
	return true;
}

bool BuildFrameGraph(RenderGraph& graph, const rhi::Buffer* vb, const rhi::Buffer* ib,
	uint32_t indexCount, const rhi::Buffer* atmoUBO, const rhi::Image* t2a,
	const rhi::Image* ms, const rhi::Image* sa, const rhi::Image* backbuffer,
	bool smoke, const RenderGraphModule& fishModule)
{
	(void)indexCount;

	RenderGraphBuilder builder;

	BufferDesc frameUBO;
	frameUBO.size = sizeof(FrameUniforms);
	frameUBO.usage = rhi::BufferUsage::Uniform;
	frameUBO.hostVisible = true;
	builder.DeclareBufferResource("FrameUniforms", frameUBO);

	BufferDesc shadowUBO;
	shadowUBO.size = sizeof(ShadowUniforms);
	shadowUBO.usage = rhi::BufferUsage::Uniform;
	shadowUBO.hostVisible = true;
	builder.DeclareBufferResource("ShadowUniforms", shadowUBO);

	BufferDesc boidsUBO;
	boidsUBO.size = sizeof(BoidsParams);
	boidsUBO.usage = rhi::BufferUsage::Uniform;
	boidsUBO.hostVisible = true;
	builder.DeclareBufferResource("BoidsParams", boidsUBO);

	builder.DeclareBufferResource("AtmosphereParams", atmoUBO);

	BufferDesc fishDataDesc;
	fishDataDesc.size = kFishDataBytes;
	fishDataDesc.usage = rhi::BufferUsage::Storage;
	fishDataDesc.hostVisible = true;
	fishDataDesc.crossFrame = true;
	builder.DeclareBufferResource("fishes", fishDataDesc);

	// 必须先声明参数写者，模块内的 BoidsEval/BoidsUpdate 才能读到本帧版本
	builder.DeclarePass("UpdateFrame", {}, { "FrameUniforms", "ShadowUniforms", "BoidsParams" },
		Pass::Host, HostPassDesc{});

	std::vector<std::string> moduleErrors;
	const auto fishInstance = builder.Instantiate(fishModule, "FishUpdate",
		{ { "fishes", "fishes" }, { "BoidsParams", "BoidsParams" } }, &moduleErrors);
	if (!fishInstance) {
		for (const auto& error : moduleErrors)
			cerr << "[FishUpdate] " << error << "\n";
		return false;
	}

	builder.DeclareBufferResource("FishVB", vb);
	builder.DeclareBufferResource("FishIB", ib);

	ImageDesc shadowMapDesc;
	shadowMapDesc.width = kShadowSize;
	shadowMapDesc.height = kShadowSize;
	shadowMapDesc.format = ImageFormat::D32F;
	builder.DeclareImageResource("shadowMap", shadowMapDesc);

	ImageDesc canvasDesc;
	canvasDesc.width = kWidth;
	canvasDesc.height = kHeight;
	canvasDesc.format = ImageFormat::RGBA8;
	builder.DeclareImageResource("canvas", canvasDesc);

	ImageDesc depthDesc;
	depthDesc.width = kWidth;
	depthDesc.height = kHeight;
	depthDesc.format = ImageFormat::D32F;
	builder.DeclareImageResource("depth", depthDesc);

	ImageOptions lutOptions;
	lutOptions.initialLayout = rhi::ImageLayout::ShaderReadOnly;
	lutOptions.crossFrame = true;
	builder.DeclareImageResource("TransmittanceLUT", t2a, lutOptions);
	builder.DeclareImageResource("MultipleScatteringLUT", ms, lutOptions);
	builder.DeclareImageResource("SkyAmbientLUT", sa, lutOptions);

	if (smoke) {
		ImageDesc outputDesc;
		outputDesc.width = kWidth;
		outputDesc.height = kHeight;
		outputDesc.format = ImageFormat::RGBA8;
		builder.DeclareImageResource("Backbuffer", outputDesc);
	}
	else {
		ImageOptions backOptions;
		backOptions.initialLayout = rhi::ImageLayout::Undefined;
		builder.DeclareImageResource("Backbuffer", backbuffer, backOptions);
	}

	RenderPassDesc shadowDesc0;
	{
		AttachmentBinding depth;
		depth.resourceName = "shadowMap";
		depth.usage = Usage::DepthStencilAttachment;
		depth.load = LoadOp::Clear;
		depth.clear.value = DepthStencilValue{ 1.0f, 0 };
		depth.store = StoreOp::Store;
		shadowDesc0.attachments.push_back(depth);

		shadowDesc0.pipeline.shaderRefs = { "shadow.vert", "fish.tesc", "fish.tese", "shadow.geom", "shadow.frag" };
		shadowDesc0.pipeline.topology = PrimitiveTopology::PatchList;
		shadowDesc0.pipeline.tessPatchControlPoints = 3;
		shadowDesc0.pipeline.cullMode = CullMode::None;
		shadowDesc0.pipeline.depthStencil.depthTestEnable = true;
		shadowDesc0.pipeline.depthStencil.depthWriteEnable = true;
		shadowDesc0.pipeline.depthStencil.depthCompareOp = CompareOp::LessEqual;

		shadowDesc0.viewport.autoFromColor = false;
		shadowDesc0.viewport.x = 0.0f;
		shadowDesc0.viewport.y = 0.0f;
		shadowDesc0.viewport.w = static_cast<float>(kShadowSize);
		shadowDesc0.viewport.h = static_cast<float>(kShadowSize);

		shadowDesc0.shaderBindings = {
			ShaderBinding::Uniform("ShadowUniforms"),
			ShaderBinding::Uniform("BoidsParams"),
			ShaderBinding::StorageRead("fishes") };
		shadowDesc0.vertexStreams.push_back(VertexStreamBinding{ "FishVB", {}, 0 });
		shadowDesc0.indexStream = IndexStreamBinding{ "FishIB", {}, 0, IndexType::UInt32 };
	}
	builder.DeclarePass("Shadow", { "ShadowUniforms", "BoidsParams", "fishes", "FishVB", "FishIB" },
		{ "shadowMap" }, Pass::Render, shadowDesc0);

	RenderPassDesc sceneDesc;
	{
		AttachmentBinding color;
		color.resourceName = "canvas";
		color.slot = 0;
		color.load = LoadOp::Clear;
		color.clear.value = std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
		sceneDesc.attachments.push_back(color);

		AttachmentBinding depth;
		depth.resourceName = "depth";
		depth.usage = Usage::DepthStencilAttachment;
		depth.load = LoadOp::Clear;
		depth.clear.value = DepthStencilValue{ 1.0f, 0 };
		depth.store = StoreOp::Store;
		sceneDesc.attachments.push_back(depth);

		sceneDesc.pipeline.shaderRefs = { "fish.vert", "fish.tesc", "fish.tese", "fish.geom", "fish.frag" };
		sceneDesc.pipeline.topology = PrimitiveTopology::PatchList;
		sceneDesc.pipeline.tessPatchControlPoints = 3;
		sceneDesc.pipeline.cullMode = CullMode::None;
		sceneDesc.pipeline.depthStencil.depthTestEnable = true;
		sceneDesc.pipeline.depthStencil.depthWriteEnable = true;

		sceneDesc.shaderBindings = {
			ShaderBinding::Uniform("FrameUniforms"),
			ShaderBinding::Uniform("BoidsParams"),
			ShaderBinding::StorageRead("fishes"),
			ShaderBinding::Uniform("AtmosphereParams"),
			ShaderBinding::Sampled("shadowMap", rhi::SamplerDesc::Nearest()) };
		sceneDesc.vertexStreams.push_back(VertexStreamBinding{ "FishVB", {}, 0 });
		sceneDesc.indexStream = IndexStreamBinding{ "FishIB", {}, 0, IndexType::UInt32 };
	}
	builder.DeclarePass("Scene",
		{ "FrameUniforms", "BoidsParams", "fishes", "AtmosphereParams", "shadowMap", "FishVB", "FishIB" },
		{ "canvas", "depth" }, Pass::Render, sceneDesc);

	RenderPassDesc backDesc;
	{
		AttachmentBinding color;
		color.resourceName = "Backbuffer";
		color.slot = 0;
		color.load = LoadOp::DontCare;
		color.store = StoreOp::Store;
		backDesc.attachments.push_back(color);

		backDesc.pipeline.shaderRefs = { "fullscreen.vert", "atmosphere.frag" };
		backDesc.pipeline.cullMode = CullMode::None;
		backDesc.pipeline.depthStencil.depthTestEnable = false;

		backDesc.shaderBindings = {
			ShaderBinding::Uniform("FrameUniforms"),
			ShaderBinding::Uniform("AtmosphereParams"),
			ShaderBinding::Sampled("TransmittanceLUT", rhi::SamplerDesc::Linear()),
			ShaderBinding::Sampled("MultipleScatteringLUT", rhi::SamplerDesc::Linear()),
			ShaderBinding::Sampled("SkyAmbientLUT", rhi::SamplerDesc::Linear()),
			ShaderBinding::Sampled("canvas", rhi::SamplerDesc::Nearest()),
			ShaderBinding::Sampled("depth", rhi::SamplerDesc::Nearest()) };
	}
	builder.DeclarePass("Back",
		{ "FrameUniforms", "AtmosphereParams", "TransmittanceLUT", "MultipleScatteringLUT",
		  "SkyAmbientLUT", "canvas", "depth" },
		{ "Backbuffer" }, Pass::Render, backDesc);

	if (smoke)
		builder.DeclarePass("Capture", { "Backbuffer" }, {}, Pass::Host, HostPassDesc{});
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
	return true;
}

void InitBoids(RenderGraph& graph, uint32_t count,
	std::vector<glm::vec3>* initialPositions = nullptr,
	std::vector<glm::vec3>* initialDirs = nullptr) {
	Resource* res = graph.FindResourceByName("fishes");
	if (res == nullptr || res->get_type() != Resource::Buffer) {
		cerr << "fishes 资源缺失\n";
		return;
	}
	rhi::Buffer* buf = const_cast<rhi::Buffer*>(static_cast<Buffer*>(res)->get_rhi());
	if (buf == nullptr)
		return;

	float* data = static_cast<float*>(buf->Map());
	if (initialPositions != nullptr) {
		initialPositions->clear();
		initialPositions->reserve(count);
	}
	if (initialDirs != nullptr) {
		initialDirs->clear();
		initialDirs->reserve(count);
	}
	for (uint32_t i = 0; i < count; ++i) {
		float n = static_cast<float>(i);
		uint32_t p = i * 3;
		uint32_t d = count * 3 + i * 3;
		uint32_t v = count * 6 + i;
		data[p + 0] = 10.0f * std::sin(n);
		data[p + 1] = 10.0f * std::sin(n) + 10.0f;
		data[p + 2] = 10.0f * std::sin(n + 10.0f);
		if (initialPositions != nullptr)
			initialPositions->emplace_back(data[p + 0], data[p + 1], data[p + 2]);

		glm::vec3 dir(std::sin(n), 0.0f, std::cos(n));
		if (glm::length(dir) > 1e-5f) dir = glm::normalize(dir);
		data[d + 0] = dir.x;
		data[d + 1] = dir.y;
		data[d + 2] = dir.z;
		data[v] = 0.1f;
		if (initialDirs != nullptr)
			initialDirs->push_back(dir);
	}
	buf->Flush();
}

void BarrierHostVisibility(rhi::Device& device, rhi::Buffer& buffer,
	rhi::PipelineStage srcStage, rhi::Access srcAccess,
	rhi::PipelineStage dstStage, rhi::Access dstAccess)
{
	rhi::CommandList cmd = device.BeginCommandList();
	cmd.TransitionBuffer(buffer, srcStage, srcAccess, dstStage, dstAccess);
	rhi::SubmitInfo submit;
	submit.commandLists = { &cmd, 1 };
	device.Submit(submit);
	device.WaitForFrame();
}

} // namespace

int main(int argc, char** argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--smoke") == 0) {
			options.smoke = true;
			if (i + 1 < argc) {
				int n = std::atoi(argv[i + 1]);
				if (n > 0) {
					options.frames = static_cast<uint32_t>(n);
					++i;
				}
			}
		}
		else if (std::strcmp(argv[i], "--emit-modules") == 0) {
			options.emitModules = true;
		}
		else if (std::strcmp(argv[i], "--boids-min-speed") == 0 && i + 1 < argc) {
			options.boidsMinSpeed = static_cast<float>(std::atof(argv[i + 1]));
			++i;
		}
	}

	if (options.emitModules) {
		ContentResolver emitAssets;
		emitAssets.Mount("shaders", kShaderDir);
		emitAssets.Mount("assets", kAssetDir);
		ShaderLibrary emitShaders(emitAssets);
		std::vector<std::string> warnings;
		if (!emitShaders.LoadManifest("shaders/shaders.manifest.json", &warnings)) {
			for (const auto& warning : warnings)
				cerr << "[ShaderLibrary] " << warning << "\n";
			cerr << "着色器清单加载失败\n";
			return 1;
		}
		return EmitModules(emitShaders) ? 0 : 1;
	}

	GLFWwindow* window = nullptr;
	AppInput input;
	Camera camera;
	camera.position = glm::vec3(0.0f, 45.0f, -90.0f);
	camera.pitch = glm::radians(-8.0f);
	bool glfwInitialized = false;

	if (!options.smoke) {
		if (!glfwInit()) {
			cerr << "glfwInit 失败\n";
			return 1;
		}
		glfwInitialized = true;
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
		window = glfwCreateWindow(kWidth, kHeight, "Engine_0 retrieve", nullptr, nullptr);
		if (window == nullptr) {
			cerr << "窗口创建失败\n";
			if (glfwInitialized) glfwTerminate();
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
	cout << "Tessellation=" << (device->SupportsTessellation() ? "yes" : "no")
	     << " Geometry=" << (device->SupportsGeometryShaders() ? "yes" : "no") << "\n";

	ContentResolver assets;
	assets.Mount("shaders", kShaderDir);
	assets.Mount("assets", kAssetDir);
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

	std::string moduleError;
	const auto atmosphereDoc = LoadModuleDocument(kModuleDir + "/atmosphere.pak",
		"modules/AtmospherePrecompute.json",
		kModuleDir + "/AtmospherePrecompute.module.json", &moduleError);
	if (!atmosphereDoc) {
		cerr << "大气模块加载失败: " << moduleError << "\n";
		if (window) glfwDestroyWindow(window);
		if (glfwInitialized) glfwTerminate();
		return 1;
	}
	const auto fishDoc = LoadModuleDocument(kModuleDir + "/fish_update.pak",
		"modules/FishUpdate.json",
		kModuleDir + "/FishUpdate.module.json", &moduleError);
	if (!fishDoc) {
		cerr << "鱼群模块加载失败: " << moduleError << "\n";
		if (window) glfwDestroyWindow(window);
		if (glfwInitialized) glfwTerminate();
		return 1;
	}

	FishMesh mesh;
	std::string meshError;
	if (!LoadFishMesh(kAssetDir + "/lowpoly_fish.glb", mesh, &meshError)) {
		cerr << "鱼网格加载失败: " << meshError << "\n";
		if (window) glfwDestroyWindow(window);
		if (glfwInitialized) glfwTerminate();
		return 1;
	}
	cout << "Fish mesh loaded: verts=" << mesh.vertices.size()
	     << " indices=" << mesh.indices.size() << "\n";

	auto vertexBuffer = device->UploadBuffer(mesh.vertices.data(),
		static_cast<uint64_t>(mesh.vertices.size() * sizeof(FishVertex)), rhi::BufferUsage::Vertex,
		rhi::PipelineStage::VertexInput, rhi::Access::VertexAttributeRead);
	if (vertexBuffer == nullptr) {
		cerr << "顶点缓冲创建失败\n";
		return 1;
	}

	auto indexBuffer = device->UploadBuffer(mesh.indices.data(),
		static_cast<uint64_t>(mesh.indices.size() * sizeof(uint32_t)), rhi::BufferUsage::Index,
		rhi::PipelineStage::VertexInput, rhi::Access::IndexRead);
	if (indexBuffer == nullptr) {
		cerr << "索引缓冲创建失败\n";
		return 1;
	}

	AtmosphereParams atmosphereParams;
	atmosphereParams.H_Rayleigh = 8500.0f;
	atmosphereParams.H_Mie = 1200.0f;
	atmosphereParams.g_Mie = 0.7f;
	atmosphereParams.PlanetRadius = 6371000.0f;
	atmosphereParams.AtmosphereRadius = 6471000.0f;
	atmosphereParams.H_Ozone = 3000.0f;
	atmosphereParams.W_Ozone = 1200.0f;
	atmosphereParams.SunLuminance = glm::vec3(12.0f, 10.0f, 10.0f);
	atmosphereParams.PlanetCenter = glm::vec3(0.0f, -6371000.0f, 0.0f);

	unique_ptr<rhi::Image> t2a;
	unique_ptr<rhi::Image> ms;
	unique_ptr<rhi::Image> sa;
	unique_ptr<rhi::Buffer> atmoUBO;
	if (!PrecomputeAtmosphere(*device, atmosphereParams, t2a, ms, sa, atmoUBO, shaders,
			*atmosphereDoc)) {
		return 1;
	}

	RenderGraphModule fishModule;
	{
		std::vector<std::string> restoreErrors;
		if (!fishDoc->Restore(fishModule, nullptr, &restoreErrors)) {
			cerr << "鱼群模块恢复失败";
			if (!restoreErrors.empty()) cerr << ": " << restoreErrors.front();
			cerr << "\n";
			return 1;
		}
	}

	using Clock = std::chrono::steady_clock;
	const auto startTime = Clock::now();
	float previousTime = 0.0f;
	float sunApproach = 0.0f;
	bool sunProc = true;

	if (window != nullptr) {
		input.camera = &camera;
		input.sunProc = &sunProc;
		glfwSetWindowUserPointer(window, &input);
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		glfwSetCursorPosCallback(window, [](GLFWwindow* w, double xpos, double ypos) {
			AppInput* in = static_cast<AppInput*>(glfwGetWindowUserPointer(w));
			if (in->firstMouse) {
				in->lastX = static_cast<float>(xpos);
				in->lastY = static_cast<float>(ypos);
				in->firstMouse = false;
			}
			float dx = static_cast<float>(xpos) - in->lastX;
			float dy = in->lastY - static_cast<float>(ypos);
			in->lastX = static_cast<float>(xpos);
			in->lastY = static_cast<float>(ypos);
			// Vulkan 投影翻转了 Y：鼠标上移应抬头（pitch 增大），滚轮上滚应前进
			in->camera->rotate(glm::radians(-0.1f * dx), glm::radians(0.1f * dy));
		});
		glfwSetScrollCallback(window, [](GLFWwindow* w, double, double yoffset) {
			AppInput* in = static_cast<AppInput*>(glfwGetWindowUserPointer(w));
			in->camera->moveForward(0.5f * static_cast<float>(yoffset) * 10.0f);
		});
		glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int, int action, int) {
			AppInput* in = static_cast<AppInput*>(glfwGetWindowUserPointer(w));
			if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
				glfwSetWindowShouldClose(w, GLFW_TRUE);
			if (key == GLFW_KEY_SPACE && action == GLFW_PRESS && in->sunProc != nullptr)
				*in->sunProc = !*in->sunProc;
		});
	}

	optional<RenderGraph> graph;
	optional<rhi::Swapchain::Frame> currentFrame;
	auto swapchain = window ? device->CreateSwapchain() : nullptr;

	bool graphReady = false;
	bool captureRequested = false;
	bool captured = false;
	std::vector<unsigned char> capturedPixels;

	std::vector<glm::vec3> initialPositions;
	std::vector<glm::vec3> initialDirs;
	uint32_t renderedFrames = 0;

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
		else if (options.smoke && renderedFrames >= options.frames) {
			break;
		}

		if (!graphReady) {
			graph.emplace(device.get(), &shaders);
			const rhi::Image* backbuffer = currentFrame.has_value() ? currentFrame->backbuffer : nullptr;
			if (!BuildFrameGraph(*graph, vertexBuffer.get(), indexBuffer.get(),
				static_cast<uint32_t>(mesh.indices.size()), atmoUBO.get(),
				t2a.get(), ms.get(), sa.get(), backbuffer, options.smoke, fishModule)) {
				return 1;
			}

			for (uint32_t i = 0; graph->GetPass(i) != nullptr; ++i) {
				Pass* p = graph->GetPass(i);
				if (p->get_type() != Pass::Render)
					continue;
				RenderPass* rp = static_cast<RenderPass*>(p);
				DrawSource draw;
				draw.kind = DrawSource::Direct;
				if (p->get_name() == "Back") {
					draw.count = 3;
					draw.instanceCount = 1;
				}
				else {
					draw.count = static_cast<uint32_t>(mesh.indices.size());
					draw.first = 0;
					draw.vertexOffset = 0;
					draw.instanceCount = kFishCount;
				}
				rp->SetDraw(draw);
			}

			InitBoids(*graph, kFishCount, &initialPositions, &initialDirs);
			Resource* fishRes = graph->FindResourceByName("fishes");
			if (fishRes != nullptr) {
				BarrierHostVisibility(*device,
					*const_cast<rhi::Buffer*>(static_cast<Buffer*>(fishRes)->get_rhi()),
					rhi::PipelineStage::Host, rhi::Access::HostWrite,
					rhi::PipelineStage::ComputeShader,
					rhi::Access::ShaderRead | rhi::Access::ShaderWrite);
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

		float now = std::chrono::duration<float>(Clock::now() - startTime).count();
		float dt = now - previousTime;
		previousTime = now;

		if (sunProc) sunApproach += dt;
		glm::vec3 lightTravelDir = glm::normalize(glm::vec3(0.5f,
			-std::cos(sunApproach * 0.1f), std::sin(sunApproach * 0.1f)));
		glm::vec3 dlightDir = -lightTravelDir;

		glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f),
			static_cast<float>(kWidth) / static_cast<float>(kHeight), 0.1f, 10000.0f);
		// Vulkan NDC y 向下：翻转投影 Y，使天空在上、地面在下，并与 gl_FragCoord 对齐
		proj[1][1] *= -1.0f;
		glm::mat4 view = camera.view();
		glm::mat4 PMat = proj * view;
		glm::mat4 PMat_iv = glm::inverse(PMat);
		glm::mat4 lightSpace = MakeLightSpace(lightTravelDir, glm::vec3(5.0f, 5.0f, 5.0f));

		FrameUniforms frameUniforms;
		frameUniforms.PMat = PMat;
		frameUniforms.PMat_iv = PMat_iv;
		frameUniforms.lightSpaceMatrix = lightSpace;
		frameUniforms.dlight = glm::vec4(dlightDir, 0.0f);
		frameUniforms.resolution = glm::vec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
		frameUniforms.time = now;
		frameUniforms.lightCol = glm::vec3(1.0f, 1.0f, 1.0f);
		frameUniforms.cameraPos = glm::vec4(camera.position, 1.0f);
		frameUniforms.lightDir = glm::vec4(lightTravelDir, 0.0f);

		ShadowUniforms shadowUniforms;
		shadowUniforms.PMat = lightSpace;
		shadowUniforms.params = glm::vec4(now, 0.0f, 0.0f, 0.0f);

		BoidsParams boidsParams;
		boidsParams.fishCount = static_cast<int>(kFishCount);
		boidsParams.time = now;
		boidsParams.noiseStrength = 0.1f;
		boidsParams.boundaryMargin = 3.0f;
		boidsParams.boundsMin = glm::vec4(-30.0f, 10.0f, -30.0f, 0.0f);
		boidsParams.boundsMax = glm::vec4(30.0f, 100.0f, 30.0f, 0.0f);
		boidsParams.boundaryStrength = 0.3f;
		boidsParams.minSpeed = options.boidsMinSpeed;

		HostPass* update = static_cast<HostPass*>(graph->FindPass("UpdateFrame"));
		update->SetCallback([frameUniforms, shadowUniforms, boidsParams](HostContext& ctx) {
			ctx.WaitForGpu();
			if (rhi::Buffer* b = ctx.WriteBuffer("FrameUniforms"))
				std::memcpy(b->Map(), &frameUniforms, sizeof(frameUniforms));
			if (rhi::Buffer* b = ctx.WriteBuffer("ShadowUniforms"))
				std::memcpy(b->Map(), &shadowUniforms, sizeof(shadowUniforms));
			if (rhi::Buffer* b = ctx.WriteBuffer("BoidsParams"))
				std::memcpy(b->Map(), &boidsParams, sizeof(boidsParams));
			ctx.SignalGpu();
		});

		if (options.smoke)
			captureRequested = (renderedFrames + 1 == options.frames);

		uint32_t slot = device->BeginFrame();
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
			cerr << "Retrieve smoke FAILED: 未捕获到输出图像\n";
		}
		else {
			int minLuma = 255;
			int maxLuma = 0;
			double sumLuma = 0.0;
			for (size_t i = 0; i + 3 < capturedPixels.size(); i += 4) {
				int luma = (int(capturedPixels[i]) + int(capturedPixels[i + 1])
					+ int(capturedPixels[i + 2])) / 3;
				minLuma = std::min(minLuma, luma);
				maxLuma = std::max(maxLuma, luma);
				sumLuma += luma;
			}
			double meanLuma = sumLuma / (static_cast<double>(kWidth) * kHeight);

			std::filesystem::create_directories("out");
			const string outputPath = "out/retrieve_smoke.png";
			stbi_write_png(outputPath.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
				4, capturedPixels.data(), static_cast<int>(kWidth) * 4);

			uint32_t active = 0, inactive = 0, badDir = 0, badPos = 0;
			uint32_t zeroVel = 0, sameAsInitDir = 0;
			double sumMove = 0.0;
			float minMove = 1e9f, maxMove = 0.0f;
			Resource* fishRes = graph->FindResourceByName("fishes");
			if (fishRes != nullptr && initialPositions.size() == kFishCount) {
				rhi::Buffer* fishBuf = const_cast<rhi::Buffer*>(static_cast<Buffer*>(fishRes)->get_rhi());
				if (fishBuf != nullptr) {
					BarrierHostVisibility(*device, *fishBuf,
						rhi::PipelineStage::ComputeShader, rhi::Access::ShaderWrite,
						rhi::PipelineStage::Host, rhi::Access::HostRead);
					device->WaitForFrame();
					fishBuf->Invalidate();
					float* data = static_cast<float*>(fishBuf->Map());
					for (uint32_t i = 0; i < kFishCount; ++i) {
						glm::vec3 pos(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
						float d = glm::length(pos - initialPositions[i]);
						if (!(d >= 0.0f)) ++badPos;
						if (d > 0.01f) ++active; else ++inactive;
						sumMove += d;
						minMove = std::min(minMove, d);
						maxMove = std::max(maxMove, d);
						uint32_t dirOff = kFishCount * 3 + i * 3;
						glm::vec3 dir(data[dirOff], data[dirOff + 1], data[dirOff + 2]);
						if (!(glm::length(dir) > 1e-6f))
							++badDir;
						float speedVal = data[kFishCount * 6 + i];
						if (!(speedVal > 1e-6f))
							++zeroVel;
						if (initialDirs.size() == kFishCount
							&& glm::length(dir - initialDirs[i]) < 1e-5f)
							++sameAsInitDir;
					}
				}
			}
			double meanMove = initialPositions.size() == kFishCount
				? sumMove / static_cast<double>(kFishCount) : 0.0;
			cout << "Boids update ok: active=" << active << "/" << kFishCount
			     << " inactive=" << inactive << " badDir=" << badDir
			     << " badPos=" << badPos << " zeroVel=" << zeroVel
			     << " sameAsInitDir=" << sameAsInitDir
			     << " meanMove=" << meanMove << " minMove=" << minMove
			     << " maxMove=" << maxMove << "\n";
			cout << "Smoke image: min=" << minLuma << " max=" << maxLuma
			     << " mean=" << meanLuma << "\n";
			cout << "Retrieve smoke ok: wrote " << outputPath << "\n";
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
