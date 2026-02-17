#include "VulkanContext.h"

namespace VWrap {

VulkanContext VulkanContext::Create(std::shared_ptr<GLFWwindow*> window,
									bool enableValidation, uint32_t maxFramesInFlight) {
	VulkanContext ctx{};

	ctx.instance = Instance::Create(enableValidation);
	ctx.surface = Surface::Create(ctx.instance, window);
	ctx.physicalDevice = PhysicalDevice::Pick(ctx.instance, ctx.surface);
	ctx.device = Device::Create(ctx.physicalDevice, enableValidation);
	ctx.allocator = Allocator::Create(ctx.instance, ctx.physicalDevice, ctx.device);

	QueueFamilyIndices indices = ctx.physicalDevice->FindQueueFamilies();

	ctx.graphicsQueue = Queue::Create(ctx.device, indices.graphicsFamily.value());
	ctx.presentQueue = Queue::Create(ctx.device, indices.presentFamily.value());
	ctx.transferQueue = Queue::Create(ctx.device, indices.transferFamily.value());
	ctx.computeQueue = Queue::Create(ctx.device, indices.computeFamily.value());
	ctx.graphicsCommandPool = CommandPool::Create(ctx.device, ctx.graphicsQueue);
	ctx.transferCommandPool = CommandPool::Create(ctx.device, ctx.transferQueue);
	ctx.computeCommandPool = CommandPool::Create(ctx.device, ctx.computeQueue);

	ctx.frameController = FrameController::Create(ctx.device, ctx.surface,
		ctx.graphicsCommandPool, ctx.presentQueue, maxFramesInFlight);

	ctx.msaaSamples = ctx.physicalDevice->GetMaxUsableSampleCount();

	return ctx;
}

} // namespace VWrap
