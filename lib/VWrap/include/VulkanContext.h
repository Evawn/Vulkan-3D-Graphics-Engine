#pragma once

#include "Instance.h"
#include "PhysicalDevice.h"
#include "Device.h"
#include "Allocator.h"
#include "Surface.h"
#include "Queue.h"
#include "CommandPool.h"
#include "FrameController.h"
#include "RenderPass.h"

namespace VWrap {

struct VulkanContext {
	std::shared_ptr<Instance> instance;
	std::shared_ptr<PhysicalDevice> physicalDevice;
	std::shared_ptr<Device> device;
	std::shared_ptr<Allocator> allocator;

	std::shared_ptr<Surface> surface;
	std::shared_ptr<FrameController> frameController;

	std::shared_ptr<Queue> graphicsQueue;
	std::shared_ptr<Queue> presentQueue;
	std::shared_ptr<Queue> transferQueue;
	std::shared_ptr<Queue> computeQueue;
	std::shared_ptr<CommandPool> graphicsCommandPool;
	std::shared_ptr<CommandPool> transferCommandPool;
	std::shared_ptr<CommandPool> computeCommandPool;

	VkSampleCountFlagBits msaaSamples;

	// Factory: creates all Vulkan objects and populates the context.
	static VulkanContext Create(std::shared_ptr<GLFWwindow*> window,
								bool enableValidation, uint32_t maxFramesInFlight);
};

} // namespace VWrap
