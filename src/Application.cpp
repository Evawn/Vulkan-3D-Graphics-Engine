#include "Application.h"
#include "ShaderCompiler.h"
#include "ScreenshotCapture.h"

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
	m_offscreen_extent = extent;

	// Create renderer
	RendererConfig rendererConfig{};
	rendererConfig.device = m_vk.device;
	rendererConfig.allocator = m_vk.allocator;
	rendererConfig.msaaSamples = m_vk.msaaSamples;
	rendererConfig.swapchainFormat = m_vk.frameController->GetSwapchain()->GetFormat();
	rendererConfig.depthFormat = VWrap::FindDepthFormat(m_vk.physicalDevice->Get());
	rendererConfig.maxFramesInFlight = MAX_FRAMES_IN_FLIGHT;
	m_renderer = Renderer(rendererConfig);

	spdlog::get("App")->debug("Creating camera...");
	m_camera = Camera::Create(45, ((float)extent.width / (float)extent.height), 0.1f, 10.0f);

	spdlog::get("App")->debug("Initializing renderers...");
	m_renderers.push_back(std::make_unique<DDATracer>());
	m_renderers.push_back(std::make_unique<ComputeTest>());
	m_renderers.push_back(std::make_unique<MeshRasterizer>());
	m_renderers.push_back(std::make_unique<SVORenderer>());
	m_renderers.push_back(std::make_unique<SVOBackup>());
	m_active_renderer_index = 0;

	BuildRenderGraph();

	spdlog::get("App")->debug("Creating GPU profiler...");
	m_gpu_profiler = GPUProfiler::Create(m_vk.device, MAX_FRAMES_IN_FLIGHT);
	spdlog::get("App")->debug("GPU profiler created");

	// Camera controller owns input polling and camera movement
	Input::Init(m_window->GetRaw());
	m_camera_controller = CameraController::Create(m_camera);
	m_camera_controller->SetReloadCallback([this]() {
		PushEvent({AppEventType::HotReloadShaders});
	});

	// Initialize panels (needs camera, controller, renderers)
	m_editor.InitPanels(&m_renderers, &m_active_renderer_index, m_camera, m_camera_controller, m_vk);

	// Wire editor callbacks to event queue
	m_editor.SetReloadCallback([this]() {
		PushEvent({AppEventType::HotReloadShaders});
	});
	m_editor.SetSwitchCallback([this](size_t idx) {
		PushEvent({AppEventType::SwitchRenderer, idx});
	});
	m_editor.SetScreenshotCallback([this]() {
		PushEvent({AppEventType::CaptureScreenshot});
	});
	m_editor.SetWireframeCallback([this]() {
		vkDeviceWaitIdle(m_vk.device->Get());
		auto ctx = BuildRenderContext();
		m_renderers[m_active_renderer_index]->RecreatePipeline(ctx);
	});

	m_state = AppState::Running;
}

void Application::InitVulkan() {
	m_vk = VWrap::VulkanContext::Create(m_window->GetHandle(), ENABLE_VALIDATION_LAYERS, MAX_FRAMES_IN_FLIGHT);

	m_vk.frameController->SetResizeCallback([this]() { Resize(); });

	m_window->SetFramebufferResizeCallback([this]() {
		m_vk.frameController->SetResized(true);
	});
	m_window->SetContentScaleCallback([this](float scale) {
		PushEvent({AppEventType::DpiChanged, 0, scale});
	});

	VkFormat swapchainFormat = m_vk.frameController->GetSwapchain()->GetFormat();
	m_presentation_render_pass = VWrap::RenderPass::CreatePresentation(m_vk.device, swapchainFormat);
}

void Application::BuildRenderGraph() {
	m_renderer.Rebuild(
		m_renderers[m_active_renderer_index].get(),
		BuildRenderContext(),
		*m_vk.frameController,
		[this](PassContext& ctx) {
			m_editor.CmdDraw(ctx.cmd);
		});

	m_editor.RegisterSceneTexture(m_renderer.GetSceneResolveView());

	// Update profiler pass count and graph snapshot for dev tooling panel
	if (m_gpu_profiler)
		m_gpu_profiler->SetPassCount(static_cast<uint32_t>(m_renderer.GetGraph().GetPassCount()));
	m_graphSnapshot = m_renderer.GetGraph().BuildSnapshot();
	m_editor.SetGraphSnapshot(&m_graphSnapshot);
}

void Application::MainLoop() {
	auto last_time = std::chrono::high_resolution_clock::now();

	while (!m_window->ShouldClose()) {
		auto current_time = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_time).count();
		last_time = current_time;

		m_camera_controller->Update(dt);
		ProcessEvents();

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
		m_editor.UpdateMetrics(m_last_metrics.fps, m_last_metrics.render_time, dt * 1000.0f);
		m_editor.SetPerformanceMetrics(&m_last_metrics);
	}
	vkDeviceWaitIdle(m_vk.device->Get());
}

void Application::Cleanup() {
	m_state = AppState::ShuttingDown;
	m_editor.Shutdown();
	m_window->Destroy();
}

void Application::PushEvent(AppEvent event) {
	m_events.push_back(event);
}

