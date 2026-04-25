#include "Application.h"
#include "MeshRasterizer.h"
#include "BrickmapPaletteRenderer.h"
#include "AnimatedGeometryRenderer.h"
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
	m_camera = Camera::Create(45, ((float)extent.width / (float)extent.height), 0.1f, 10.0f);

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
	m_rendering.AddTechnique(std::make_unique<BrickmapPaletteRenderer>());
	m_rendering.AddTechnique(std::make_unique<AnimatedGeometryRenderer>());
	m_rendering.AddTechnique(std::make_unique<MeshRasterizer>());

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

	// Initialize panels (needs camera, controller, technique list)
	m_editor.InitPanels(&m_rendering.GetTechniques(),
	                    m_rendering.GetActiveTechniqueIndexPtr(),
	                    m_camera, m_camera_controller, m_vk);

	// Wire scene lighting + post-process chain into the inspector so ImGui can edit them.
	m_editor.SetLighting(&m_rendering.GetLighting());
	m_editor.SetPostProcess(&m_rendering.GetPostProcess());

	// Editor-issued requests fan out into the rendering event queue.
	m_editor.SetReloadCallback    ([this] { m_rendering.RequestReload(); });
	m_editor.SetSwitchCallback    ([this](size_t idx) { m_rendering.RequestSwitchTechnique(idx); });
	m_editor.SetScreenshotCallback([this] { m_rendering.RequestScreenshot(); });

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

	// Handle viewport resize from panel
	if (m_editor.ViewportWasResized()) {
		VkExtent2D desired = m_editor.GetDesiredViewportExtent();
		if (desired.width > 0 && desired.height > 0) {
			m_rendering.HandleViewportResize(desired);
			m_camera->SetAspect((float)desired.width / (float)desired.height);
		}
	}

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
	m_vk.frameController->Render();
}
