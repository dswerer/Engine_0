#include "RHI.h"

#include "../external/vk_mem_alloc.h"

// Win32 表面：vulkan_win32.h 依赖 windows.h 提供的 HWND / HINSTANCE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_win32.h>

// windows.h 把 CreateSemaphore 宏定义为 CreateSemaphoreA/W，与 RHI 的 Device::CreateSemaphore 同名冲突
#undef CreateSemaphore

#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>

// RHI 实现落脚点。当前阶段已落地 Device 初始化（Instance / 表面 / 物理设备 / 逻辑设备 / VMA）、
// 交换链（Swapchain）、视图（ImageView）与管线 / 描述符（Pipeline / Descriptor）；
// 命令录制与提交留待后续阶段。

using namespace dEngine::rhi;

namespace {

	constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

	// 校验层是否可用（不可用则静默降级为无校验）
	bool HasValidationLayer() {
		uint32_t count = 0;
		if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
		std::vector<VkLayerProperties> layers(count);
		vkEnumerateInstanceLayerProperties(&count, layers.data());
		for (const auto& l : layers) {
			if (std::strcmp(l.layerName, kValidationLayer) == 0) return true;
		}
		return false;
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
		VkDebugUtilsMessageTypeFlagsEXT             /*type*/,
		const VkDebugUtilsMessengerCallbackDataEXT* data,
		void*                                       /*userData*/) {
		const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR"
			: (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN" : "INFO";
		std::cerr << "[Vulkan " << level << "] " << data->pMessage << "\n";
		return VK_FALSE;
	}

	// 设备是否支持交换链扩展（有 surface 时的必要条件）
	bool SupportsSwapchain(VkPhysicalDevice pd) {
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> extensions(count);
		vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, extensions.data());
		for (const auto& e : extensions) {
			if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
		}
		return false;
	}

