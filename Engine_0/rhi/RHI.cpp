#include "RHI.h"

#include "../external/vk_mem_alloc.h"
#include "../external/spv_reflect/spirv_reflect.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_win32.h>

#undef CreateSemaphore

#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>

using namespace dEngine::rhi;

namespace {

	constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

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
		VkDebugUtilsMessageTypeFlagsEXT              ,
		const VkDebugUtilsMessengerCallbackDataEXT* data,
		void*                                        ) {
		const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR"
			: (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN" : "INFO";
		std::cerr << "[Vulkan " << level << "] " << data->pMessage << "\n";
		return VK_FALSE;
	}

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
					if (present == UINT32_MAX) continue;
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
		outComputeFamily  = (bestCompute  != UINT32_MAX) ? bestCompute  : bestGraphics;
		outTransferFamily = (bestTransfer != UINT32_MAX) ? bestTransfer : bestGraphics;
		return best;
	}

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
		if (u & static_cast<uint32_t>(BufferUsage::Indirect))    flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		return flags;
	}

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

	VkFilter ToVkFilter(Filter filter) {
		return filter == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	}
	VkSamplerMipmapMode ToVkSamplerMipmapMode(SamplerMipmapMode mode) {
		return mode == SamplerMipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
			: VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}
	VkSamplerAddressMode ToVkAddressMode(SamplerAddressMode mode) {
		switch (mode) {
			case SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case SamplerAddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case SamplerAddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			default:                                 return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}
	}

	ShaderStageMask ToStageMask(ShaderStage stage) {
		switch (stage) {
			case ShaderStage::Vertex:      return ShaderStageMask::Vertex;
			case ShaderStage::Fragment:    return ShaderStageMask::Fragment;
			case ShaderStage::TessControl: return ShaderStageMask::TessControl;
			case ShaderStage::TessEval:    return ShaderStageMask::TessEval;
			case ShaderStage::Geometry:    return ShaderStageMask::Geometry;
			case ShaderStage::Compute:     return ShaderStageMask::Compute;
			case ShaderStage::Task:        return ShaderStageMask::Task;
			case ShaderStage::Mesh:        return ShaderStageMask::Mesh;
			default:                       return ShaderStageMask::None;
		}
	}

	VkShaderStageFlags ToVkStageMask(ShaderStageMask mask) {
		VkShaderStageFlags flags = 0;
		const uint32_t m = static_cast<uint32_t>(mask);
		if (m & static_cast<uint32_t>(ShaderStageMask::Vertex))      flags |= VK_SHADER_STAGE_VERTEX_BIT;
		if (m & static_cast<uint32_t>(ShaderStageMask::TessControl)) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		if (m & static_cast<uint32_t>(ShaderStageMask::TessEval))    flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
		if (m & static_cast<uint32_t>(ShaderStageMask::Geometry))    flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
		if (m & static_cast<uint32_t>(ShaderStageMask::Fragment))    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
		if (m & static_cast<uint32_t>(ShaderStageMask::Task))        flags |= VK_SHADER_STAGE_TASK_BIT_EXT;
		if (m & static_cast<uint32_t>(ShaderStageMask::Mesh))        flags |= VK_SHADER_STAGE_MESH_BIT_EXT;
		if (m & static_cast<uint32_t>(ShaderStageMask::Compute))     flags |= VK_SHADER_STAGE_COMPUTE_BIT;
		return flags;
	}

	VkImageAspectFlags ToVkAspect(VkFormat format) {
		switch (format) {
			case VK_FORMAT_D32_SFLOAT:           return VK_IMAGE_ASPECT_DEPTH_BIT;
			case VK_FORMAT_S8_UINT:              return VK_IMAGE_ASPECT_STENCIL_BIT;
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:   return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default:                             return VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

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

	VkBlendFactor ToVkBlendFactor(BlendFactor f) {
		switch (f) {
			case BlendFactor::Zero:                 return VK_BLEND_FACTOR_ZERO;
			case BlendFactor::One:                  return VK_BLEND_FACTOR_ONE;
			case BlendFactor::SrcColor:             return VK_BLEND_FACTOR_SRC_COLOR;
			case BlendFactor::OneMinusSrcColor:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
			case BlendFactor::DstColor:             return VK_BLEND_FACTOR_DST_COLOR;
			case BlendFactor::OneMinusDstColor:     return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			case BlendFactor::SrcAlpha:             return VK_BLEND_FACTOR_SRC_ALPHA;
			case BlendFactor::OneMinusSrcAlpha:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			case BlendFactor::DstAlpha:             return VK_BLEND_FACTOR_DST_ALPHA;
			case BlendFactor::OneMinusDstAlpha:     return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			case BlendFactor::ConstantColor:        return VK_BLEND_FACTOR_CONSTANT_COLOR;
			case BlendFactor::OneMinusConstantColor:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
			case BlendFactor::ConstantAlpha:        return VK_BLEND_FACTOR_CONSTANT_ALPHA;
			case BlendFactor::OneMinusConstantAlpha:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
			case BlendFactor::SrcAlphaSaturate:     return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
			default:                                return VK_BLEND_FACTOR_ONE;
		}
	}

	VkBlendOp ToVkBlendOp(BlendOp op) {
		switch (op) {
			case BlendOp::Add:             return VK_BLEND_OP_ADD;
			case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
			case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
			case BlendOp::Min:             return VK_BLEND_OP_MIN;
			case BlendOp::Max:             return VK_BLEND_OP_MAX;
			default:                       return VK_BLEND_OP_ADD;
		}
	}

	VkColorComponentFlags ToVkColorWriteMask(ColorWriteMask mask) {
		const uint8_t m = static_cast<uint8_t>(mask);
		VkColorComponentFlags flags = 0;
		if (m & static_cast<uint8_t>(ColorWriteMask::R)) flags |= VK_COLOR_COMPONENT_R_BIT;
		if (m & static_cast<uint8_t>(ColorWriteMask::G)) flags |= VK_COLOR_COMPONENT_G_BIT;
		if (m & static_cast<uint8_t>(ColorWriteMask::B)) flags |= VK_COLOR_COMPONENT_B_BIT;
		if (m & static_cast<uint8_t>(ColorWriteMask::A)) flags |= VK_COLOR_COMPONENT_A_BIT;
		return flags;
	}

	VkStencilOp ToVkStencilOp(StencilOp op) {
		switch (op) {
			case StencilOp::Keep:       return VK_STENCIL_OP_KEEP;
			case StencilOp::Zero:       return VK_STENCIL_OP_ZERO;
			case StencilOp::Replace:    return VK_STENCIL_OP_REPLACE;
			case StencilOp::IncrClamp:  return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
			case StencilOp::DecrClamp:  return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
			case StencilOp::Invert:     return VK_STENCIL_OP_INVERT;
			case StencilOp::IncrWrap:   return VK_STENCIL_OP_INCREMENT_AND_WRAP;
			case StencilOp::DecrWrap:   return VK_STENCIL_OP_DECREMENT_AND_WRAP;
			default:                    return VK_STENCIL_OP_KEEP;
		}
	}

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

	VkPipelineStageFlags2 ToVkPipelineStage(PipelineStage stage) {
		const uint64_t s = static_cast<uint64_t>(stage);
		VkPipelineStageFlags2 flags = 0;
		if (s & static_cast<uint64_t>(PipelineStage::VertexShader))       flags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::TessControlShader))  flags |= VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::TessEvalShader))     flags |= VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::GeometryShader))     flags |= VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::FragmentShader))     flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::EarlyFragmentTests)) flags |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::LateFragmentTests))  flags |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::ColorAttachmentOut)) flags |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::ComputeShader))      flags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::Transfer))           flags |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::AllCommands))        flags |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::Host))               flags |= VK_PIPELINE_STAGE_2_HOST_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::VertexInput))        flags |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
		if (s & static_cast<uint64_t>(PipelineStage::DrawIndirect))       flags |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		return flags;
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
		if (a & static_cast<uint64_t>(Access::VertexAttributeRead))  flags |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
		if (a & static_cast<uint64_t>(Access::IndexRead))            flags |= VK_ACCESS_2_INDEX_READ_BIT;
		if (a & static_cast<uint64_t>(Access::HostWrite))            flags |= VK_ACCESS_2_HOST_WRITE_BIT;
		if (a & static_cast<uint64_t>(Access::HostRead))             flags |= VK_ACCESS_2_HOST_READ_BIT;
		if (a & static_cast<uint64_t>(Access::IndirectCommandRead))  flags |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
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

	VkClearValue ToVkClearValue(const ClearValue& value) {
		VkClearValue out{};
		if (const auto* color = std::get_if<std::array<float, 4>>(&value.value)) {
			out.color = { { (*color)[0], (*color)[1], (*color)[2], (*color)[3] } };
		} else if (const auto* ds = std::get_if<DepthStencilValue>(&value.value)) {
			out.depthStencil = { ds->depth, ds->stencil };
		}
		return out;
	}

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

	VkShaderStageFlagBits ToVkStage(ShaderStage stage) {
		switch (stage) {
			case ShaderStage::Vertex:      return VK_SHADER_STAGE_VERTEX_BIT;
			case ShaderStage::Fragment:    return VK_SHADER_STAGE_FRAGMENT_BIT;
			case ShaderStage::TessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			case ShaderStage::TessEval:    return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
			case ShaderStage::Geometry:    return VK_SHADER_STAGE_GEOMETRY_BIT;
			case ShaderStage::Task:        return VK_SHADER_STAGE_TASK_BIT_EXT;
			case ShaderStage::Mesh:        return VK_SHADER_STAGE_MESH_BIT_EXT;
			case ShaderStage::Compute:     return VK_SHADER_STAGE_COMPUTE_BIT;
			default:                       return VK_SHADER_STAGE_VERTEX_BIT;
		}
	}

	VkPrimitiveTopology ToVkTopology(PrimitiveTopology t) {
		switch (t) {
			case PrimitiveTopology::PointList:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case PrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case PrimitiveTopology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			case PrimitiveTopology::PatchList:     return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
			default:                               return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}
	}

	VkFormat ToVkVertexFormat(VertexFormat f) {
		switch (f) {
			case VertexFormat::Float:   return VK_FORMAT_R32_SFLOAT;
			case VertexFormat::Float2:  return VK_FORMAT_R32G32_SFLOAT;
			case VertexFormat::Float3:  return VK_FORMAT_R32G32B32_SFLOAT;
			case VertexFormat::Float4:  return VK_FORMAT_R32G32B32A32_SFLOAT;
			default:                    return VK_FORMAT_R32G32B32_SFLOAT;
		}
	}

	const char* ShaderStageName(ShaderStage stage) {
		switch (stage) {
			case ShaderStage::Vertex:      return "vertex";
			case ShaderStage::Fragment:    return "fragment";
			case ShaderStage::TessControl: return "tessellation control";
			case ShaderStage::TessEval:    return "tessellation evaluation";
			case ShaderStage::Geometry:    return "geometry";
			case ShaderStage::Task:        return "task";
			case ShaderStage::Mesh:        return "mesh";
			case ShaderStage::Compute:     return "compute";
			default:                       return "unknown";
		}
	}

	bool ValidateStageCombination(const std::vector<ShaderStageDesc>& shaders) {
		if (shaders.empty()) {
			std::cerr << "[RHI] 管线须至少声明一个着色器阶段\n";
			return false;
		}
		bool hasVertex = false, hasMesh = false, hasTask = false, hasTC = false, hasTE = false;
		for (size_t i = 0; i < shaders.size(); ++i) {
			for (size_t j = i + 1; j < shaders.size(); ++j) {
				if (shaders[i].stage == shaders[j].stage) {
					std::cerr << "[RHI] 重复的着色器阶段: " << ShaderStageName(shaders[i].stage) << "\n";
					return false;
				}
			}
			switch (shaders[i].stage) {
				case ShaderStage::Vertex:      hasVertex = true; break;
				case ShaderStage::Mesh:        hasMesh = true;   break;
				case ShaderStage::Task:        hasTask = true;   break;
				case ShaderStage::TessControl: hasTC = true;     break;
				case ShaderStage::TessEval:    hasTE = true;     break;
				default: break;
			}
		}
		if (hasVertex && (hasMesh || hasTask)) {
			std::cerr << "[RHI] Vertex 与 Task/Mesh 互斥（顶点来源二选一）\n";
			return false;
		}
		if (!hasVertex && !hasMesh) {
			std::cerr << "[RHI] 图形管线须有 Vertex 或 Mesh 阶段\n";
			return false;
		}
		if (hasTask && !hasMesh) {
			std::cerr << "[RHI] Task 阶段必须伴随 Mesh\n";
			return false;
		}
		if (hasTC != hasTE) {
			std::cerr << "[RHI] TessControl/TessEval 须成对出现\n";
			return false;
		}
		if ((hasTC || hasTE) && (hasMesh || hasTask)) {
			std::cerr << "[RHI] Tessellation 与 Task/Mesh 不能同时使用\n";
			return false;
		}
		return true;
	}

	bool ToResourceType(SpvReflectDescriptorType t, ResourceType& out) {
		switch (t) {
			case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: out = ResourceType::SampledImage;   return true;
			case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:          out = ResourceType::StorageImage;   return true;
			case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:         out = ResourceType::UniformBuffer;  return true;
			case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:         out = ResourceType::StorageBuffer;  return true;
			default:                                                                                      return false;
		}
	}

	bool ToVertexFormat(SpvReflectFormat f, VertexFormat& out, uint32_t& byteSize) {
		switch (f) {
			case SPV_REFLECT_FORMAT_R32_SFLOAT:          out = VertexFormat::Float;  byteSize = 4;  return true;
			case SPV_REFLECT_FORMAT_R32G32_SFLOAT:       out = VertexFormat::Float2; byteSize = 8;  return true;
			case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:    out = VertexFormat::Float3; byteSize = 12; return true;
			case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT: out = VertexFormat::Float4; byteSize = 16; return true;
			default:                                                                               return false;
		}
	}

	bool ReflectDescriptorBindings(const std::vector<uint32_t>& code, const char* stageLabel,
		std::vector<DescriptorBinding>& out) {
		SpvReflectShaderModule module{};
		if (spvReflectCreateShaderModule(code.size() * sizeof(uint32_t), code.data(), &module)
			!= SPV_REFLECT_RESULT_SUCCESS) {
			std::cerr << "[RHI] SPIR-V 反射失败（" << stageLabel << "）\n";
			return false;
		}

		uint32_t count = 0;
		std::vector<SpvReflectDescriptorBinding*> bindings;
		if (spvReflectEnumerateDescriptorBindings(&module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS
			&& count > 0) {
			bindings.resize(count);
			if (spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data())
				!= SPV_REFLECT_RESULT_SUCCESS) {
				std::cerr << "[RHI] 枚举描述符绑定失败（" << stageLabel << "）\n";
				spvReflectDestroyShaderModule(&module);
				return false;
			}
		}

		out.reserve(out.size() + count);
		for (uint32_t i = 0; i < count; ++i) {
			const SpvReflectDescriptorBinding* b = bindings[i];
			DescriptorBinding db;
			db.set = b->set;
			db.binding = b->binding;
			if (b->name == nullptr || b->name[0] == '\0') {
				const char* blockName = (b->type_description != nullptr
					&& b->type_description->type_name != nullptr)
					? b->type_description->type_name : "?";
				std::cerr << "[RHI] " << stageLabel << " 反射到无名的描述符变量: set" << db.set
					<< " binding" << db.binding << "（block=\"" << blockName
					<< "\" 缺少实例名；请写成 uniform " << blockName
					<< " { ... } instance;，并让图资源名等于 instance）\n";
				spvReflectDestroyShaderModule(&module);
				return false;
			}
			db.name = b->name;
			db.nonWritable =
				(b->decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
			bool runtimeArray = false;
			for (uint32_t d = 0; d < b->array.dims_count; ++d) {
				if (b->array.dims[d] == SPV_REFLECT_ARRAY_DIM_RUNTIME) {
					runtimeArray = true;
					break;
				}
			}
			if (runtimeArray) {
				std::cerr << "[RHI] " << stageLabel << " 反射到运行时长度数组绑定: " << db.name
					<< "（暂不支持，请改用固定长度，如 [4]）\n";
				spvReflectDestroyShaderModule(&module);
				return false;
			}
			db.arrayCount = std::max(1u, b->count);
			if (!ToResourceType(b->descriptor_type, db.type)) {
				std::cerr << "[RHI] " << stageLabel << " 反射到暂不支持的描述符类型: "
					<< db.name << "（descriptor_type=" << b->descriptor_type << "）\n";
				spvReflectDestroyShaderModule(&module);
				return false;
			}
			out.push_back(std::move(db));
		}

		spvReflectDestroyShaderModule(&module);
		return true;
	}

	bool DeriveVertexLayout(const std::vector<uint32_t>& code,
		std::vector<VkVertexInputBindingDescription>& outBindings,
		std::vector<VkVertexInputAttributeDescription>& outAttributes)
	{
		SpvReflectShaderModule module{};
		if (spvReflectCreateShaderModule(code.size() * sizeof(uint32_t), code.data(), &module)
			!= SPV_REFLECT_RESULT_SUCCESS) {
			std::cerr << "[RHI] 顶点输入反射失败\n";
			return false;
		}

		auto fail = [&module](const char* msg, const SpvReflectInterfaceVariable* v) {
			std::cerr << "[RHI] " << msg << (v && v->name ? v->name : "?")
				<< "（自动顶点布局不支持，请在 PipelineDesc 显式声明 vertexBindings/vertexAttributes）\n";
			spvReflectDestroyShaderModule(&module);
			return false;
		};

		uint32_t count = 0;
		std::vector<SpvReflectInterfaceVariable*> vars;
		if (spvReflectEnumerateInputVariables(&module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS
			&& count > 0) {
			vars.resize(count);
			if (spvReflectEnumerateInputVariables(&module, &count, vars.data())
				!= SPV_REFLECT_RESULT_SUCCESS) {
				std::cerr << "[RHI] 枚举顶点输入变量失败\n";
				spvReflectDestroyShaderModule(&module);
				return false;
			}
		}

		std::sort(vars.begin(), vars.end(),
			[](const SpvReflectInterfaceVariable* a, const SpvReflectInterfaceVariable* b) {
				return a->location < b->location;
			});

		uint32_t stride = 0;
		for (const auto* v : vars) {
			if (v->built_in >= 0) continue;
			if (v->array.dims_count > 0
				|| (v->type_description && (v->type_description->type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX)))
				return fail("顶点输入为矩阵/数组: ", v);
			VertexFormat format;
			uint32_t bytes = 0;
			if (!ToVertexFormat(v->format, format, bytes))
				return fail("顶点输入格式暂不支持（仅 float 1~4 分量）: ", v);
			VkVertexInputAttributeDescription att{};
			att.location = v->location;
			att.binding = 0;
			att.format = ToVkVertexFormat(format);
			att.offset = stride;
			stride += bytes;
			outAttributes.push_back(att);
		}

		spvReflectDestroyShaderModule(&module);
		if (outAttributes.empty())
			return true;
		VkVertexInputBindingDescription binding{};
		binding.binding = 0;
		binding.stride = stride;
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		outBindings.push_back(binding);
		return true;
	}

	struct StageCode {
		ShaderStage stage;
		const std::vector<uint32_t>* code;
	};

	bool ReflectAndMergeBindings(const std::vector<StageCode>& stages,
		std::vector<MergedDescriptor>& outSlots,
		VkShaderStageFlags& outAllStages)
	{
		outSlots.clear();
		outAllStages = 0;

		for (const auto& sc : stages) {
			std::vector<DescriptorBinding> stageBindings;
			if (!ReflectDescriptorBindings(*sc.code, ShaderStageName(sc.stage), stageBindings))
				return false;
			const VkShaderStageFlags stageFlag = ToVkStage(sc.stage);
			outAllStages |= stageFlag;
			const ShaderStageMask stageMask = ToStageMask(sc.stage);

			for (auto& b : stageBindings) {
				b.stages = b.stages | stageMask;
				bool slotFound = false;
				for (auto& slot : outSlots) {
					if (slot.binding.set == b.set && slot.binding.binding == b.binding) {
						if (slot.binding.type != b.type || slot.binding.arrayCount != b.arrayCount
							|| slot.binding.name != b.name) {
							std::cerr << "[RHI] 跨 stage 反射到失配的共用槽位: set" << b.set
								<< " binding" << b.binding << "（" << slot.binding.name << " vs " << b.name << "）\n";
							return false;
						}
						slot.stages |= stageFlag;
						slot.binding.stages = slot.binding.stages | b.stages;
						slot.binding.nonWritable = slot.binding.nonWritable && b.nonWritable;
						slotFound = true;
						break;
					}
				}
				if (!slotFound)
					outSlots.push_back({ std::move(b), stageFlag });
			}
		}
		return true;
	}

}

std::unique_ptr<Device> Device::Create(const DeviceDesc& desc)
{
	auto device = std::make_unique<Device>();

	const bool validation = desc.enableValidation && HasValidationLayer();

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

	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device->m_physicalDevice, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> familyProps(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device->m_physicalDevice, &familyCount, familyProps.data());
		auto validBits = [&familyProps](uint32_t family) {
			return family < familyProps.size() ? familyProps[family].timestampValidBits : 0u;
		};
		const uint32_t timestampFamilies[3] = {
			device->m_graphicsQueueFamily, device->m_computeQueueFamily, device->m_transferQueueFamily
		};
		device->m_timestampsSupported = validBits(timestampFamilies[0]) > 0
			&& validBits(timestampFamilies[1]) > 0
			&& validBits(timestampFamilies[2]) > 0;
		device->m_timestampComparable = props.limits.timestampComputeAndGraphics == VK_TRUE;
		device->m_timestampPeriod = props.limits.timestampPeriod;
	}

	std::vector<uint32_t> families;
	auto pushFamily = [&families](uint32_t f) {
		for (uint32_t existing : families) if (existing == f) return;
		families.push_back(f);
	};
	pushFamily(device->m_graphicsQueueFamily);
	pushFamily(device->m_computeQueueFamily);
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

	VkPhysicalDeviceVulkan12Features supported12{};
	supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	VkPhysicalDeviceFeatures2 supportedFeatures{};
	supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	supportedFeatures.pNext = &supported12;
	vkGetPhysicalDeviceFeatures2(device->m_physicalDevice, &supportedFeatures);
	device->m_timelineSupported = supported12.timelineSemaphore == VK_TRUE;
	device->m_bindlessSupported = supported12.descriptorIndexing == VK_TRUE
		&& supported12.descriptorBindingPartiallyBound == VK_TRUE
		&& supportedFeatures.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
	device->m_depthBoundsSupported = supportedFeatures.features.depthBounds == VK_TRUE;
	device->m_independentBlendSupported = supportedFeatures.features.independentBlend == VK_TRUE;
	device->m_tessellationSupported = supportedFeatures.features.tessellationShader == VK_TRUE;
	device->m_geometrySupported = supportedFeatures.features.geometryShader == VK_TRUE;
	device->m_vertexPipelineStoresAndAtomics =
		supportedFeatures.features.vertexPipelineStoresAndAtomics == VK_TRUE;

	VkPhysicalDeviceProperties physProps{};
	vkGetPhysicalDeviceProperties(device->m_physicalDevice, &physProps);
	device->m_maxSamplerAnisotropy = physProps.limits.maxSamplerAnisotropy > 0.0f
		? physProps.limits.maxSamplerAnisotropy : 1.0f;
	device->m_maxTessellationPatchSize = physProps.limits.maxTessellationPatchSize > 0
		? physProps.limits.maxTessellationPatchSize : 1u;

	VkPhysicalDeviceVulkan12Features features12{};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.timelineSemaphore = supported12.timelineSemaphore;
	if (device->m_bindlessSupported) {
		features12.descriptorIndexing = supported12.descriptorIndexing;
		features12.descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound;
		features12.shaderSampledImageArrayNonUniformIndexing =
			supported12.shaderSampledImageArrayNonUniformIndexing;
	}
	features12.pNext = &features13;

	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &features12;
	features2.features.samplerAnisotropy = supportedFeatures.features.samplerAnisotropy;
	features2.features.independentBlend = supportedFeatures.features.independentBlend;
	features2.features.depthBounds = supportedFeatures.features.depthBounds;
	features2.features.tessellationShader = device->m_tessellationSupported ? VK_TRUE : VK_FALSE;
	features2.features.geometryShader = device->m_geometrySupported ? VK_TRUE : VK_FALSE;
	features2.features.vertexPipelineStoresAndAtomics =
		device->m_vertexPipelineStoresAndAtomics ? VK_TRUE : VK_FALSE;
	if (device->m_bindlessSupported)
		features2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

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
	vkGetDeviceQueue(device->m_device, device->m_presentQueueFamily, 0, &device->m_presentQueue);
	vkGetDeviceQueue(device->m_device, device->m_computeQueueFamily, 0, &device->m_computeQueue);
	vkGetDeviceQueue(device->m_device, device->m_transferQueueFamily, 0, &device->m_transferQueue);

	if (device->m_timelineSupported) {
		for (uint32_t i = 0; i < 3; ++i)
			device->m_queueTimelines[i] = device->CreateTimelineSemaphore(0);
	}

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

	VkPipelineCacheCreateInfo cacheCI{};
	cacheCI.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (vkCreatePipelineCache(device->m_device, &cacheCI, nullptr, &device->m_pipelineCache) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreatePipelineCache 失败\n";
		return nullptr;
	}

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
		if (slot->m_pools.size() != slotFamilies.size()) {
			std::cerr << "[RHI] 帧槽创建失败（slot " << i << "）\n";
			return nullptr;
		}
		device->m_frameSlots.push_back(std::move(slot));
	}

	return device;
}

