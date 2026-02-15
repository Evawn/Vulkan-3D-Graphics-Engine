#include "DescriptorSet.h"

namespace VWrap {

	std::shared_ptr<DescriptorSet> DescriptorSet::Create(std::shared_ptr<DescriptorPool> descriptor_pool, std::shared_ptr<DescriptorSetLayout> layout) {
		auto ret = std::make_shared<DescriptorSet>();
		ret->m_descriptor_pool = descriptor_pool;

		std::vector<std::shared_ptr<DescriptorSetLayout>> layouts = {layout};
		
		return CreateMany(descriptor_pool, layouts)[0];

	}

	std::vector<std::shared_ptr<DescriptorSet>> DescriptorSet::CreateMany(std::shared_ptr<DescriptorPool> descriptor_pool, const std::vector<std::shared_ptr<DescriptorSetLayout>> layouts) {
		size_t count = layouts.size();

		auto ret = std::vector<std::shared_ptr<DescriptorSet>>(count);
		auto handles = std::vector<VkDescriptorSet>(count);

		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = descriptor_pool->Get();

		std::vector<VkDescriptorSetLayout> vk_layouts(count);
		for (uint32_t i = 0; i < count; i++)
			vk_layouts[i] = layouts[i]->Get();

		allocInfo.pSetLayouts = vk_layouts.data();
		allocInfo.descriptorSetCount = static_cast<uint32_t>(count);

		if (vkAllocateDescriptorSets(descriptor_pool->GetDevice()->Get(), &allocInfo, handles.data()) != VK_SUCCESS) {
			throw std::runtime_error("Failed to allocate descriptor set!");
		}

		for (uint32_t i = 0; i < count; i++) {
			ret[i] = std::make_shared<DescriptorSet>();
			ret[i]->m_descriptor_pool = descriptor_pool;
			ret[i]->m_descriptor_set = handles[i];
			ret[i]->m_layout = layouts[i];
		}

		return ret;
	}
}