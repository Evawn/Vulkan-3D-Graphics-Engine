#include "RenderingSystem.h"
#include "ShaderCompiler.h"
#include "ScreenshotCapture.h"
#include <spdlog/spdlog.h>

void RenderingSystem::Init(const RenderingSystemConfig& cfg) {
	m_cfg = cfg;
	m_offscreenExtent = cfg.initialOffscreenExtent;

	RendererConfig rc{};
	rc.device              = cfg.vk->device;
	rc.allocator           = cfg.vk->allocator;
	rc.msaaSamples         = cfg.vk->msaaSamples;
	rc.swapchainFormat     = cfg.vk->frameController->GetSwapchain()->GetFormat();
	rc.depthFormat         = VWrap::FindDepthFormat(cfg.vk->physicalDevice->Get());
	rc.maxFramesInFlight   = cfg.maxFramesInFlight;
	rc.computeQueue        = cfg.vk->computeQueue;
	rc.computeCommandPool  = cfg.vk->computeCommandPool;
	rc.graphicsQueueFamily = cfg.vk->graphicsQueue->GetQueueFamilyIndex();
	m_renderer = Renderer(rc);

	m_profiler = GPUProfiler::Create(cfg.vk->device, cfg.maxFramesInFlight);
}

void RenderingSystem::Shutdown() {
	// Render graph + profiler hold device-bound resources; ensure no pending GPU
	// work before destruction. Application calls vkDeviceWaitIdle in its own
	// shutdown, but we run before that on some paths.
	if (m_cfg.vk && m_cfg.vk->device) {
		vkDeviceWaitIdle(m_cfg.vk->device->Get());
	}
	m_techniques.clear();
	m_profiler.reset();
}

void RenderingSystem::AddTechnique(std::unique_ptr<RenderTechnique> tech) {
	tech->SetEventSink([this](AppEvent e) { PushEvent(e); });
	m_techniques.push_back(std::move(tech));
}

void RenderingSystem::AddPostProcessEffect(std::unique_ptr<PostProcessEffect> effect) {
	m_renderer.GetPostProcess().AddEffect(std::move(effect));
}

void RenderingSystem::SetUiRecord(std::function<void(PassContext&)> uiRecord) {
	m_uiRecord = std::move(uiRecord);
}

void RenderingSystem::BuildInitialGraph() {
	RebuildGraph();
}

RenderContext RenderingSystem::BuildRenderContext() const {
	RenderContext ctx{};
	ctx.device            = m_cfg.vk->device;
	ctx.allocator         = m_cfg.vk->allocator;
	ctx.graphicsPool      = m_cfg.vk->graphicsCommandPool;
	ctx.computePool       = m_cfg.vk->computeCommandPool;
	ctx.extent            = m_offscreenExtent;
	ctx.maxFramesInFlight = m_cfg.maxFramesInFlight;
	ctx.camera            = m_cfg.camera;
	// Lighting lives inside the Renderer; const_cast is incidental — see
	// Application::BuildRenderContext (the original site of this pattern).
	ctx.lighting          = const_cast<SceneLighting*>(&m_renderer.GetLighting());
	// Scene is owned here; const_cast follows the same pattern (RenderContext
	// is logically a non-owning bundle, and m_scene is mutable engine state).
	ctx.scene             = const_cast<RenderScene*>(&m_scene);
	return ctx;
}

void RenderingSystem::RebuildGraph() {
	if (m_onBeforeGraphRebuild) m_onBeforeGraphRebuild();

	// Capture m_uiRecord by value into a fresh std::function so Renderer can
	// std::move it into the UI pass without invalidating our stored copy on the
	// next rebuild.
	std::function<void(PassContext&)> uiCopy = m_uiRecord;
	m_renderer.Rebuild(
		m_techniques[m_activeIndex].get(),
		BuildRenderContext(),
		*m_cfg.vk->frameController,
		std::move(uiCopy));

	if (m_profiler) {
		m_profiler->SetPassCount(static_cast<uint32_t>(m_renderer.GetGraph().GetPassCount()));
	}
	m_graphSnapshot = m_renderer.GetGraph().BuildSnapshot();

	if (m_onAfterGraphRebuild) m_onAfterGraphRebuild();
}

void RenderingSystem::RequestReload()                  { PushEvent({AppEventType::HotReloadShaders}); }
void RenderingSystem::RequestSwitchTechnique(size_t i) { PushEvent({AppEventType::SwitchRenderer, i}); }
void RenderingSystem::RequestScreenshot()              { PushEvent({AppEventType::CaptureScreenshot}); }

void RenderingSystem::HandleSwapchainResize() {
	// Swapchain rebuild — graph needs full rebuild so the UI pass picks up new
	// swapchain framebuffers. Scene offscreen images stay at m_offscreenExtent.
	RebuildGraph();
}