Device::~Device()
{
	if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);
	m_frameSlots.clear();
	for (auto& timeline : m_queueTimelines)
		timeline.reset();
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

#pragma region Image

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
	m_views.clear();

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

const ImageView& Image::CreateView(ImageSubresource sub) const
{
	ImageViewDesc desc;
	desc.range.base = sub;
	desc.range.mipLevelCount = 1;
	desc.range.layerCount = 1;
	return CreateView(desc);
}

const ImageView& Image::CreateView(const ImageViewDesc& view) const
{
	const uint32_t maxMip = m_desc.mipLevels == 0 ? 1 : m_desc.mipLevels;
	const uint32_t maxLayer = m_desc.arrayLayers == 0 ? 1 : m_desc.arrayLayers;
	ImageSubresourceRange range = view.range;
	if (range.base.mipLevel >= maxMip) range.base.mipLevel = maxMip - 1;
	if (range.base.layer >= maxLayer) range.base.layer = maxLayer - 1;
	if (range.mipLevelCount == 0) range.mipLevelCount = maxMip - range.base.mipLevel;
	if (range.layerCount == 0) range.layerCount = maxLayer - range.base.layer;
	range.mipLevelCount = std::min(range.mipLevelCount, maxMip - range.base.mipLevel);
	range.layerCount = std::min(range.layerCount, maxLayer - range.base.layer);

	const VkFormat format = view.format == ImageFormat::None ? m_format : ToVkFormat(view.format);
	ImageViewType viewType = view.viewType;
	if (viewType == ImageViewType::None) {
		viewType = m_desc.viewType;
		if (range.layerCount > 1 && viewType == ImageViewType::Dim2D)
			viewType = ImageViewType::Dim2DArray;
	}

	const uint64_t key =
		(static_cast<uint64_t>(range.base.mipLevel & 0xFFu) << 56) |
		(static_cast<uint64_t>(range.mipLevelCount & 0xFFu) << 48) |
		(static_cast<uint64_t>(range.base.layer & 0xFFFFu) << 32) |
		(static_cast<uint64_t>(range.layerCount & 0xFFFFu) << 16) |
		(static_cast<uint64_t>(static_cast<uint32_t>(format) & 0xFFu) << 8) |
		static_cast<uint64_t>(static_cast<uint32_t>(viewType) & 0xFu);
	if (auto it = m_views.find(key); it != m_views.end()) return *it->second;

	VkImageViewCreateInfo viewCI{};
	viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewCI.image = m_image;
	viewCI.viewType = ToVkImageViewType(viewType);
	viewCI.format = format;
	viewCI.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
	viewCI.subresourceRange.aspectMask = ToVkAspect(format);
	viewCI.subresourceRange.baseMipLevel = range.base.mipLevel;
	viewCI.subresourceRange.levelCount = range.mipLevelCount;
	viewCI.subresourceRange.baseArrayLayer = range.base.layer;
	viewCI.subresourceRange.layerCount = range.layerCount;

	VkImageView handle = VK_NULL_HANDLE;
	if (m_device == VK_NULL_HANDLE || m_image == VK_NULL_HANDLE ||
		vkCreateImageView(m_device, &viewCI, nullptr, &handle) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateImageView 失败\n";
		handle = VK_NULL_HANDLE;
	}

	auto imageView = std::unique_ptr<ImageView>(new ImageView(m_device, handle));
	const ImageView& ref = *imageView;
	m_views.emplace(key, std::move(imageView));
	return ref;
}

