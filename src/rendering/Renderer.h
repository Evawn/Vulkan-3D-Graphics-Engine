#pragma once

#include "RenderGraph.h"
#include "RenderTechnique.h"
#include "SceneLighting.h"
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
};

class Renderer {
public:
	Renderer() = default;
	Renderer(const RendererConfig& config);

	// Build the full render graph for a technique.
	// presentRecordFn: UI/ImGui drawing callback provided by Application.
	void Build(
		RenderTechnique* technique,
		const RenderContext& ctx,
		std::shared_ptr<VWrap::ImageView> swapchainView,
		VkExtent2D swapchainExtent,
		std::function<void(PassContext&)> presentRecordFn);

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
		std::function<void(PassContext&)> presentRecordFn);

	// Direct access to the resolved scene image view.
	std::shared_ptr<VWrap::ImageView> GetSceneResolveView() const;

	// Access to graph resources (for screenshot capture, ImGui texture registration).
	RenderGraph& GetGraph() { return m_graph; }
	const RenderGraph& GetGraph() const { return m_graph; }

	ImageHandle GetSceneResolve() const { return m_sceneResolve; }
	// The final image handed to the UI pass — equals m_sceneResolve if the
	// post-process chain is empty/fully-disabled, otherwise the output of the
	// last enabled effect. Use this for screenshot capture and ImGui viewport
	// registration so the user sees the post-processed result.
	ImageHandle GetFinalScene() const { return m_finalScene; }
	std::shared_ptr<VWrap::ImageView> GetFinalSceneView() const;
	VkExtent2D GetOffscreenExtent() const { return m_offscreenExtent; }

	PostProcessChain& GetPostProcess() { return m_postProcess; }
	const PostProcessChain& GetPostProcess() const { return m_postProcess; }

	SceneLighting& GetLighting() { return m_lighting; }
	const SceneLighting& GetLighting() const { return m_lighting; }

private:
	RendererConfig m_config{};
	RenderGraph m_graph;

	ImageHandle m_sceneColor;
	ImageHandle m_sceneDepth;
	ImageHandle m_sceneResolve;
	ImageHandle m_finalScene;   // output of the post-process chain (or m_sceneResolve if empty)
	ImageHandle m_swapchain;
	VkExtent2D m_offscreenExtent{};

	SceneLighting m_lighting{};
	PostProcessChain m_postProcess;
};