void RenderingSystem::HandleViewportResize(VkExtent2D newExtent) {
	if (newExtent.width == 0 || newExtent.height == 0) return;
	vkDeviceWaitIdle(m_cfg.vk->device->Get());

	if (m_onBeforeGraphRebuild) m_onBeforeGraphRebuild();
	m_offscreenExtent = newExtent;
	m_renderer.OnViewportResize(newExtent, m_techniques[m_activeIndex].get());
	if (m_onAfterGraphRebuild) m_onAfterGraphRebuild();
}

static bool EventNeedsDeviceIdle(AppEventType type) {
	switch (type) {
		case AppEventType::HotReloadShaders:
		case AppEventType::SwitchRenderer:
		case AppEventType::ReloadTechnique:
		case AppEventType::RebuildGraph:
		case AppEventType::RecreatePipelines:
			return true;
		case AppEventType::CaptureScreenshot:
			return false;
	}
	return false;
}

void RenderingSystem::ProcessEvents() {
	// Drain in a loop: dispatched events (e.g. ReloadTechnique → RebuildGraph)
	// can post follow-ups, and we want them handled in the same tick.
	while (!m_events.empty()) {
		auto batch = std::move(m_events);
		m_events.clear();

		bool needs_idle = false;
		for (const auto& e : batch) {
			if (EventNeedsDeviceIdle(e.type)) { needs_idle = true; break; }
		}
		if (needs_idle) {
			vkDeviceWaitIdle(m_cfg.vk->device->Get());
		}

		for (const auto& event : batch) {
			DispatchEvent(event);
		}
	}
}

void RenderingSystem::DispatchEvent(const AppEvent& event) {
	switch (event.type) {
		case AppEventType::HotReloadShaders:
			HotReloadShaders();
			break;
		case AppEventType::SwitchRenderer:
			SwitchRenderer(event.index);
			break;
		case AppEventType::CaptureScreenshot:
			CaptureScreenshot();
			break;
		case AppEventType::ReloadTechnique:
			m_techniques[m_activeIndex]->Reload(BuildRenderContext());
			break;
		case AppEventType::RebuildGraph:
			RebuildGraph();
			break;
		case AppEventType::RecreatePipelines:
			m_renderer.GetGraph().RecreatePipelines();
			break;
	}
}

void RenderingSystem::HotReloadShaders() {
	auto logger = spdlog::get("Render");
	logger->info("Hot-reloading shaders...");

	auto& technique = m_techniques[m_activeIndex];
	auto spvPaths = technique->GetShaderPaths();
	auto ppPaths  = m_renderer.GetPostProcess().GetShaderPaths();
	spvPaths.insert(spvPaths.end(), ppPaths.begin(), ppPaths.end());

	auto results = ShaderCompiler::CompileAll(spvPaths);
	for (const auto& r : results) {
		if (!r.success) {
			logger->warn("Shader compilation failed - keeping old pipeline");
			return;
		}
	}

	m_renderer.GetGraph().RecreatePipelines();
	logger->info("Pipeline recreated successfully");
}

void RenderingSystem::SwitchRenderer(size_t index) {
	if (index >= m_techniques.size() || index == m_activeIndex) return;

	auto logger = spdlog::get("App");
	logger->info("Switching renderer to: {}", m_techniques[index]->GetDisplayName());
	m_activeIndex = index;
	RebuildGraph();
	logger->info("Switched to: {}", m_techniques[index]->GetDisplayName());
}

void RenderingSystem::CaptureScreenshot() {
	auto& graph = m_renderer.GetGraph();
	auto finalScene = m_renderer.GetFinalScene();
	auto resolveImage = graph.GetImage(finalScene);
	auto resolveDesc  = graph.GetImageDesc(finalScene);
	VkFormat format   = graph.GetImageFormat(finalScene);
	VkExtent2D extent = { resolveDesc.width, resolveDesc.height };

	auto path = ScreenshotCapture::Capture(
		m_cfg.vk->device, m_cfg.vk->allocator, m_cfg.vk->graphicsCommandPool,
		resolveImage, format, extent);
	if (!path.empty() && m_onScreenshotSaved) m_onScreenshotSaved(path);
}

void RenderingSystem::DrawFrame(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex) {
	// Per-frame scene rebuild. Clear once, then let every active technique drop
	// items in. The graph reads this through PassContext::scene during Execute.
	// Future scene-graph integration replaces the per-technique loop with a
	// single graph-traversal-emits-items call; nothing else here changes.
	m_scene.Clear();
	auto rctx = BuildRenderContext();
	for (auto& tech : m_techniques) {
		tech->EmitItems(m_scene, rctx);
	}
	m_renderer.GetGraph().SetScene(&m_scene);

	m_lastMetrics = m_profiler->GetMetrics(frameIndex);
	m_profiler->CmdBegin(cmd, frameIndex);
	m_renderer.Execute(cmd, frameIndex, m_profiler.get());
	m_profiler->CmdEnd(cmd, frameIndex);
}

void RenderingSystem::UpdateSwapchainView(std::shared_ptr<VWrap::ImageView> view) {
	m_renderer.UpdateSwapchainView(view);
}
