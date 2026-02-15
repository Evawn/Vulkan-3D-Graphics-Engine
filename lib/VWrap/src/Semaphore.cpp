#include "Semaphore.h"

namespace VWrap {

	std::shared_ptr<Semaphore> Semaphore::Create(std::shared_ptr<Device> device) {
		auto ret = std::make_shared<Semaphore>();
		ret->m_device = device;

		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		if (vkCreateSemaphore(device->Get(), &semaphoreInfo, nullptr, &ret->m_semaphore) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create semaphore!");
		}

		return ret;
	}

	Semaphore::~Semaphore() {
		if (m_semaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(m_device->Get(), m_semaphore, nullptr);
	}
}