#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include "Device.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "DescriptorSet.h"

class DescriptorSetBuilder {
public:
	DescriptorSetBuilder(std::shared_ptr<VWrap::Device> device);

	DescriptorSetBuilder& AddBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stage);

	struct Result {
		std::shared_ptr<VWrap::DescriptorSetLayout> layout;
		std::shared_ptr<VWrap::DescriptorPool> pool;
		std::vector<std::shared_ptr<VWrap::DescriptorSet>> sets;
	};

	Result Build(uint32_t setCount);

private:
	std::shared_ptr<VWrap::Device> m_device;
	std::vector<VkDescriptorSetLayoutBinding> m_bindings;
};