const ImageView& Image::ReinterpretView(ImageFormat format, ImageSubresourceRange range) const
{
	ImageViewDesc desc;
	desc.range = range;
	desc.format = format;
	return CreateView(desc);
}

Sampler::Sampler(VkDevice device, VkSampler sampler) : m_device(device), m_sampler(sampler) {}

Sampler::~Sampler()
{
	if (m_sampler != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
		vkDestroySampler(m_device, m_sampler, nullptr);
}

VkSampler Sampler::NativeHandle() const { return m_sampler; }

std::unique_ptr<Sampler> Device::CreateSampler(const SamplerDesc& desc)
{
	if (m_device == VK_NULL_HANDLE) return nullptr;

	const bool anisotropy = desc.anisotropyEnable && m_maxSamplerAnisotropy > 1.0f;
	VkSamplerCreateInfo samplerCI{};
	samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCI.magFilter = ToVkFilter(desc.magFilter);
	samplerCI.minFilter = ToVkFilter(desc.minFilter);
	samplerCI.mipmapMode = ToVkSamplerMipmapMode(desc.mipmapMode);
	samplerCI.addressModeU = ToVkAddressMode(desc.addressU);
	samplerCI.addressModeV = ToVkAddressMode(desc.addressV);
	samplerCI.addressModeW = ToVkAddressMode(desc.addressW);
	samplerCI.mipLodBias = desc.mipLodBias;
	samplerCI.anisotropyEnable = anisotropy ? VK_TRUE : VK_FALSE;
	samplerCI.maxAnisotropy = anisotropy
		? std::clamp(desc.maxAnisotropy, 1.0f, m_maxSamplerAnisotropy) : 1.0f;
	samplerCI.minLod = desc.minLod;
	samplerCI.maxLod = desc.maxLod;
	samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	samplerCI.unnormalizedCoordinates = VK_FALSE;

	VkSampler handle = VK_NULL_HANDLE;
	if (vkCreateSampler(m_device, &samplerCI, nullptr, &handle) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateSampler 失败\n";
		return nullptr;
	}
	return std::unique_ptr<Sampler>(new Sampler(m_device, handle));
}

std::unique_ptr<Image> Device::CreateImage(const ImageCreateInfo& info)
{
	VkImageCreateInfo imageCI{};
	imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageCI.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
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

std::unique_ptr<Image> Device::UploadImage(const void* data, std::uint64_t byteSize,
	ImageCreateInfo info, ImageLayout finalLayout, PipelineStage dstStage, Access dstAccess)
{
	if (data == nullptr || byteSize == 0 || info.width == 0 || info.height == 0) {
		std::cerr << "[RHI] UploadImage 参数非法\n";
		return nullptr;
	}
	info.usage = info.usage | ImageUsage::TransferDst;
	auto image = CreateImage(info);
	if (!image) return nullptr;

	BufferCreateInfo stagingInfo;
	stagingInfo.size = byteSize;
	stagingInfo.usage = BufferUsage::TransferSrc;
	stagingInfo.hostVisible = true;
	auto staging = CreateBuffer(stagingInfo);
	if (!staging) return nullptr;
	std::memcpy(staging->Map(), data, static_cast<size_t>(byteSize));
	staging->Flush();

	CommandList cmd = BeginCommandList();
	ImageBarrier toDst;
	toDst.image = image.get();
	toDst.oldLayout = ImageLayout::Undefined;
	toDst.newLayout = ImageLayout::TransferDst;
	toDst.dstStage = PipelineStage::Transfer;
	toDst.dstAccess = Access::TransferWrite;
	cmd.Barrier({ &toDst, 1 }, {});
	cmd.CopyBufferToImage(*staging, *image, ImageLayout::TransferDst, 0);
	ImageBarrier toFinal;
	toFinal.image = image.get();
	toFinal.oldLayout = ImageLayout::TransferDst;
	toFinal.newLayout = finalLayout;
	toFinal.srcStage = PipelineStage::Transfer;
	toFinal.srcAccess = Access::TransferWrite;
	toFinal.dstStage = dstStage;
	toFinal.dstAccess = dstAccess;
	cmd.Barrier({ &toFinal, 1 }, {});
	SubmitInfo submit;
	submit.commandLists = { &cmd, 1 };
	Submit(submit);
	WaitForFrame();
	return image;
}

std::unique_ptr<Buffer> Device::UploadBuffer(const void* data, std::uint64_t byteSize,
	BufferUsage usage, PipelineStage dstStage, Access dstAccess)
{
	if (data == nullptr || byteSize == 0) {
		std::cerr << "[RHI] UploadBuffer 参数非法\n";
		return nullptr;
	}
	BufferCreateInfo info;
	info.size = byteSize;
	info.usage = usage;
	info.hostVisible = true;
	auto buffer = CreateBuffer(info);
	if (!buffer) return nullptr;
	std::memcpy(buffer->Map(), data, static_cast<size_t>(byteSize));
	buffer->Flush();

	CommandList cmd = BeginCommandList();
	cmd.TransitionBuffer(*buffer, PipelineStage::Host, Access::HostWrite, dstStage, dstAccess);
	SubmitInfo submit;
	submit.commandLists = { &cmd, 1 };
	Submit(submit);
	WaitForFrame();
	return buffer;
}

VkImage                Image::NativeHandle() const { return m_image; }
VkFormat               Image::NativeFormat() const { return m_format; }
const ImageCreateInfo& Image::Desc() const { return m_desc; }

#pragma endregion

#pragma region Buffer

Buffer::Buffer(VmaAllocator_T* allocator, VkBuffer buffer, VmaAllocation_T* allocation,
	const BufferCreateInfo& desc, void* mapped)
	: m_desc(desc), m_buffer(buffer), m_allocation(allocation), m_allocator(allocator), m_mapped(mapped) {}

Buffer::~Buffer()
{
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

void* Buffer::Map()
{
	return m_mapped;
}

const void* Buffer::Map() const
{
	return m_mapped;
}

void Buffer::Flush(std::uint64_t offset, std::uint64_t size) const
{
	if (m_allocator == nullptr || m_allocation == nullptr || m_mapped == nullptr)
		return;
	const std::uint64_t total = m_desc.size;
	if (offset >= total)
		return;
	const std::uint64_t range = (size == 0 || size > total - offset) ? total - offset : size;
	if (vmaFlushAllocation(m_allocator, m_allocation, offset, range) != VK_SUCCESS)
		std::cerr << "[RHI] vmaFlushAllocation 失败\n";
}

void Buffer::Invalidate(std::uint64_t offset, std::uint64_t size) const
{
	if (m_allocator == nullptr || m_allocation == nullptr || m_mapped == nullptr)
		return;
	const std::uint64_t total = m_desc.size;
	if (offset >= total)
		return;
	const std::uint64_t range = (size == 0 || size > total - offset) ? total - offset : size;
	if (vmaInvalidateAllocation(m_allocator, m_allocation, offset, range) != VK_SUCCESS)
		std::cerr << "[RHI] vmaInvalidateAllocation 失败\n";
}

VkBuffer                Buffer::NativeHandle() const { return m_buffer; }
const BufferCreateInfo& Buffer::Desc() const { return m_desc; }

#pragma endregion

#pragma region Sync

Semaphore::Semaphore(VkDevice device, VkSemaphore semaphore, bool timeline)
	: m_device(device), m_semaphore(semaphore), m_timeline(timeline) {}

Semaphore::~Semaphore()
{
	if (m_device && m_semaphore != VK_NULL_HANDLE) {
		vkDestroySemaphore(m_device, m_semaphore, nullptr);
		m_semaphore = VK_NULL_HANDLE;
	}
}

VkSemaphore Semaphore::NativeHandle() const { return m_semaphore; }

bool Semaphore::Signal(std::uint64_t value)
{
	if (!m_timeline || m_semaphore == VK_NULL_HANDLE) return false;
	VkSemaphoreSignalInfo signalInfo{};
	signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
	signalInfo.semaphore = m_semaphore;
	signalInfo.value = value;
	if (vkSignalSemaphore(m_device, &signalInfo) != VK_SUCCESS) {
		std::cerr << "[RHI] vkSignalSemaphore 失败（value=" << value << "）\n";
		return false;
	}
	return true;
}

bool Semaphore::Wait(std::uint64_t value, std::uint64_t timeoutNs) const
{
	if (!m_timeline || m_semaphore == VK_NULL_HANDLE) return false;
	VkSemaphoreWaitInfo waitInfo{};
	waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &m_semaphore;
	waitInfo.pValues = &value;
	VkResult result = vkWaitSemaphores(m_device, &waitInfo, timeoutNs);
	if (result == VK_TIMEOUT) return false;
	if (result != VK_SUCCESS) {
		std::cerr << "[RHI] vkWaitSemaphores 失败\n";
		return false;
	}
	return true;
}

std::uint64_t Semaphore::Value() const
{
	if (!m_timeline || m_semaphore == VK_NULL_HANDLE) return 0;
	std::uint64_t value = 0;
	if (vkGetSemaphoreCounterValue(m_device, m_semaphore, &value) != VK_SUCCESS) {
		std::cerr << "[RHI] vkGetSemaphoreCounterValue 失败\n";
		return 0;
	}
	return value;
}

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
	return std::unique_ptr<Semaphore>(new Semaphore(m_device, semaphore, false));
}

std::unique_ptr<Semaphore> Device::CreateTimelineSemaphore(std::uint64_t initialValue)
{
	if (!m_timelineSupported) {
		std::cerr << "[RHI] 设备不支持 timelineSemaphore\n";
		return nullptr;
	}
	VkSemaphoreTypeCreateInfo typeCI{};
	typeCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	typeCI.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	typeCI.initialValue = initialValue;

	VkSemaphoreCreateInfo semaphoreCI{};
	semaphoreCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphoreCI.pNext = &typeCI;

	VkSemaphore semaphore = VK_NULL_HANDLE;
	if (vkCreateSemaphore(m_device, &semaphoreCI, nullptr, &semaphore) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateSemaphore（timeline）失败\n";
		return nullptr;
	}
	return std::unique_ptr<Semaphore>(new Semaphore(m_device, semaphore, true));
}

#pragma endregion

#pragma region Swapchain

bool Swapchain::Build(const Context& context, VkSwapchainKHR oldSwapchain, BuildResult& result)
{
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

	VkSurfaceFormatKHR chosen = formats[0];
	for (const auto& f : formats) {
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			chosen = f;
			break;
		}
	}
	if (formatCount == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
		chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
		chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	}

	VkExtent2D extent = caps.currentExtent;
	if (extent.width == UINT32_MAX) {
		extent.width = std::clamp(context.windowWidth, caps.minImageExtent.width, caps.maxImageExtent.width);
		extent.height = std::clamp(context.windowHeight, caps.minImageExtent.height, caps.maxImageExtent.height);
	}
	if (extent.width == 0 || extent.height == 0) {
		return false;
	}

	uint32_t imageCount = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

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
		result.images.push_back(Image::Borrow(context.device, image, chosen.format, backbufferDesc));
	}

	result.surfaceFormat = chosen;
	result.extent = extent;
	return true;
}

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
	m_renderDone = CreateAcquireSemaphores(m_context.device, static_cast<uint32_t>(m_images.size()));
	if (m_renderDone.size() != m_images.size()) {
		std::cerr << "[RHI] 渲染完成信号量创建失败\n";
		m_renderDone.clear();
	}
}

