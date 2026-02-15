#include "DescriptorSetLayout.h"

namespace VWrap {

    std::shared_ptr<DescriptorSetLayout> DescriptorSetLayout::Create(std::shared_ptr<Device> device, std::vector<VkDescriptorSetLayoutBinding> bindings)
    {
        auto ret = std::make_shared<DescriptorSetLayout>();
        ret->m_device = device;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(device->Get(), &layoutInfo, nullptr, &ret->m_layout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create descriptor set layout!");
        }

        return ret;
    }


	DescriptorSetLayout::~DescriptorSetLayout() {
        if (m_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device->Get(), m_layout, nullptr);
	}
}