#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"

namespace VWrap {
	/// <summary>
	/// Represents a Vulkan descriptor pool.
	/// </summary>
	class DescriptorPool
	{
	private:

		/// <summary>
		/// The underlying Vulkan descriptor pool.
		/// </summary>
		VkDescriptorPool m_descriptor_pool;
		
		/// <summary>
		/// The device that created this descriptor pool.
		/// </summary>
		std::shared_ptr<Device> m_device;

	public:

		/// <summary>
		/// Creates a new descriptor pool from the given device, that can allocate the given number of descriptor sets.
		/// </summary>
		static std::shared_ptr<DescriptorPool> Create(std::shared_ptr<Device> device, std::vector<VkDescriptorPoolSize> pool_sizes, uint32_t max_sets, VkDescriptorPoolCreateFlags flags);

		/// <summary>
		/// Gets the underlying Vulkan descriptor pool.
		/// </summary>
		VkDescriptorPool Get() const { return m_descriptor_pool; }

		/// <summary>
		/// Gets the device that created this descriptor pool.
		/// </summary>
		std::shared_ptr<Device> GetDevice() const { return m_device; }

		/// <summary>
		/// Destroys the underlying Vulkan descriptor pool.
		/// </summary>
		~DescriptorPool();
	};
}