Swapchain::~Swapchain()
{
	if (m_context.device) vkDeviceWaitIdle(m_context.device);
	m_images.clear();
	if (m_context.device && m_swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(m_context.device, m_swapchain, nullptr);
		m_swapchain = VK_NULL_HANDLE;
	}
	m_acquireSemaphores.clear();
}

bool Swapchain::Recreate()
{
	vkDeviceWaitIdle(m_context.device);

	BuildResult built;
	if (!Build(m_context, m_swapchain, built)) return false;

	auto acquireSemaphores = CreateAcquireSemaphores(m_context.device, maxFrameInFlight);
	if (acquireSemaphores.size() != maxFrameInFlight) {
		vkDestroySwapchainKHR(m_context.device, built.swapchain, nullptr);
		return false;
	}

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
	context.maxFramesInFlight = static_cast<uint32_t>(m_frameSlots.size());

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
	Frame frame;
	m_lastFrame = Frame{};

	if (m_recreatePending || NeedsRecreate()) {
		Recreate();
		return frame;
	}

	VkSemaphore acquire = m_acquireSemaphores[m_frameIndex]->NativeHandle();

	uint32_t imageIndex = 0;
	VkResult result = vkAcquireNextImageKHR(m_context.device, m_swapchain, UINT64_MAX,
		acquire, VK_NULL_HANDLE, &imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		Recreate();
		return frame;
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		std::cerr << "[RHI] vkAcquireNextImageKHR 失败: " << result << "\n";
		return frame;
	}
	if (result == VK_SUBOPTIMAL_KHR) m_recreatePending = true;

	frame.imageIndex = imageIndex;
	frame.backbuffer = m_images[imageIndex].get();
	frame.ready = m_acquireSemaphores[m_frameIndex].get();
	frame.presentReady = imageIndex < m_renderDone.size() ? m_renderDone[imageIndex].get() : nullptr;
	frame.acquired = true;
	m_lastFrame = frame;
	m_frameIndex = (m_frameIndex + 1) % maxFrameInFlight;
	return frame;
}

void Swapchain::Present(const Semaphore* presentWait)
{
	if (!m_lastFrame.acquired) return;

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
		Recreate();
	} else if (result == VK_SUBOPTIMAL_KHR) {
		m_recreatePending = true;
	} else if (result != VK_SUCCESS) {
		std::cerr << "[RHI] vkQueuePresentKHR 失败: " << result << "\n";
	}
}

ImageFormat Swapchain::Format() const { return FromVkFormat(m_surfaceFormat.format); }
uint32_t    Swapchain::Width()  const { return m_extent.width; }
uint32_t    Swapchain::Height() const { return m_extent.height; }

#pragma endregion

#pragma region Descriptor

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
	if (m_device && m_pool && m_set != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(m_device, m_pool, 1, &m_set);
		m_set = VK_NULL_HANDLE;
	}
}

VkDescriptorSet DescriptorSet::NativeHandle() const { return m_set; }

void DescriptorSet::Write(uint32_t binding, const Image& image, ImageSubresource view, uint32_t count,
	const Sampler* sampler, ImageFormat viewFormat, uint32_t firstElement, ImageLayout layout)
{
	if (count == 0) {
		std::cerr << "[RHI] 描述符写入失败：count 为 0（binding " << binding << "）\n";
		return;
	}

	VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	if (auto it = m_bindingTypes.find(binding); it != m_bindingTypes.end()) {
		if (it->second == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	}
	const bool storage = (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	if (layout == ImageLayout::Undefined)
		layout = storage ? ImageLayout::General : ImageLayout::ShaderReadOnly;

	std::vector<VkDescriptorImageInfo> imageInfos(count);
	for (uint32_t i = 0; i < count; ++i) {
		ImageViewDesc viewDesc;
		viewDesc.range.base = ImageSubresource{ view.mipLevel, view.layer + i };
		viewDesc.range.mipLevelCount = 1;
		viewDesc.range.layerCount = 1;
		viewDesc.format = viewFormat;
		const ImageView& imageView = image.CreateView(viewDesc);
		if (imageView.NativeHandle() == VK_NULL_HANDLE) {
			std::cerr << "[RHI] 描述符写入失败：图像视图无效（binding " << binding
				<< " 元素 " << i << "）\n";
			return;
		}
		imageInfos[i].sampler = storage ? VK_NULL_HANDLE
			: (sampler != nullptr ? sampler->NativeHandle() : m_defaultSampler);
		imageInfos[i].imageView = imageView.NativeHandle();
		imageInfos[i].imageLayout = ToVkImageLayout(layout);
	}

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_set;
	write.dstBinding = binding;
	write.dstArrayElement = firstElement;
	write.descriptorCount = count;
	write.descriptorType = type;
	write.pImageInfo = imageInfos.data();

	vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

void DescriptorSet::Write(uint32_t binding, const Buffer& buffer, uint64_t offset, uint64_t range, uint32_t count)
{
	if (count == 0 || (count > 1 && range == 0)) {
		std::cerr << "[RHI] 描述符写入失败：数组绑定需 count > 0 且 range 可步进（binding " << binding << "）\n";
		return;
	}

	VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	if (auto it = m_bindingTypes.find(binding); it != m_bindingTypes.end()) {
		if (it->second == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	}

	std::vector<VkDescriptorBufferInfo> bufferInfos(count);
	for (uint32_t i = 0; i < count; ++i) {
		bufferInfos[i].buffer = buffer.NativeHandle();
		bufferInfos[i].offset = offset + static_cast<uint64_t>(i) * range;
		bufferInfos[i].range = range;
	}

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_set;
	write.dstBinding = binding;
	write.dstArrayElement = 0;
	write.descriptorCount = count;
	write.descriptorType = type;
	write.pBufferInfo = bufferInfos.data();

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

	std::unordered_map<uint32_t, VkDescriptorType> bindingTypes;
	for (const auto& b : layout.Bindings()) bindingTypes[b.binding] = ToVkDescriptorType(b.type);

	return std::unique_ptr<DescriptorSet>(new DescriptorSet(m_device, m_descriptorPool,
		m_defaultSampler, set, std::move(bindingTypes)));
}

std::unique_ptr<DescriptorSetLayout> Device::CreateDescriptorSetLayout(
	const std::vector<DescriptorBinding>& bindings, bool partiallyBound)
{
	if (bindings.empty()) {
		std::cerr << "[RHI] CreateDescriptorSetLayout 失败：bindings 为空\n";
		return nullptr;
	}

	std::vector<VkDescriptorSetLayoutBinding> vkBindings(bindings.size());
	std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(),
		partiallyBound ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0);
	for (size_t i = 0; i < bindings.size(); ++i) {
		const DescriptorBinding& b = bindings[i];
		VkDescriptorSetLayoutBinding vb{};
		vb.binding = b.binding;
		vb.descriptorType = ToVkDescriptorType(b.type);
		vb.descriptorCount = b.arrayCount;
		vb.stageFlags = b.stages == ShaderStageMask::None
			? VK_SHADER_STAGE_ALL : ToVkStageMask(b.stages);
		vkBindings[i] = vb;
	}

	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCI{};
	flagsCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	flagsCI.bindingCount = partiallyBound ? static_cast<uint32_t>(bindingFlags.size()) : 0;
	flagsCI.pBindingFlags = partiallyBound ? bindingFlags.data() : nullptr;

	VkDescriptorSetLayoutCreateInfo layoutCI{};
	layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutCI.pNext = partiallyBound ? &flagsCI : nullptr;
	layoutCI.bindingCount = static_cast<uint32_t>(vkBindings.size());
	layoutCI.pBindings = vkBindings.data();

	VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
	if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &rawLayout) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateDescriptorSetLayout 失败（手工布局）\n";
		return nullptr;
	}
	return std::unique_ptr<DescriptorSetLayout>(
		new DescriptorSetLayout(m_device, rawLayout, bindings));
}

#pragma endregion

#pragma region Pipeline

Pipeline::Pipeline(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout, uint32_t pushConstantSize,
	VkShaderStageFlags stageFlags, VkPipelineBindPoint bindPoint,
	std::vector<std::unique_ptr<DescriptorSetLayout>> setLayouts)
	: m_device(device), m_pipeline(pipeline), m_layout(layout)
	, m_pushConstantSize(pushConstantSize), m_stageFlags(stageFlags), m_bindPoint(bindPoint)
	, m_setLayouts(std::move(setLayouts)) {}

Pipeline::~Pipeline()
{
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

VkPipelineBindPoint Pipeline::NativeBindPoint() const { return m_bindPoint; }

VkShaderStageFlags Pipeline::NativeStageFlags() const { return m_stageFlags; }

uint32_t Pipeline::PushConstantSize() const { return m_pushConstantSize; }

const DescriptorSetLayout* Pipeline::SetLayout(uint32_t setIndex) const
{
	return setIndex < m_setLayouts.size() ? m_setLayouts[setIndex].get() : nullptr;
}

bool Device::BuildSetLayouts(std::vector<MergedDescriptor> slots, uint32_t pushConstantSize,
	VkShaderStageFlags pushStages, const DescriptorSetLayout* externalSet0,
	std::vector<std::unique_ptr<DescriptorSetLayout>>& outLayouts, VkPipelineLayout& outLayout)
{
	if (pushConstantSize > 0 && (pushConstantSize % 4 != 0 || pushConstantSize > 128)) {
		std::cerr << "[RHI] pushConstantSize 须 4 字节对齐且 <= 128: " << pushConstantSize << "\n";
		return false;
	}

	if (externalSet0 != nullptr) {
		slots.erase(std::remove_if(slots.begin(), slots.end(),
			[](const MergedDescriptor& s) { return s.binding.set == 0; }), slots.end());
	}

	uint32_t setCount = 0;
	for (const auto& s : slots) setCount = std::max(setCount, s.binding.set + 1);
	if (externalSet0 != nullptr) setCount = std::max(setCount, 1u);

	std::vector<std::vector<MergedDescriptor>> bySet(setCount);
	for (auto& s : slots)
		bySet[s.binding.set].push_back(std::move(s));
	for (auto& group : bySet) {
		std::sort(group.begin(), group.end(),
			[](const MergedDescriptor& a, const MergedDescriptor& c) { return a.binding.binding < c.binding.binding; });
	}

	outLayouts.clear();
	outLayouts.resize(setCount);
	const uint32_t firstOwnedSet = externalSet0 != nullptr ? 1u : 0u;
	for (uint32_t i = firstOwnedSet; i < setCount; ++i) {
		std::vector<VkDescriptorSetLayoutBinding> vkBindings;
		vkBindings.reserve(bySet[i].size());
		std::vector<DescriptorBinding> groupDescs;
		groupDescs.reserve(bySet[i].size());
		for (auto& s : bySet[i]) {
			VkDescriptorSetLayoutBinding vb{};
			vb.binding = s.binding.binding;
			vb.descriptorType = ToVkDescriptorType(s.binding.type);
			vb.descriptorCount = s.binding.arrayCount;
			vb.stageFlags = s.stages;
			vkBindings.push_back(vb);
			groupDescs.push_back(std::move(s.binding));
		}

		VkDescriptorSetLayoutCreateInfo layoutCI{};
		layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutCI.bindingCount = static_cast<uint32_t>(vkBindings.size());
		layoutCI.pBindings = vkBindings.empty() ? nullptr : vkBindings.data();

		VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
		if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &rawLayout) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateDescriptorSetLayout 失败（set " << i << "）\n";
			outLayouts.clear();
			return false;
		}
		outLayouts[i].reset(new DescriptorSetLayout(m_device, rawLayout, std::move(groupDescs)));
	}

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = pushStages;
	pushRange.size = pushConstantSize;

	VkPipelineLayoutCreateInfo layoutCI{};
	layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutCI.setLayoutCount = static_cast<uint32_t>(outLayouts.size());
	std::vector<VkDescriptorSetLayout> rawLayouts(outLayouts.size(), VK_NULL_HANDLE);
	for (size_t i = 0; i < outLayouts.size(); ++i) {
		if (i == 0 && externalSet0 != nullptr) rawLayouts[i] = externalSet0->NativeHandle();
		else if (outLayouts[i]) rawLayouts[i] = outLayouts[i]->NativeHandle();
	}
	layoutCI.pSetLayouts = rawLayouts.empty() ? nullptr : rawLayouts.data();
	layoutCI.pushConstantRangeCount = (pushConstantSize > 0) ? 1u : 0u;
	layoutCI.pPushConstantRanges = (pushConstantSize > 0) ? &pushRange : nullptr;

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &pipelineLayout) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreatePipelineLayout 失败\n";
		outLayouts.clear();
		return false;
	}

	outLayout = pipelineLayout;
	return true;
}

