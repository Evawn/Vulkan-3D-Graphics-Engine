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
#include "DescriptorSetLayout.h"
#include "Pipeline.h"
#include "ComputePipeline.h"

// ---- Handles ----
//
// `gen` guards against use-after-Clear: every Clear() bumps the graph's gen
// counter and stamps it into resources at creation time. Get*() asserts the
// handle's gen matches the resource's, so a stale handle from a prior graph
// build fires immediately instead of silently aliasing an unrelated resource.

struct ImageHandle  { uint32_t id = UINT32_MAX; uint32_t gen = 0; };
struct BufferHandle { uint32_t id = UINT32_MAX; uint32_t gen = 0; };

// ---- Lifetime ----
//
// Transient: rebuilt on Resize() (default). Persistent: survives Resize() —
// use for resources holding data that no producer pass refills (e.g. uploaded
// .vox volumes). Imported is tracked separately by the `imported` bool on the
// resource since imported resources have completely different metadata.

enum class Lifetime { Transient, Persistent };

// ---- Descriptors ----

struct ImageDesc {
	uint32_t width, height;
	uint32_t depth = 1;
	VkFormat format;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageType imageType = VK_IMAGE_TYPE_2D;
	VkImageUsageFlags extraUsage = 0; // ORed into auto-derived usage flags
	Lifetime lifetime = Lifetime::Transient;
};

struct BufferDesc {
	VkDeviceSize size;
	VkBufferUsageFlags usage = 0; // User-specified base usage (e.g., VERTEX_BUFFER_BIT)
	Lifetime lifetime = Lifetime::Transient;
};

// ---- Enums ----

enum class PassType { Graphics, Compute };
enum class LoadOp { Clear, Load, DontCare };
enum class StoreOp { Store, DontCare };

// Reference to a pass within RenderGraph's storage. Promoted here so DAGBuilder
// (which is not a friend of RenderGraph) can name it without pulling in
// RenderGraph.h.
struct PassRef {
	PassType type;
	size_t   index;
};

// ---- Resource usage ----
//
// How a Read()/Write() declaration intends to use the resource. The graph maps
// each usage to (image layout, access mask, pipeline stage) so barrier
// synthesis is precise instead of always-conservative. `Default` keeps legacy
// behavior (compute = GENERAL + COMPUTE; graphics fragment = SHADER_READ_ONLY +
// VERTEX|FRAGMENT) so unmigrated call sites are unchanged.

enum class ResourceUsage {
	Default,
	SampledRead,    // sampled image / combined image sampler
	StorageRead,    // storage image / buffer read
	StorageWrite,   // storage image / buffer write
	UniformRead,    // UBO
	VertexBuffer,
	IndexBuffer,
	IndirectArg,    // VkCmdDraw*Indirect / VkCmdDispatchIndirect
};

// ---- Pipeline descriptions (graph-owned) ----
//
// Techniques no longer construct VkPipelines directly. They hand the graph a
// description of the pipeline they want; the graph instantiates it after
// Compile() against the canonical render pass.

struct GraphicsPipelineDesc {
	// SPV paths. The graph re-reads these on every (re)build, so hot-reload
	// flows through naturally.
	std::string vertSpvPath;
	std::string fragSpvPath;

	std::shared_ptr<VWrap::DescriptorSetLayout> descriptorSetLayout;
	std::vector<VkPushConstantRange> pushConstantRanges;

	// Owned vertex-input storage. The graph builds VkPipelineVertexInputStateCreateInfo
	// from these vectors at pipeline-create time so the underlying storage stays alive.
	std::vector<VkVertexInputBindingDescription> vertexBindings;
	std::vector<VkVertexInputAttributeDescription> vertexAttributes;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	VkPipelineRasterizationStateCreateInfo rasterizer{};
	VkPipelineDepthStencilStateCreateInfo depthStencil{};

	// Owned dynamic-state storage. Empty == no dynamic state.
	std::vector<VkDynamicState> dynamicStates;
};

struct ComputePipelineDesc {
	std::string compSpvPath;
	std::shared_ptr<VWrap::DescriptorSetLayout> descriptorSetLayout;
	std::vector<VkPushConstantRange> pushConstantRanges;
};

// ---- Pass Context ----

struct PassContext {
	std::shared_ptr<VWrap::CommandBuffer> cmd;
	uint32_t frameIndex;
	VkExtent2D extent;

	// Set by the graph before the record callback runs. Whichever pointer is
	// populated depends on the pass type; stays null if SetPipeline() wasn't
	// called (e.g. the UI pass uses ImGui's internal pipeline).
	std::shared_ptr<VWrap::Pipeline> graphicsPipeline;
	std::shared_ptr<VWrap::ComputePipeline> computePipeline;
};

// ---- Internal resource storage ----

struct ImageResource {
	std::string name;
	ImageDesc desc;
	bool imported = false;
	uint32_t gen = 0;  // matched against ImageHandle::gen on Get*()

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
	uint32_t gen = 0;  // matched against BufferHandle::gen on Get*()

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