	// 选择物理设备：必须支持图形队列；有 surface 时还需支持呈现与交换链扩展；优先独显
	//需要注意的是：队列族的需求仅在这里显式出现。compute/transfer 挑专用族（不含 graphics 位），
	// 无专用族时兜底 graphics 族（规范保证 GRAPHICS 必带 COMPUTE/TRANSFER 能力位，兜底恒有效）
	VkPhysicalDevice PickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface,
		uint32_t& outGraphicsFamily, uint32_t& outPresentFamily,
		uint32_t& outComputeFamily, uint32_t& outTransferFamily) {
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		if (count == 0) return VK_NULL_HANDLE;

		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(instance, &count, devices.data());

		VkPhysicalDevice best = VK_NULL_HANDLE;
		uint32_t bestGraphics = 0, bestPresent = 0, bestCompute = UINT32_MAX, bestTransfer = UINT32_MAX;
		int bestScore = -1;

		for (VkPhysicalDevice pd : devices) {
			if (surface != VK_NULL_HANDLE && !SupportsSwapchain(pd)) continue;

			uint32_t familyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, families.data());

			uint32_t graphics = UINT32_MAX;
			for (uint32_t i = 0; i < familyCount; ++i) {
				if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { graphics = i; break; }
			}
			if (graphics == UINT32_MAX) continue;

			// 专用 compute（不含 graphics 位）/ 专用 transfer（不含 graphics、compute 位）：各取首个命中族
			uint32_t compute = UINT32_MAX, transfer = UINT32_MAX;
			for (uint32_t i = 0; i < familyCount; ++i) {
				const VkQueueFlags flags = families[i].queueFlags;
				if (compute == UINT32_MAX && (flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)) {
					compute = i;
				}
				if (transfer == UINT32_MAX && (flags & VK_QUEUE_TRANSFER_BIT)
					&& !(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
					transfer = i;
				}
			}

			// 呈现队列：优先与图形同族（复用同一队列）；否则另找支持呈现的队列族
			uint32_t present = graphics;
			if (surface != VK_NULL_HANDLE) {
				VkBool32 support = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(pd, graphics, surface, &support);
				if (!support) {
					present = UINT32_MAX;
					for (uint32_t i = 0; i < familyCount; ++i) {
						vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &support);
						if (support) { present = i; break; }
					}
					if (present == UINT32_MAX) continue; // 无呈现队列，跳过该设备
				}
			}

			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(pd, &props);
			int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 1000
				: props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 100 : 1;

			if (score > bestScore) {
				bestScore = score; best = pd;
				bestGraphics = graphics; bestPresent = present;
				bestCompute = compute; bestTransfer = transfer;
			}
		}

		outGraphicsFamily = bestGraphics;
		outPresentFamily = bestPresent;
		// 兜底：无专用族时回落 graphics（规范保证其必带 COMPUTE/TRANSFER 能力位）
		outComputeFamily  = (bestCompute  != UINT32_MAX) ? bestCompute  : bestGraphics;
		outTransferFamily = (bestTransfer != UINT32_MAX) ? bestTransfer : bestGraphics;
		return best;
	}

	// ---- 描述层词表 -> Vulkan 枚举映射 ----
	VkFormat ToVkFormat(ImageFormat format) {
		switch (format) {
			case ImageFormat::R8:         return VK_FORMAT_R8_UNORM;
			case ImageFormat::RG8:        return VK_FORMAT_R8G8_UNORM;
			case ImageFormat::RGBA8:      return VK_FORMAT_R8G8B8A8_UNORM;
			case ImageFormat::BGRA8:      return VK_FORMAT_B8G8R8A8_UNORM;
			case ImageFormat::R16F:       return VK_FORMAT_R16_SFLOAT;
			case ImageFormat::RG16F:      return VK_FORMAT_R16G16_SFLOAT;
			case ImageFormat::RGBA16F:    return VK_FORMAT_R16G16B16A16_SFLOAT;
			case ImageFormat::R32F:       return VK_FORMAT_R32_SFLOAT;
			case ImageFormat::RG32F:      return VK_FORMAT_R32G32_SFLOAT;
			case ImageFormat::RGBA32F:    return VK_FORMAT_R32G32B32A32_SFLOAT;
			case ImageFormat::R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
			case ImageFormat::D32F:       return VK_FORMAT_D32_SFLOAT;
			case ImageFormat::D24S8:      return VK_FORMAT_D24_UNORM_S8_UINT;
			case ImageFormat::S8:         return VK_FORMAT_S8_UINT;
			case ImageFormat::None:
			default:                      return VK_FORMAT_UNDEFINED;
		}
	}

	// Vulkan 枚举 -> 描述层词表反查（把 surface 选定格式回填到借用的 backbuffer 描述）
	// 注意：描述层无 sRGB 变体，*_SRGB 与对应 UNORM 归一到同一 ImageFormat
	ImageFormat FromVkFormat(VkFormat format) {
		switch (format) {
			case VK_FORMAT_R8_UNORM:                 return ImageFormat::R8;
			case VK_FORMAT_R8G8_UNORM:               return ImageFormat::RG8;
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB:            return ImageFormat::RGBA8;
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB:            return ImageFormat::BGRA8;
			case VK_FORMAT_R16_SFLOAT:               return ImageFormat::R16F;
			case VK_FORMAT_R16G16_SFLOAT:             return ImageFormat::RG16F;
			case VK_FORMAT_R16G16B16A16_SFLOAT:      return ImageFormat::RGBA16F;
			case VK_FORMAT_R32_SFLOAT:               return ImageFormat::R32F;
			case VK_FORMAT_R32G32_SFLOAT:            return ImageFormat::RG32F;
			case VK_FORMAT_R32G32B32A32_SFLOAT:      return ImageFormat::RGBA32F;
			case VK_FORMAT_B10G11R11_UFLOAT_PACK32:  return ImageFormat::R11G11B10F;
			case VK_FORMAT_D32_SFLOAT:               return ImageFormat::D32F;
			case VK_FORMAT_D24_UNORM_S8_UINT:        return ImageFormat::D24S8;
			case VK_FORMAT_S8_UINT:                  return ImageFormat::S8;
			default:                                 return ImageFormat::None;
		}
	}

	VkImageType ToVkImageType(ImageViewType viewType) {
		switch (viewType) {
			case ImageViewType::Dim1D:
			case ImageViewType::Dim1DArray: return VK_IMAGE_TYPE_1D;
			case ImageViewType::Dim3D:      return VK_IMAGE_TYPE_3D;
			default:                        return VK_IMAGE_TYPE_2D;
		}
	}

	VkImageUsageFlags ToVkImageUsage(ImageUsage usage) {
		const uint32_t u = static_cast<uint32_t>(usage);
		VkImageUsageFlags flags = 0;
		if (u & static_cast<uint32_t>(ImageUsage::Sampled))         flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
		if (u & static_cast<uint32_t>(ImageUsage::Storage))         flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		if (u & static_cast<uint32_t>(ImageUsage::ColorAttachment)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (u & static_cast<uint32_t>(ImageUsage::DepthStencil))    flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		if (u & static_cast<uint32_t>(ImageUsage::TransferSrc))     flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (u & static_cast<uint32_t>(ImageUsage::TransferDst))     flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		return flags;
	}

	VkBufferUsageFlags ToVkBufferUsage(BufferUsage usage) {
		const uint32_t u = static_cast<uint32_t>(usage);
		VkBufferUsageFlags flags = 0;
		if (u & static_cast<uint32_t>(BufferUsage::Vertex))      flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if (u & static_cast<uint32_t>(BufferUsage::Index))       flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (u & static_cast<uint32_t>(BufferUsage::Uniform))     flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if (u & static_cast<uint32_t>(BufferUsage::Storage))     flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if (u & static_cast<uint32_t>(BufferUsage::TransferSrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (u & static_cast<uint32_t>(BufferUsage::TransferDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		return flags;
	}

	// 描述层 view 类型 -> VkImageViewType（建视图用；与 ToVkImageType 的 VkImageType 是两套枚举）
	VkImageViewType ToVkImageViewType(ImageViewType viewType) {
		switch (viewType) {
			case ImageViewType::Dim1D:      return VK_IMAGE_VIEW_TYPE_1D;
			case ImageViewType::Dim1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			case ImageViewType::Dim3D:      return VK_IMAGE_VIEW_TYPE_3D;
			case ImageViewType::Dim2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			case ImageViewType::Cube:       return VK_IMAGE_VIEW_TYPE_CUBE;
			case ImageViewType::CubeArray:  return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
			default:                        return VK_IMAGE_VIEW_TYPE_2D;
		}
	}

	// 真实 VkFormat -> 视图 aspect（深度/模版格式走 Depth/Stencil，其余走 Color）
	VkImageAspectFlags ToVkAspect(VkFormat format) {
		switch (format) {
			case VK_FORMAT_D32_SFLOAT:           return VK_IMAGE_ASPECT_DEPTH_BIT;
			case VK_FORMAT_S8_UINT:              return VK_IMAGE_ASPECT_STENCIL_BIT;
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:   return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default:                             return VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

	// 描述符资源类别 -> VkDescriptorType（SampledImage 走 combined image sampler，复用 RHI 默认采样器）
	VkDescriptorType ToVkDescriptorType(ResourceType type) {
		switch (type) {
			case ResourceType::SampledImage:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			case ResourceType::StorageImage:  return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case ResourceType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			case ResourceType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			default:                          return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		}
	}

	VkCompareOp ToVkCompareOp(CompareOp op) {
		switch (op) {
			case CompareOp::Never:        return VK_COMPARE_OP_NEVER;
			case CompareOp::Less:         return VK_COMPARE_OP_LESS;
			case CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
			case CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
			case CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
			case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
			default:                      return VK_COMPARE_OP_LESS;
		}
	}

	VkCullModeFlags ToVkCullMode(CullMode mode) {
		switch (mode) {
			case CullMode::None:  return VK_CULL_MODE_NONE;
			case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
			case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
			default:              return VK_CULL_MODE_BACK_BIT;
		}
	}

	// 描述层布局 -> VkImageLayout
	VkImageLayout ToVkImageLayout(ImageLayout layout) {
		switch (layout) {
			case ImageLayout::General:                return VK_IMAGE_LAYOUT_GENERAL;
			case ImageLayout::ColorAttachment:        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case ImageLayout::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case ImageLayout::ShaderReadOnly:         return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case ImageLayout::TransferSrc:            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case ImageLayout::TransferDst:            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case ImageLayout::PresentSrc:             return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case ImageLayout::Undefined:
			default:                                  return VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	// PipelineStage -> VkPipelineStageFlags2（TopOfPipe/BottomOfPipe 在 sync2 中废弃，折到 None）
	VkPipelineStageFlags2 ToVkPipelineStage(PipelineStage stage) {
		switch (stage) {
			case PipelineStage::VertexShader:       return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
			case PipelineStage::FragmentShader:     return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			case PipelineStage::EarlyFragmentTests: return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
			case PipelineStage::LateFragmentTests:  return VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			case PipelineStage::ColorAttachmentOut: return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			case PipelineStage::ComputeShader:      return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			case PipelineStage::Transfer:           return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			case PipelineStage::AllCommands:        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			case PipelineStage::TopOfPipe:
			case PipelineStage::BottomOfPipe:
			case PipelineStage::None:
			default:                                return VK_PIPELINE_STAGE_2_NONE;
		}
	}

	VkAccessFlags2 ToVkAccess(Access access) {
		VkAccessFlags2 flags = 0;
		const uint64_t a = static_cast<uint64_t>(access);
		if (a & static_cast<uint64_t>(Access::ShaderRead))           flags |= VK_ACCESS_2_SHADER_READ_BIT;
		if (a & static_cast<uint64_t>(Access::ShaderWrite))          flags |= VK_ACCESS_2_SHADER_WRITE_BIT;
		if (a & static_cast<uint64_t>(Access::ColorAttachmentRead))  flags |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
		if (a & static_cast<uint64_t>(Access::ColorAttachmentWrite)) flags |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		if (a & static_cast<uint64_t>(Access::DepthStencilRead))     flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		if (a & static_cast<uint64_t>(Access::DepthStencilWrite))    flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		if (a & static_cast<uint64_t>(Access::TransferRead))         flags |= VK_ACCESS_2_TRANSFER_READ_BIT;
		if (a & static_cast<uint64_t>(Access::TransferWrite))        flags |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
		if (a & static_cast<uint64_t>(Access::MemoryRead))           flags |= VK_ACCESS_2_MEMORY_READ_BIT;
		if (a & static_cast<uint64_t>(Access::MemoryWrite))          flags |= VK_ACCESS_2_MEMORY_WRITE_BIT;
		return flags;
	}

	VkIndexType ToVkIndexType(IndexType type) {
		return type == IndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	}

	VkAttachmentLoadOp ToVkLoadOp(LoadOp op) {
		return op == LoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
			: op == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	}

	VkAttachmentStoreOp ToVkStoreOp(StoreOp op) {
		return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
	}

	// ClearValue variant -> VkClearValue（颜色走 color 分量，深度模版走 depthStencil 分量）
	VkClearValue ToVkClearValue(const ClearValue& value) {
		VkClearValue out{};
		if (const auto* color = std::get_if<std::array<float, 4>>(&value.value)) {
			out.color = { { (*color)[0], (*color)[1], (*color)[2], (*color)[3] } };
		} else if (const auto* ds = std::get_if<DepthStencilValue>(&value.value)) {
			out.depthStencil = { ds->depth, ds->stencil };
		}
		return out;
	}

	// 读取 SPIR-V 二进制；打不开或长度非 4 的倍数时返回空（Vulkan 要求 codeSize 为 4 的倍数）
	std::vector<uint32_t> ReadSpirv(const std::string& path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) return {};
		const std::streamsize size = file.tellg();
		if (size <= 0 || size % 4 != 0) return {};
		file.seekg(0, std::ios::beg);
		std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
		if (!file.read(reinterpret_cast<char*>(code.data()), size)) return {};
		return code;
	}

} // namespace

//创建设备（instance/physical device/logical device/queue family）
std::unique_ptr<Device> Device::Create(const DeviceDesc& desc)
{
	auto device = std::make_unique<Device>();
	// nativeWindowHandle 非空时启用表面 + 交换链路径；为空则纯离屏（无 surface / swapchain）

	// ---- 校验层（不可用则降级为无校验）----
	const bool validation = desc.enableValidation && HasValidationLayer();

	// ---- VkInstance ----
	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "Engine_0";
	app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	app.pEngineName = "dEngine";
	app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
	app.apiVersion = VK_API_VERSION_1_3;

	std::vector<const char*> layers;
	std::vector<const char*> extensions;
	if (validation) {
		layers.push_back(kValidationLayer);
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}
	// 有窗口句柄时启用表面扩展（Win32 平台固定可用）
	if (desc.nativeWindowHandle) {
		extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
		extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
	}

	VkInstanceCreateInfo instanceCI{};
	instanceCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceCI.pApplicationInfo = &app;
	instanceCI.enabledLayerCount = static_cast<uint32_t>(layers.size());
	instanceCI.ppEnabledLayerNames = layers.data();
	instanceCI.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	instanceCI.ppEnabledExtensionNames = extensions.data();

	if (vkCreateInstance(&instanceCI, nullptr, &device->m_instance) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateInstance 失败\n";
		return nullptr;
	}

	// ---- 调试信使（仅在开启校验时）----
	if (validation) {
		VkDebugUtilsMessengerCreateInfoEXT messengerCI{};
		messengerCI.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		messengerCI.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messengerCI.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messengerCI.pfnUserCallback = &DebugCallback;

		auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(device->m_instance, "vkCreateDebugUtilsMessengerEXT"));
		if (createMessenger) {
			createMessenger(device->m_instance, &messengerCI, nullptr, &device->m_debugMessenger);
		}
	}

	// ---- 表面（仅窗口路径创建；离屏/测试路径跳过）----
	if (desc.nativeWindowHandle) {
		VkWin32SurfaceCreateInfoKHR surfaceCI{};
		surfaceCI.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		surfaceCI.hinstance = GetModuleHandle(nullptr);
		surfaceCI.hwnd = static_cast<HWND>(desc.nativeWindowHandle);
		if (vkCreateWin32SurfaceKHR(device->m_instance, &surfaceCI, nullptr, &device->m_surface) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateWin32SurfaceKHR 失败\n";
			return nullptr;
		}
		device->m_windowWidth = desc.width;
		device->m_windowHeight = desc.height;
	}

	// ---- 物理设备（须支持图形队列；有表面时还须支持呈现与交换链扩展）----
	device->m_physicalDevice = PickPhysicalDevice(device->m_instance, device->m_surface,
		device->m_graphicsQueueFamily, device->m_presentQueueFamily,
		device->m_computeQueueFamily, device->m_transferQueueFamily);
	if (device->m_physicalDevice == VK_NULL_HANDLE) {
		std::cerr << "[RHI] 未找到支持图形队列的物理设备\n";
		return nullptr;
	}

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(device->m_physicalDevice, &props);
	device->m_deviceName = props.deviceName;
	device->m_apiVersion = props.apiVersion;

	// ---- 逻辑设备 + 队列（图形必选；呈现/compute/transfer 族去重后各建一条）（1.3 核心：dynamic rendering + synchronization2）----
	std::vector<uint32_t> families;
	auto pushFamily = [&families](uint32_t f) {
		for (uint32_t existing : families) if (existing == f) return;
		families.push_back(f);
	};
	pushFamily(device->m_graphicsQueueFamily);
	pushFamily(device->m_computeQueueFamily);   // 兜底时与 graphics 同族，去重后跳过
	pushFamily(device->m_transferQueueFamily);
	if (device->m_presentQueueFamily != device->m_graphicsQueueFamily) {
		pushFamily(device->m_presentQueueFamily);
	}

	float priority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueCIs;
	for (uint32_t family : families) {
		VkDeviceQueueCreateInfo queueCI{};
		queueCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCI.queueFamilyIndex = family;
		queueCI.queueCount = 1;
		queueCI.pQueuePriorities = &priority;
		queueCIs.push_back(queueCI);
	}

	std::vector<const char*> deviceExtensions;
	if (device->m_surface != VK_NULL_HANDLE) {
		deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}

	VkPhysicalDeviceVulkan13Features features13{};
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13.dynamicRendering = VK_TRUE;
	features13.synchronization2 = VK_TRUE;

	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &features13;

	VkDeviceCreateInfo deviceCI{};
	deviceCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceCI.pNext = &features2;
	deviceCI.queueCreateInfoCount = static_cast<uint32_t>(queueCIs.size());
	deviceCI.pQueueCreateInfos = queueCIs.data();
	deviceCI.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	deviceCI.ppEnabledExtensionNames = deviceExtensions.data();

	if (vkCreateDevice(device->m_physicalDevice, &deviceCI, nullptr, &device->m_device) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateDevice 失败\n";
		return nullptr;
	}
	vkGetDeviceQueue(device->m_device, device->m_graphicsQueueFamily, 0, &device->m_graphicsQueue);
	// 呈现与图形同族时取到的是同一队列句柄
	vkGetDeviceQueue(device->m_device, device->m_presentQueueFamily, 0, &device->m_presentQueue);
	// compute/transfer 兜底与图形同族时同样取到同一队列句柄
	vkGetDeviceQueue(device->m_device, device->m_computeQueueFamily, 0, &device->m_computeQueue);
	vkGetDeviceQueue(device->m_device, device->m_transferQueueFamily, 0, &device->m_transferQueue);

	// ---- VMA 分配器 ----
	VmaVulkanFunctions vulkanFunctions{};
	vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo allocatorCI{};
	allocatorCI.instance = device->m_instance;
	allocatorCI.physicalDevice = device->m_physicalDevice;
	allocatorCI.device = device->m_device;
	allocatorCI.vulkanApiVersion = VK_API_VERSION_1_3;
	allocatorCI.pVulkanFunctions = &vulkanFunctions;

	if (vmaCreateAllocator(&allocatorCI, &device->m_allocator) != VK_SUCCESS) {
		std::cerr << "[RHI] vmaCreateAllocator 失败\n";
		return nullptr;
	}

	// ---- descriptor pool（带 FREE 位，允许单个集合归还；容量按类型留足）----
	const VkDescriptorPoolSize poolSizes[] = {
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1024 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1024 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1024 },
	};
	VkDescriptorPoolCreateInfo poolCI{};
	poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolCI.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolCI.maxSets = 1024;
	poolCI.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
	poolCI.pPoolSizes = poolSizes;
	if (vkCreateDescriptorPool(device->m_device, &poolCI, nullptr, &device->m_descriptorPool) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateDescriptorPool 失败\n";
		return nullptr;
	}

	// ---- 默认采样器（SampledImage 复用）：线性过滤 + 重复寻址 ----
	VkSamplerCreateInfo samplerCI{};
	samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerCI.maxLod = VK_LOD_CLAMP_NONE;
	if (vkCreateSampler(device->m_device, &samplerCI, nullptr, &device->m_defaultSampler) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateSampler 失败\n";
		return nullptr;
	}

	// ---- 管线缓存（跨管线复用编译结果）----
	VkPipelineCacheCreateInfo cacheCI{};
	cacheCI.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (vkCreatePipelineCache(device->m_device, &cacheCI, nullptr, &device->m_pipelineCache) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreatePipelineCache 失败\n";
		return nullptr;
	}

	// ---- 帧槽（数量来自 DeviceDesc，钳制 [1,8]；创建期决定，运行期不变更）----
	// 命令池覆盖 graphics/compute/transfer 三类（present 族不录命令，不建池）
	std::vector<uint32_t> slotFamilies;
	auto pushSlotFamily = [&slotFamilies](uint32_t f) {
		for (uint32_t existing : slotFamilies) if (existing == f) return;
		slotFamilies.push_back(f);
	};
	pushSlotFamily(device->m_graphicsQueueFamily);
	pushSlotFamily(device->m_computeQueueFamily);
	pushSlotFamily(device->m_transferQueueFamily);

	const uint32_t slotCount = std::clamp(desc.maxFramesInFlight, 1u, 8u);
	device->m_frameSlots.reserve(slotCount);
	for (uint32_t i = 0; i < slotCount; ++i) {
		auto slot = std::unique_ptr<FrameSlot>(new FrameSlot(device->m_device, slotFamilies));
		if (slot->m_pools.size() != slotFamilies.size() || slot->m_fence == VK_NULL_HANDLE) {
			std::cerr << "[RHI] 帧槽创建失败（slot " << i << "）\n";
			return nullptr;
		}
		device->m_frameSlots.push_back(std::move(slot));
	}

	return device;
}

Device::~Device()
{
	// 兜底：用户未等完在飞提交就销毁设备时，先等设备空闲（后续销毁均要求无在飞命令）
	if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);
	// 逆序清理（帧槽 / descriptor pool / 采样器 / 管线缓存须先于逻辑设备销毁；
	// 帧槽虽是成员 vector，但若不在此显式清空，其析构将晚于 vkDestroyDevice）
	m_frameSlots.clear();
	if (m_descriptorPool) {
		vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
		m_descriptorPool = VK_NULL_HANDLE;
	}
	if (m_defaultSampler) {
		vkDestroySampler(m_device, m_defaultSampler, nullptr);
		m_defaultSampler = VK_NULL_HANDLE;
	}
	if (m_pipelineCache) {
		vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
		m_pipelineCache = VK_NULL_HANDLE;
	}
	if (m_allocator) {
		vmaDestroyAllocator(m_allocator);
		m_allocator = nullptr;
	}
	if (m_device) {
		vkDestroyDevice(m_device, nullptr);
		m_device = VK_NULL_HANDLE;
	}
	if (m_surface && m_instance) {
		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		m_surface = VK_NULL_HANDLE;
	}
	if (m_debugMessenger && m_instance) {
		auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
		if (destroyMessenger) destroyMessenger(m_instance, m_debugMessenger, nullptr);
		m_debugMessenger = VK_NULL_HANDLE;
	}
	if (m_instance) {
		vkDestroyInstance(m_instance, nullptr);
		m_instance = VK_NULL_HANDLE;
	}
}

const char* Device::DeviceName() const { return m_deviceName.c_str(); }
uint32_t    Device::ApiVersion() const { return m_apiVersion; }

#pragma region Image // 图像资源 / 视图

ImageView::ImageView(VkDevice device, VkImageView view)
	: m_device(device), m_view(view) {}

ImageView::~ImageView()
{
	if (m_device && m_view != VK_NULL_HANDLE) {
		vkDestroyImageView(m_device, m_view, nullptr);
		m_view = VK_NULL_HANDLE;
	}
}

VkImageView ImageView::NativeHandle() const { return m_view; }

Image::Image(VkDevice device, VkFormat format, VmaAllocator_T* allocator, VkImage image,
	VmaAllocation_T* allocation, const ImageCreateInfo& desc, bool borrowed)
	: m_desc(desc), m_device(device), m_format(format), m_image(image),
	  m_allocation(allocation), m_allocator(allocator), m_borrowed(borrowed) {}

Image::~Image()
{
	// 视图先于底层图像销毁（视图由 RHI 创建，借用图像同样如此）
	m_views.clear();

	// 借用（交换链 backbuffer / 外部导入）不拥有底层句柄，跳过销毁
	if (m_borrowed || !m_allocator || m_image == VK_NULL_HANDLE) return;
	vmaDestroyImage(m_allocator, m_image, m_allocation);
	m_image = VK_NULL_HANDLE;
	m_allocation = nullptr;
}

std::unique_ptr<Image> Image::Borrow(VkDevice device, VkImage image, VkFormat format,
	const ImageCreateInfo& desc)
{
	return std::unique_ptr<Image>(new Image(device, format, nullptr, image, nullptr, desc, true));
}

// 显式注册视图：按 (mipLevel, layer) 去重，命中直接返回同一引用；视图随 Image 析构统一销毁
const ImageView& Image::CreateView(ImageSubresource sub) const
{
	const uint64_t key = (static_cast<uint64_t>(sub.mipLevel) << 32) | sub.layer;
	if (auto it = m_views.find(key); it != m_views.end()) return *it->second;

	VkImageViewCreateInfo viewCI{};
	viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewCI.image = m_image;
	viewCI.viewType = ToVkImageViewType(m_desc.viewType);
	viewCI.format = m_format;
	viewCI.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
	viewCI.subresourceRange.aspectMask = ToVkAspect(m_format);
	viewCI.subresourceRange.baseMipLevel = sub.mipLevel;
	viewCI.subresourceRange.levelCount = 1;
	viewCI.subresourceRange.baseArrayLayer = sub.layer;
	viewCI.subresourceRange.layerCount = 1;

	VkImageView handle = VK_NULL_HANDLE;
	if (m_device == VK_NULL_HANDLE || m_image == VK_NULL_HANDLE ||
		vkCreateImageView(m_device, &viewCI, nullptr, &handle) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateImageView 失败\n";
		handle = VK_NULL_HANDLE;
	}

	// 用 node 容器承载，先取引用再 move：move unique_ptr 不搬移所指对象，引用保持有效
	auto view = std::unique_ptr<ImageView>(new ImageView(m_device, handle));
	const ImageView& ref = *view;
	m_views.emplace(key, std::move(view));
	return ref;
}

std::unique_ptr<Image> Device::CreateImage(const ImageCreateInfo& info)
{
	VkImageCreateInfo imageCI{};
	imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageCI.imageType = ToVkImageType(info.viewType);
	imageCI.format = ToVkFormat(info.format);
	imageCI.extent = { info.width, info.height, info.depth };
	imageCI.mipLevels = info.mipLevels;
	imageCI.arrayLayers = info.arrayLayers;
	imageCI.samples = static_cast<VkSampleCountFlagBits>(info.samples);
	imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageCI.usage = ToVkImageUsage(info.usage);
	imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	// 图像一律走设备本地内存（VMA 按 usage 自动选堆）
	VmaAllocationCreateInfo allocCI{};
	allocCI.usage = VMA_MEMORY_USAGE_AUTO;

	VkImage       image = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	if (vmaCreateImage(m_allocator, &imageCI, &allocCI, &image, &allocation, nullptr) != VK_SUCCESS) {
		std::cerr << "[RHI] vmaCreateImage 失败\n";
		return nullptr;
	}

	return std::unique_ptr<Image>(new Image(m_device, imageCI.format, m_allocator, image, allocation, info, false));
}

VkImage                Image::NativeHandle() const { return m_image; }
VkFormat               Image::NativeFormat() const { return m_format; }
const ImageCreateInfo& Image::Desc() const { return m_desc; }

#pragma endregion

#pragma region Buffer // 缓冲资源

Buffer::Buffer(VmaAllocator_T* allocator, VkBuffer buffer, VmaAllocation_T* allocation,
	const BufferCreateInfo& desc, void* mapped)
	: m_desc(desc), m_buffer(buffer), m_allocation(allocation), m_allocator(allocator), m_mapped(mapped) {}

Buffer::~Buffer()
{
	// 主机可见缓冲走 VMA 的持久映射（MAPPED_BIT），解除映射由 VMA 在销毁时统一处理
	if (!m_allocator || m_buffer == VK_NULL_HANDLE) return;
	vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
	m_buffer = VK_NULL_HANDLE;
	m_allocation = nullptr;
	m_mapped = nullptr;
}

std::unique_ptr<Buffer> Device::CreateBuffer(const BufferCreateInfo& info)
{
	VkBufferCreateInfo bufferCI{};
	bufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCI.size = info.size;
	bufferCI.usage = ToVkBufferUsage(info.usage);
	bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocCI{};
	allocCI.usage = VMA_MEMORY_USAGE_AUTO;
	VmaAllocationInfo allocInfo{};
	if (info.hostVisible) {
		// 顺序写入 + 持久映射：Map() 直接返回已映射指针
		allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
			| VMA_ALLOCATION_CREATE_MAPPED_BIT;
	}

	VkBuffer      buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	if (vmaCreateBuffer(m_allocator, &bufferCI, &allocCI, &buffer, &allocation, &allocInfo) != VK_SUCCESS) {
		std::cerr << "[RHI] vmaCreateBuffer 失败\n";
		return nullptr;
	}

	return std::unique_ptr<Buffer>(new Buffer(m_allocator, buffer, allocation, info, allocInfo.pMappedData));
}

// 持久映射：hostVisible 缓冲在创建时即映射，这里直接返回缓存指针；非 hostVisible 返回空
void* Buffer::Map()
{
	return m_mapped;
}

VkBuffer                Buffer::NativeHandle() const { return m_buffer; }
const BufferCreateInfo& Buffer::Desc() const { return m_desc; }

#pragma endregion

#pragma region Sync // 同步原语

Semaphore::Semaphore(VkDevice device, VkSemaphore semaphore)
	: m_device(device), m_semaphore(semaphore) {}

Semaphore::~Semaphore()
{
	if (m_device && m_semaphore != VK_NULL_HANDLE) {
		vkDestroySemaphore(m_device, m_semaphore, nullptr);
		m_semaphore = VK_NULL_HANDLE;
	}
}

VkSemaphore Semaphore::NativeHandle() const { return m_semaphore; }

Fence::Fence(VkDevice device, VkFence fence)
	: m_device(device), m_fence(fence) {}

Fence::~Fence()
{
	if (m_device && m_fence != VK_NULL_HANDLE) {
		vkDestroyFence(m_device, m_fence, nullptr);
		m_fence = VK_NULL_HANDLE;
	}
}

void Fence::Wait()
{
	if (vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
		std::cerr << "[RHI] vkWaitForFences 失败\n";
	}
}

void Fence::Reset()
{
	if (vkResetFences(m_device, 1, &m_fence) != VK_SUCCESS) {
		std::cerr << "[RHI] vkResetFences 失败\n";
	}
}

std::unique_ptr<Fence> Device::CreateFence(bool signaled)
{
	VkFenceCreateInfo fenceCI{};
	fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceCI.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;

	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(m_device, &fenceCI, nullptr, &fence) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateFence 失败\n";
		return nullptr;
	}
	return std::unique_ptr<Fence>(new Fence(m_device, fence));
}

std::unique_ptr<Semaphore> Device::CreateSemaphore()
{
	VkSemaphoreCreateInfo semaphoreCI{};
	semaphoreCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkSemaphore semaphore = VK_NULL_HANDLE;
	if (vkCreateSemaphore(m_device, &semaphoreCI, nullptr, &semaphore) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateSemaphore 失败\n";
		return nullptr;
	}
	return std::unique_ptr<Semaphore>(new Semaphore(m_device, semaphore));
}

#pragma endregion

#pragma region Swapchain // 交换链

// 纯构建：查询表面能力、选定格式与尺寸、创建交换链并取回 backbuffer（首次与重建共用）
bool Swapchain::Build(const Context& context, VkSwapchainKHR oldSwapchain, BuildResult& result)
{
	// ---- 表面能力与格式 ----
	VkSurfaceCapabilitiesKHR caps{};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context.physicalDevice, context.surface, &caps);

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(context.physicalDevice, context.surface, &formatCount, nullptr);
	if (formatCount == 0) {
		std::cerr << "[RHI] 表面无可用格式\n";
		return false;
	}
	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(context.physicalDevice, context.surface, &formatCount, formats.data());

	// 优先 B8G8R8A8_UNORM + SRGB_NONLINEAR（描述层词表无 sRGB 变体，Format() 会归一化为 BGRA8，
	// 优选 UNORM 使管线附件格式签名与实际交换链格式一致）；否则取首个
	VkSurfaceFormatKHR chosen = formats[0];
	for (const auto& f : formats) {
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			chosen = f;
			break;
		}
	}
	// 旧式单值 UNDEFINED 报告：按常用 8 位 BGRA 处理
	if (formatCount == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
		chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
		chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	}

	// ---- 范围：优先表面当前尺寸；无定义时按窗口尺寸夹到能力区间 ----
	VkExtent2D extent = caps.currentExtent;
	if (extent.width == UINT32_MAX) {
		extent.width = std::clamp(context.windowWidth, caps.minImageExtent.width, caps.maxImageExtent.width);
		extent.height = std::clamp(context.windowHeight, caps.minImageExtent.height, caps.maxImageExtent.height);
	}
	// 最小化 / 尺寸为 0：无法创建交换链，本次不重建（等下一帧尺寸恢复）
	if (extent.width == 0 || extent.height == 0) {
		return false;
	}

	// ---- 图像数：最小 +1（避免等垂直同步），不超过上限 ----
	uint32_t imageCount = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

	// ---- 创建（presentMode 固定 FIFO，规范保证始终可用）----
	VkSwapchainCreateInfoKHR swapchainCI{};
	swapchainCI.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchainCI.surface = context.surface;
	swapchainCI.minImageCount = imageCount;
	swapchainCI.imageFormat = chosen.format;
	swapchainCI.imageColorSpace = chosen.colorSpace;
	swapchainCI.imageExtent = extent;
	swapchainCI.imageArrayLayers = 1;
	swapchainCI.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	swapchainCI.preTransform = caps.currentTransform;
	swapchainCI.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchainCI.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	swapchainCI.clipped = VK_TRUE;
	swapchainCI.oldSwapchain = oldSwapchain;

	uint32_t families[2] = { context.graphicsFamily, context.presentFamily };
	if (context.graphicsFamily != context.presentFamily) {
		// 图形与呈现在不同队列族：并发共享，避免上层自行处理队列所有权转移
		swapchainCI.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		swapchainCI.queueFamilyIndexCount = 2;
		swapchainCI.pQueueFamilyIndices = families;
	} else {
		swapchainCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	if (vkCreateSwapchainKHR(context.device, &swapchainCI, nullptr, &result.swapchain) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateSwapchainKHR 失败\n";
		result.swapchain = VK_NULL_HANDLE;
		return false;
	}

	// ---- backbuffer：逐张包成非拥有 Image（借用，析构不销毁 VkImage）----
	uint32_t imageCountActual = 0;
	vkGetSwapchainImagesKHR(context.device, result.swapchain, &imageCountActual, nullptr);
	std::vector<VkImage> images(imageCountActual);
	vkGetSwapchainImagesKHR(context.device, result.swapchain, &imageCountActual, images.data());

	ImageCreateInfo backbufferDesc;
	backbufferDesc.width = extent.width;
	backbufferDesc.height = extent.height;
	backbufferDesc.format = FromVkFormat(chosen.format);
	backbufferDesc.viewType = ImageViewType::Dim2D;
	backbufferDesc.usage = ImageUsage::ColorAttachment;

	result.images.clear();
	result.images.reserve(images.size());
	for (VkImage image : images) {
		// 真实格式原样传入（backbuffer 常为 SRGB，视图格式须与图像一致）
		result.images.push_back(Image::Borrow(context.device, image, chosen.format, backbufferDesc));
	}

	result.surfaceFormat = chosen;
	result.extent = extent;
	return true;
}

// 创建 count 个 acquire 信号量；任一失败返回空（表示失败，已创建的由 unique_ptr 释放）
std::vector<std::unique_ptr<Semaphore>> Swapchain::CreateAcquireSemaphores(VkDevice device, uint32_t count)
{
	std::vector<std::unique_ptr<Semaphore>> semaphores;
	semaphores.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		VkSemaphoreCreateInfo semaphoreCI{};
		semaphoreCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkSemaphore semaphore = VK_NULL_HANDLE;
		if (vkCreateSemaphore(device, &semaphoreCI, nullptr, &semaphore) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateSemaphore 失败\n";
			return {};
		}
		semaphores.push_back(std::unique_ptr<Semaphore>(new Semaphore(device, semaphore)));
	}
	return semaphores;
}

Swapchain::Swapchain(const Context& context, BuildResult&& built,
	std::vector<std::unique_ptr<Semaphore>> acquireSemaphores, uint32_t maxFramesInFlight)
	: m_context(context), m_swapchain(built.swapchain), m_surfaceFormat(built.surfaceFormat),
	  m_extent(built.extent), m_images(std::move(built.images)),
	  m_acquireSemaphores(std::move(acquireSemaphores)), maxFrameInFlight(maxFramesInFlight) {
	// 渲染完成信号量按 image 数建：同一 image 重新 acquire 前其上次 present 必已完成，
	// 此刻 signal 复用才安全（单一信号量会在 present 消费完成前被下一帧再次 signal）
	m_renderDone = CreateAcquireSemaphores(m_context.device, static_cast<uint32_t>(m_images.size()));
	if (m_renderDone.size() != m_images.size()) {
		std::cerr << "[RHI] 渲染完成信号量创建失败\n";
		m_renderDone.clear(); // 降级：presentReady 为空，Present 不等待
	}
}

Swapchain::~Swapchain()
{
	// renderDone 的 wait 由呈现引擎消费，帧回收的 fence 等不到它：销毁前先等设备完全空闲
	if (m_context.device) vkDeviceWaitIdle(m_context.device);
	// 逆序：先释放借用图像包装（不销毁底层 VkImage），再销毁交换链，最后释放信号量
	m_images.clear();
	if (m_context.device && m_swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(m_context.device, m_swapchain, nullptr);
		m_swapchain = VK_NULL_HANDLE;
	}
	m_acquireSemaphores.clear();
}

// 自动重建：先等设备空闲——既确认旧链图像 / 旧信号量不再被引用，也让旧 acquire 信号量可安全销毁
bool Swapchain::Recreate()
{
	vkDeviceWaitIdle(m_context.device);

	// 新链以旧链为 oldSwapchain 复用（降低资源峰值）；失败则保留现状，下一帧再试
	BuildResult built;
	if (!Build(m_context, m_swapchain, built)) return false;

	// acquire 信号量一并重造：旧的可能留有 acquire 置位但未被 Submit 消费的信号，无法复用
	auto acquireSemaphores = CreateAcquireSemaphores(m_context.device, maxFrameInFlight);
	if (acquireSemaphores.size() != maxFrameInFlight) {
		vkDestroySwapchainKHR(m_context.device, built.swapchain, nullptr);
		return false;
	}

	// renderDone 同步重造：image 数可能已随新链尺寸变化
	auto renderDone = CreateAcquireSemaphores(m_context.device, static_cast<uint32_t>(built.images.size()));
	if (renderDone.size() != built.images.size()) {
		vkDestroySwapchainKHR(m_context.device, built.swapchain, nullptr);
		return false;
	}

	m_images.clear();
	vkDestroySwapchainKHR(m_context.device, m_swapchain, nullptr);

	m_swapchain = built.swapchain;
	m_surfaceFormat = built.surfaceFormat;
	m_extent = built.extent;
	m_images = std::move(built.images);
	m_acquireSemaphores = std::move(acquireSemaphores);
	m_renderDone = std::move(renderDone);

	m_frameIndex = 0;
	m_recreatePending = false;
	return true;
}

// 表面尺寸是否已与交换链不符：部分驱动（实测 NVIDIA/Win32）在窗口 resize 后仍返回 VK_SUCCESS，
// 因此不能只依赖 acquire 返回值，需按表面能力主动比对。currentExtent 无定义时无法据此判断。
bool Swapchain::NeedsRecreate() const
{
	VkSurfaceCapabilitiesKHR caps{};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_context.physicalDevice, m_context.surface, &caps);
	if (caps.currentExtent.width == UINT32_MAX) return false;
	return caps.currentExtent.width != m_extent.width || caps.currentExtent.height != m_extent.height;
}

