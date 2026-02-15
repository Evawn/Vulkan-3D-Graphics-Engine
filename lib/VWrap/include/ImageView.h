#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "Image.h"

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan image view.
	/// </summary>
	class ImageView {
	private:

		/// <summary> The Vulkan image view handle. </summary>
		VkImageView m_image_view{ VK_NULL_HANDLE };

		/// <summary> The device that owns this image view. </summary>
		std::shared_ptr<Device> m_device;

		/// <summary> The image that this view is created from. </summary>
		std::shared_ptr<Image> m_image;

	public:

		/// <summary> Creates a new image view. </summary>
		static std::shared_ptr<ImageView> Create(std::shared_ptr<Device> device, std::shared_ptr<Image> image, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

		/// <summary> 
		/// Creates a new image view directly from a VkImage handle. Used for wrapping swapchain images. 
		/// </summary>
		static std::shared_ptr<ImageView> Create(std::shared_ptr<Device> device, VkImage image, VkFormat format, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t mip_levels = 1, VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D);

		/// <summary> Gets the Vulkan image view handle. </summary>
		VkImageView Get() const { return m_image_view; }

		~ImageView();
	};
}