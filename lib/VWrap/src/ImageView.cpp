#include "ImageView.h"

namespace VWrap {

	std::shared_ptr<ImageView> ImageView::Create(std::shared_ptr<Device> device, std::shared_ptr<Image> image, VkImageAspectFlags aspect) {
		// 2D + (arrayLayers>1 OR view_as_array intent) => 2D_ARRAY. The intent
		// flag covers static volumes (frameCount==1, layers==1) that share the
		// animated-volume `usampler2DArray` shader convention — without it the
		// view falls back to plain 2D and the shader's binding type silently
		// mismatches the descriptor.
		VkImageViewType type;
		if (image->GetImageType() == VK_IMAGE_TYPE_2D) {
			const bool isArray = image->GetArrayLayers() > 1 || image->ViewAsArray();
			type = isArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
		} else {
			type = VK_IMAGE_VIEW_TYPE_3D;
		}
		auto ret = Create(device, image->Get(), image->GetFormat(), aspect, image->GetMipLevels(), type, image->GetArrayLayers());
		ret->m_image = image;
		return ret;
	}

	std::shared_ptr<ImageView> ImageView::Create(std::shared_ptr<Device> device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mip_levels, VkImageViewType type, uint32_t layer_count) {
		auto ret = std::make_shared<ImageView>();
		ret->m_device = device;

		VkImageViewCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		createInfo.image = image;
		createInfo.format = format;
		createInfo.viewType = type;
		createInfo.subresourceRange.aspectMask = aspect;
		createInfo.subresourceRange.baseArrayLayer = 0;
		createInfo.subresourceRange.baseMipLevel = 0;
		createInfo.subresourceRange.layerCount = layer_count == 0 ? 1 : layer_count;
		createInfo.subresourceRange.levelCount = mip_levels;

		if (vkCreateImageView(device->Get(), &createInfo, nullptr, &ret->m_image_view) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create texture image view!");
		}
		return ret;
	}

	ImageView::~ImageView() {
		if (m_image_view != VK_NULL_HANDLE)
			vkDestroyImageView(m_device->Get(), m_image_view, nullptr);
	}
}