std::unique_ptr<Swapchain> Device::CreateSwapchain()
{
	if (m_surface == VK_NULL_HANDLE) {
		std::cerr << "[RHI] 无表面（创建 Device 时未提供窗口句柄），无法创建交换链\n";
		return nullptr;
	}

	Swapchain::Context context;
	context.device = m_device;
	context.presentQueue = m_presentQueue;
	context.physicalDevice = m_physicalDevice;
	context.surface = m_surface;
	context.graphicsFamily = m_graphicsQueueFamily;
	context.presentFamily = m_presentQueueFamily;
	context.windowWidth = m_windowWidth;
	context.windowHeight = m_windowHeight;
	context.maxFramesInFlight = static_cast<uint32_t>(m_frameSlots.size()); // acquire 信号量轮转数与 Device 帧槽数一致

	Swapchain::BuildResult built;
	if (!Swapchain::Build(context, VK_NULL_HANDLE, built)) return nullptr;

	const uint32_t framesInFlight = context.maxFramesInFlight;
	auto acquireSemaphores = Swapchain::CreateAcquireSemaphores(m_device, framesInFlight);
	if (acquireSemaphores.size() != framesInFlight) {
		vkDestroySwapchainKHR(m_device, built.swapchain, nullptr);
		return nullptr;
	}

	return std::unique_ptr<Swapchain>(new Swapchain(context, std::move(built),
		std::move(acquireSemaphores), framesInFlight));
}

