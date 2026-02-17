#include "DescriptorSetBuilder.h"

DescriptorSetBuilder::DescriptorSetBuilder(std::shared_ptr<VWrap::Device> device)
	: m_device(device) {}

DescriptorSetBuilder& DescriptorSetBuilder::AddBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stage) {
	VkDescriptorSetLayoutBinding b{};
	b.binding = binding;
	b.descriptorCount = 1;
	b.descriptorType = type;
	b.stageFlags = stage;
	m_bindings.push_back(b);
	return *this;
}

DescriptorSetBuilder::Result DescriptorSetBuilder::Build(uint32_t setCount) {
	Result result;

	result.layout = VWrap::DescriptorSetLayout::Create(m_device, m_bindings);

	std::vector<VkDescriptorPoolSize> poolSizes;
	for (const auto& b : m_bindings) {
		poolSizes.push_back({ b.descriptorType, setCount });
	}
	result.pool = VWrap::DescriptorPool::Create(m_device, poolSizes, setCount, 0);

	std::vector<std::shared_ptr<VWrap::DescriptorSetLayout>> layouts(setCount, result.layout);
	result.sets = VWrap::DescriptorSet::CreateMany(result.pool, layouts);

	return result;
}
