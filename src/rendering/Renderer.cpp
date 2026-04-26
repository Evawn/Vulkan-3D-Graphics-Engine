#include "Renderer.h"
#include <spdlog/spdlog.h>

Renderer::Renderer(const RendererConfig& config)
	: m_config(config)
	, m_graph(config.device, config.allocator)
{
	// Wire async-compute queue + per-frame command buffers / semaphores into the
	// graph. If config.computeQueue shares the graphics family, ConfigureAsync
	// records that and treats async as unavailable — every AsyncCompute hint
	// will be silently demoted at Compile time.
	RenderGraphAsyncConfig acfg{};
	acfg.computeQueue        = config.computeQueue;
	acfg.computeCommandPool  = config.computeCommandPool;
	acfg.graphicsQueueFamily = config.graphicsQueueFamily;
	acfg.framesInFlight      = config.maxFramesInFlight;
	m_graph.ConfigureAsync(acfg);
}

RendererCaps Renderer::GetCaps() const {
	RendererCaps caps{};
	caps.swapchainFormat   = m_config.swapchainFormat;
	caps.depthFormat       = m_config.depthFormat;
	caps.msaaSamples       = m_config.msaaSamples;
	caps.maxFramesInFlight = m_config.maxFramesInFlight;
	return caps;
}

Renderer::AllocatedTargets Renderer::AllocateTargets(const RenderTargetDesc& desc, VkExtent2D extent) {
	AllocatedTargets out{};

	out.handles.color = m_graph.CreateImage("scene_color", {
		extent.width, extent.height, 1,
		desc.color.format, desc.color.samples });

	if (desc.color.needsResolve) {
		out.handles.resolve = m_graph.CreateImage("scene_resolve", {
			extent.width, extent.height, 1,
			desc.color.format, VK_SAMPLE_COUNT_1_BIT });
	}

	if (desc.hasDepth) {
		out.handles.depth = m_graph.CreateImage("scene_depth", {
			extent.width, extent.height, 1,
			desc.depthFormat, desc.depthSamples });
	}

	out.sceneOutput = desc.color.needsResolve ? out.handles.resolve : out.handles.color;
	return out;
}

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

	// Ask the technique what scene-image stack it needs, then allocate it.
	RenderTargetDesc desc = technique->DescribeTargets(GetCaps());
	auto allocated = AllocateTargets(desc, m_offscreenExtent);
	m_targets = allocated.handles;

	// Register technique passes against the allocated targets
	technique->RegisterPasses(m_graph, ctx, m_targets);

	// Register post-process passes; chain threads the technique's scene output
	// through any enabled effects and returns the final scene image.
	m_finalScene = m_postProcess.Register(m_graph, allocated.sceneOutput, m_offscreenExtent);

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

	// Post-compile hook: techniques run any one-shot work that needs the graph's
	// allocated resources (e.g. seeding a storage image). Descriptor writes are
	// already handled by BindingTable.
	technique->OnPostCompile(m_graph);
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
	(void)technique;
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

std::shared_ptr<VWrap::ImageView> Renderer::GetFinalSceneView() const {
	return m_graph.GetImageView(m_finalScene);
}
