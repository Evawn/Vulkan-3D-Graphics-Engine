#include "Renderer.h"
#include <spdlog/spdlog.h>

Renderer::Renderer(const RendererConfig& config)
	: m_config(config)
	, m_graph(config.device, config.allocator)
{}

void Renderer::Build(
	RenderTechnique* technique,
	const RenderContext& ctx,
	std::shared_ptr<VWrap::ImageView> swapchainView,
	VkExtent2D swapchainExtent,
	std::function<void(PassContext&)> presentRecordFn)
{
	m_graph.Clear();
	m_offscreenExtent = ctx.extent;

	// Ensure the post-process chain has up-to-date context (effects may be
	// registered by Application before Build() is first called).
	PostProcessContext ppCtx{};
	ppCtx.device = m_config.device;
	ppCtx.allocator = m_config.allocator;
	ppCtx.sceneFormat = m_config.swapchainFormat;
	ppCtx.maxFramesInFlight = m_config.maxFramesInFlight;
	ppCtx.camera = ctx.camera;
	ppCtx.lighting = &m_lighting;
	m_postProcess.SetContext(ppCtx);

	// Create scene images (MSAA color, depth, resolve)
	m_sceneColor = m_graph.CreateImage("scene_color", {
		m_offscreenExtent.width, m_offscreenExtent.height, 1,
		m_config.swapchainFormat, m_config.msaaSamples });
	m_sceneDepth = m_graph.CreateImage("scene_depth", {
		m_offscreenExtent.width, m_offscreenExtent.height, 1,
		m_config.depthFormat, m_config.msaaSamples });
	m_sceneResolve = m_graph.CreateImage("scene_resolve", {
		m_offscreenExtent.width, m_offscreenExtent.height, 1,
		m_config.swapchainFormat });

	// Register technique passes
	technique->RegisterPasses(m_graph, ctx, m_sceneColor, m_sceneDepth, m_sceneResolve);

	// Register post-process passes; chain threads sceneResolve through any
	// enabled effects and returns the final scene image.
	m_finalScene = m_postProcess.Register(m_graph, m_sceneResolve, m_offscreenExtent);

	// Import swapchain image (updated per-frame)
	m_swapchain = m_graph.ImportImage("swapchain",
		swapchainView, m_config.swapchainFormat,
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, swapchainExtent);

	// UI/presentation pass: renders to swapchain, reads post-processed scene
	m_graph.AddGraphicsPass("UI")
		.SetColorAttachment(m_swapchain, LoadOp::Clear, StoreOp::Store,
			0.059f, 0.059f, 0.059f, 1.0f)
		.Read(m_finalScene)
		.SetRecord(std::move(presentRecordFn));

	m_graph.Compile();

	// Post-compile: techniques and effects write descriptors for graph-allocated images
	technique->WriteGraphDescriptors(m_graph);
	m_postProcess.WriteGraphDescriptors(m_graph);
}

void Renderer::Execute(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex,
                       GPUProfiler* profiler) {
	m_graph.Execute(cmd, frameIndex, profiler);
}

void Renderer::UpdateSwapchainView(std::shared_ptr<VWrap::ImageView> view) {
	m_graph.UpdateImport(m_swapchain, view);
}

void Renderer::OnSwapchainResize(
	RenderTechnique* technique,
	const RenderContext& ctx,
	std::shared_ptr<VWrap::ImageView> swapchainView,
	VkExtent2D swapchainExtent,
	std::function<void(PassContext&)> presentRecordFn)
{
	Build(technique, ctx, swapchainView, swapchainExtent, std::move(presentRecordFn));
}

void Renderer::OnViewportResize(VkExtent2D newExtent, RenderTechnique* technique) {
	m_offscreenExtent = newExtent;
	m_graph.Resize(newExtent);
	technique->WriteGraphDescriptors(m_graph);
	technique->OnResize(newExtent, m_graph);
	m_postProcess.OnResize(newExtent, m_graph);
}

void Renderer::Rebuild(
	RenderTechnique* technique,
	const RenderContext& ctx,
	VWrap::FrameController& fc,
	std::function<void(PassContext&)> presentRecordFn)
{
	auto swapchainView = fc.GetImageViews()[0];
	auto swapchainExtent = fc.GetSwapchain()->GetExtent();
	Build(technique, ctx, swapchainView, swapchainExtent, std::move(presentRecordFn));
}

std::shared_ptr<VWrap::ImageView> Renderer::GetSceneResolveView() const {
	return m_graph.GetImageView(m_sceneResolve);
}

std::shared_ptr<VWrap::ImageView> Renderer::GetFinalSceneView() const {
	return m_graph.GetImageView(m_finalScene);
}
