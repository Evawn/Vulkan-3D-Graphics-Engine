#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan queue.
	/// </summary>
	class Queue
	{
	private:

		/// <summary> The underlying Vulkan queue. </summary>
		VkQueue m_queue;

		/// <summary> The queue family index. </summary>
		uint32_t m_queue_family_index;

		/// <summary> The device of the queue </summary>
		std::shared_ptr<Device> m_device;

	public:

		/// <summary>
		/// Creates a new queue, representing the first queue of the given queue family.
		/// </summary>
		static std::shared_ptr<Queue> Create(std::shared_ptr<Device> device, uint32_t queue_family_index);

		/// <summary> Gets the the queue handle. </summary>
		VkQueue Get() const { return m_queue; }

		/// <summary> Gets the queue family index. </summary>
		uint32_t GetQueueFamilyIndex() const { return m_queue_family_index; }
	};
}

