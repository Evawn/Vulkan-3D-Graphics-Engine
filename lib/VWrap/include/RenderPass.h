#pragma once
#include <vulkan/vulkan.h>
#include "Device.h"
#include "Image.h"
#include <memory>

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan render pass
	/// </summary>
	class RenderPass {

	private:

		/// <summary> The underlying Vulkan render pass </summary>
		VkRenderPass m_render_pass;

		/// <summary> The number of samples used by this render pass </summary>
		VkSampleCountFlagBits m_samples;

		/// <summary> The device that owns this render pass </summary>
		std::shared_ptr<Device> m_device;

	public:

		/// <summary>
		/// Creates a presentation render pass for ImGui rendering to swapchain.
		/// Single color attachment, 1x samples, finalLayout = PRESENT_SRC_KHR.
		/// </summary>
		static std::shared_ptr<RenderPass> CreatePresentation(std::shared_ptr<Device> device, VkFormat swapchainFormat);

		/// <summary>
		/// Creates a render pass from a VkRenderPassCreateInfo. General-purpose factory
		/// used by the render graph to create render passes from declarative attachment specs.
		/// </summary>
		static std::shared_ptr<RenderPass> Create(std::shared_ptr<Device> device, const VkRenderPassCreateInfo& createInfo, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

		/// <summary> Gets the underlying Vulkan render pass </summary>
		VkRenderPass Get() const { return m_render_pass; }

		/// <summary> Gets the number of samples used by this render pass </summary>
		VkSampleCountFlagBits GetSamples() const { return m_samples; }

		~RenderPass();
	};
}
