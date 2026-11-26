#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "rhi/RHI.h"
#include "AssetLoader.h"
#include "ShaderLibrary.h"
#include "AssetPack.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#undef CreateSemaphore

#include <cstdint>
#include <iostream>
#include <memory>

using namespace dEngine;

namespace {

	constexpr uint32_t kWidth = 1280;
	constexpr uint32_t kHeight = 720;


	void OnKey(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
		if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
			glfwSetWindowShouldClose(window, GLFW_TRUE);
	}

} // namespace

GLFWwindow* m_CreateWindow() {
	if (!glfwInit()) {
		std::cerr << "glfwInit 失败\n";
		return nullptr;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

	GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "Engine_0 Deferred", nullptr, nullptr);
	if (window == nullptr) {
		std::cerr << "窗口创建失败\n";
		glfwTerminate();
		return nullptr;
	}
	return window;
}

std::unique_ptr<rhi::Device> MakeDevice(GLFWwindow* window) {
	dEngine::rhi::DeviceDesc desc;
	desc.nativeWindowHandle = glfwGetWin32Window(window);
	desc.width = kWidth;
	desc.height = kHeight;
	desc.enableValidation = true;
	desc.maxFramesInFlight = 2;
	desc.forceFixedArrayHeap = false;
	
	auto device = rhi::Device::Create(desc);
	if (!device) {
		std::cerr << "rhi::Device创建失败\n";
		glfwDestroyWindow(window);
		glfwTerminate();
		return nullptr;
	}
	return device;
}



int main() {
	GLFWwindow* window = m_CreateWindow();
	if (window == nullptr)
		return 1;
	glfwSetKeyCallback(window, OnKey);

	auto device = MakeDevice(window);
	if (!device)
		return 1;
	std::cout << "Device: " << device->DeviceName() << "\n";

	ContentResolver resolver;
	resolver.Mount("shaders", DEFERRED_SHADER_DIR);
	resolver.Mount("assets", DEFERRED_ASSET_DIR);

	AssetCatalog catalog(resolver);
	catalog.RegisterImage({.name="skyview",.logicalPath="assets/skyview.png"}, nullptr);

	ShaderLibrary shaders(resolver);
	shaders.Register({ .name = "quad.vert",.stage = rhi::ShaderStage::Vertex,.logicalPath = "shaders/quad.vert",.entryPoint="main"}, nullptr);
	shaders.Register({ .name = "A.frag",.stage = rhi::ShaderStage::Fragment,.logicalPath = "shaders/A.frag",.entryPoint = "main" }, nullptr);

	AssetLoader loader(*device, catalog);
	

	while (!glfwWindowShouldClose(window))
		glfwPollEvents();

	device->WaitIdle();
	device.reset();
	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