Swapchain::Frame Swapchain::AcquireNextImage()
{
	Frame frame;              // acquired=false；交换链失效时上层跳过渲染
	m_lastFrame = Frame{};    // 清空上一帧记录，成功获取时再回填

	// 重建判定有两路来源：
	//   1) 显式挂起标志（suboptimal / present 失配）
	//   2) 表面尺寸已与交换链不符——部分驱动（实测 NVIDIA/Win32）resize 后仍返回 VK_SUCCESS，
	//      不能只依赖 acquire 返回值，须主动比对表面能力
	// 重建成功则本帧无图像（acquired=false），上层跳过渲染；重建失败（如最小化）同样跳过，下次调用再试
	if (m_recreatePending || NeedsRecreate()) {
		Recreate();
		return frame;
	}

	VkSemaphore acquire = m_acquireSemaphores[m_frameIndex]->NativeHandle();

	uint32_t imageIndex = 0;
	VkResult result = vkAcquireNextImageKHR(m_context.device, m_swapchain, UINT64_MAX,
		acquire, VK_NULL_HANDLE, &imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		// 表面已失效：本帧没有未呈现的图像，立即重建；本帧由上层跳过
		Recreate();
		return frame;
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		std::cerr << "[RHI] vkAcquireNextImageKHR 失败: " << result << "\n";
		return frame;
	}
	// SUBOPTIMAL：图像仍可用（acquire 信号量已置位），本帧照常渲染，重建推迟到下次获取前
	if (result == VK_SUBOPTIMAL_KHR) m_recreatePending = true;

	frame.imageIndex = imageIndex;
	frame.backbuffer = m_images[imageIndex].get();
	frame.ready = m_acquireSemaphores[m_frameIndex].get(); // 与上面 acquire 用的是同一槽（尚未轮转）
	frame.presentReady = imageIndex < m_renderDone.size() ? m_renderDone[imageIndex].get() : nullptr;
	frame.acquired = true;
	m_lastFrame = frame;
	m_frameIndex = (m_frameIndex + 1) % maxFrameInFlight;
	return frame;
}

