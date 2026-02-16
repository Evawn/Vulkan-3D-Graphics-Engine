#include "ScreenshotCapture.h"
#include "Buffer.h"
#include "CommandBuffer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <spdlog/spdlog.h>
#include <ctime>
#include <filesystem>

namespace ScreenshotCapture {

std::string Capture(
	std::shared_ptr<VWrap::Device> device,
	std::shared_ptr<VWrap::Allocator> allocator,
	std::shared_ptr<VWrap::CommandPool> commandPool,
	std::shared_ptr<VWrap::Image> resolveImage,
	VkFormat format,
	VkExtent2D extent)
{
	auto logger = spdlog::get("App");

	vkDeviceWaitIdle(device->Get());

	VkDeviceSize imageSize = extent.width * extent.height * 4; // RGBA
	auto staging = VWrap::Buffer::CreateStaging(allocator, imageSize);

	// Record copy commands
	auto cmd = VWrap::CommandBuffer::Create(commandPool);
	cmd->BeginSingle();

	cmd->CmdTransitionImageLayout(resolveImage, format,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { extent.width, extent.height, 1 };

	vkCmdCopyImageToBuffer(cmd->Get(), resolveImage->Get(),
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->Get(), 1, &region);

	cmd->CmdTransitionImageLayout(resolveImage, format,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	cmd->EndAndSubmit();

	// Map staging buffer and write PNG
	void* data;
	vmaMapMemory(allocator->Get(), staging->GetAllocation(), &data);

	std::filesystem::create_directories("screenshots");
	std::time_t t = std::time(nullptr);
	char timestr[64];
	std::strftime(timestr, sizeof(timestr), "%Y%m%d_%H%M%S", std::localtime(&t));
	std::string filename = std::string("screenshots/screenshot_") + timestr + ".png";

	// Handle BGRA -> RGBA swizzle if needed
	if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM) {
		uint8_t* pixels = static_cast<uint8_t*>(data);
		for (uint32_t i = 0; i < extent.width * extent.height; i++) {
			std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
		}
	}

	int result = stbi_write_png(filename.c_str(), extent.width, extent.height, 4,
		data, extent.width * 4);

	vmaUnmapMemory(allocator->Get(), staging->GetAllocation());

	if (result) {
		logger->info("Screenshot saved: {}", filename);
		return filename;
	} else {
		logger->error("Failed to save screenshot");
		return "";
	}
}

} // namespace ScreenshotCapture
