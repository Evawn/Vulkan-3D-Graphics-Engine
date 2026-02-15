#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"

namespace VWrap {
	/// <summary>
	/// Represents a vulkan fence
	/// </summary>
	class Fence
	{
	private:

		/// <summary>
		/// The underlying vulkan fence
		/// </summary>
		VkFence m_fence;

		/// <summary>
		/// The device that created this fence
		/// </summary>
		std::shared_ptr<Device> m_device;

	public:

		/// <summary>
		/// Creates a new fence from the given device
		/// </summary>
		static std::shared_ptr<Fence> Create(std::shared_ptr<Device> device);

		/// <summary>
		/// Gets the underlying vulkan fence
		/// </summary>
		VkFence Get() const { return m_fence; }

		/// <summary>
		/// Destroys the underlying fence
		/// </summary>
		~Fence();
	};

}