std::unique_ptr<Pipeline> Device::CreatePipeline(const GraphicsPipelineDesc& desc,
	const DescriptorSetLayout* externalSet0)
{
	if (!ValidateStageCombination(desc.shaders))
		return nullptr;
	bool hasTess = false;
	bool hasGeometry = false;
	for (const auto& s : desc.shaders) {
		switch (s.stage) {
			case ShaderStage::Vertex:
			case ShaderStage::Fragment:
			case ShaderStage::TessControl:
			case ShaderStage::TessEval:
			case ShaderStage::Geometry:
				break;
			default:
				std::cerr << "[RHI] 暂不支持的着色器阶段: " << ShaderStageName(s.stage)
					<< "（task/mesh 待后续版本）\n";
				return nullptr;
		}
		if (s.stage == ShaderStage::TessControl || s.stage == ShaderStage::TessEval)
			hasTess = true;
		if (s.stage == ShaderStage::Geometry)
			hasGeometry = true;
	}
	if (hasGeometry && !m_geometrySupported) {
		std::cerr << "[RHI] 设备不支持几何着色器\n";
		return nullptr;
	}
	if (hasTess) {
		if (!m_tessellationSupported) {
			std::cerr << "[RHI] 设备不支持细分着色器\n";
			return nullptr;
		}
		if (desc.topology != PrimitiveTopology::PatchList) {
			std::cerr << "[RHI] 细分管线要求 topology = PatchList\n";
			return nullptr;
		}
		if (desc.tessPatchControlPoints == 0
			|| desc.tessPatchControlPoints > m_maxTessellationPatchSize) {
			std::cerr << "[RHI] tessPatchControlPoints(" << desc.tessPatchControlPoints
				<< ") 越界，设备上限为 " << m_maxTessellationPatchSize << "\n";
			return nullptr;
		}
	}

	if (!desc.colorStates.empty() && desc.colorStates.size() != desc.colorAttachmentCount) {
		std::cerr << "[RHI] colorStates 数量(" << desc.colorStates.size()
			<< ") 与颜色附件数(" << desc.colorAttachmentCount << ") 不一致\n";
		return nullptr;
	}
	if (!desc.colorStates.empty() && !m_independentBlendSupported) {
		const ColorAttachmentState& first = desc.colorStates.front();
		for (const ColorAttachmentState& st : desc.colorStates) {
			if (st.enableBlend != first.enableBlend || st.srcColor != first.srcColor
				|| st.dstColor != first.dstColor || st.srcAlpha != first.srcAlpha
				|| st.dstAlpha != first.dstAlpha || st.colorOp != first.colorOp
				|| st.alphaOp != first.alphaOp || st.writeMask != first.writeMask) {
				std::cerr << "[RHI] 设备不支持 independentBlend：逐附件 blend/writeMask 必须完全一致\n";
				return nullptr;
			}
		}
	}
	if (desc.depthFormat != ImageFormat::None && desc.depthStencil.depthBoundsTestEnable
		&& !m_depthBoundsSupported) {
		std::cerr << "[RHI] 设备不支持 depthBounds，已关闭该测试（depth test/compare 保留）\n";
	}

	std::vector<std::vector<uint32_t>> codes(desc.shaders.size());
	for (size_t i = 0; i < desc.shaders.size(); ++i) {
		codes[i] = ReadSpirv(desc.shaders[i].spirvPath);
		if (codes[i].empty()) {
			std::cerr << "[RHI] 读取 SPIR-V 失败: " << desc.shaders[i].spirvPath << "\n";
			return nullptr;
		}
	}

	std::vector<StageCode> stageCodes;
	stageCodes.reserve(desc.shaders.size());
	for (size_t i = 0; i < desc.shaders.size(); ++i)
		stageCodes.push_back({ desc.shaders[i].stage, &codes[i] });

	std::vector<MergedDescriptor> merged;
	VkShaderStageFlags allStages = 0;
	if (!ReflectAndMergeBindings(stageCodes, merged, allStages))
		return nullptr;

	if (!m_vertexPipelineStoresAndAtomics) {
		const uint32_t vertexish = static_cast<uint32_t>(
			ShaderStageMask::Vertex | ShaderStageMask::TessControl
			| ShaderStageMask::TessEval | ShaderStageMask::Geometry);
		for (const auto& slot : merged) {
			if ((static_cast<uint32_t>(slot.binding.stages) & vertexish) == 0)
				continue;
			if (slot.binding.nonWritable)
				continue;
			if (slot.binding.type == ResourceType::StorageBuffer
				|| slot.binding.type == ResourceType::StorageImage) {
				std::cerr << "[RHI] 顶点/细分/几何阶段引用可写 storage 资源 \""
					<< slot.binding.name
					<< "\"；请在 shader 中声明 readonly，或使用支持 vertexPipelineStoresAndAtomics 的设备\n";
			}
		}
	}

	std::vector<std::unique_ptr<DescriptorSetLayout>> layouts;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	if (!BuildSetLayouts(merged, desc.pushConstantSize, allStages, externalSet0, layouts, pipelineLayout))
		return nullptr;

	auto createModule = [&](const std::vector<uint32_t>& code, VkShaderModule& out) {
		VkShaderModuleCreateInfo moduleCI{};
		moduleCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleCI.codeSize = code.size() * sizeof(uint32_t);
		moduleCI.pCode = code.data();
		if (vkCreateShaderModule(m_device, &moduleCI, nullptr, &out) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateShaderModule 失败\n";
			return false;
		}
		return true;
	};

	std::vector<VkShaderModule> modules(desc.shaders.size(), VK_NULL_HANDLE);
	bool modulesOk = true;
	for (size_t i = 0; i < desc.shaders.size() && modulesOk; ++i)
		modulesOk = createModule(codes[i], modules[i]);
	if (!modulesOk) {
		for (VkShaderModule m : modules) if (m) vkDestroyShaderModule(m_device, m, nullptr);
		vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
		return nullptr;
	}

	std::vector<VkPipelineShaderStageCreateInfo> stages(desc.shaders.size());
	for (size_t i = 0; i < desc.shaders.size(); ++i) {
		stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[i].stage = ToVkStage(desc.shaders[i].stage);
		stages[i].module = modules[i];
		stages[i].pName = "main";
	}

	std::vector<VkVertexInputBindingDescription> vkBindings;
	std::vector<VkVertexInputAttributeDescription> vkAttributes;
	if (desc.vertexBindings.empty() && desc.vertexAttributes.empty()) {
		const std::vector<uint32_t>* vertexCode = nullptr;
		for (size_t i = 0; i < desc.shaders.size(); ++i)
			if (desc.shaders[i].stage == ShaderStage::Vertex) vertexCode = &codes[i];
		if (vertexCode && !DeriveVertexLayout(*vertexCode, vkBindings, vkAttributes)) {
			for (VkShaderModule m : modules) vkDestroyShaderModule(m_device, m, nullptr);
			vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
			return nullptr;
		}
	}
	else {
		vkBindings.reserve(desc.vertexBindings.size());
		for (const auto& b : desc.vertexBindings) {
			VkVertexInputBindingDescription vk{};
			vk.binding = b.binding;
			vk.stride = b.stride;
			vk.inputRate = b.perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
			vkBindings.push_back(vk);
		}
		vkAttributes.reserve(desc.vertexAttributes.size());
		for (const auto& a : desc.vertexAttributes) {
			VkVertexInputAttributeDescription vk{};
			vk.location = a.location;
			vk.binding = a.binding;
			vk.format = ToVkVertexFormat(a.format);
			vk.offset = a.offset;
			vkAttributes.push_back(vk);
		}
	}

	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vkBindings.size());
	vertexInput.pVertexBindingDescriptions = vkBindings.data();
	vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vkAttributes.size());
	vertexInput.pVertexAttributeDescriptions = vkAttributes.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = ToVkTopology(desc.topology);

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
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.samples == 0 ? 1u : desc.samples);

	const bool hasDepth = desc.depthFormat != ImageFormat::None;
	const bool hasStencil = desc.depthFormat == ImageFormat::D24S8;
	const DepthStencilState& ds = desc.depthStencil;
	rasterization.depthBiasEnable = ds.depthBiasEnable ? VK_TRUE : VK_FALSE;
	rasterization.depthBiasConstantFactor = ds.depthBiasConstant;
	rasterization.depthBiasSlopeFactor = ds.depthBiasSlope;
	rasterization.depthBiasClamp = ds.depthBiasClamp;
	VkStencilOpState frontFace{};
	VkStencilOpState backFace{};
	auto fillStencilFace = [](VkStencilOpState& out, const StencilFaceState& face) {
		out.failOp = ToVkStencilOp(face.failOp);
		out.passOp = ToVkStencilOp(face.passOp);
		out.depthFailOp = ToVkStencilOp(face.depthFailOp);
		out.compareOp = ToVkCompareOp(face.compareOp);
		out.compareMask = face.compareMask;
		out.writeMask = face.writeMask;
		out.reference = face.reference;
	};
	fillStencilFace(frontFace, ds.front);
	fillStencilFace(backFace, ds.back);

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = (hasDepth && ds.depthTestEnable) ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = (hasDepth && ds.depthWriteEnable) ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = ToVkCompareOp(ds.depthCompareOp);
	depthStencil.depthBoundsTestEnable = (hasDepth && ds.depthBoundsTestEnable && m_depthBoundsSupported)
		? VK_TRUE : VK_FALSE;
	depthStencil.minDepthBounds = ds.minDepthBounds;
	depthStencil.maxDepthBounds = ds.maxDepthBounds;
	depthStencil.stencilTestEnable = (hasStencil && ds.stencilTestEnable) ? VK_TRUE : VK_FALSE;
	depthStencil.front = frontFace;
	depthStencil.back = backFace;

	const ColorAttachmentState defaultColorState{};
	std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
	blendAttachments.reserve(desc.colorAttachmentCount);
	for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
		const ColorAttachmentState& st = desc.colorStates.empty() ? defaultColorState : desc.colorStates[i];
		VkPipelineColorBlendAttachmentState blend{};
		blend.blendEnable = st.enableBlend ? VK_TRUE : VK_FALSE;
		blend.srcColorBlendFactor = ToVkBlendFactor(st.srcColor);
		blend.dstColorBlendFactor = ToVkBlendFactor(st.dstColor);
		blend.colorBlendOp = ToVkBlendOp(st.colorOp);
		blend.srcAlphaBlendFactor = ToVkBlendFactor(st.srcAlpha);
		blend.dstAlphaBlendFactor = ToVkBlendFactor(st.dstAlpha);
		blend.alphaBlendOp = ToVkBlendOp(st.alphaOp);
		blend.colorWriteMask = ToVkColorWriteMask(st.writeMask);
		blendAttachments.push_back(blend);
	}

	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
	colorBlend.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();
	std::copy(desc.blendConstants.begin(), desc.blendConstants.end(), colorBlend.blendConstants);

	std::vector<VkFormat> colorFormats;
	colorFormats.reserve(desc.colorAttachmentCount);
	for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
		colorFormats.push_back(ToVkFormat(desc.colorFormats[i]));
	}
	const VkFormat depthFormat = hasDepth ? ToVkFormat(desc.depthFormat) : VK_FORMAT_UNDEFINED;
	const VkFormat stencilFormat = (desc.depthFormat == ImageFormat::D24S8)
		? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_UNDEFINED;

	VkPipelineRenderingCreateInfo renderingInfo{};
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
	renderingInfo.pColorAttachmentFormats = colorFormats.empty() ? nullptr : colorFormats.data();
	renderingInfo.depthAttachmentFormat = depthFormat;
	renderingInfo.stencilAttachmentFormat = stencilFormat;

	VkPipelineTessellationStateCreateInfo tessellation{};
	if (hasTess) {
		tessellation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
		tessellation.patchControlPoints = desc.tessPatchControlPoints;
	}

	VkGraphicsPipelineCreateInfo pipelineCI{};
	pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCI.pNext = &renderingInfo;
	pipelineCI.stageCount = static_cast<uint32_t>(stages.size());
	pipelineCI.pStages = stages.data();
	pipelineCI.pVertexInputState = &vertexInput;
	pipelineCI.pInputAssemblyState = &inputAssembly;
	pipelineCI.pTessellationState = hasTess ? &tessellation : nullptr;
	pipelineCI.pViewportState = &viewportState;
	pipelineCI.pRasterizationState = &rasterization;
	pipelineCI.pMultisampleState = &multisample;
	pipelineCI.pDepthStencilState = &depthStencil;
	pipelineCI.pColorBlendState = &colorBlend;
	pipelineCI.pDynamicState = &dynamicState;
	pipelineCI.layout = pipelineLayout;
	pipelineCI.renderPass = VK_NULL_HANDLE;
	pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
	pipelineCI.basePipelineIndex = -1;

	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &pipeline);

	for (VkShaderModule m : modules) vkDestroyShaderModule(m_device, m, nullptr);

	if (result != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateGraphicsPipelines 失败: " << result << "\n";
		vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
		return nullptr;
	}

	return std::unique_ptr<Pipeline>(new Pipeline(m_device, pipeline, pipelineLayout,
		desc.pushConstantSize, allStages, VK_PIPELINE_BIND_POINT_GRAPHICS, std::move(layouts)));
}

