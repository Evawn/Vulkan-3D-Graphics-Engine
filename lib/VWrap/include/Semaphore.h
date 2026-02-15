#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan semaphore.
	/// </summary>
	class Semaphore
	{
	private:

		/// <summary> The semaphore handle. </summary>
		VkSemaphore m_semaphore;

		/// <summary>
		/// The device that owns this semaphore.
		/// </summary>
		std::shared_ptr<Device> m_device;

	public:

		/// <summary> Creates a new semaphore. </summary>
		static std::shared_ptr<Semaphore> Create(std::shared_ptr<Device> device);

		/// <summary> Gets the semaphore handle. </summary>
		VkSemaphore Get() const { return m_semaphore; }

		~Semaphore();
	};
}