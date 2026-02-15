#include "Fence.h"

namespace VWrap {

	std::shared_ptr<Fence> Fence::Create(std::shared_ptr<Device> device) {
		auto ret = std::make_shared<Fence>();
		ret->m_device = device;

		VkFenceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		if (vkCreateFence(device->Get(), &info, nullptr, &ret->m_fence) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create Fence!");
		}

		return ret;
	}

	Fence::~Fence() {
		if (m_fence != VK_NULL_HANDLE)
			vkDestroyFence(m_device->Get(), m_fence, nullptr);
	}
}