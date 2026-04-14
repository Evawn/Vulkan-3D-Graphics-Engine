#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <optional>

#include "Image.h"
#include "ImageView.h"
#include "Buffer.h"
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
	VkImageUsageFlags extraUsage = 0; // ORed into auto-derived usage flags
};

struct BufferDesc {
	VkDeviceSize size;
	VkBufferUsageFlags usage = 0; // User-specified base usage (e.g., VERTEX_BUFFER_BIT)
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

// ---- Internal buffer storage ----

struct BufferResource {
	std::string name;
	BufferDesc desc;
	bool imported = false;

	std::shared_ptr<VWrap::Buffer> buffer;

	VkBufferUsageFlags usageFlags = 0;
};

// ---- Barriers ----

struct ImageBarrier {
	ImageHandle image;
	VkPipelineStageFlags srcStage;
	VkPipelineStageFlags dstStage;
	VkAccessFlags srcAccess;
	VkAccessFlags dstAccess;
	VkImageLayout oldLayout;
	VkImageLayout newLayout;
};

struct BufferBarrier {
	BufferHandle buffer;
	VkPipelineStageFlags srcStage;
	VkPipelineStageFlags dstStage;
	VkAccessFlags srcAccess;
	VkAccessFlags dstAccess;
};

// ---- Introspection (read-only snapshots) ----

struct PassInfo {
	std::string name;
	PassType type;
	bool enabled;

	std::vector<ImageHandle> readImages;
	std::vector<BufferHandle> readBuffers;
	std::vector<ImageHandle> writeImages;
	std::vector<BufferHandle> writeBuffers;

	struct ColorAttachmentDetail {
		ImageHandle target;
		LoadOp load = LoadOp::DontCare;
		StoreOp store = StoreOp::DontCare;
	};

	struct GraphicsDetail {
		std::vector<ColorAttachmentDetail> colorAttachments;
		ImageHandle depthTarget;
		ImageHandle resolveTarget;
		bool hasDepth = false;
		bool hasResolve = false;
		LoadOp depthLoad = LoadOp::DontCare;
		StoreOp depthStore = StoreOp::DontCare;
	};
	std::optional<GraphicsDetail> gfx;
};

struct PassBarrierSnapshot {
	std::vector<ImageBarrier> imageBarriers;
	std::vector<BufferBarrier> bufferBarriers;
};

struct GraphSnapshot {
	std::vector<PassInfo> passes;
	std::vector<PassBarrierSnapshot> barriers;
	std::vector<ImageResource> images;
	std::vector<BufferResource> buffers;
};