void Swapchain::Present(const Semaphore* presentWait)
{
	if (!m_lastFrame.acquired) return; // 未获取到图像（交换链失效），无需呈现

	// 等待信号量：外部传入优先；否则用当帧 image 对应的内部渲染完成信号量（Submit 经 signals 产出）
	VkSemaphore wait = presentWait ? presentWait->NativeHandle()
		: (m_lastFrame.presentReady ? m_lastFrame.presentReady->NativeHandle() : VK_NULL_HANDLE);

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = wait ? 1u : 0u;
	presentInfo.pWaitSemaphores = wait ? &wait : nullptr;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &m_swapchain;
	presentInfo.pImageIndices = &m_lastFrame.imageIndex;

	VkResult result = vkQueuePresentKHR(m_context.presentQueue, &presentInfo);

	m_lastFrame = Frame{};

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		Recreate();               // 表面失效：图像已交给呈现引擎，立即重建
	} else if (result == VK_SUBOPTIMAL_KHR) {
		m_recreatePending = true; // 仍可呈现：推迟到下次获取前重建
	} else if (result != VK_SUCCESS) {
		std::cerr << "[RHI] vkQueuePresentKHR 失败: " << result << "\n";
	}
}

ImageFormat Swapchain::Format() const { return FromVkFormat(m_surfaceFormat.format); }
uint32_t    Swapchain::Width()  const { return m_extent.width; }
uint32_t    Swapchain::Height() const { return m_extent.height; }

#pragma endregion

#pragma region Descriptor // 描述符布局 / 集合

DescriptorSetLayout::DescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
	std::vector<DescriptorBinding> bindings)
	: m_device(device), m_layout(layout), m_bindings(std::move(bindings)) {}

DescriptorSetLayout::~DescriptorSetLayout()
{
	if (m_device && m_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
		m_layout = VK_NULL_HANDLE;
	}
}

VkDescriptorSetLayout DescriptorSetLayout::NativeHandle() const { return m_layout; }
const std::vector<DescriptorBinding>& DescriptorSetLayout::Bindings() const { return m_bindings; }

DescriptorSet::DescriptorSet(VkDevice device, VkDescriptorPool pool, VkSampler defaultSampler,
	VkDescriptorSet set, std::unordered_map<uint32_t, VkDescriptorType> bindingTypes)
	: m_device(device), m_pool(pool), m_defaultSampler(defaultSampler), m_set(set),
	  m_bindingTypes(std::move(bindingTypes)) {}

DescriptorSet::~DescriptorSet()
{
	// 归还单个集合：须确保引用它的命令缓冲已完成（池创建时带 FREE 位）
	if (m_device && m_pool && m_set != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(m_device, m_pool, 1, &m_set);
		m_set = VK_NULL_HANDLE;
	}
}

VkDescriptorSet DescriptorSet::NativeHandle() const { return m_set; }

