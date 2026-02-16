#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <unordered_map>

#include "Device.h"
#include "Allocator.h"
#include "Image.h"
#include "ImageView.h"
#include "RenderPass.h"
#include "Framebuffer.h"
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

// ---- Forward declarations ----

class RenderGraph;

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

// ---- Pass Builders ----

class GraphicsPassBuilder {
public:
	GraphicsPassBuilder(const std::string& name, RenderGraph& graph);

	GraphicsPassBuilder& SetColorAttachment(
		ImageHandle target, LoadOp load, StoreOp store,
		float r = 0, float g = 0, float b = 0, float a = 1);
	GraphicsPassBuilder& SetDepthAttachment(
		ImageHandle target, LoadOp load, StoreOp store,
		float depth = 1.0f, uint32_t stencil = 0);
	GraphicsPassBuilder& SetResolveTarget(ImageHandle target);
	GraphicsPassBuilder& Read(ImageHandle resource);
	GraphicsPassBuilder& Read(BufferHandle resource);
	GraphicsPassBuilder& SetRecord(std::function<void(PassContext&)> fn);

	VkRenderPass GetRenderPass();
	std::shared_ptr<VWrap::RenderPass> GetRenderPassPtr();

	const std::string& GetName() const { return m_name; }
	bool IsEnabled() const { return m_enabled; }
	void SetEnabled(bool enabled) { m_enabled = enabled; }

private:
	friend class RenderGraph;

	std::string m_name;
	bool m_enabled = true;
	RenderGraph& m_graph;

	// Color attachment
	ImageHandle m_colorTarget;
	LoadOp m_colorLoad = LoadOp::DontCare;
	StoreOp m_colorStore = StoreOp::DontCare;
	VkClearColorValue m_clearColor{};

	// Depth attachment
	ImageHandle m_depthTarget;
	bool m_hasDepth = false;
	LoadOp m_depthLoad = LoadOp::DontCare;
	StoreOp m_depthStore = StoreOp::DontCare;
	VkClearDepthStencilValue m_clearDepthStencil{1.0f, 0};

	// Resolve target
	ImageHandle m_resolveTarget;
	bool m_hasResolve = false;

	// Sampled inputs
	std::vector<ImageHandle> m_readImages;
	std::vector<BufferHandle> m_readBuffers;

	// Record callback
	std::function<void(PassContext&)> m_recordFn;

	// Created lazily by GetRenderPass / during Compile
	std::shared_ptr<VWrap::RenderPass> m_renderPass;
	std::unordered_map<VkImageView, std::shared_ptr<VWrap::Framebuffer>> m_framebufferCache;
	std::shared_ptr<VWrap::Framebuffer> m_activeFramebuffer;

	void CreateRenderPass(VkImageLayout colorFinalLayout, VkImageLayout resolveFinalLayout);
	void CreateFramebuffer();
};

class ComputePassBuilder {
public:
	ComputePassBuilder(const std::string& name, RenderGraph& graph);

	ComputePassBuilder& Read(ImageHandle resource);
	ComputePassBuilder& Read(BufferHandle resource);
	ComputePassBuilder& Write(ImageHandle resource);
	ComputePassBuilder& Write(BufferHandle resource);
	ComputePassBuilder& SetRecord(std::function<void(PassContext&)> fn);

	const std::string& GetName() const { return m_name; }
	bool IsEnabled() const { return m_enabled; }
	void SetEnabled(bool enabled) { m_enabled = enabled; }

private:
	friend class RenderGraph;

	std::string m_name;
	bool m_enabled = true;
	RenderGraph& m_graph;

	std::vector<ImageHandle> m_readImages;
	std::vector<BufferHandle> m_readBuffers;
	std::vector<ImageHandle> m_writeImages;
	std::vector<BufferHandle> m_writeBuffers;

	std::function<void(PassContext&)> m_recordFn;
};

// ---- Render Graph ----

class RenderGraph {
public:
	RenderGraph() = default;
	RenderGraph(std::shared_ptr<VWrap::Device> device, std::shared_ptr<VWrap::Allocator> allocator);

	// ---- Resources ----
	ImageHandle CreateImage(const std::string& name, const ImageDesc& desc);
	BufferHandle CreateBuffer(const std::string& name, const BufferDesc& desc);
	ImageHandle ImportImage(const std::string& name,
	                        std::shared_ptr<VWrap::ImageView> view,
	                        VkFormat format, VkImageLayout externalLayout,
	                        VkExtent2D extent);

	// ---- Passes ----
	GraphicsPassBuilder& AddGraphicsPass(const std::string& name);
	ComputePassBuilder& AddComputePass(const std::string& name);
	void SetPassEnabled(const std::string& name, bool enabled);

	// ---- Lifecycle ----
	void Compile();
	void Execute(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex);
	void Resize(VkExtent2D newExtent);
	void Clear();

	// ---- Resource Access ----
	std::shared_ptr<VWrap::ImageView> GetImageView(ImageHandle handle) const;
	VkImage GetVkImage(ImageHandle handle) const;
	std::shared_ptr<VWrap::Image> GetImage(ImageHandle handle) const;
	ImageDesc GetImageDesc(ImageHandle handle) const;
	VkFormat GetImageFormat(ImageHandle handle) const;

	// ---- Dynamic imports ----
	void UpdateImport(ImageHandle handle, std::shared_ptr<VWrap::ImageView> view);

	// ---- Internal access for builders ----
	const ImageResource& GetImageResource(ImageHandle handle) const;
	std::shared_ptr<VWrap::Device> GetDevice() const { return m_device; }

private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;

	// Resources
	std::vector<ImageResource> m_images;

	// Pass storage (unique_ptr for pointer stability)
	std::vector<std::unique_ptr<GraphicsPassBuilder>> m_graphicsPasses;
	std::vector<std::unique_ptr<ComputePassBuilder>> m_computePasses;

	// Execution order
	struct PassRef { PassType type; size_t index; };
	std::vector<PassRef> m_executionOrder;

	// Pre-computed barriers per execution step
	std::vector<std::vector<ImageBarrier>> m_barriers;

	bool m_compiled = false;

	// Internal helpers
	void AccumulateUsageFlags();
	void AllocateTransientImages();
	void CreateRenderPasses();
	void CreateFramebuffers();
	void ComputeBarriers();

	VkImageLayout DetermineColorFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	VkImageLayout DetermineResolveFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	bool IsImageReadDownstream(size_t afterOrderIndex, ImageHandle image) const;
};
