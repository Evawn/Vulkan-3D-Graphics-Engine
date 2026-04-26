#pragma once

#include "RenderGraphTypes.h"
#include "GraphicsPassBuilder.h"
#include "ComputePassBuilder.h"
#include "PassDAG.h"
#include "Device.h"
#include "Allocator.h"
#include "CommandBuffer.h"
#include "CommandPool.h"
#include "Queue.h"
#include "Semaphore.h"
#include <memory>
#include <vector>

class GPUProfiler;
class RenderScene;

// Optional async-compute plumbing for the render graph. When asyncQueue is
// non-null AND its queue family differs from the graphics family, the graph
// honors AsyncCompute affinity hints by recording compute work into a
// per-frame-in-flight async command buffer and submitting it on asyncQueue.
// Cross-queue handoff uses one binary semaphore per frame-in-flight, signaled
// by the async submit and waited on by the next graphics submit. If the queue
// is null OR shares a family with graphics, every AsyncCompute hint is
// silently demoted back to the graphics stream.
struct RenderGraphAsyncConfig {
	std::shared_ptr<VWrap::Queue>       computeQueue;
	std::shared_ptr<VWrap::CommandPool> computeCommandPool;
	uint32_t                            graphicsQueueFamily = 0;
	uint32_t                            framesInFlight      = 1;
};

class RenderGraph {
public:
	RenderGraph() = default;
	RenderGraph(std::shared_ptr<VWrap::Device> device, std::shared_ptr<VWrap::Allocator> allocator);

	// Set once after construction (typically by Renderer) to enable async-compute
	// scheduling. Calling with a queue whose family equals graphicsQueueFamily
	// disables async; the graph keeps the config but treats async-availability
	// as false.
	void ConfigureAsync(const RenderGraphAsyncConfig& cfg);

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

	// Frame-local scene the graph hands to each pass via PassContext::scene.
	// Set by RenderingSystem before each Execute(). Non-owning. The graph never
	// modifies the scene; passes only read items out of it.
	void SetScene(const RenderScene* scene) { m_scene = scene; }

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

	// One-shot CPU→device staging upload into a graph-managed buffer. The buffer
	// must already be allocated (i.e. Compile() has run). Used for geometry that
	// outlives a single frame — Lifetime::Persistent buffers holding OBJ data,
	// generated foliage proxies, animated voxel asset metadata, etc.
	void UploadBufferData(BufferHandle handle, const void* data, size_t size,
	                      std::shared_ptr<VWrap::CommandPool> pool);

	// ---- Introspection ----
	GraphSnapshot BuildSnapshot() const;
	size_t GetPassCount() const { return m_executionOrder.size(); }

	// What the host-side graphics submit needs to wait on this frame, if anything.
	// Populated by Execute() — cleared at the start of every Execute call. Empty
	// vector = no async work was submitted; the caller may submit graphics with
	// only its existing waits (image-available). Non-empty = the graphics queue
	// must additionally wait on the listed semaphores at the listed stages
	// before reading anything the async batch produced.
	struct GraphicsQueueWait {
		std::vector<VkSemaphore>          semaphores;
		std::vector<VkPipelineStageFlags> stages;
	};
	const GraphicsQueueWait& GetGraphicsQueueWait() const { return m_graphicsQueueWait; }

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

	// Per-pass resolved queue stream (parallel to m_executionOrder). Set by
	// Compile() from DAGBuilder's affinity output, after demotion.
	std::vector<QueueAffinity> m_executionOrderStreams;
	bool                       m_hasAsyncWork = false;

	// Explicit sink resources marked by callers via MarkSink(). Combined with
	// imported / Persistent resources (implicit sinks) at Compile time.
	std::vector<uint32_t> m_explicitImageSinks;
	std::vector<uint32_t> m_explicitBufferSinks;

	// Pre-computed barriers per execution step. The release barriers are
	// emitted *after* the pass body (only populated for passes that produce
	// resources consumed on a different queue stream); the regular barriers
	// are emitted before the pass body and may include cross-stream acquires.
	struct PassBarriers {
		std::vector<ImageBarrier> imageBarriers;
		std::vector<BufferBarrier> bufferBarriers;
		std::vector<ImageBarrier> imageReleaseBarriers;
		std::vector<BufferBarrier> bufferReleaseBarriers;
	};
	std::vector<PassBarriers> m_barriers;

	// ---- Async-compute plumbing (only populated when ConfigureAsync was called
	// with a distinct compute queue family) ----
	RenderGraphAsyncConfig                            m_asyncCfg{};
	bool                                              m_asyncAvailable = false;
	std::vector<std::shared_ptr<VWrap::CommandBuffer>> m_asyncCmdBuffers;     // one per frame-in-flight
	std::vector<std::shared_ptr<VWrap::Semaphore>>     m_asyncDoneSemaphores; // one per frame-in-flight
	GraphicsQueueWait                                  m_graphicsQueueWait{}; // recomputed each Execute()

	bool m_compiled = false;

	// Non-owning per-frame pointer set by RenderingSystem before Execute().
	// Forwarded into every PassContext so record callbacks can iterate items
	// of the type they consume.
	const RenderScene* m_scene = nullptr;

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

	// Records every pass in m_executionOrder whose stream matches `targetStream`
	// into the given command buffer. Writes pre-pass barriers (intra-stream +
	// cross-stream acquires), the pass body, and post-pass release barriers.
	void RecordStream(std::shared_ptr<VWrap::CommandBuffer> cmd,
	                  uint32_t frameIndex, GPUProfiler* profiler,
	                  QueueAffinity targetStream);

	void BuildGraphicsPipeline(GraphicsPassBuilder& pass);
	void BuildComputePipeline(ComputePassBuilder& pass);

	VkImageLayout DetermineColorFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	VkImageLayout DetermineResolveFinalLayout(size_t passOrderIndex, ImageHandle image) const;
	bool IsImageReadDownstream(size_t afterOrderIndex, ImageHandle image) const;
};