// 图像绑定：写入持久视图 + RHI 内置默认采样器（combined image sampler）
void DescriptorSet::Write(uint32_t binding, const Image& image, ImageSubresource view)
{
	const ImageView& imageView = image.CreateView(view);
	if (imageView.NativeHandle() == VK_NULL_HANDLE) {
		std::cerr << "[RHI] 描述符写入失败：图像视图无效（binding " << binding << "）\n";
		return;
	}

	VkDescriptorImageInfo imageInfo{};
	imageInfo.sampler = m_defaultSampler;
	imageInfo.imageView = imageView.NativeHandle();
	// 固定只读布局：实际绘制前须由 barrier 把资源转到该布局
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_set;
	write.dstBinding = binding;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;

	vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

// 缓冲绑定：range 为 0 表示到缓冲末尾；类型由分配时记录的 binding 表决定
void DescriptorSet::Write(uint32_t binding, const Buffer& buffer, uint64_t offset, uint64_t range)
{
	VkDescriptorBufferInfo bufferInfo{};
	bufferInfo.buffer = buffer.NativeHandle();
	bufferInfo.offset = offset;
	bufferInfo.range = range;

	VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	if (auto it = m_bindingTypes.find(binding); it != m_bindingTypes.end()) {
		if (it->second == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	}

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_set;
	write.dstBinding = binding;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = type;
	write.pBufferInfo = &bufferInfo;

	vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

std::unique_ptr<DescriptorSet> Device::AllocateDescriptorSet(const DescriptorSetLayout& layout)
{
	VkDescriptorSetLayout rawLayout = layout.NativeHandle();

	VkDescriptorSetAllocateInfo allocCI{};
	allocCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocCI.descriptorPool = m_descriptorPool;
	allocCI.descriptorSetCount = 1;
	allocCI.pSetLayouts = &rawLayout;

	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(m_device, &allocCI, &set) != VK_SUCCESS) {
		std::cerr << "[RHI] vkAllocateDescriptorSets 失败\n";
		return nullptr;
	}

	// 记录 binding -> 类型，供缓冲区写接口选择正确的 VkDescriptorType
	std::unordered_map<uint32_t, VkDescriptorType> bindingTypes;
	for (const auto& b : layout.Bindings()) bindingTypes[b.binding] = ToVkDescriptorType(b.type);

	return std::unique_ptr<DescriptorSet>(new DescriptorSet(m_device, m_descriptorPool,
		m_defaultSampler, set, std::move(bindingTypes)));
}

#pragma endregion

#pragma region Pipeline // 管线与管线布局

Pipeline::Pipeline(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout, uint32_t pushConstantSize,
	std::vector<std::unique_ptr<DescriptorSetLayout>> setLayouts)
	: m_device(device), m_pipeline(pipeline), m_layout(layout)
	, m_pushConstantSize(pushConstantSize), m_setLayouts(std::move(setLayouts)) {}

Pipeline::~Pipeline()
{
	// 顺序：管线 -> 管线布局；集合布局由 m_setLayouts 成员析构自动释放
	if (m_device && m_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(m_device, m_pipeline, nullptr);
		m_pipeline = VK_NULL_HANDLE;
	}
	if (m_device && m_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(m_device, m_layout, nullptr);
		m_layout = VK_NULL_HANDLE;
	}
}

VkPipeline Pipeline::NativeHandle() const { return m_pipeline; }

VkPipelineLayout Pipeline::NativeLayout() const { return m_layout; }

uint32_t Pipeline::PushConstantSize() const { return m_pushConstantSize; }

const DescriptorSetLayout* Pipeline::SetLayout(uint32_t setIndex) const
{
	return setIndex < m_setLayouts.size() ? m_setLayouts[setIndex].get() : nullptr;
}

// dynamic rendering 下的图形管线：多态渲染无需 VkRenderPass，附件格式经 VkPipelineRenderingCreateInfo 声明
std::unique_ptr<Pipeline> Device::CreatePipeline(const GraphicsPipelineDesc& desc)
{
	// ---- 按 set 分组（组内 binding 升序，保证布局可预测）----
	uint32_t setCount = 0;
	for (const auto& b : desc.bindings) setCount = std::max(setCount, b.set + 1);

	std::vector<std::vector<DescriptorBinding>> bySet(setCount);
	for (const auto& b : desc.bindings) bySet[b.set].push_back(b);
	for (auto& group : bySet) {
		std::sort(group.begin(), group.end(),
			[](const DescriptorBinding& a, const DescriptorBinding& c) { return a.binding < c.binding; });
	}

	// ---- 各 set 的描述符集合布局（由 Pipeline 持有）----
	std::vector<std::unique_ptr<DescriptorSetLayout>> layouts;
	std::vector<VkDescriptorSetLayout> rawLayouts;
	layouts.reserve(setCount);
	rawLayouts.reserve(setCount);
	for (uint32_t i = 0; i < setCount; ++i) {
		std::vector<VkDescriptorSetLayoutBinding> vkBindings;
		vkBindings.reserve(bySet[i].size());
		for (const auto& b : bySet[i]) {
			VkDescriptorSetLayoutBinding vb{};
			vb.binding = b.binding;
			vb.descriptorType = ToVkDescriptorType(b.type);
			vb.descriptorCount = b.arrayCount;
			vb.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS; // 绑定未携带 stage 信息，统一给图形全阶段
			vkBindings.push_back(vb);
		}

		VkDescriptorSetLayoutCreateInfo layoutCI{};
		layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutCI.bindingCount = static_cast<uint32_t>(vkBindings.size());
		layoutCI.pBindings = vkBindings.empty() ? nullptr : vkBindings.data();

		VkDescriptorSetLayout raw = VK_NULL_HANDLE;
		if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &raw) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateDescriptorSetLayout 失败（set " << i << "）\n";
			return nullptr;
		}
		rawLayouts.push_back(raw);
		layouts.push_back(std::unique_ptr<DescriptorSetLayout>(
			new DescriptorSetLayout(m_device, raw, std::move(bySet[i]))));
	}

	// ---- 管线布局（push constant：VS/FS 共享、offset 0；desc 未声明则无 range）----
	VkPushConstantRange pushRange{};
	if (desc.pushConstantSize > 0) {
		if (desc.pushConstantSize % 4 != 0 || desc.pushConstantSize > 128) {
			std::cerr << "[RHI] pushConstantSize 须 4 字节对齐且 <= 128: " << desc.pushConstantSize << "\n";
			return nullptr;
		}
		pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushRange.offset = 0;
		pushRange.size = desc.pushConstantSize;
	}

	VkPipelineLayoutCreateInfo layoutCI{};
	layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutCI.setLayoutCount = static_cast<uint32_t>(rawLayouts.size());
	layoutCI.pSetLayouts = rawLayouts.empty() ? nullptr : rawLayouts.data();
	layoutCI.pushConstantRangeCount = (desc.pushConstantSize > 0) ? 1u : 0u;
	layoutCI.pPushConstantRanges = (desc.pushConstantSize > 0) ? &pushRange : nullptr;

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &pipelineLayout) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreatePipelineLayout 失败\n";
		return nullptr;
	}

	// ---- shader 模块（离线 SPIR-V 二进制，入口固定 main）----
	auto createModule = [&](const std::string& path, VkShaderModule& out) {
		const std::vector<uint32_t> code = ReadSpirv(path);
		if (code.empty()) {
			std::cerr << "[RHI] 读取 SPIR-V 失败: " << path << "\n";
			return false;
		}
		VkShaderModuleCreateInfo moduleCI{};
		moduleCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleCI.codeSize = code.size() * sizeof(uint32_t);
		moduleCI.pCode = code.data();
		if (vkCreateShaderModule(m_device, &moduleCI, nullptr, &out) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateShaderModule 失败: " << path << "\n";
			return false;
		}
		return true;
	};

	VkShaderModule vsModule = VK_NULL_HANDLE;
	VkShaderModule psModule = VK_NULL_HANDLE;
	if (!createModule(desc.vsSpirvPath, vsModule) || !createModule(desc.psSpirvPath, psModule)) {
		if (vsModule) vkDestroyShaderModule(m_device, vsModule, nullptr);
		if (psModule) vkDestroyShaderModule(m_device, psModule, nullptr);
		vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
		return nullptr;
	}

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vsModule;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = psModule;
	stages[1].pName = "main";

	// ---- 固定功能状态 ----
	// 顶点输入留空：顶点数据由 shader 内建 gl_VertexIndex 生成（无顶点缓冲）
	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	// viewport/scissor 走动态态（CommandList::SetViewport 设置）；结构中的 count 仍须 >= 1
	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	constexpr VkDynamicState kDynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<uint32_t>(std::size(kDynamicStates));
	dynamicState.pDynamicStates = kDynamicStates;

	VkPipelineRasterizationStateCreateInfo rasterization{};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = ToVkCullMode(desc.cullMode);
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // 与 GLSL 约定一致
	rasterization.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// 无深度附件时必须关闭深度测试/写入，否则语义与实际附件不符
	const bool hasDepth = desc.depthFormat != ImageFormat::None;
	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = (hasDepth && desc.depthTestEnable) ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = (hasDepth && desc.depthWriteEnable) ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = ToVkCompareOp(desc.depthCompareOp);
	depthStencil.maxDepthBounds = 1.0f;

	// 每个颜色槽一份混合状态；数量必须等于 colorAttachmentCount
	std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
	blendAttachments.reserve(desc.colorAttachmentCount);
	for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
		VkPipelineColorBlendAttachmentState blend{};
		blend.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.colorBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blendAttachments.push_back(blend);
	}

	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
	colorBlend.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();

	// ---- dynamic rendering 的附件格式签名 ----
	std::vector<VkFormat> colorFormats;
	colorFormats.reserve(desc.colorAttachmentCount);
	for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
		colorFormats.push_back(ToVkFormat(desc.colorFormats[i]));
	}
	// D24S8 带模版分量，须同时声明；D32F 无模版，stencil 留 UNDEFINED
	const VkFormat depthFormat = hasDepth ? ToVkFormat(desc.depthFormat) : VK_FORMAT_UNDEFINED;
	const VkFormat stencilFormat = (desc.depthFormat == ImageFormat::D24S8)
		? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_UNDEFINED;

	VkPipelineRenderingCreateInfo renderingInfo{};
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
	renderingInfo.pColorAttachmentFormats = colorFormats.empty() ? nullptr : colorFormats.data();
	renderingInfo.depthAttachmentFormat = depthFormat;
	renderingInfo.stencilAttachmentFormat = stencilFormat;

	VkGraphicsPipelineCreateInfo pipelineCI{};
	pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCI.pNext = &renderingInfo;
	pipelineCI.stageCount = 2;
	pipelineCI.pStages = stages;
	pipelineCI.pVertexInputState = &vertexInput;
	pipelineCI.pInputAssemblyState = &inputAssembly;
	pipelineCI.pViewportState = &viewportState;
	pipelineCI.pRasterizationState = &rasterization;
	pipelineCI.pMultisampleState = &multisample;
	pipelineCI.pDepthStencilState = &depthStencil;
	pipelineCI.pColorBlendState = &colorBlend;
	pipelineCI.pDynamicState = &dynamicState;
	pipelineCI.layout = pipelineLayout;
	pipelineCI.renderPass = VK_NULL_HANDLE;      // dynamic rendering
	pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
	pipelineCI.basePipelineIndex = -1;           // 零初始化会给 0，须显式写 -1

	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &pipeline);

	vkDestroyShaderModule(m_device, vsModule, nullptr);
	vkDestroyShaderModule(m_device, psModule, nullptr);

	if (result != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateGraphicsPipelines 失败: " << result << "\n";
		vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
		return nullptr;
	}

	return std::unique_ptr<Pipeline>(new Pipeline(m_device, pipeline, pipelineLayout,
		desc.pushConstantSize, std::move(layouts)));
}

