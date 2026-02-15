#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "vk_mem_alloc.h"
#include "Allocator.h"

namespace VWrap {

	/// <summary>
	/// Represents a VkBuffer and its associated VmaAllocation.
	/// </summary>
	class Buffer
	{
	private:

		/// <summary>
		/// The underlying buffer handle.
		/// </summary>
		VkBuffer m_buffer;

		/// <summary>
		/// The allocation handle.
		/// </summary>
		VmaAllocation m_allocation;

		/// <summary>
		/// The allocator that created this buffer.
		/// </summary>
		std::shared_ptr<Allocator> m_allocator;

	public:

		/// <summary>
		/// Creates a buffer with the given parameters, including VMA Allocation Flags
		/// </summary>
		static std::shared_ptr<Buffer> Create(std::shared_ptr<Allocator> allocator,
			VkDeviceSize size,
			VkBufferUsageFlags usage,
			VkMemoryPropertyFlags properties,
			VmaAllocationCreateFlags flags);

		/// <summary>
		/// Creates a buffer with parameters suited for staging
		/// </summary>
		static std::shared_ptr<Buffer> CreateStaging(std::shared_ptr<Allocator> allocator, VkDeviceSize size);

		/// <summary>
		/// Creates a buffer with parameters suited for persistent mapped memory
		/// </summary>
		static std::shared_ptr<Buffer> CreateMapped(std::shared_ptr<Allocator> allocator,
			VkDeviceSize size,
			VkBufferUsageFlags usage,
			VkMemoryPropertyFlags properties,
			void*& data);

		/// <summary>
		/// Gets the underlying buffer handle.
		/// </summary>
		VkBuffer Get() const { return m_buffer; }

		VmaAllocation GetAllocation() const { return m_allocation; }

		/// <summary>
		/// Destroys the underlying VkBuffer and VmaAllocation.
		/// </summary>
		~Buffer();
	};
}