std::unique_ptr<Pipeline> Device::CreateComputePipeline(const ComputePipelineDesc& desc,
	const DescriptorSetLayout* externalSet0)
{
	if (desc.shader.stage != ShaderStage::Compute) {
		std::cerr << "[RHI] 计算管线须声明 Compute 阶段（收到 "
			<< ShaderStageName(desc.shader.stage) << "）\n";
		return nullptr;
	}

	const std::vector<uint32_t> code = ReadSpirv(desc.shader.spirvPath);
	if (code.empty()) {
		std::cerr << "[RHI] 读取 SPIR-V 失败: " << desc.shader.spirvPath << "\n";
		return nullptr;
	}

	std::vector<MergedDescriptor> slots;
	VkShaderStageFlags allStages = 0;
	if (!ReflectAndMergeBindings({ { ShaderStage::Compute, &code } }, slots, allStages))
		return nullptr;

	std::vector<std::unique_ptr<DescriptorSetLayout>> layouts;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	if (!BuildSetLayouts(slots, desc.pushConstantSize, allStages, externalSet0, layouts, pipelineLayout))
		return nullptr;

	VkShaderModuleCreateInfo moduleCI{};
	moduleCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	moduleCI.codeSize = code.size() * sizeof(uint32_t);
	moduleCI.pCode = code.data();

	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(m_device, &moduleCI, nullptr, &module) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateShaderModule 失败\n";
		vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
		return nullptr;
	}

	VkComputePipelineCreateInfo pipelineCI{};
	pipelineCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineCI.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipelineCI.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipelineCI.stage.module = module;
	pipelineCI.stage.pName = "main";
	pipelineCI.layout = pipelineLayout;
	pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
	pipelineCI.basePipelineIndex = -1;

	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result = vkCreateComputePipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &pipeline);

	vkDestroyShaderModule(m_device, module, nullptr);

	if (result != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateComputePipelines 失败: " << result << "\n";
		vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
		return nullptr;
	}

	return std::unique_ptr<Pipeline>(new Pipeline(m_device, pipeline, pipelineLayout,
		desc.pushConstantSize, VK_SHADER_STAGE_COMPUTE_BIT, VK_PIPELINE_BIND_POINT_COMPUTE,
		std::move(layouts)));
}

#pragma endregion

#pragma region FrameSlot

FrameSlot::FrameSlot(VkDevice device, std::vector<uint32_t> queueFamilies)
	: m_device(device)
{
	for (uint32_t family : queueFamilies) {
		VkCommandPoolCreateInfo poolCI{};
		poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolCI.queueFamilyIndex = family;
		VkCommandPool pool = VK_NULL_HANDLE;
		if (vkCreateCommandPool(m_device, &poolCI, nullptr, &pool) != VK_SUCCESS) {
			std::cerr << "[RHI] vkCreateCommandPool 失败（family=" << family << "）\n";
			return;
		}
		m_pools.push_back(FamilyPool{ family, pool });
	}
	m_liveBuffers.resize(m_pools.size());

	VkQueryPoolCreateInfo queryCI{};
	queryCI.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	queryCI.queryType = VK_QUERY_TYPE_TIMESTAMP;
	queryCI.queryCount = kTimestampQueries;
	if (vkCreateQueryPool(m_device, &queryCI, nullptr, &m_timestampPool) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateQueryPool（时间戳）失败\n";
		m_timestampPool = VK_NULL_HANDLE;
	}
}

FrameSlot::~FrameSlot()
{
	if (m_device) {
		for (size_t i = 0; i < m_pools.size() && i < m_liveBuffers.size(); ++i) {
			auto& live = m_liveBuffers[i];
			if (!live.empty()) {
				vkFreeCommandBuffers(m_device, m_pools[i].pool,
					static_cast<uint32_t>(live.size()), live.data());
				live.clear();
			}
		}
		m_liveBuffers.clear();
		for (VkFence fence : m_fencePool) {
			if (fence != VK_NULL_HANDLE) {
				vkDestroyFence(m_device, fence, nullptr);
			}
		}
		m_fencePool.clear();
		m_pending.clear();
		if (m_timestampPool != VK_NULL_HANDLE) {
			vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
			m_timestampPool = VK_NULL_HANDLE;
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
	beginCI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(buffer, &beginCI) != VK_SUCCESS) {
		std::cerr << "[RHI] vkBeginCommandBuffer 失败\n";
		vkFreeCommandBuffers(m_device, m_pools[poolIndex].pool, 1, &buffer);
		return VK_NULL_HANDLE;
	}

	m_liveBuffers[poolIndex].push_back(buffer);
	return buffer;
}

VkFence FrameSlot::AcquireFence()
{
	if (!m_fencePool.empty()) {
		VkFence fence = m_fencePool.back();
		m_fencePool.pop_back();
		return fence;
	}
	VkFenceCreateInfo fenceCI{};
	fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(m_device, &fenceCI, nullptr, &fence) != VK_SUCCESS) {
		std::cerr << "[RHI] vkCreateFence（帧槽）失败\n";
		return VK_NULL_HANDLE;
	}
	return fence;
}

void FrameSlot::ReleaseFence(VkFence fence)
{
	if (fence != VK_NULL_HANDLE) {
		m_fencePool.push_back(fence);
	}
}

void FrameSlot::MarkPending(VkFence fence, bool owned)
{
	if (fence != VK_NULL_HANDLE) {
		m_pending.push_back(PendingSubmit{ fence, owned });
	}
}

void FrameSlot::DrainPending()
{
	if (m_pending.empty()) return;

	std::vector<VkFence> fences;
	fences.reserve(m_pending.size());
	for (const PendingSubmit& p : m_pending) {
		if (p.fence != VK_NULL_HANDLE) fences.push_back(p.fence);
	}
	if (!fences.empty()
		&& vkWaitForFences(m_device, static_cast<uint32_t>(fences.size()), fences.data(),
			VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
		std::cerr << "[RHI] 帧槽回收：vkWaitForFences 失败\n";
	}

	for (const PendingSubmit& p : m_pending) {
		if (p.owned && p.fence != VK_NULL_HANDLE) {
			vkResetFences(m_device, 1, &p.fence);
			m_fencePool.push_back(p.fence);
		}
	}
	m_pending.clear();
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

#pragma region CommandList

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
		vk.srcQueueFamilyIndex = b.srcQueueFamily;
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
		vk.srcQueueFamilyIndex = b.srcQueueFamily;
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

void CommandList::TransitionImage(Image& image, ImageLayout oldLayout, ImageLayout newLayout,
	ImageSubresourceRange range, PipelineStage srcStage, Access srcAccess,
	PipelineStage dstStage, Access dstAccess, uint32_t srcQueueFamily, uint32_t dstQueueFamily)
{
	ImageBarrier barrier;
	barrier.image = &image;
	barrier.range = range;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcStage = srcStage;
	barrier.dstStage = dstStage;
	barrier.srcAccess = srcAccess;
	barrier.dstAccess = dstAccess;
	barrier.srcQueueFamily = srcQueueFamily;
	barrier.dstQueueFamily = dstQueueFamily;
	Barrier({ &barrier, 1 }, {});
}

void CommandList::TransitionBuffer(Buffer& buffer, PipelineStage srcStage, Access srcAccess,
	PipelineStage dstStage, Access dstAccess, uint64_t offset, uint64_t size,
	uint32_t srcQueueFamily, uint32_t dstQueueFamily)
{
	BufferBarrier barrier;
	barrier.buffer = &buffer;
	barrier.offset = offset;
	barrier.size = size;
	barrier.srcStage = srcStage;
	barrier.dstStage = dstStage;
	barrier.srcAccess = srcAccess;
	barrier.dstAccess = dstAccess;
	barrier.srcQueueFamily = srcQueueFamily;
	barrier.dstQueueFamily = dstQueueFamily;
	Barrier({}, { &barrier, 1 });
}

void CommandList::BeginRendering(const RenderingInfo& info)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	if (info.colorCount > 0 && info.colors == nullptr) return;

	std::vector<VkRenderingAttachmentInfo> colors;
	colors.reserve(info.colorCount);
	for (uint32_t i = 0; i < info.colorCount; ++i) {
		const RenderingAttachment& att = info.colors[i];
		if (!att.image) continue;
		VkRenderingAttachmentInfo vk{};
		vk.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		ImageViewDesc viewDesc;
		viewDesc.range = att.range;
		vk.imageView = att.image->CreateView(viewDesc).NativeHandle();
		vk.imageLayout = ToVkImageLayout(att.layout);
		vk.loadOp = ToVkLoadOp(att.load);
		vk.storeOp = ToVkStoreOp(att.store);
		vk.clearValue = ToVkClearValue(att.clear);
		// Vulkan 1.4 起解算是逐附件字段（取代旧的 pResolveAttachments 数组）
		vk.resolveMode = VK_RESOLVE_MODE_NONE;
		if (att.resolveImage != nullptr) {
			ImageViewDesc resolveDesc;
			resolveDesc.range = att.resolveRange;
			vk.resolveImageView = att.resolveImage->CreateView(resolveDesc).NativeHandle();
			vk.resolveImageLayout = ToVkImageLayout(att.resolveLayout);
			vk.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
		}
		colors.push_back(vk);
	}

	VkRenderingAttachmentInfo depth{};
	const bool hasDepth = info.depth && info.depth->image;
	if (hasDepth) {
		depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		ImageViewDesc viewDesc;
		viewDesc.range = info.depth->range;
		depth.imageView = info.depth->image->CreateView(viewDesc).NativeHandle();
		depth.imageLayout = ToVkImageLayout(info.depth->layout);
		depth.loadOp = ToVkLoadOp(info.depth->load);
		depth.storeOp = ToVkStoreOp(info.depth->store);
		depth.clearValue = ToVkClearValue(info.depth->clear);
	}

	VkRenderingInfo rendering{};
	rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea.offset = { info.renderArea.x, info.renderArea.y };
	rendering.renderArea.extent = { info.renderArea.width, info.renderArea.height };
	rendering.layerCount = info.layerCount == 0 ? 1 : info.layerCount;
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
	vkCmdBindPipeline(m_commandBuffer, pipeline.NativeBindPoint(), pipeline.NativeHandle());
	m_pipelineLayout = pipeline.NativeLayout();
	m_pushConstantSize = pipeline.PushConstantSize();
	m_bindPoint = pipeline.NativeBindPoint();
	m_stageFlags = pipeline.NativeStageFlags();
}

void CommandList::BindDescriptorSet(uint32_t setIndex, const DescriptorSet& set)
{
	if (m_commandBuffer == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE) return;
	VkDescriptorSet raw = set.NativeHandle();
	vkCmdBindDescriptorSets(m_commandBuffer, m_bindPoint, m_pipelineLayout,
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
	vkCmdPushConstants(m_commandBuffer, m_pipelineLayout, m_stageFlags, 0, size, data);
}

void CommandList::SetViewport(const Viewport& viewport)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	VkViewport vk{ viewport.x, viewport.y, viewport.w, viewport.h, viewport.minDepth, viewport.maxDepth };
	vkCmdSetViewport(m_commandBuffer, 0, 1, &vk);
	VkRect2D scissor{ { static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y) },
		{ static_cast<uint32_t>(viewport.w), static_cast<uint32_t>(viewport.h) } };
	vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
}

void CommandList::BindVertexBuffer(const Buffer& buffer, uint64_t offset, uint32_t firstBinding)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	VkBuffer raw = buffer.NativeHandle();
	VkDeviceSize off = offset;
	vkCmdBindVertexBuffers(m_commandBuffer, firstBinding, 1, &raw, &off);
}