#pragma endregion

#pragma region FrameSlot // 帧槽

FrameSlot::FrameSlot(VkDevice device, std::vector<uint32_t> queueFamilies)
	: m_device(device)
{
	// 每队列族一池：命令缓冲只能提交到与其 pool 同族的队列
	for (uint32_t family : queueFamilies) {
		VkCommandPoolCreateInfo poolCI{};
		poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolCI.queueFamilyIndex = family;
		VkCommandPool pool = VK_NULL_HANDLE;
		if (vkCreateCommandPool(m_device, &poolCI, nullptr, &pool) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateCommandPool 失败（family=" << family << "）\n";
			return; // 半成品由 Device::Create 的池数量校验拦截
		}
		m_pools.push_back(FamilyPool{ family, pool });
	}
	m_liveBuffers.resize(m_pools.size());

	VkFenceCreateInfo fenceCI{};
	fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	// 内部节流 fence：unsignaled 创建（首帧 Submit 即首次信号，规避对已信号 fence 的二次信号）
	if (vkCreateFence(m_device, &fenceCI, nullptr, &m_fence) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateFence（帧槽）失败\n";
		return;
	}
}

FrameSlot::~FrameSlot()
{
	if (m_device) {
		// 在录 buffer 先按池释放（正常路径已在 Recycle 清空；此处兜底 Begin 后未提交即销毁的场景）
		for (size_t i = 0; i < m_pools.size() && i < m_liveBuffers.size(); ++i) {
			auto& live = m_liveBuffers[i];
			if (!live.empty()) {
				vkFreeCommandBuffers(m_device, m_pools[i].pool,
					static_cast<uint32_t>(live.size()), live.data());
				live.clear();
			}
		}
		m_liveBuffers.clear();
		if (m_fence != VK_NULL_HANDLE) {
			vkDestroyFence(m_device, m_fence, nullptr);
			m_fence = VK_NULL_HANDLE;
		}
		for (const FamilyPool& fp : m_pools) {
			if (fp.pool != VK_NULL_HANDLE) {
				vkDestroyCommandPool(m_device, fp.pool, nullptr);
			}
		}
		m_pools.clear();
	}
}

VkCommandBuffer FrameSlot::Allocate(uint32_t queueFamily)
{
	// 按族查池（≤3 项线性扫）；未建池的族属调用方错误
	size_t poolIndex = m_pools.size();
	for (size_t i = 0; i < m_pools.size(); ++i) {
		if (m_pools[i].family == queueFamily) { poolIndex = i; break; }
	}
	if (poolIndex == m_pools.size()) {
		std::cerr << "[RHI] 帧槽无该队列族的命令池（family=" << queueFamily << "）\n";
		return VK_NULL_HANDLE;
	}

	VkCommandBufferAllocateInfo allocCI{};
	allocCI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocCI.commandPool = m_pools[poolIndex].pool;
	allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocCI.commandBufferCount = 1;

	VkCommandBuffer buffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(m_device, &allocCI, &buffer) != VK_SUCCESS) {
		std::cerr << "[RHI] vkAllocateCommandBuffers 失败\n";
		return VK_NULL_HANDLE;
	}

	VkCommandBufferBeginInfo beginCI{};
	beginCI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginCI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; // 一帧内有效
	if (vkBeginCommandBuffer(buffer, &beginCI) != VK_SUCCESS) {
		std::cerr << "[RHI] vkBeginCommandBuffer 失败\n";
		vkFreeCommandBuffers(m_device, m_pools[poolIndex].pool, 1, &buffer);
		return VK_NULL_HANDLE;
	}

	m_liveBuffers[poolIndex].push_back(buffer);
	return buffer;
}

VkFence FrameSlot::InternalFence() const { return m_fence; }

void FrameSlot::MarkPending(VkFence fence)
{
	m_pendingFence = fence;
}

void FrameSlot::DrainPending()
{
	// pending 为空绝不 wait：避免对无挂起命令的 unsignaled fence 死锁
	if (m_pendingFence == VK_NULL_HANDLE) return;
	if (vkWaitForFences(m_device, 1, &m_pendingFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
		std::cerr << "[RHI] 帧槽回收：vkWaitForFences 失败\n";
	}
	// 内部 fence 由本槽持有并复位；外部 fence 的复位交给使用者
	if (m_pendingFence == m_fence) {
		vkResetFences(m_device, 1, &m_pendingFence);
	}
	m_pendingFence = VK_NULL_HANDLE;
}

void FrameSlot::Recycle()
{
	DrainPending();
	for (size_t i = 0; i < m_pools.size() && i < m_liveBuffers.size(); ++i) {
		auto& live = m_liveBuffers[i];
		if (!live.empty()) {
			vkFreeCommandBuffers(m_device, m_pools[i].pool,
				static_cast<uint32_t>(live.size()), live.data());
			live.clear();
		}
	}
}

#pragma endregion

#pragma region CommandList // 命令录制

void CommandList::Barrier(std::span<const ImageBarrier> images, std::span<const BufferBarrier> buffers)
{
	if (m_commandBuffer == VK_NULL_HANDLE || (images.empty() && buffers.empty())) return;

	std::vector<VkImageMemoryBarrier2> imageBarriers;
	imageBarriers.reserve(images.size());
	for (const auto& b : images) {
		if (!b.image) continue;
		VkImageMemoryBarrier2 vk{};
		vk.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		vk.srcStageMask = ToVkPipelineStage(b.srcStage);
		vk.srcAccessMask = ToVkAccess(b.srcAccess);
		vk.dstStageMask = ToVkPipelineStage(b.dstStage);
		vk.dstAccessMask = ToVkAccess(b.dstAccess);
		vk.oldLayout = ToVkImageLayout(b.oldLayout);
		vk.newLayout = ToVkImageLayout(b.newLayout);
		vk.srcQueueFamilyIndex = b.srcQueueFamily; // 默认 kQueueFamilyIgnored，与 IGNORED 同值；QFOT 时由调用方显式填
		vk.dstQueueFamilyIndex = b.dstQueueFamily;
		vk.image = b.image->NativeHandle();
		vk.subresourceRange.aspectMask = ToVkAspect(b.image->NativeFormat());
		vk.subresourceRange.baseMipLevel = b.range.base.mipLevel;
		vk.subresourceRange.levelCount = b.range.mipLevelCount;
		vk.subresourceRange.baseArrayLayer = b.range.base.layer;
		vk.subresourceRange.layerCount = b.range.layerCount;
		imageBarriers.push_back(vk);
	}

	std::vector<VkBufferMemoryBarrier2> bufferBarriers;
	bufferBarriers.reserve(buffers.size());
	for (const auto& b : buffers) {
		if (!b.buffer) continue;
		VkBufferMemoryBarrier2 vk{};
		vk.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
		vk.srcStageMask = ToVkPipelineStage(b.srcStage);
		vk.srcAccessMask = ToVkAccess(b.srcAccess);
		vk.dstStageMask = ToVkPipelineStage(b.dstStage);
		vk.dstAccessMask = ToVkAccess(b.dstAccess);
		vk.srcQueueFamilyIndex = b.srcQueueFamily; // 默认 kQueueFamilyIgnored，与 IGNORED 同值；QFOT 时由调用方显式填
		vk.dstQueueFamilyIndex = b.dstQueueFamily;
		vk.buffer = b.buffer->NativeHandle();
		vk.offset = b.offset;
		vk.size = (b.size == 0) ? VK_WHOLE_SIZE : b.size;
		bufferBarriers.push_back(vk);
	}

	VkDependencyInfo dep{};
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size());
	dep.pImageMemoryBarriers = imageBarriers.empty() ? nullptr : imageBarriers.data();
	dep.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size());
	dep.pBufferMemoryBarriers = bufferBarriers.empty() ? nullptr : bufferBarriers.data();
	vkCmdPipelineBarrier2(m_commandBuffer, &dep);
}

