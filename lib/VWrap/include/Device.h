#pragma once
#include <vulkan/vulkan.h>
#include <memory>
#include "PhysicalDevice.h"

namespace VWrap {

	/// <summary>
	/// Represents a logical device
	/// </summary>
	class Device
	{
	private:
		
		/// <summary>
		/// The underlying vulkan device handle
		/// </summary>
		VkDevice m_device{ VK_NULL_HANDLE };

		/// <summary>
		/// The physical device this device was created from
		/// </summary>
		std::shared_ptr<PhysicalDevice> m_physical_device;

	public:

		/// <summary>
		/// Creates a new logical device from the given physical device.
		/// </summary>
		/// <param name="physical_device">The physical device to create the logical device from</param>
		/// <param name="enable_validation_layers">Whether to enable validation layers</param>
		/// <returns>A shared pointer to the newly created logical device</returns>
		static std::shared_ptr<Device> Create(std::shared_ptr<PhysicalDevice> physical_device, bool enable_validation_layers);

		/// <summary>
		/// Gets the underlying vulkan device handle
		/// </summary>
		VkDevice Get() const {
			return m_device;
		}

		/// <summary>
		/// Gets the physical device this device was created from
		/// </summary>
		std::shared_ptr<PhysicalDevice> GetPhysicalDevice() const {
			return m_physical_device;
		}

		/// <summary>
		/// Destroys the underlying vulkan device
		/// </summary>
		~Device();
	};
}