void CommandList::BindIndexBuffer(const Buffer& buffer, uint64_t offset, IndexType type)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdBindIndexBuffer(m_commandBuffer, buffer.NativeHandle(), offset, ToVkIndexType(type));
}

void CommandList::Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instanceCount)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, firstVertex, 0);
}

void CommandList::DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset,
	uint32_t instanceCount)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, 0);
}

void CommandList::DrawIndirect(const Buffer& args, uint64_t offset, uint32_t drawCount)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdDrawIndirect(m_commandBuffer, args.NativeHandle(), offset, drawCount, 0);
}

void CommandList::DrawIndexedIndirect(const Buffer& args, uint64_t offset, uint32_t drawCount)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdDrawIndexedIndirect(m_commandBuffer, args.NativeHandle(), offset, drawCount, 0);
}

void CommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	if (groupCountX == 0 || groupCountY == 0 || groupCountZ == 0) {
		std::cerr << "[RHI] Dispatch 工作组数须非零（" << groupCountX << ","
			<< groupCountY << "," << groupCountZ << "）\n";
		return;
	}
	vkCmdDispatch(m_commandBuffer, groupCountX, groupCountY, groupCountZ);
}

void CommandList::DispatchIndirect(const Buffer& args, uint64_t offset)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	vkCmdDispatchIndirect(m_commandBuffer, args.NativeHandle(), offset);
}

static uint32_t TexelBytes(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_S8_UINT:                              return 1;
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R16_SFLOAT:                           return 2;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:                   return 4;
	case VK_FORMAT_R16G16B16A16_SFLOAT:                  return 8;
	case VK_FORMAT_R32G32_SFLOAT:                        return 8;
	case VK_FORMAT_R32G32B32A32_SFLOAT:                  return 16;
	default:                                             return 0;
	}
}

static void MipExtent(const ImageCreateInfo& desc, uint32_t mip,
	uint32_t& width, uint32_t& height, uint32_t& depth)
{
	width = std::max(1u, desc.width >> mip);
	height = std::max(1u, (desc.height == 0 ? 1u : desc.height) >> mip);
	depth = std::max(1u, (desc.depth == 0 ? 1u : desc.depth) >> mip);
}

static bool BuildBufferImageRegion(const Buffer& buffer, const Image& image,
	const BufferImageCopyRegion& in, VkBufferImageCopy& out)
{
	const ImageCreateInfo& desc = image.Desc();
	if (in.subresource.mipLevelCount != 1) {
		std::cerr << "[RHI] 拷贝区域须只含一个 mip\n";
		return false;
	}
	ImageSubresourceRange range = in.subresource;
	if (!NormalizeImageSubresourceRange(range, desc.mipLevels, desc.arrayLayers)) {
		std::cerr << "[RHI] 拷贝区域子资源越界\n";
		return false;
	}

	uint32_t mipW = 0, mipH = 0, mipD = 0;
	MipExtent(desc, range.base.mipLevel, mipW, mipH, mipD);
	const uint32_t extentW = in.imageExtentWidth == 0 ? mipW : in.imageExtentWidth;
	const uint32_t extentH = in.imageExtentHeight == 0 ? mipH : in.imageExtentHeight;
	const uint32_t extentD = in.imageExtentDepth == 0 ? mipD : in.imageExtentDepth;
	if (in.imageOffsetX < 0 || in.imageOffsetY < 0 || in.imageOffsetZ < 0
		|| static_cast<uint32_t>(in.imageOffsetX) + extentW > mipW
		|| static_cast<uint32_t>(in.imageOffsetY) + extentH > mipH
		|| static_cast<uint32_t>(in.imageOffsetZ) + extentD > mipD) {
		std::cerr << "[RHI] 拷贝区域超出 mip 尺寸\n";
		return false;
	}

	const uint32_t texel = TexelBytes(image.NativeFormat());
	if (texel == 0) {
		std::cerr << "[RHI] 拷贝区域格式不受支持\n";
		return false;
	}
	const uint64_t rowLength = in.bufferRowLength == 0 ? extentW : in.bufferRowLength;
	const uint64_t imageHeight = in.bufferImageHeight == 0 ? extentH : in.bufferImageHeight;
	const uint64_t required = in.bufferOffset
		+ static_cast<uint64_t>(range.layerCount) * extentD * imageHeight * rowLength * texel;
	if (required > buffer.Desc().size) {
		std::cerr << "[RHI] 拷贝区域超出 buffer 容量\n";
		return false;
	}

	out.bufferOffset = in.bufferOffset;
	out.bufferRowLength = in.bufferRowLength;
	out.bufferImageHeight = in.bufferImageHeight;
	out.imageSubresource.aspectMask = ToVkAspect(image.NativeFormat());
	out.imageSubresource.mipLevel = range.base.mipLevel;
	out.imageSubresource.baseArrayLayer = range.base.layer;
	out.imageSubresource.layerCount = range.layerCount;
	out.imageOffset = { in.imageOffsetX, in.imageOffsetY, in.imageOffsetZ };
	out.imageExtent = { extentW, extentH, extentD };
	return true;
}

void CommandList::CopyBufferToImage(const Buffer& src, Image& dst, ImageLayout dstLayout,
	std::span<const BufferImageCopyRegion> regions)
{
	if (m_commandBuffer == VK_NULL_HANDLE || regions.empty()) return;
	std::vector<VkBufferImageCopy> vkRegions;
	vkRegions.reserve(regions.size());
	for (const auto& r : regions) {
		VkBufferImageCopy vk{};
		if (BuildBufferImageRegion(src, dst, r, vk))
			vkRegions.push_back(vk);
	}
	if (vkRegions.empty()) return;
	vkCmdCopyBufferToImage(m_commandBuffer, src.NativeHandle(), dst.NativeHandle(),
		ToVkImageLayout(dstLayout), static_cast<uint32_t>(vkRegions.size()), vkRegions.data());
}

void CommandList::CopyBufferToImage(const Buffer& src, Image& dst, ImageLayout dstLayout, uint64_t bufferOffset)
{
	const ImageCreateInfo& desc = dst.Desc();
	BufferImageCopyRegion region;
	region.bufferOffset = bufferOffset;
	region.subresource = { { 0, 0 }, 1, desc.arrayLayers == 0 ? 1 : desc.arrayLayers };
	region.imageExtentWidth = desc.width;
	region.imageExtentHeight = desc.height;
	region.imageExtentDepth = desc.depth;
	CopyBufferToImage(src, dst, dstLayout,
		std::span<const BufferImageCopyRegion>(&region, 1));
}

void CommandList::CopyImageToBuffer(const Image& src, const Buffer& dst, ImageLayout srcLayout,
	std::span<const BufferImageCopyRegion> regions)
{
	if (m_commandBuffer == VK_NULL_HANDLE || regions.empty()) return;
	std::vector<VkBufferImageCopy> vkRegions;
	vkRegions.reserve(regions.size());
	for (const auto& r : regions) {
		VkBufferImageCopy vk{};
		if (BuildBufferImageRegion(dst, src, r, vk))
			vkRegions.push_back(vk);
	}
	if (vkRegions.empty()) return;
	vkCmdCopyImageToBuffer(m_commandBuffer, src.NativeHandle(), ToVkImageLayout(srcLayout),
		dst.NativeHandle(), static_cast<uint32_t>(vkRegions.size()), vkRegions.data());
}

void CommandList::CopyImageToBuffer(const Image& src, const Buffer& dst, ImageLayout srcLayout, uint64_t bufferOffset)
{
	const ImageCreateInfo& desc = src.Desc();
	BufferImageCopyRegion region;
	region.bufferOffset = bufferOffset;
	region.subresource = { { 0, 0 }, 1, desc.arrayLayers == 0 ? 1 : desc.arrayLayers };
	region.imageExtentWidth = desc.width;
	region.imageExtentHeight = desc.height;
	region.imageExtentDepth = desc.depth;
	CopyImageToBuffer(src, dst, srcLayout,
		std::span<const BufferImageCopyRegion>(&region, 1));
}

void CommandList::CopyBuffer(const Buffer& src, const Buffer& dst, uint64_t srcOffset,
	uint64_t dstOffset, uint64_t size)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	if (size == 0)
		size = std::min(src.Desc().size, dst.Desc().size);
	if (size == 0) return;

	VkBufferCopy region{};
	region.srcOffset = srcOffset;
	region.dstOffset = dstOffset;
	region.size = size;
	vkCmdCopyBuffer(m_commandBuffer, src.NativeHandle(), dst.NativeHandle(), 1, &region);
}

void CommandList::CopyImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout,
	std::span<const ImageCopyRegion> regions)
{
	if (m_commandBuffer == VK_NULL_HANDLE || regions.empty()) return;
	if (src.NativeFormat() != dst.NativeFormat()) {
		std::cerr << "[RHI] image 拷贝要求相同格式\n";
		return;
	}
	const ImageCreateInfo& s = src.Desc();
	const ImageCreateInfo& d = dst.Desc();

	std::vector<VkImageCopy> vkRegions;
	vkRegions.reserve(regions.size());
	for (const auto& r : regions) {
		if (r.srcSubresource.mipLevelCount != 1 || r.dstSubresource.mipLevelCount != 1) {
			std::cerr << "[RHI] image 拷贝区域须各只含一个 mip\n";
			continue;
		}
		ImageSubresourceRange srcRange = r.srcSubresource;
		ImageSubresourceRange dstRange = r.dstSubresource;
		if (!NormalizeImageSubresourceRange(srcRange, s.mipLevels, s.arrayLayers)
			|| !NormalizeImageSubresourceRange(dstRange, d.mipLevels, d.arrayLayers)) {
			std::cerr << "[RHI] image 拷贝区域子资源越界\n";
			continue;
		}
		if (srcRange.layerCount != dstRange.layerCount) {
			std::cerr << "[RHI] image 拷贝的源/目的层数须一致\n";
			continue;
		}
		uint32_t srcW = 0, srcH = 0, srcD = 0;
		uint32_t dstW = 0, dstH = 0, dstD = 0;
		MipExtent(s, srcRange.base.mipLevel, srcW, srcH, srcD);
		MipExtent(d, dstRange.base.mipLevel, dstW, dstH, dstD);
		const uint32_t ew = r.extentWidth == 0 ? srcW : r.extentWidth;
		const uint32_t eh = r.extentHeight == 0 ? srcH : r.extentHeight;
		const uint32_t ed = r.extentDepth == 0 ? srcD : r.extentDepth;
		if (r.srcOffsetX < 0 || r.srcOffsetY < 0 || r.srcOffsetZ < 0
			|| r.dstOffsetX < 0 || r.dstOffsetY < 0 || r.dstOffsetZ < 0
			|| static_cast<uint32_t>(r.srcOffsetX) + ew > srcW
			|| static_cast<uint32_t>(r.srcOffsetY) + eh > srcH
			|| static_cast<uint32_t>(r.srcOffsetZ) + ed > srcD
			|| static_cast<uint32_t>(r.dstOffsetX) + ew > dstW
			|| static_cast<uint32_t>(r.dstOffsetY) + eh > dstH
			|| static_cast<uint32_t>(r.dstOffsetZ) + ed > dstD) {
			std::cerr << "[RHI] image 拷贝区域越界\n";
			continue;
		}

		VkImageCopy vk{};
		vk.srcSubresource.aspectMask = ToVkAspect(src.NativeFormat());
		vk.srcSubresource.mipLevel = srcRange.base.mipLevel;
		vk.srcSubresource.baseArrayLayer = srcRange.base.layer;
		vk.srcSubresource.layerCount = srcRange.layerCount;
		vk.dstSubresource.aspectMask = ToVkAspect(dst.NativeFormat());
		vk.dstSubresource.mipLevel = dstRange.base.mipLevel;
		vk.dstSubresource.baseArrayLayer = dstRange.base.layer;
		vk.dstSubresource.layerCount = dstRange.layerCount;
		vk.srcOffset = { r.srcOffsetX, r.srcOffsetY, r.srcOffsetZ };
		vk.dstOffset = { r.dstOffsetX, r.dstOffsetY, r.dstOffsetZ };
		vk.extent = { ew, eh, ed };
		vkRegions.push_back(vk);
	}
	if (vkRegions.empty()) return;
	vkCmdCopyImage(m_commandBuffer, src.NativeHandle(), ToVkImageLayout(srcLayout),
		dst.NativeHandle(), ToVkImageLayout(dstLayout),
		static_cast<uint32_t>(vkRegions.size()), vkRegions.data());
}

void CommandList::CopyImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout)
{
	const ImageCreateInfo& s = src.Desc();
	ImageCopyRegion region;
	region.srcSubresource = { { 0, 0 }, 1, s.arrayLayers == 0 ? 1 : s.arrayLayers };
	region.dstSubresource = { { 0, 0 }, 1, dst.Desc().arrayLayers == 0 ? 1 : dst.Desc().arrayLayers };
	region.extentWidth = s.width;
	region.extentHeight = s.height;
	region.extentDepth = s.depth;
	CopyImage(src, dst, srcLayout, dstLayout, std::span<const ImageCopyRegion>(&region, 1));
}