void CommandList::BeginRendering(const RenderingInfo& info)
{
	if (m_commandBuffer == VK_NULL_HANDLE || info.colors == nullptr) return;

	// 颜色附件（null image 的槽位跳过，后续槽位前移）
	std::vector<VkRenderingAttachmentInfo> colors;
	colors.reserve(info.colorCount);
	for (uint32_t i = 0; i < info.colorCount; ++i) {
		const RenderingAttachment& att = info.colors[i];
		if (!att.image) continue;
		VkRenderingAttachmentInfo vk{};
		vk.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		vk.imageView = att.image->CreateView(att.view).NativeHandle();
		vk.imageLayout = ToVkImageLayout(att.layout);
		vk.loadOp = ToVkLoadOp(att.load);
		vk.storeOp = ToVkStoreOp(att.store);
		vk.clearValue = ToVkClearValue(att.clear);
		colors.push_back(vk);
	}

	VkRenderingAttachmentInfo depth{};
	const bool hasDepth = info.depth && info.depth->image;
	if (hasDepth) {
		depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depth.imageView = info.depth->image->CreateView(info.depth->view).NativeHandle();
		depth.imageLayout = ToVkImageLayout(info.depth->layout);
		depth.loadOp = ToVkLoadOp(info.depth->load);
		depth.storeOp = ToVkStoreOp(info.depth->store);
		depth.clearValue = ToVkClearValue(info.depth->clear);
	}

	VkRenderingInfo rendering{};
	rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea.offset = { info.renderArea.x, info.renderArea.y };
	rendering.renderArea.extent = { info.renderArea.width, info.renderArea.height };
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = static_cast<uint32_t>(colors.size());
	rendering.pColorAttachments = colors.empty() ? nullptr : colors.data();
	rendering.pDepthAttachment = hasDepth ? &depth : nullptr;
	vkCmdBeginRendering(m_commandBuffer, &rendering);
}

void CommandList::EndRendering()
{
	if (m_commandBuffer != VK_NULL_HANDLE) vkCmdEndRendering(m_commandBuffer);
}

void CommandList::BindPipeline(const Pipeline& pipeline)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.NativeHandle());
	// 记录布局与 push constant 声明，供后续 BindDescriptorSet / PushConstants 使用
	m_pipelineLayout = pipeline.NativeLayout();
	m_pushConstantSize = pipeline.PushConstantSize();
}

void CommandList::BindDescriptorSet(uint32_t setIndex, const DescriptorSet& set)
{
	if (m_commandBuffer == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE) return;
	VkDescriptorSet raw = set.NativeHandle();
	vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
		setIndex, 1, &raw, 0, nullptr);
}

void CommandList::PushConstants(const void* data, uint32_t size)
{
	if (m_commandBuffer == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE || data == nullptr) return;
	if (m_pushConstantSize == 0 || size != m_pushConstantSize) {
		std::cerr << "[RHI] PushConstants 大小与管线声明不符（管线 " << m_pushConstantSize
			<< "，传入 " << size << "）\n";
		return;
	}
	vkCmdPushConstants(m_commandBuffer, m_pipelineLayout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void CommandList::SetViewport(const Viewport& viewport)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	VkViewport vk{ viewport.x, viewport.y, viewport.w, viewport.h, viewport.minDepth, viewport.maxDepth };
	vkCmdSetViewport(m_commandBuffer, 0, 1, &vk);
	// 管线开了 dynamic scissor，绘制前必须设置（VUID 07832）；与视口同矩形
	VkRect2D scissor{ { static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y) },
		{ static_cast<uint32_t>(viewport.w), static_cast<uint32_t>(viewport.h) } };
	vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
}

void CommandList::BindVertexBuffer(const Buffer& buffer, uint64_t offset)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	VkBuffer raw = buffer.NativeHandle();
	VkDeviceSize off = offset;
	vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, &raw, &off);
}

void CommandList::BindIndexBuffer(const Buffer& buffer, uint64_t offset, IndexType type)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdBindIndexBuffer(m_commandBuffer, buffer.NativeHandle(), offset, ToVkIndexType(type));
}

void CommandList::Draw(uint32_t vertexCount, uint32_t firstVertex)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdDraw(m_commandBuffer, vertexCount, 1, firstVertex, 0);
}

void CommandList::DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdDrawIndexed(m_commandBuffer, indexCount, 1, firstIndex, vertexOffset, 0);
}

void CommandList::CopyBufferToImage(const Buffer& src, Image& dst, ImageLayout dstLayout)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	const ImageCreateInfo& desc = dst.Desc();

	// 单 region 覆盖全层（接口未暴露逐 region 拷贝；仅 mip0，逐 mip 待接口扩展）
	VkBufferImageCopy region{};
	region.imageSubresource.aspectMask = ToVkAspect(dst.NativeFormat());
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = VK_REMAINING_ARRAY_LAYERS;
	region.imageExtent = { desc.width, desc.height, desc.depth };
	vkCmdCopyBufferToImage(m_commandBuffer, src.NativeHandle(), dst.NativeHandle(),
		ToVkImageLayout(dstLayout), 1, &region);
}

void CommandList::ClearColor(Image& image, ImageSubresource view, const float rgba[4], ImageLayout layout)
{
	if (m_commandBuffer == VK_NULL_HANDLE || rgba == nullptr) return;
	VkClearColorValue color{ { rgba[0], rgba[1], rgba[2], rgba[3] } };
	VkImageSubresourceRange range{};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; // ClearColorImage 仅允许 color aspect
	range.baseMipLevel = view.mipLevel;
	range.levelCount = 1;
	range.baseArrayLayer = view.layer;
	range.layerCount = 1;
	vkCmdClearColorImage(m_commandBuffer, image.NativeHandle(), ToVkImageLayout(layout), &color, 1, &range);
}

VkCommandBuffer CommandList::NativeHandle() const { return m_commandBuffer; }

#pragma endregion

#pragma region Frame // 帧上下文与提交

CommandList Device::BeginCommandList(QueueClass queue)
{
	CommandList list;
	if (m_frameSlots.empty()) return list;

	// 类别 -> 族（选定期已兜底，无需判空）
	uint32_t family = m_graphicsQueueFamily;
	if (queue == QueueClass::AsyncCompute) family = m_computeQueueFamily;
	else if (queue == QueueClass::Transfer) family = m_transferQueueFamily;

	// 回收当期槽（等在飞提交完成 -> 释放旧命令缓冲）后分配新的
	FrameSlot& slot = *m_frameSlots[m_frameIndex];
	slot.Recycle();
	list.m_commandBuffer = slot.Allocate(family);
	return list;
}

uint32_t Device::QueueFamily(QueueClass cls) const
{
	switch (cls) {
		case QueueClass::AsyncCompute: return m_computeQueueFamily;
		case QueueClass::Transfer:     return m_transferQueueFamily;
		case QueueClass::Graphics:
		default:                       return m_graphicsQueueFamily;
	}
}

void Device::Submit(const SubmitInfo& info)
{
	if (m_frameSlots.empty()) return;
	FrameSlot& slot = *m_frameSlots[m_frameIndex];

	// 结束录制（BeginCommandList 自动 begin，Submit 自动 end）
	std::vector<VkCommandBufferSubmitInfo> commandInfos;
	commandInfos.reserve(info.commandLists.size());
	for (const auto& list : info.commandLists) {
		if (list.m_commandBuffer == VK_NULL_HANDLE) continue;
		if (vkEndCommandBuffer(list.m_commandBuffer) != VK_SUCCESS) {
			std::cerr << "[RHI] vkEndCommandBuffer 失败\n";
			return;
		}
		VkCommandBufferSubmitInfo ci{};
		ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		ci.commandBuffer = list.m_commandBuffer;
		commandInfos.push_back(ci);
	}
	if (commandInfos.empty()) return;

	// 等待信号量（acquire / 跨队列）；stageMask 禁 0，None 折到 AllCommands
	std::vector<VkSemaphoreSubmitInfo> waitInfos;
	waitInfos.reserve(info.waits.size());
	for (const auto& w : info.waits) {
		if (!w.semaphore) continue;
		VkSemaphoreSubmitInfo wi{};
		wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		wi.semaphore = w.semaphore->NativeHandle();
		wi.stageMask = (w.stage == PipelineStage::None)
			? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : ToVkPipelineStage(w.stage);
		waitInfos.push_back(wi);
	}

	std::vector<VkSemaphoreSubmitInfo> signalInfos;
	signalInfos.reserve(info.signals.size());
	for (const auto& s : info.signals) {
		if (!s.semaphore) continue;
		VkSemaphoreSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		si.semaphore = s.semaphore->NativeHandle();
		si.stageMask = (s.stage == PipelineStage::None)
			? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : ToVkPipelineStage(s.stage);
		signalInfos.push_back(si);
	}

	// fence：外部优先（回收时由 RHI 等待，Reset 复用交给使用者）；
	// 内部 fence 时若本帧已有提交在飞则先收账（帧内多次 Submit 串行化，保证单次挂起）
	VkFence fence = VK_NULL_HANDLE;
	if (info.signalFence) {
		fence = info.signalFence->m_fence;
	} else {
		fence = slot.InternalFence();
		slot.DrainPending();
	}

	// 目标队列：类别映射（选定期已兜底）；commandLists 须全部来自同族池
	VkQueue targetQueue = m_graphicsQueue;
	if (info.queue == QueueClass::AsyncCompute) targetQueue = m_computeQueue;
	else if (info.queue == QueueClass::Transfer) targetQueue = m_transferQueue;

	VkSubmitInfo2 submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submitInfo.commandBufferInfoCount = static_cast<uint32_t>(commandInfos.size());
	submitInfo.pCommandBufferInfos = commandInfos.data();
	submitInfo.waitSemaphoreInfoCount = static_cast<uint32_t>(waitInfos.size());
	submitInfo.pWaitSemaphoreInfos = waitInfos.empty() ? nullptr : waitInfos.data();
	submitInfo.signalSemaphoreInfoCount = static_cast<uint32_t>(signalInfos.size());
	submitInfo.pSignalSemaphoreInfos = signalInfos.empty() ? nullptr : signalInfos.data();

	if (vkQueueSubmit2(targetQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
		std::cerr << "[RHI] vkQueueSubmit2 失败\n";
		return;
	}
	slot.MarkPending(fence);
}

void Device::WaitForFrame()
{
	if (m_frameSlots.empty()) return;
	m_frameSlots[m_frameIndex]->Recycle();
	m_frameIndex = (m_frameIndex + 1) % static_cast<uint32_t>(m_frameSlots.size());
}

void Device::WaitIdle()
{
	// WaitForFrame 只等 Submit 侧（fence）；present 消费 renderDone 等信号量不为其覆盖，
	// 销毁信号量前须等设备完全空闲
	if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);
}

#pragma endregion

