#include "Image.h"

namespace VWrap {

	std::shared_ptr<Image> Image::Create(std::shared_ptr<Allocator> allocator, ImageCreateInfo& info)
	{
		auto ret = std::make_shared<Image>();
		ret->m_allocator = allocator;
		ret->m_format = info.format;
		ret->m_mip_levels = info.mip_levels == 0 ? 1 : info.mip_levels;
		ret->m_width = info.width;
		ret->m_height = info.height;
		ret->m_image_type = info.image_type;

		VkSampleCountFlagBits samples = info.samples ? info.samples : VK_SAMPLE_COUNT_1_BIT;

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = info.image_type;
		imageInfo.extent.width = info.width;
		imageInfo.extent.height = info.height;
		imageInfo.extent.depth = (info.image_type == VK_IMAGE_TYPE_3D) ? info.depth : 1;
		imageInfo.format = info.format;
		imageInfo.arrayLayers = 1;
		imageInfo.mipLevels = ret->m_mip_levels;
		imageInfo.tiling = info.tiling;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = info.usage;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.samples = samples;
		imageInfo.flags = 0; // Optional

		VmaAllocationCreateInfo allocCreateInfo{};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

		if (vmaCreateImage(allocator->Get(), &imageInfo, &allocCreateInfo, &ret->m_image, &ret->m_allocation, nullptr) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create image!");
		}

		return ret;
	}

	Image::~Image() {
		if (m_image != VK_NULL_HANDLE && m_allocation != nullptr)
			vmaDestroyImage(m_allocator->Get(), m_image, m_allocation);
	}
}