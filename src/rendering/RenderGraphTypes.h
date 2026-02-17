#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

#include "Image.h"
#include "ImageView.h"
#include "CommandBuffer.h"

// ---- Handles ----

struct ImageHandle { uint32_t id = UINT32_MAX; };
struct BufferHandle { uint32_t id = UINT32_MAX; };

// ---- Descriptors ----

struct ImageDesc {
	uint32_t width, height;
	uint32_t depth = 1;
	VkFormat format;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageType imageType = VK_IMAGE_TYPE_2D;
};

struct BufferDesc {
	VkDeviceSize size;
};

// ---- Enums ----

enum class PassType { Graphics, Compute };
enum class LoadOp { Clear, Load, DontCare };
enum class StoreOp { Store, DontCare };

// ---- Pass Context ----

struct PassContext {
	std::shared_ptr<VWrap::CommandBuffer> cmd;
	uint32_t frameIndex;
	VkExtent2D extent;
};

// ---- Internal resource storage ----

struct ImageResource {
	std::string name;
	ImageDesc desc;
	bool imported = false;

	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageLayout externalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	std::shared_ptr<VWrap::Image> image;
	std::shared_ptr<VWrap::ImageView> view;

	VkImageUsageFlags usageFlags = 0;
};

// ---- Barrier ----

struct ImageBarrier {
	ImageHandle image;
	VkPipelineStageFlags srcStage;
	VkPipelineStageFlags dstStage;
	VkAccessFlags srcAccess;
	VkAccessFlags dstAccess;
	VkImageLayout oldLayout;
	VkImageLayout newLayout;
};
