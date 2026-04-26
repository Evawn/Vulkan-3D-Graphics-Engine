#pragma once

#include "RenderGraph.h"
#include "RenderTechnique.h"
#include "post-process/PostProcessChain.h"
#include "FrameController.h"
#include "Sampler.h"
#include "GPUProfiler.h"
#include <functional>
#include <memory>

struct RendererConfig {
	std::shared_ptr<VWrap::Device> device;
	std::shared_ptr<VWrap::Allocator> allocator;
	VkSampleCountFlagBits msaaSamples;
	VkFormat swapchainFormat;
	VkFormat depthFormat;
	uint32_t maxFramesInFlight;

	// Optional async-compute plumbing. Set by RenderingSystem from VulkanContext.
	// When the compute queue's family differs from graphicsQueueFamily, the
	// graph honors AsyncCompute affinity hints; otherwise it silently demotes
	// them all to the graphics queue.
	std::shared_ptr<VWrap::Queue>       computeQueue;
	std::shared_ptr<VWrap::CommandPool> computeCommandPool;
	uint32_t                            graphicsQueueFamily = 0;
};

class Renderer {
public:
	Renderer() = default;
	Renderer(const RendererConfig& config);

	// Build the full render graph for a technique.
	// presentRecordFn: UI/ImGui drawing callback provided by Application.
	// beforeRegisterFn (optional): runs after graph.Clear() and before the
	//   technique's RegisterPasses — the AssetRegistry uses this slot to
	//   declare its persistent buffers/images so techniques can query handles.
	// afterCompileFn (optional): runs after graph.Compile() and before the
	//   technique's OnPostCompile — the AssetRegistry uses this slot to
	//   upload host data into the now-allocated device resources.
	void Build(
		RenderTechnique* technique,
		const RenderContext& ctx,
		std::shared_ptr<VWrap::ImageView> swapchainView,
		VkExtent2D swapchainExtent,
		std::function<void(PassContext&)> presentRecordFn,
		std::function<void(RenderGraph&)> beforeRegisterFn = nullptr,
		std::function<void(RenderGraph&)> afterCompileFn   = nullptr);

	// Execute the compiled graph for one frame.
	void Execute(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex,
	             GPUProfiler* profiler = nullptr);

	// Update the swapchain image for the current frame (called per-frame before Execute).
	void UpdateSwapchainView(std::shared_ptr<VWrap::ImageView> view);

	// Handle swapchain resize (requires full rebuild).
	void OnSwapchainResize(
		RenderTechnique* technique,
		const RenderContext& ctx,
		std::shared_ptr<VWrap::ImageView> swapchainView,
		VkExtent2D swapchainExtent,
		std::function<void(PassContext&)> presentRecordFn);

	// Handle viewport/offscreen resize (scene images change, swapchain unchanged).
	void OnViewportResize(VkExtent2D newExtent, RenderTechnique* technique);

	// Convenience: fetches swapchain info from FrameController internally.
	void Rebuild(
		RenderTechnique* technique,
		const RenderContext& ctx,
		VWrap::FrameController& fc,
		std::function<void(PassContext&)> presentRecordFn,
		std::function<void(RenderGraph&)> beforeRegisterFn = nullptr,
		std::function<void(RenderGraph&)> afterCompileFn   = nullptr);

	// Access to graph resources (for screenshot capture, ImGui texture registration).
	RenderGraph& GetGraph() { return m_graph; }
	const RenderGraph& GetGraph() const { return m_graph; }

	// The final image handed to the UI pass — the post-processed scene if the
	// chain is non-empty, otherwise the technique's chosen scene-output handle
	// (resolve target if MSAA, color target otherwise). Use this for screenshot
	// capture and ImGui viewport registration so the user sees what the chain
	// actually produced.
	ImageHandle GetFinalScene() const { return m_finalScene; }
	std::shared_ptr<VWrap::ImageView> GetFinalSceneView() const;
	VkExtent2D GetOffscreenExtent() const { return m_offscreenExtent; }

	// Capabilities exposed to techniques so DescribeTargets() can pick formats /
	// sample counts that match the Renderer's swapchain & MSAA configuration.
	RendererCaps GetCaps() const;

	PostProcessChain& GetPostProcess() { return m_postProcess; }
	const PostProcessChain& GetPostProcess() const { return m_postProcess; }

private:
	// Allocate scene images (color/depth/resolve) according to a technique's
	// declared target needs, returning the handles the technique receives via
	// RegisterPasses. The "scene output" handle (resolve if MSAA, else color)
	// is also returned so Build() can thread it into the post-process chain.
	struct AllocatedTargets {
		TechniqueTargets handles;
		ImageHandle      sceneOutput;   // = handles.resolve if valid, else handles.color
	};
	AllocatedTargets AllocateTargets(const RenderTargetDesc& desc, VkExtent2D extent);

	RendererConfig m_config{};
	RenderGraph m_graph;

	TechniqueTargets m_targets{};   // shape allocated for the active technique this build
	ImageHandle m_finalScene;       // output of the post-process chain (or scene output if empty)
	ImageHandle m_swapchain;
	VkExtent2D m_offscreenExtent{};

	PostProcessChain m_postProcess;
};
