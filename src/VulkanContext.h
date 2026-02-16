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

struct VulkanContext {
	std::shared_ptr<VWrap::Instance> instance;
	std::shared_ptr<VWrap::PhysicalDevice> physicalDevice;
	std::shared_ptr<VWrap::Device> device;
	std::shared_ptr<VWrap::Allocator> allocator;

	std::shared_ptr<VWrap::Surface> surface;
	std::shared_ptr<VWrap::FrameController> frameController;

	std::shared_ptr<VWrap::Queue> graphicsQueue;
	std::shared_ptr<VWrap::Queue> presentQueue;
	std::shared_ptr<VWrap::Queue> transferQueue;
	std::shared_ptr<VWrap::CommandPool> graphicsCommandPool;
	std::shared_ptr<VWrap::CommandPool> transferCommandPool;

	VkSampleCountFlagBits msaaSamples;
};