void CommandList::BlitImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout,
	std::span<const ImageBlitRegion> regions, bool linearFilter)
{
	if (m_commandBuffer == VK_NULL_HANDLE || regions.empty()) return;
	if (src.NativeFormat() != dst.NativeFormat()) {
		std::cerr << "[RHI] image blit 要求相同格式\n";
		return;
	}
	const ImageCreateInfo& s = src.Desc();
	const ImageCreateInfo& d = dst.Desc();
	const VkImageAspectFlags aspect = ToVkAspect(src.NativeFormat());
	const bool depthStencil = aspect != VK_IMAGE_ASPECT_COLOR_BIT;
	if (depthStencil && linearFilter) {
		std::cerr << "[RHI] 深度/模板 blit 不支持线性过滤，已改用最近邻\n";
		linearFilter = false;
	}

	std::vector<VkImageBlit> vkRegions;
	vkRegions.reserve(regions.size());
	for (const auto& r : regions) {
		if (r.srcSubresource.mipLevelCount != 1 || r.dstSubresource.mipLevelCount != 1) {
			std::cerr << "[RHI] blit 区域须各只含一个 mip\n";
			continue;
		}
		ImageSubresourceRange srcRange = r.srcSubresource;
		ImageSubresourceRange dstRange = r.dstSubresource;
		if (!NormalizeImageSubresourceRange(srcRange, s.mipLevels, s.arrayLayers)
			|| !NormalizeImageSubresourceRange(dstRange, d.mipLevels, d.arrayLayers)) {
			std::cerr << "[RHI] blit 区域子资源越界\n";
			continue;
		}
		if (srcRange.layerCount != dstRange.layerCount) {
			std::cerr << "[RHI] blit 的源/目的层数须一致\n";
			continue;
		}
		if (r.srcOffsetX1 <= r.srcOffsetX0 || r.srcOffsetY1 <= r.srcOffsetY0
			|| r.srcOffsetZ1 <= r.srcOffsetZ0 || r.dstOffsetX1 <= r.dstOffsetX0
			|| r.dstOffsetY1 <= r.dstOffsetY0 || r.dstOffsetZ1 <= r.dstOffsetZ0) {
			std::cerr << "[RHI] blit 偏移区间无效\n";
			continue;
		}

		VkImageBlit vk{};
		vk.srcSubresource.aspectMask = ToVkAspect(src.NativeFormat());
		vk.srcSubresource.mipLevel = srcRange.base.mipLevel;
		vk.srcSubresource.baseArrayLayer = srcRange.base.layer;
		vk.srcSubresource.layerCount = srcRange.layerCount;
		vk.srcOffsets[0] = { r.srcOffsetX0, r.srcOffsetY0, r.srcOffsetZ0 };
		vk.srcOffsets[1] = { r.srcOffsetX1, r.srcOffsetY1, r.srcOffsetZ1 };
		vk.dstSubresource.aspectMask = ToVkAspect(dst.NativeFormat());
		vk.dstSubresource.mipLevel = dstRange.base.mipLevel;
		vk.dstSubresource.baseArrayLayer = dstRange.base.layer;
		vk.dstSubresource.layerCount = dstRange.layerCount;
		vk.dstOffsets[0] = { r.dstOffsetX0, r.dstOffsetY0, r.dstOffsetZ0 };
		vk.dstOffsets[1] = { r.dstOffsetX1, r.dstOffsetY1, r.dstOffsetZ1 };
		vkRegions.push_back(vk);
	}
	if (vkRegions.empty()) return;
	vkCmdBlitImage(m_commandBuffer, src.NativeHandle(), ToVkImageLayout(srcLayout),
		dst.NativeHandle(), ToVkImageLayout(dstLayout),
		static_cast<uint32_t>(vkRegions.size()), vkRegions.data(),
		linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void CommandList::BlitImage(const Image& src, Image& dst, ImageLayout srcLayout, ImageLayout dstLayout,
	bool linearFilter)
{
	const ImageCreateInfo& s = src.Desc();
	const ImageCreateInfo& d = dst.Desc();
	ImageBlitRegion region;
	region.srcSubresource = { { 0, 0 }, 1, s.arrayLayers == 0 ? 1 : s.arrayLayers };
	region.dstSubresource = { { 0, 0 }, 1, d.arrayLayers == 0 ? 1 : d.arrayLayers };
	region.srcOffsetX1 = static_cast<int32_t>(s.width);
	region.srcOffsetY1 = static_cast<int32_t>(s.height);
	region.srcOffsetZ1 = static_cast<int32_t>(s.depth);
	region.dstOffsetX1 = static_cast<int32_t>(d.width);
	region.dstOffsetY1 = static_cast<int32_t>(d.height);
	region.dstOffsetZ1 = static_cast<int32_t>(d.depth);
	BlitImage(src, dst, srcLayout, dstLayout, std::span<const ImageBlitRegion>(&region, 1), linearFilter);
}

void CommandList::ClearImage(Image& image, ImageSubresourceRange range, ImageLayout layout,
	const ClearValue& value)
{
	if (m_commandBuffer == VK_NULL_HANDLE) return;
	const ImageCreateInfo& desc = image.Desc();
	if (!NormalizeImageSubresourceRange(range, desc.mipLevels, desc.arrayLayers)) {
		std::cerr << "[RHI] 清场范围越界\n";
		return;
	}
	const VkImageAspectFlags aspect = ToVkAspect(image.NativeFormat());
	VkImageSubresourceRange vkRange{};
	vkRange.aspectMask = aspect;
	vkRange.baseMipLevel = range.base.mipLevel;
	vkRange.levelCount = range.mipLevelCount;
	vkRange.baseArrayLayer = range.base.layer;
	vkRange.layerCount = range.layerCount;

	if (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
		const DepthStencilValue* ds = std::get_if<DepthStencilValue>(&value.value);
		if (ds == nullptr) {
			std::cerr << "[RHI] 深度/模板清场值类型不匹配\n";
			return;
		}
		VkClearDepthStencilValue vk{ ds->depth, ds->stencil };
		vkCmdClearDepthStencilImage(m_commandBuffer, image.NativeHandle(),
			ToVkImageLayout(layout), &vk, 1, &vkRange);
	}
	else {
		const std::array<float, 4>* color = std::get_if<std::array<float, 4>>(&value.value);
		if (color == nullptr) {
			std::cerr << "[RHI] 颜色清场值类型不匹配\n";
			return;
		}
		VkClearColorValue vk{ { (*color)[0], (*color)[1], (*color)[2], (*color)[3] } };
		vkCmdClearColorImage(m_commandBuffer, image.NativeHandle(),
			ToVkImageLayout(layout), &vk, 1, &vkRange);
	}
}

void CommandList::ClearColor(Image& image, ImageSubresource view, const float rgba[4], ImageLayout layout)
{
	if (rgba == nullptr) return;
	ClearValue value;
	value.value = std::array<float, 4>{ rgba[0], rgba[1], rgba[2], rgba[3] };
	ClearImage(image, ImageSubresourceRange{ { view.mipLevel, view.layer }, 1, 1 }, layout, value);
}

void CommandList::ResetTimestamps(uint32_t first, uint32_t count)
{
	if (m_commandBuffer == VK_NULL_HANDLE || m_timestampPool == VK_NULL_HANDLE || count == 0) return;
	vkCmdResetQueryPool(m_commandBuffer, m_timestampPool, first, count);
}

void CommandList::WriteTimestamp(uint32_t index)
{
	if (m_commandBuffer == VK_NULL_HANDLE || m_timestampPool == VK_NULL_HANDLE) return;
	vkCmdWriteTimestamp2(m_commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, index);
}

VkCommandBuffer CommandList::NativeHandle() const { return m_commandBuffer; }

#pragma endregion

#pragma region Frame

CommandList Device::BeginCommandList(QueueClass queue)
{
	CommandList list;
	if (m_frameSlots.empty()) return list;

	uint32_t family = m_graphicsQueueFamily;
	if (queue == QueueClass::AsyncCompute) family = m_computeQueueFamily;
	else if (queue == QueueClass::Transfer) family = m_transferQueueFamily;

	FrameSlot& slot = *m_frameSlots[m_currentFrameSlot];
	list.m_commandBuffer = slot.Allocate(family);
	list.m_timestampPool = m_timestampsSupported ? slot.TimestampPool() : VK_NULL_HANDLE;
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

std::uint64_t Device::Submit(const SubmitInfo& info)
{
	if (m_frameSlots.empty()) return 0;
	FrameSlot& slot = *m_frameSlots[m_currentFrameSlot];

	std::vector<VkCommandBufferSubmitInfo> commandInfos;
	commandInfos.reserve(info.commandLists.size());
	for (const auto& list : info.commandLists) {
		if (list.m_commandBuffer == VK_NULL_HANDLE) continue;
		if (vkEndCommandBuffer(list.m_commandBuffer) != VK_SUCCESS) {
			std::cerr << "[RHI] vkEndCommandBuffer 失败\n";
			return 0;
		}
		VkCommandBufferSubmitInfo ci{};
		ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		ci.commandBuffer = list.m_commandBuffer;
		commandInfos.push_back(ci);
	}
	if (commandInfos.empty()) return 0;

	std::vector<VkSemaphoreSubmitInfo> waitInfos;
	waitInfos.reserve(info.waits.size());
	for (const auto& w : info.waits) {
		if (!w.semaphore) continue;
		VkSemaphoreSubmitInfo wi{};
		wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		wi.semaphore = w.semaphore->NativeHandle();
		wi.stageMask = (w.stage == PipelineStage::None)
			? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : ToVkPipelineStage(w.stage);
		wi.value = w.value;
		waitInfos.push_back(wi);
	}

	std::vector<VkSemaphoreSubmitInfo> signalInfos;
	signalInfos.reserve(info.signals.size() + 1);
	for (const auto& s : info.signals) {
		if (!s.semaphore) continue;
		VkSemaphoreSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		si.semaphore = s.semaphore->NativeHandle();
		si.stageMask = (s.stage == PipelineStage::None)
			? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : ToVkPipelineStage(s.stage);
		si.value = s.value;
		signalInfos.push_back(si);
	}

	std::uint64_t timelineValue = 0;
	const uint32_t queueIndex = static_cast<uint32_t>(info.queue);
	if (m_queueTimelines[queueIndex]) {
		timelineValue = ++m_queueTimelineValue[queueIndex];
		VkSemaphoreSubmitInfo ti{};
		ti.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		ti.semaphore = m_queueTimelines[queueIndex]->NativeHandle();
		ti.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		ti.value = timelineValue;
		signalInfos.push_back(ti);
	}

	VkFence fence = VK_NULL_HANDLE;
	bool ownedFence = false;
	if (info.signalFence) {
		fence = info.signalFence->m_fence;
	} else {
		fence = slot.AcquireFence();
		ownedFence = true;
	}

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
		if (ownedFence) slot.ReleaseFence(fence);
		if (timelineValue != 0) --m_queueTimelineValue[queueIndex];
		return 0;
	}
	slot.MarkPending(fence, ownedFence);
	return timelineValue;
}

Semaphore* Device::QueueTimeline(QueueClass cls) const
{
	const uint32_t index = static_cast<uint32_t>(cls);
	return (index < 3) ? m_queueTimelines[index].get() : nullptr;
}

std::uint64_t Device::LastQueueTimelineValue(QueueClass cls) const
{
	const uint32_t index = static_cast<uint32_t>(cls);
	return (index < 3) ? m_queueTimelineValue[index] : 0;
}

uint32_t Device::BeginFrame()
{
	if (m_frameSlots.empty()) return 0;
	const uint32_t slotCount = static_cast<uint32_t>(m_frameSlots.size());
	const uint32_t slotIndex = m_frameIndex;

	if (m_framesBegun >= slotCount)
		m_frameSlots[slotIndex]->Recycle();

	m_currentFrameSlot = slotIndex;
	m_frameIndex = (slotIndex + 1) % slotCount;
	++m_framesBegun;
	return slotIndex;
}

void Device::WaitForFrame()
{
	if (m_frameSlots.empty()) return;
	m_frameSlots[m_currentFrameSlot]->Recycle();
}

void Device::WaitAllFrames()
{
	for (auto& slot : m_frameSlots) {
		if (slot) slot->Recycle();
	}
}

bool Device::ReadFrameTimestamps(uint32_t first, uint32_t count, std::uint64_t* out) const
{
	if (!m_timestampsSupported || m_frameSlots.empty() || out == nullptr || count == 0) return false;
	if (first + count > FrameSlot::kTimestampQueries) return false;

	VkQueryPool pool = m_frameSlots[m_currentFrameSlot]->TimestampPool();
	if (pool == VK_NULL_HANDLE) return false;

	return vkGetQueryPoolResults(m_device, pool, first, count,
		sizeof(std::uint64_t) * count, out, sizeof(std::uint64_t),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS;
}

void Device::WaitIdle()
{
	if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);
}

#pragma endregion
