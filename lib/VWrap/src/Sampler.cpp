#include "Sampler.h"

namespace VWrap {

	std::shared_ptr<Sampler> Sampler::Create(std::shared_ptr<Device> device) {
		auto ret = std::make_shared<Sampler>();
		ret->m_device = device;

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device->GetPhysicalDevice()->Get(), &properties);
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = properties.limits.maxSamplerAnisotropy;

        info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;
        info.compareEnable = VK_FALSE;
        info.compareOp = VK_COMPARE_OP_ALWAYS;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.mipLodBias = 0.0f;
        info.minLod = 0.0f;
        info.maxLod = 10.0f;

        if (vkCreateSampler(device->Get(), &info, nullptr, &ret->m_sampler) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create texture sampler!");
        }

		return ret;
	}

	Sampler::~Sampler() {
		if (m_sampler != VK_NULL_HANDLE)
			vkDestroySampler(m_device->Get(), m_sampler, nullptr);
	}
}