#pragma once

#include "RenderGraphTypes.h"
#include "GraphicsPassBuilder.h"
#include "ComputePassBuilder.h"
#include "Device.h"
#include "Allocator.h"
#include <memory>
#include <vector>

class GPUProfiler;

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
	BufferHandle ImportBuffer(const std::string& name,
	                          std::shared_ptr<VWrap::Buffer> buffer,
	                          VkDeviceSize size);

	// ---- Passes ----
	GraphicsPassBuilder& AddGraphicsPass(const std::string& name);
	ComputePassBuilder& AddComputePass(const std::string& name);
	void SetPassEnabled(const std::string& name, bool enabled);

	// ---- Lifecycle ----
	void Compile();
	void Execute(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex,
	             GPUProfiler* profiler = nullptr);
	void Resize(VkExtent2D newExtent);
	void Clear();

	// Re-evaluate every pass's pipeline desc factory and rebuild the VkPipeline.
	// Use for hot-reload (re-reads SPV) and for state changes that need a fresh
	// pipeline (e.g. wireframe toggle). Render passes / framebuffers are unchanged.
	void RecreatePipelines();

	// ---- Resource Access ----
	std::shared_ptr<VWrap::ImageView> GetImageView(ImageHandle handle) const;
	VkImage GetVkImage(ImageHandle handle) const;
	std::shared_ptr<VWrap::Image> GetImage(ImageHandle handle) const;
	ImageDesc GetImageDesc(ImageHandle handle) const;
	VkFormat GetImageFormat(ImageHandle handle) const;

	std::shared_ptr<VWrap::Buffer> GetBuffer(BufferHandle handle) const;
	VkBuffer GetVkBuffer(BufferHandle handle) const;

	// ---- Dynamic imports ----
	void UpdateImport(ImageHandle handle, std::shared_ptr<VWrap::ImageView> view);

	// ---- Introspection ----
	GraphSnapshot BuildSnapshot() const;
	size_t GetPassCount() const { return m_executionOrder.size(); }

	// ---- Internal access for builders ----
	const ImageResource& GetImageResource(ImageHandle handle) const;
	const BufferResource& GetBufferResource(BufferHandle handle) const;
	std::shared_ptr<VWrap::Device> GetDevice() const { return m_device; }

private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;

	// Resources
	std::vector<ImageResource> m_images;
	std::vector<BufferResource> m_buffers;

	// Pass storage (unique_ptr for pointer stability)
	std::vector<std::unique_ptr<GraphicsPassBuilder>> m_graphicsPasses;
	std::vector<std::unique_ptr<ComputePassBuilder>> m_computePasses;

	// Execution order
	struct PassRef { PassType type; size_t index; };
	std::vector<PassRef> m_executionOrder;

	// Pre-computed barriers per execution step
	struct PassBarriers {
		std::vector<ImageBarrier> imageBarriers;
		std::vector<BufferBarrier> bufferBarriers;
	};
	std::vector<PassBarriers> m_barriers;

	bool m_compiled = false;

	// Internal helpers
	void AccumulateUsageFlags();
	void AllocateTransientResources();
	void CreateRenderPasses();
	void CreateFramebuffers();
	void CreatePipelines();
	void ComputeBarriers();

	void BuildGraphicsPipeline(GraphicsPassBuilder& pass);
	void BuildComputePipeline(ComputePassBuilder& pass);

	VkImageLayout DetermineColorFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	VkImageLayout DetermineResolveFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	bool IsImageReadDownstream(size_t afterOrderIndex, ImageHandle image) const;
};
