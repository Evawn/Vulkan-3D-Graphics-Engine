#pragma once

#include "Renderer.h"
#include "RenderTechnique.h"
#include "RenderScene.h"
#include "SceneLighting.h"
#include "Scene.h"
#include "SceneExtractor.h"
#include "AssetRegistry.h"
#include "AppEvent.h"
#include "VulkanContext.h"
#include "Camera.h"
#include "GPUProfiler.h"
#include "RenderGraphTypes.h"
#include "post-process/PostProcessEffect.h"

#include <functional>
#include <memory>
#include <vector>

// RenderingSystem sits between Application and Renderer. It owns:
//   - the Renderer
//   - the list of techniques + active selection
//   - the AppEvent queue (rendering-related events; DPI events stay in Application)
//   - the GPU profiler
//   - the graph snapshot used by the dev tooling panel
//
// Application is responsible only for the window, vulkan bootstrap, editor, and
// the main loop tick — it talks to RenderingSystem through Request* methods and
// the per-frame ProcessEvents() / DrawFrame() pair.

struct RenderingSystemConfig {
	VWrap::VulkanContext* vk = nullptr;          // non-owning; Application owns the context
	uint32_t              maxFramesInFlight = 1;
	// Camera handed to the Scene — shared_ptr because the input system / camera
	// controller in Application also retains a reference to drive movement.
	std::shared_ptr<Camera> camera;
	VkExtent2D            initialOffscreenExtent{};
};

class RenderingSystem {
public:
	void Init(const RenderingSystemConfig& cfg);
	void Shutdown();

	// Application registers the specific techniques and post-process effects it
	// wants — RenderingSystem doesn't hardcode the list. Each technique's event
	// sink is wired here so .vox reload / volume resize can post events without
	// touching Application directly.
	void AddTechnique(std::unique_ptr<RenderTechnique> tech);
	void AddPostProcessEffect(std::unique_ptr<PostProcessEffect> effect);

	// Set once after Init; stored and re-used on every graph rebuild so the UI
	// pass keeps recording ImGui draws after a switch / resize / hot-reload.
	void SetUiRecord(std::function<void(PassContext&)> uiRecord);

	// Called once after AddTechnique() calls so the system can build the initial
	// graph (technique[0] active by default). Separated from Init() so the
	// caller can wire callbacks (UI record, on-after-rebuild) before the first
	// build runs.
	void BuildInitialGraph();

	// Public requests posted to the queue and drained in ProcessEvents().
	void RequestReload();
	void RequestSwitchTechnique(size_t idx);
	void RequestScreenshot();

	// Bound to FrameController's swapchain-resize callback.
	void HandleSwapchainResize();
	// Bound to the editor's viewport-resize signal.
	void HandleViewportResize(VkExtent2D newExtent);

	// Drains the queue. Issues vkDeviceWaitIdle once for the batch if any event
	// requires it. Re-runs until the queue is empty (events can post follow-ups).
	void ProcessEvents();

	// Per-frame pass: executes the graph and runs the profiler. Caller owns
	// command-buffer lifecycle (Begin/End) so Application keeps the option to
	// inject other work around the render-graph execution.
	void DrawFrame(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex);
	void UpdateSwapchainView(std::shared_ptr<VWrap::ImageView> view);

	// ---- Editor wiring ----
	std::vector<std::unique_ptr<RenderTechnique>>& GetTechniques()        { return m_techniques; }
	size_t* GetActiveTechniqueIndexPtr()                                  { return &m_activeIndex; }
	SceneLighting&    GetLighting()                                       { return m_world.GetLighting(); }
	PostProcessChain& GetPostProcess()                                    { return m_renderer.GetPostProcess(); }
	const GraphSnapshot* GetGraphSnapshot() const                         { return &m_graphSnapshot; }
	GPUProfiler*      GetProfiler()                                       { return m_profiler.get(); }
	GPUProfiler::PerformanceMetrics GetLastMetrics() const                { return m_lastMetrics; }
	std::shared_ptr<VWrap::ImageView> GetFinalSceneView() const           { return m_renderer.GetFinalSceneView(); }
	Renderer&         GetRenderer()                                       { return m_renderer; }

	// ---- Scene module access (techniques + Application populate the scene) ----
	Scene&            GetScene()                                          { return m_world; }
	const Scene&      GetScene() const                                    { return m_world; }
	AssetRegistry&    GetAssets()                                         { return m_assets; }
	const AssetRegistry& GetAssets() const                                { return m_assets; }

	// Application registers callbacks here so the scene-texture binding (ImGui
	// viewport image) can be dropped/re-acquired around graph rebuilds. Setting
	// these is optional — they're invoked only if non-null.
	void SetOnBeforeGraphRebuild(std::function<void()> cb) { m_onBeforeGraphRebuild = std::move(cb); }
	void SetOnAfterGraphRebuild (std::function<void()> cb) { m_onAfterGraphRebuild  = std::move(cb); }
	// Optional: invoked with the saved screenshot path. Application uses this to
	// forward the path to the editor's "last screenshot" footer.
	void SetOnScreenshotSaved(std::function<void(const std::string&)> cb) { m_onScreenshotSaved = std::move(cb); }

	// Push from outside (e.g. technique event sinks set up by AddTechnique).
	void PushEvent(AppEvent e) { m_events.push_back(e); }

private:
	void DispatchEvent(const AppEvent& e);
	void RebuildGraph();
	void HotReloadShaders();
	void SwitchRenderer(size_t index);
	void CaptureScreenshot();
	RenderContext BuildRenderContext() const;

	RenderingSystemConfig m_cfg{};
	Renderer m_renderer;
	std::vector<std::unique_ptr<RenderTechnique>> m_techniques;
	size_t m_activeIndex = 0;
	std::vector<AppEvent> m_events;

	VkExtent2D m_offscreenExtent{};
	GraphSnapshot m_graphSnapshot{};
	std::shared_ptr<GPUProfiler> m_profiler;
	GPUProfiler::PerformanceMetrics m_lastMetrics{};

	std::function<void(PassContext&)> m_uiRecord;
	std::function<void()> m_onBeforeGraphRebuild;
	std::function<void()> m_onAfterGraphRebuild;
	std::function<void(const std::string&)> m_onScreenshotSaved;

	// Frame-local item list. Cleared each DrawFrame, refilled by SceneExtractor
	// from m_world. Plumbed into RenderContext (RegisterPasses-time) and into
	// PassContext (record-time) so passes consume what extraction emits.
	RenderScene m_scene;

	// World state: source-of-truth scene tree, owned here. Techniques and the
	// application populate it during Init / on file picks; the extractor walks
	// it every frame to fill m_scene. Lighting + sky now live on the Scene
	// (RenderingSystem::GetLighting() forwards to m_world.GetLighting()).
	Scene m_world;

	// Engine-wide asset storage. Techniques register their assets here; the
	// registry declares persistent graph resources before each technique
	// RegisterPasses, and uploads host data after Compile.
	AssetRegistry m_assets;

	// Per-frame producer that consumes m_world + m_assets and writes m_scene.
	SceneExtractor m_extractor;
};
