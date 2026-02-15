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
		/// Creates a new render pass on the device, with the given image format and sample count
		/// </summary>
		static std::shared_ptr<RenderPass> CreateImGUI(std::shared_ptr<Device> device, VkFormat format, VkSampleCountFlagBits samples);

		/// <summary> Gets the underlying Vulkan render pass </summary>
		VkRenderPass Get() const { return m_render_pass; }

		/// <summary> Gets the number of samples used by this render pass </summary>
		VkSampleCountFlagBits GetSamples() const { return m_samples; }

		~RenderPass();
	};
}
