#include "Queue.h"

namespace VWrap
{
	std::shared_ptr<Queue> Queue::Create(std::shared_ptr<Device> device, uint32_t queue_family_index)
	{
		auto ret = std::make_shared<Queue>();
		ret->m_device = device;
		ret->m_queue_family_index = queue_family_index;

		vkGetDeviceQueue(device->Get(), queue_family_index, 0, &ret->m_queue);

		return ret;
	}
}