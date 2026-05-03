#include "Application.h"
#include "MeshRasterizer.h"
#include "BrickmapPaletteRenderer.h"
#include "AnimatedGeometryRenderer.h"
#include "InstancedVoxelTechnique.h"
#include "CombinedRenderer.h"
#include "GltfImportTechnique.h"
#include "Scene.h"
#include "post-process/BloomEffect.h"
#include "post-process/LensFlareEffect.h"

void Application::Run() {
	Init();
	MainLoop();
	Cleanup();
}

void Application::Init() {
	m_window = Window::Create("Vulkan");
	InitVulkan();

	// Editor (ImGui + panels)
	m_editor.InitImGui(m_vk, m_presentation_render_pass, *m_window);

	VkExtent2D extent = m_vk.frameController->GetSwapchain()->GetExtent();

	spdlog::get("App")->debug("Creating camera...");
	m_camera = Camera::Create(45, ((float)extent.width / (float)extent.height), 0.001f, 10000.0f);

	// Bring up the rendering system (owns Renderer + technique list + event queue + profiler)
	RenderingSystemConfig rsc{};
	rsc.vk                     = &m_vk;
	rsc.maxFramesInFlight      = MAX_FRAMES_IN_FLIGHT;
	rsc.camera                 = m_camera;
	rsc.initialOffscreenExtent = extent;
	m_rendering.Init(rsc);

	// Register default post-process effects. Order matters: bloom first (operates
	// on the raw scene-with-sun), lens flare second (overlays ghosts on top of
	// the bloomed image so halos interact visually with the ghosts).
	m_rendering.AddPostProcessEffect(std::make_unique<BloomEffect>());
	m_rendering.AddPostProcessEffect(std::make_unique<LensFlareEffect>());

	spdlog::get("App")->debug("Initializing renderers...");
	// First-registered technique becomes the active one at startup
	// (RenderingSystem::m_activeIndex defaults to 0).
	m_rendering.AddTechnique(std::make_unique<CombinedRenderer>());
	m_rendering.AddTechnique(std::make_unique<BrickmapPaletteRenderer>());
	m_rendering.AddTechnique(std::make_unique<AnimatedGeometryRenderer>());
	m_rendering.AddTechnique(std::make_unique<InstancedVoxelTechnique>());
	m_rendering.AddTechnique(std::make_unique<MeshRasterizer>());
	// Workspace-locked technique for the Import & Bake workspace. Coexists
	// with the Scene techniques — Editor's workspace switcher (M2) routes
	// the user here when they enter ImportBake mode.
	auto gltfImport = std::make_unique<GltfImportTechnique>();
	GltfImportTechnique* gltfImportPtr = gltfImport.get();
	m_rendering.AddTechnique(std::move(gltfImport));

	// Wire UI record + before/after rebuild hooks BEFORE first build runs, so
	// the initial graph already has the ImGui draw callback wired in.
	m_rendering.SetUiRecord([this](PassContext& ctx) { m_editor.CmdDraw(ctx.cmd); });
	m_rendering.SetOnBeforeGraphRebuild([this] {
		m_editor.RemoveSceneTexture();
	});
	m_rendering.SetOnAfterGraphRebuild([this] {
		m_editor.RegisterSceneTexture(m_rendering.GetFinalSceneView());
		m_editor.SetGraphSnapshot(m_rendering.GetGraphSnapshot());
	});
	m_rendering.SetOnScreenshotSaved([this](const std::string& path) {
		m_editor.SetLastScreenshotPath(path);
	});
	m_rendering.SetOnRecordingSaved([this](const std::string& path) {
		m_editor.SetLastRecordingPath(path);
	});

	m_rendering.BuildInitialGraph();

	// Camera controller owns input polling and camera movement
	Input::Init(m_window->GetRaw());
	m_camera_controller = CameraController::Create(m_camera);
	m_camera_controller->SetReloadCallback([this]() {
		m_rendering.RequestReload();
	});
	m_camera_controller->SetToggleViewportOnlyCallback([this]() {
		auto* s = m_editor.GetState();
		s->viewport_only = !s->viewport_only;
	});
	m_camera_controller->SetToggleFullscreenCallback([this]() {
		bool fs = m_window->ToggleFullscreen();
		m_editor.GetState()->os_fullscreen = fs;
	});
	m_camera_controller->SetFocusChangedCallback([this](bool focused) {
		m_editor.GetState()->camera_focused = focused;
	});
	// Capture hotkeys ride the same input controller because it's already the
	// engine's main input context (F5 reload, F11 fullscreen, F viewport-only).
	// P toggles recording, F12 takes a screenshot — both fan out through the
	// rendering event queue to keep ordering with menu-driven invocations.
	m_camera_controller->SetToggleRecordingCallback([this] {
		m_rendering.RequestToggleRecording();
	});
	m_camera_controller->SetTakeScreenshotCallback([this] {
		m_rendering.RequestScreenshot();
	});

	// Inspector asset-name resolution for SceneNode component rows. Set before
	// InitPanels so the first GetParameters() call sees the registry.
	SceneNode::SetAssetRegistryForInspector(&m_rendering.GetAssets());

	// Initialize panels (needs camera, controller, technique list, scene)
	m_editor.InitPanels(&m_rendering.GetTechniques(),
	                    m_rendering.GetActiveTechniqueIndexPtr(),
	                    m_camera, m_camera_controller, m_vk,
	                    &m_rendering.GetScene());

	// Wire scene lighting + sky + post-process chain into the inspector so ImGui can edit them.
	m_editor.SetLighting(&m_rendering.GetScene().GetLighting());
	m_editor.SetSky     (&m_rendering.GetScene().GetSky());
	m_editor.SetPostProcess(&m_rendering.GetPostProcess());

	// Editor-issued requests fan out into the rendering event queue.
	m_editor.SetReloadCallback    ([this] { m_rendering.RequestReload(); });
	m_editor.SetSwitchCallback    ([this](size_t idx) { m_rendering.RequestSwitchTechnique(idx); });
	m_editor.SetScreenshotCallback([this] { m_rendering.RequestScreenshot(); });
	m_editor.SetToggleRecordingCallback([this] { m_rendering.RequestToggleRecording(); });

	// Editor reads capture state directly (live status for the indicator + UI footer).
	m_editor.SetCaptureSystem(&m_rendering.GetCapture());

	// Workspace plumbing: editor needs to drive the import technique through
	// the BakerPanel + ask the rendering system to switch techniques by name
	// when the workspace changes.
	m_editor.SetGltfImportTechnique(gltfImportPtr);
	m_editor.SetSwitchTechniqueByNameCallback([this](const std::string& name) {
		m_rendering.RequestSwitchTechniqueByName(name);
	});

	m_state = AppState::Running;
}

