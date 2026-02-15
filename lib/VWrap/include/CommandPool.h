#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "Queue.h"

namespace VWrap{

	/// <summary>
	/// Represents a command pool the submits to a single queue.
	/// </summary>
	class CommandPool
	{
	private:

		/// <summary>
		/// The underlying vulkan command pool.
		/// </summary>
		VkCommandPool m_command_pool;

		/// <summary>
		/// The device that created this command pool.
		/// </summary>
		std::shared_ptr<Device> m_device; 

		/// <summary>
		/// The queue that this command pool submits to.
		/// </summary>
		std::shared_ptr<Queue> m_queue;

	public:

		/// <summary>
		/// Creates a new command pool from the given device, that submits to the given queue.
		/// </summary>
		/// <param name="device">The device that creates the command pool.</param>
		/// <param name="queue">The queue that the command pool submits to.</param>
		/// <returns>A shared pointer to the newly created command pool.</returns>
		static std::shared_ptr<CommandPool> Create(std::shared_ptr<Device> device, std::shared_ptr<Queue> queue);

		/// <summary>
		/// The underlying vulkan command pool.
		/// </summary>
		VkCommandPool Get() const { return m_command_pool; }

		/// <summary>
		/// Gets the device that created this command pool.
		/// </summary>
		std::shared_ptr<Device> GetDevice() const { return m_device; }

		/// <summary>
		/// Gets the queue that this command pool submits to.
		/// </summary>
		std::shared_ptr<Queue> GetQueue() const { return m_queue; }

		/// <summary>
		/// Destroys the underlying vulkan command pool.
		/// </summary>
		~CommandPool();
	};
}