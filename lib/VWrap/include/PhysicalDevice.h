#pragma once
#include <vulkan/vulkan.h>
#include <memory>
#include "Utils.h"
#include "Instance.h"
#include "Surface.h"
#include <optional>
#include <set>
#include <string>

namespace VWrap
{
	/// <summary>
	/// Represents a Vulkan physical device
	/// </summary>
	class PhysicalDevice
	{
	private:

		/// <summary> The underlying Vulkan physical device handle </summary>
		VkPhysicalDevice m_physical_device{ VK_NULL_HANDLE };

		/// <summary>
		/// The surface for which to query physical device support
		/// </summary>
		std::shared_ptr<Surface> m_surface;

		/// <summary> Whether or not the physical device supports all required extensions </summary>
		bool isPhysicalDeviceSuitable();

	public:
		/// <summary>
		/// Picks the best physical device for the given instance and surface
		/// </summary>
		static std::shared_ptr<PhysicalDevice> Pick(std::shared_ptr<Instance> instance, std::shared_ptr<Surface> surface);

		/// <summary>
		/// Finds the queue families supported by the physical device
		/// </summary>
		QueueFamilyIndices FindQueueFamilies();

		/// <summary> Gets the underlying Vulkan physical device handle </summary>
		VkPhysicalDevice Get() const { return m_physical_device; }

		/// <summary>
		/// Gets the memory type index for the given type filter and properties
		/// </summary>
		uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

		/// <summary> Gets the swapchain support details for the physical device </summary>
		SwapchainSupportDetails QuerySwapchainSupport();

		/// <summary> Gets the maximum sample count supported by the physical device </summary>
		VkSampleCountFlagBits GetMaxUsableSampleCount();

		/// <summary>
		/// Queries 'device' for support for extensions defines in DEVICE_EXTENSIONS.
		/// </summary>
		bool checkDeviceExtensions();

		/// <summary> Gets the memory properties of the physical device </summary>
		VkPhysicalDeviceMemoryProperties GetMemoryProperties() const {
			VkPhysicalDeviceMemoryProperties props;
			vkGetPhysicalDeviceMemoryProperties(m_physical_device, &props);
			return props;
		}

		/// <summary>
		/// True iff the device exposes a compute queue family that is distinct from
		/// the graphics queue family. When false, compute and graphics share a queue
		/// family and there is no async-compute parallelism to be had — the render
		/// graph silently demotes any AsyncCompute affinity hints back to the
		/// graphics queue in this case.
		/// </summary>
		bool HasAsyncComputeQueue();

	};
}