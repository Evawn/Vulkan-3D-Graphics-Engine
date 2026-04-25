#include "PipelineLayout.h"

namespace VWrap {

	std::shared_ptr<PipelineLayout> PipelineLayout::Create(
		std::shared_ptr<Device> device,
		std::vector<std::shared_ptr<DescriptorSetLayout>> setLayouts,
		const std::vector<VkPushConstantRange>& pushConstantRanges)
	{
		auto ret = std::make_shared<PipelineLayout>();
		ret->m_device = device;
		ret->m_set_layouts = std::move(setLayouts);

		std::vector<VkDescriptorSetLayout> handles;
		handles.reserve(ret->m_set_layouts.size());
		for (const auto& sl : ret->m_set_layouts) {
			handles.push_back(sl ? sl->Get() : VK_NULL_HANDLE);
		}

		VkPipelineLayoutCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		info.setLayoutCount = static_cast<uint32_t>(handles.size());
		info.pSetLayouts = handles.empty() ? nullptr : handles.data();
		info.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
		info.pPushConstantRanges = pushConstantRanges.empty() ? nullptr : pushConstantRanges.data();

		if (vkCreatePipelineLayout(device->Get(), &info, nullptr, &ret->m_layout) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create pipeline layout!");
		}
		return ret;
	}

	std::shared_ptr<PipelineLayout> PipelineLayout::Create(
		std::shared_ptr<Device> device,
		std::shared_ptr<DescriptorSetLayout> setLayout,
		const std::vector<VkPushConstantRange>& pushConstantRanges)
	{
		std::vector<std::shared_ptr<DescriptorSetLayout>> layouts;
		if (setLayout) layouts.push_back(std::move(setLayout));
		return Create(std::move(device), std::move(layouts), pushConstantRanges);
	}

	PipelineLayout::~PipelineLayout() {
		if (m_layout != VK_NULL_HANDLE && m_device) {
			vkDestroyPipelineLayout(m_device->Get(), m_layout, nullptr);
		}
	}

}
