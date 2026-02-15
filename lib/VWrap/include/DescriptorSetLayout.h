#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include <array>

namespace VWrap {

	/// <summary>
	/// Represents a descriptor set layout.
	/// </summary>
	class DescriptorSetLayout {
	private:

		/// <summary>
		/// The underlying vulkan descriptor set layout.
		/// </summary>
		VkDescriptorSetLayout m_layout;

		/// <summary>
		/// The device that created this descriptor set layout.
		/// </summary>
		std::shared_ptr<Device> m_device;

	public:

		static std::shared_ptr<DescriptorSetLayout> Create(std::shared_ptr<Device> device, std::vector<VkDescriptorSetLayoutBinding> bindings);


		/// <summary>
		/// Gets the underlying vulkan descriptor set layout.
		///	</summary>
		VkDescriptorSetLayout Get() const { return m_layout; }

		/// <summary>
		///	Destroys the underlying vulkan descriptor set layout.
		/// </summary>
		~DescriptorSetLayout();
	};
}