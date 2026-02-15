#include "Allocator.h"

namespace VWrap
{
	std::shared_ptr<Allocator> VWrap::Allocator::Create(std::shared_ptr<Instance> instance, std::shared_ptr<PhysicalDevice> physical_device, std::shared_ptr<Device> device) {
		auto ret = std::make_shared<Allocator>();
		ret->m_device = device;

		VmaAllocatorCreateInfo allocatorCreateInfo = {};
		allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_0;
		allocatorCreateInfo.physicalDevice = physical_device->Get();
		allocatorCreateInfo.device = device->Get();
		allocatorCreateInfo.instance = instance->Get();
		allocatorCreateInfo.pVulkanFunctions = nullptr;

		vmaCreateAllocator(&allocatorCreateInfo, &ret->m_allocator);
		return ret;
	}
}
