#pragma once

#include "RenderGraphTypes.h"
#include "GraphicsPassBuilder.h"
#include "ComputePassBuilder.h"
#include "PassDAG.h"
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

	// Mark a resource as a graph sink. The pass that last writes this resource
	// is preserved during DAG pruning, along with anything it transitively
	// depends on. Imported and Persistent resources are implicit sinks already;
	// MarkSink is the explicit opt-in for transient resources whose output
	// matters for reasons the graph can't infer (e.g. screenshot capture
	// targets, user-facing outputs not consumed by another pass).
	void MarkSink(ImageHandle handle);
	void MarkSink(BufferHandle handle);

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

	// Pass declaration order — populated by Add*Pass as the user constructs the
	// graph. Compile() feeds this into DAGBuilder, which produces the topo-sorted
	// m_executionOrder. In Phase 1 they're equal-by-construction (the DAG only
	// records edges that go forward in declaration order, so the topo sort
	// reproduces declaration order); the rename signals that the input/output
	// distinction now exists.
	std::vector<PassRef> m_declarationOrder;
	std::vector<PassRef> m_executionOrder;

	// DAG over passes — built in Compile(), referenced by IsImageReadDownstream
	// and (Phase 2) future barrier / reachability queries.
	PassDAG               m_dag;
	std::vector<uint32_t> m_dagDeclToNode;       // decl idx -> node id (INVALID if pruned)
	std::vector<uint32_t> m_executionOrderNodes; // parallel to m_executionOrder; DAG node ids
	std::vector<size_t>   m_nodeToDecl;          // node id -> decl idx (inverse of declToNode)
	std::vector<bool>     m_nodeAlive;           // node id -> is in m_executionOrder

	// Explicit sink resources marked by callers via MarkSink(). Combined with
	// imported / Persistent resources (implicit sinks) at Compile time.
	std::vector<uint32_t> m_explicitImageSinks;
	std::vector<uint32_t> m_explicitBufferSinks;

	// Pre-computed barriers per execution step
	struct PassBarriers {
		std::vector<ImageBarrier> imageBarriers;
		std::vector<BufferBarrier> bufferBarriers;
	};
	std::vector<PassBarriers> m_barriers;

	bool m_compiled = false;

	// Generation counters bumped on Clear(); stamped onto every new
	// ImageHandle/BufferHandle so stale handles fail the gen check in Get*().
	uint32_t m_imageGen = 1;
	uint32_t m_bufferGen = 1;

	// Internal helpers
	void AccumulateUsageFlags();
	void AllocateTransientResources();
	void CreateRenderPasses();
	void CreateFramebuffers();
	void CreatePipelines();
	void UpdateBindings();
	void ComputeBarriers();

	void BuildGraphicsPipeline(GraphicsPassBuilder& pass);
	void BuildComputePipeline(ComputePassBuilder& pass);

	VkImageLayout DetermineColorFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	VkImageLayout DetermineResolveFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	bool IsImageReadDownstream(size_t afterOrderIndex, ImageHandle image) const;
};
