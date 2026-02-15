#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "DescriptorPool.h"
#include "DescriptorSetLayout.h"
#include <vector>

namespace VWrap {
	
	/// <summary>
	/// Represents a descriptor set. Contains static functions for creating descriptor sets from pools and layouts.
	/// </summary>
	class DescriptorSet
	{
	private:

		/// <summary>
		/// The underlying vulkan descriptor set handle.
		/// </summary>
		VkDescriptorSet m_descriptor_set{ VK_NULL_HANDLE };

		/// <summary>
		/// The descriptor pool that this descriptor set was allocated from.
		/// </summary>
		std::shared_ptr<DescriptorPool> m_descriptor_pool;

		/// <summary>
		/// The descriptor set layout that this descriptor set was allocated from.
		/// </summary>
		std::shared_ptr<DescriptorSetLayout> m_layout;

	public:

		/// <summary>
		/// Creates a descriptor set from a pool and layout.
		/// </summary>
		static std::shared_ptr<DescriptorSet> Create(std::shared_ptr<DescriptorPool> descriptor_pool, std::shared_ptr<DescriptorSetLayout> layout);

		/// <summary>
		/// Creates multiple descriptor sets from a pool and vector of layouts.
		/// </summary>
		static std::vector<std::shared_ptr<DescriptorSet>> CreateMany(std::shared_ptr<DescriptorPool> descriptor_pool, std::vector<std::shared_ptr<DescriptorSetLayout>> layouts);

		/// <summary>
		/// Gets the underlying vulkan descriptor set handle.
		/// </summary>
		VkDescriptorSet Get() const { return m_descriptor_set; }

		/// <summary>
		/// Gets the descriptor pool that this descriptor set was allocated from.
		/// </summary>
		std::shared_ptr<DescriptorPool> GetDescriptorPool() const { return m_descriptor_pool; }
	};
}