void Application::ProcessEvents() {
	if (m_events.empty()) return;

	// GPU-mutating events require device idle
	bool needs_idle = false;
	for (const auto& e : m_events) {
		if (e.type == AppEventType::HotReloadShaders ||
			e.type == AppEventType::SwitchRenderer ||
			e.type == AppEventType::DpiChanged) {
			needs_idle = true;
			break;
		}
	}
	if (needs_idle) {
		vkDeviceWaitIdle(m_vk.device->Get());
	}

	for (const auto& event : m_events) {
		switch (event.type) {
			case AppEventType::HotReloadShaders:
				HotReloadShaders();
				break;
			case AppEventType::SwitchRenderer:
				SwitchRenderer(event.index);
				break;
			case AppEventType::DpiChanged:
				m_editor.OnDpiChanged(event.scale);
				break;
			case AppEventType::CaptureScreenshot:
				CaptureScreenshot();
				break;
		}
	}
	m_events.clear();

	// Check for deferred technique reloads (e.g., model file changed via File parameter)
	auto& technique = m_renderers[m_active_renderer_index];
	if (technique->NeedsReload()) {
		vkDeviceWaitIdle(m_vk.device->Get());
		technique->PerformReload(BuildRenderContext());
	}
}

void Application::DrawFrame() {

	// Handle viewport resize from panel
	if (m_editor.ViewportWasResized()) {
		VkExtent2D desired = m_editor.GetDesiredViewportExtent();
		if (desired.width > 0 && desired.height > 0) {
			vkDeviceWaitIdle(m_vk.device->Get());
			m_editor.RemoveSceneTexture();
			m_offscreen_extent = desired;
			m_renderer.OnViewportResize(desired, m_renderers[m_active_renderer_index].get());
			m_camera->SetAspect((float)desired.width / (float)desired.height);
			m_editor.RegisterSceneTexture(m_renderer.GetSceneResolveView());
		}
	}

	// ACQUIRE FRAME ------------------------------------------------
	m_vk.frameController->AcquireNext();
	uint32_t image_index = m_vk.frameController->GetImageIndex();
	uint32_t frame_index = m_vk.frameController->GetCurrentFrame();
	auto command_buffer = m_vk.frameController->GetCurrentCommandBuffer();

	// Read metrics from previous cycle (fence wait in AcquireNext guarantees GPU work done)
	m_last_metrics = m_gpu_profiler->GetMetrics(frame_index);

	// Update swapchain import for current image
	m_renderer.UpdateSwapchainView(m_vk.frameController->GetImageViews()[image_index]);

	command_buffer->Begin();

	m_gpu_profiler->CmdBegin(command_buffer, frame_index);
	m_renderer.Execute(command_buffer, frame_index, m_gpu_profiler.get());
	m_gpu_profiler->CmdEnd(command_buffer, frame_index);

	if (vkEndCommandBuffer(command_buffer->Get()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to end command buffer recording!");
	}

	// PRESENT ------------------------------------------------
	m_vk.frameController->Render();
}

void Application::Resize() {
	// Swapchain resized — rebuild the graph so the UI pass gets new swapchain framebuffers.
	// Scene offscreen resources keep their current m_offscreen_extent.
	BuildRenderGraph();
}

void Application::HotReloadShaders() {
	auto logger = spdlog::get("Render");
	logger->info("Hot-reloading shaders...");

	auto& renderer = m_renderers[m_active_renderer_index];
	auto spvPaths = renderer->GetShaderPaths();

	auto results = ShaderCompiler::CompileAll(spvPaths);

	bool allSuccess = true;
	for (const auto& r : results) {
		if (!r.success) {
			allSuccess = false;
			break;
		}
	}

	if (!allSuccess) {
		logger->warn("Shader compilation failed - keeping old pipeline");
		return;
	}

	auto ctx = BuildRenderContext();
	renderer->RecreatePipeline(ctx);
	logger->info("Pipeline recreated successfully");
}

void Application::SwitchRenderer(size_t index) {
	if (index >= m_renderers.size() || index == m_active_renderer_index) return;

	auto logger = spdlog::get("App");
	logger->info("Switching renderer to: {}", m_renderers[index]->GetName());

	m_editor.RemoveSceneTexture();
	m_active_renderer_index = index;
	BuildRenderGraph();

	logger->info("Switched to: {}", m_renderers[index]->GetName());
}

void Application::CaptureScreenshot() {
	auto& graph = m_renderer.GetGraph();
	auto sceneResolve = m_renderer.GetSceneResolve();
	auto resolveImage = graph.GetImage(sceneResolve);
	auto resolveDesc = graph.GetImageDesc(sceneResolve);
	VkFormat format = graph.GetImageFormat(sceneResolve);
	VkExtent2D extent = { resolveDesc.width, resolveDesc.height };

	auto path = ScreenshotCapture::Capture(
		m_vk.device, m_vk.allocator, m_vk.graphicsCommandPool,
		resolveImage, format, extent);
	if (!path.empty()) {
		m_editor.SetLastScreenshotPath(path);
	}
}

RenderContext Application::BuildRenderContext() const {
	RenderContext ctx{};
	ctx.device = m_vk.device;
	ctx.allocator = m_vk.allocator;
	ctx.graphicsPool = m_vk.graphicsCommandPool;
	ctx.computePool = m_vk.computeCommandPool;
	ctx.extent = m_offscreen_extent;
	ctx.maxFramesInFlight = MAX_FRAMES_IN_FLIGHT;
	ctx.camera = m_camera;
	return ctx;
}
