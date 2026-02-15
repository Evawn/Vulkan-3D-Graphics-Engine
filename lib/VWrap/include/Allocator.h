#pragma once
#include "vk_mem_alloc.h"
#include <memory>

#include "Instance.h"
#include "PhysicalDevice.h"
#include "Device.h"

namespace VWrap
{
	/// <summary>
	/// Represents a VMA allocator.
	/// </summary>
	class Allocator
	{
	private:
		/// <summary>
		/// The underlying VMA allocator.
		/// </summary>
		VmaAllocator m_allocator;

		/// <summary>
		/// The device this allocator is associated with.
		/// </summary>
		std::shared_ptr<Device> m_device;

	public:
		/// <summary>
		/// Creates a new allocator.
		/// </summary>
		static std::shared_ptr<Allocator> Create(std::shared_ptr<Instance> instance, std::shared_ptr<PhysicalDevice> physical_device, std::shared_ptr<Device> device);

		/// <summary>
		/// Gets the underlying VMA allocator.
		/// </summary>
		VmaAllocator Get() {
			return m_allocator;
		}

		/// <summary>
		/// Gets the device this allocator is associated with.
		/// </summary>
		std::shared_ptr<Device> GetDevice() {
			return m_device;
		}

		~Allocator() {
			if (m_allocator != VK_NULL_HANDLE)
				vmaDestroyAllocator(m_allocator);
		}
	};
}
