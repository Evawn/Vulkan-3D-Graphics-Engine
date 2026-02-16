#pragma once

#include "Device.h"
#include "Allocator.h"
#include "CommandPool.h"
#include "Image.h"
#include <string>

namespace ScreenshotCapture {

std::string Capture(
	std::shared_ptr<VWrap::Device> device,
	std::shared_ptr<VWrap::Allocator> allocator,
	std::shared_ptr<VWrap::CommandPool> commandPool,
	std::shared_ptr<VWrap::Image> resolveImage,
	VkFormat format,
	VkExtent2D extent);

} // namespace ScreenshotCapture
