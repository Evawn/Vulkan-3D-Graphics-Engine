#include "CommandPool.h"

namespace VWrap {

	std::shared_ptr<CommandPool> CommandPool::Create(std::shared_ptr<Device> device, std::shared_ptr<Queue> queue)
	{
		auto ret = std::make_shared<CommandPool>();
		ret->m_device = device;
		ret->m_queue = queue;

		VkCommandPoolCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		info.queueFamilyIndex = queue->GetQueueFamilyIndex();
		info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		if (vkCreateCommandPool(device->Get(), &info, nullptr, &ret->m_command_pool) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create command pool!");
		}

		return ret;
	}

	CommandPool::~CommandPool()
	{
		if (m_command_pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(m_device->Get(), m_command_pool, nullptr);
	}
}