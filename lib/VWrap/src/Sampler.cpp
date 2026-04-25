#include "Sampler.h"

namespace VWrap {

	std::shared_ptr<Sampler> Sampler::Create(std::shared_ptr<Device> device, const VkSamplerCreateInfo& info) {
		auto ret = std::make_shared<Sampler>();
		ret->m_device = device;
		if (vkCreateSampler(device->Get(), &info, nullptr, &ret->m_sampler) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create texture sampler!");
		}
		return ret;
	}

	std::shared_ptr<Sampler> Sampler::CreateLinearRepeat(std::shared_ptr<Device> device) {
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

		return Create(device, info);
	}

	std::shared_ptr<Sampler> Sampler::CreateLinearClamp(std::shared_ptr<Device> device) {
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.anisotropyEnable = VK_FALSE;
		info.maxAnisotropy = 1.0f;
		info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		info.unnormalizedCoordinates = VK_FALSE;
		info.compareEnable = VK_FALSE;
		info.compareOp = VK_COMPARE_OP_ALWAYS;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		info.mipLodBias = 0.0f;
		info.minLod = 0.0f;
		info.maxLod = 0.0f;
		return Create(device, info);
	}

	std::shared_ptr<Sampler> Sampler::CreateNearestClamp(std::shared_ptr<Device> device) {
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_NEAREST;
		info.minFilter = VK_FILTER_NEAREST;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.anisotropyEnable = VK_FALSE;
		info.maxAnisotropy = 1.0f;
		info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		info.unnormalizedCoordinates = VK_FALSE;
		info.compareEnable = VK_FALSE;
		info.compareOp = VK_COMPARE_OP_ALWAYS;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.mipLodBias = 0.0f;
		info.minLod = 0.0f;
		info.maxLod = 0.0f;
		return Create(device, info);
	}

	Sampler::~Sampler() {
		if (m_sampler != VK_NULL_HANDLE)
			vkDestroySampler(m_device->Get(), m_sampler, nullptr);
	}
}
