#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan sampler object.
	/// </summary>
	class Sampler
	{
	private:

		/// <summary> The underlying Vulkan sampler object. </summary>
		VkSampler m_sampler{ VK_NULL_HANDLE };

		/// <summary> The device that created this sampler. </summary>
		std::shared_ptr<Device> m_device;

	public:

		/// <summary> Creates a new sampler object. </summary>
		static std::shared_ptr<Sampler> Create(std::shared_ptr<Device> device);

		/// <summary> Gets the underlying Vulkan sampler object. </summary>
		VkSampler Get() const { return m_sampler; }

		~Sampler();
	};
}