void Application::InitVulkan() {
	m_vk = VWrap::VulkanContext::Create(m_window->GetHandle(), ENABLE_VALIDATION_LAYERS, MAX_FRAMES_IN_FLIGHT);

	m_vk.frameController->SetResizeCallback([this]() { m_rendering.HandleSwapchainResize(); });

	m_window->SetFramebufferResizeCallback([this]() {
		m_vk.frameController->SetResized(true);
		// Preserve side/bottom panel pixel widths — let the viewport absorb the delta.
		m_editor.GetState()->layout_dirty = true;
	});
	m_window->SetContentScaleCallback([this](float scale) {
		// DPI changes are pure UI state — no graph rebuild, no device idle needed.
		m_editor.OnDpiChanged(scale);
	});

	VkFormat swapchainFormat = m_vk.frameController->GetSwapchain()->GetFormat();
	m_presentation_render_pass = VWrap::RenderPass::CreatePresentation(m_vk.device, swapchainFormat);
}

void Application::MainLoop() {
	auto last_time = std::chrono::high_resolution_clock::now();

	while (!m_window->ShouldClose()) {
		auto current_time = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_time).count();
		last_time = current_time;

		m_camera_controller->Update(dt);
		m_rendering.ProcessEvents();

		// Click to capture viewport
		if (!m_camera_controller->IsFocused() && m_editor.ViewportWasClicked()) {
			m_camera_controller->SetFocused(true);
		}

		if (m_camera_controller->IsFocused()) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		} else {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
		}

		m_editor.UpdateViewportTexture();
		m_editor.BeginFrame();
		DrawFrame();
		m_metrics_snapshot = m_rendering.GetLastMetrics();
		m_editor.UpdateMetrics(m_metrics_snapshot.fps, m_metrics_snapshot.render_time, dt * 1000.0f);
		m_editor.SetPerformanceMetrics(&m_metrics_snapshot);
	}
	vkDeviceWaitIdle(m_vk.device->Get());
}

void Application::Cleanup() {
	m_state = AppState::ShuttingDown;
	m_editor.Shutdown();
	m_window->Destroy();
}

void Application::DrawFrame() {

	// Handle viewport resize from panel.
	//
	// "Resized" now triggers on three things:
	//   1. The panel's content region changed (drag, layout swap, dpi).
	//   2. The user picked a new resolution mode in the status bar.
	//   3. The user picked a new target resolution in the dropdown.
	// All three route through ViewportPanel::m_was_resized; the editor's
	// status-bar widget calls MarkResized() for #2 and #3.
	//
	// GetEffectiveRenderExtent collapses (mode, target, panel) into the single
	// extent the renderer should produce. Camera aspect comes from a parallel
	// helper so Center mode letterboxes without squishing the scene.
	if (m_editor.ViewportWasResized()) {
		VkExtent2D desired = m_editor.GetEffectiveRenderExtent();
		if (desired.width > 0 && desired.height > 0) {
			m_rendering.HandleViewportResize(desired);
			m_camera->SetAspect(m_editor.GetEffectiveCameraAspect());
		}
	}
	// Mirror the renderer's live offscreen extent back into the editor so the
	// status-bar "Native" label and the viewport panel's Center-mode blit can
	// read it. Cheap (one VkExtent2D copy/frame); avoids leaking the renderer
	// pointer into the editor.
	m_editor.SetLiveOffscreenExtent(m_rendering.GetOffscreenExtent());

	// ACQUIRE FRAME ------------------------------------------------
	m_vk.frameController->AcquireNext();
	uint32_t image_index = m_vk.frameController->GetImageIndex();
	uint32_t frame_index = m_vk.frameController->GetCurrentFrame();
	auto command_buffer = m_vk.frameController->GetCurrentCommandBuffer();

	// Update swapchain import for current image
	m_rendering.UpdateSwapchainView(m_vk.frameController->GetImageViews()[image_index]);

	command_buffer->Begin();
	m_rendering.DrawFrame(command_buffer, frame_index);
	if (vkEndCommandBuffer(command_buffer->Get()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to end command buffer recording!");
	}

	// PRESENT ------------------------------------------------
	// The render graph may have submitted async-compute work earlier in this
	// frame; if so, the graphics submit needs to wait on its completion
	// semaphore before reading anything that work produced. The wait list is
	// empty when no async work was submitted.
	const auto& gfxWait = m_rendering.GetRenderer().GetGraph().GetGraphicsQueueWait();
	m_vk.frameController->Render(gfxWait.semaphores, gfxWait.stages);
}
