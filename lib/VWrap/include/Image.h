#pragma once
#include <vulkan/vulkan.h>
#include "Device.h"
#include <memory>
#include <string>
#include "vk_mem_alloc.h"
#include "Allocator.h"

namespace VWrap {

	/// <summary> Describes parameters for creating an image </summary>
	struct ImageCreateInfo {
		uint32_t width, height, depth;
		VkFormat format;
		VkImageTiling tiling;
		VkImageUsageFlags usage;
		VkMemoryPropertyFlags properties;
		uint32_t mip_levels;
		VkSampleCountFlagBits samples;
		VkImageType image_type;
		// Layered images: animated voxel volumes pack each frame as one layer
		// of a 2D-array image, sidestepping the maxImageDimension3D ceiling.
		// Default 1 keeps every existing caller behaving as before.
		uint32_t array_layers = 1;
		// Force the auto-derived ImageView to VK_IMAGE_VIEW_TYPE_2D_ARRAY even
		// when array_layers == 1. Needed for the "static volume as 1-layer
		// 2D-array" unification — shader-side `usampler2DArray` requires an
		// array view, and array_layers alone can't disambiguate "plain 2D" from
		// "1-layer 2D array intent".
		bool view_as_array = false;
	};

	/// <summary> Represents a Vulkan image and its corresponding memory allocation </summary>
	class Image {

	private:

		/// <summary> The Vulkan image handle </summary>
		VkImage m_image;

		/// <summary> The VMA allocation handle </summary>
		VmaAllocation m_allocation;

		/// <summary> The image format </summary>
		VkFormat m_format;

		/// <summary> The number of mip levels </summary>
		uint32_t m_mip_levels;

		/// <summary> The width and height of the image </summary>
		uint32_t m_width, m_height;

		VkImageType m_image_type;

		/// <summary> Number of array layers (1 unless this is a 2D-array image) </summary>
		uint32_t m_array_layers;

		/// <summary> When true, the auto-derived ImageView uses VIEW_TYPE_2D_ARRAY
		/// even with m_array_layers == 1 (intent flag for static volumes that
		/// share the animated-volume sampling convention). </summary>
		bool m_view_as_array;

		/// <summary> The allocator used to create the image </summary>
		std::shared_ptr<Allocator> m_allocator;

	public:

		static std::shared_ptr<Image> Create(std::shared_ptr<Allocator> allocator, ImageCreateInfo& info);

		/// <summary>
		/// Returns the image handle
		/// </summary>
		VkImage Get() const { return m_image; }

		/// <summary>
		/// Gets the image format
		/// </summary>
		VkFormat GetFormat() const { return m_format; }

		/// <summary>
		/// Gets the number of mip levels
		/// </summary>
		uint32_t GetMipLevels() const { return m_mip_levels; }

		VkImageType GetImageType() const { return m_image_type; }

		/// <summary> Number of array layers (1 unless this is a 2D-array image) </summary>
		uint32_t GetArrayLayers() const { return m_array_layers; }

		/// <summary> True if the image was declared with the "view as array" intent. </summary>
		bool ViewAsArray() const { return m_view_as_array; }

		/// <summary>
		/// Gets the allocator used to create the image
		/// </summary>
		std::shared_ptr<Allocator> GetAllocator() const { return m_allocator; }

		~Image();
	};
}
