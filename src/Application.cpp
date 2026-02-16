#include "Application.h"
#include "ShaderCompiler.h"
#include "Log.h"
#include "UIStyle.h"
#include "ScreenshotCapture.h"

void Application::Run() {
	Init();
	MainLoop();
	Cleanup();
}

void Application::Init() {
	InitWindow();
	InitVulkan();
	InitImGui();

	VkExtent2D extent = m_vk.frameController->GetSwapchain()->GetExtent();
	m_offscreen_extent = extent;

	// Create render graph
	m_render_graph = RenderGraph(m_vk.device, m_vk.allocator);
	m_scene_sampler = VWrap::Sampler::Create(m_vk.device);

	spdlog::get("App")->debug("Creating camera...");
	m_camera = Camera::Create(45, ((float)extent.width / (float)extent.height), 0.1f, 10.0f);

	spdlog::get("App")->debug("Initializing renderers...");
	m_renderers.push_back(std::make_unique<DDATracer>());
	m_renderers.push_back(std::make_unique<ComputeTest>());
	m_renderers.push_back(std::make_unique<MeshRasterizer>());
	m_active_renderer_index = 0;

	BuildRenderGraph();

	spdlog::get("App")->debug("Creating GPU profiler...");
	m_gpu_profiler = GPUProfiler::Create(m_vk.device, MAX_FRAMES_IN_FLIGHT);
	spdlog::get("App")->debug("GPU profiler created");

	// Camera controller owns input polling and camera movement
	Input::Init(m_glfw_window.get()[0]);
	m_camera_controller = CameraController::Create(m_camera);
	m_camera_controller->SetReloadCallback([this]() { HotReloadShaders(); });

	InitPanels();
}

void Application::InitPanels() {
	// Viewport panel
	m_viewport_panel.SetTextureID(m_scene_texture);
	m_gui_renderer->RegisterPanel("Viewport", [this]() { m_viewport_panel.Draw(); });

	// Metrics panel
	m_metrics_panel.SetRenderers(&m_renderers, &m_active_renderer_index);
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(m_vk.physicalDevice->Get(), &memProps);
	m_metrics_panel.SetAllocator(m_vk.allocator->Get(), memProps.memoryHeapCount);
	m_metrics_panel.SetWireframeCallback([this]() {
		vkDeviceWaitIdle(m_vk.device->Get());
		auto ctx = BuildRenderContext();
		m_renderers[m_active_renderer_index]->RecreatePipeline(ctx);
	});
	m_gui_renderer->RegisterPanel("Metrics", [this]() { m_metrics_panel.Draw(); });

	// Output panel
	m_output_panel.SetSink(Log::GetImGuiSink());
	m_gui_renderer->RegisterPanel("Output", [this]() { m_output_panel.Draw(); });

	// Inspector panel
	m_inspector_panel.SetRenderers(&m_renderers, &m_active_renderer_index);
	m_inspector_panel.SetReloadCallback([this]() { m_pending_hot_reload = true; });
	m_inspector_panel.SetSwitchCallback([this](size_t idx) { m_pending_renderer_switch = idx; });
	m_inspector_panel.SetCamera(m_camera);
	m_inspector_panel.SetAppControls(m_camera_controller->SensitivityPtr(), m_camera_controller->SpeedPtr());
	m_inspector_panel.SetScreenshotCallback([this]() { CaptureScreenshot(); });
	m_gui_renderer->RegisterPanel("Inspector", [this]() { m_inspector_panel.Draw(); });
}

void Application::InitWindow() {
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	// Size window to 80% of the screen, centered
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);
	int win_w = static_cast<int>(mode->width * 0.8f);
	int win_h = static_cast<int>(mode->height * 0.8f);

	m_glfw_window = std::make_shared<GLFWwindow*>(glfwCreateWindow(win_w, win_h, "Vulkan", nullptr, nullptr));

	glfwSetWindowUserPointer(m_glfw_window.get()[0], this);
	glfwSetFramebufferSizeCallback(m_glfw_window.get()[0], glfw_FramebufferResizeCallback);
	glfwSetWindowFocusCallback(m_glfw_window.get()[0], glfw_WindowFocusCallback);

	glfwSetWindowPos(m_glfw_window.get()[0], (mode->width - win_w) / 2, (mode->height - win_h) / 2);
}

void Application::glfw_FramebufferResizeCallback(GLFWwindow* window, int width, int height) {
	auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
	app->m_vk.frameController->SetResized(true);
}

void Application::glfw_WindowFocusCallback(GLFWwindow* window, int focused) {
}

void Application::InitVulkan() {
	m_vk.instance = VWrap::Instance::Create(ENABLE_VALIDATION_LAYERS);
	m_vk.surface = VWrap::Surface::Create(m_vk.instance, m_glfw_window);
	m_vk.physicalDevice = VWrap::PhysicalDevice::Pick(m_vk.instance, m_vk.surface);
	m_vk.device = VWrap::Device::Create(m_vk.physicalDevice, ENABLE_VALIDATION_LAYERS);
	m_vk.allocator = VWrap::Allocator::Create(m_vk.instance, m_vk.physicalDevice, m_vk.device);

	VWrap::QueueFamilyIndices indices = m_vk.physicalDevice->FindQueueFamilies();

	m_vk.graphicsQueue = VWrap::Queue::Create(m_vk.device, indices.graphicsFamily.value());
	m_vk.presentQueue = VWrap::Queue::Create(m_vk.device, indices.presentFamily.value());
	m_vk.transferQueue = VWrap::Queue::Create(m_vk.device, indices.transferFamily.value());
	m_vk.computeQueue = VWrap::Queue::Create(m_vk.device, indices.computeFamily.value());
	m_vk.graphicsCommandPool = VWrap::CommandPool::Create(m_vk.device, m_vk.graphicsQueue);
	m_vk.transferCommandPool = VWrap::CommandPool::Create(m_vk.device, m_vk.transferQueue);
	m_vk.computeCommandPool = VWrap::CommandPool::Create(m_vk.device, m_vk.computeQueue);

	m_vk.frameController = VWrap::FrameController::Create(m_vk.device, m_vk.surface, m_vk.graphicsCommandPool, m_vk.presentQueue, MAX_FRAMES_IN_FLIGHT);
	m_vk.frameController->SetResizeCallback([this]() { Resize(); });

	m_vk.msaaSamples = m_vk.physicalDevice->GetMaxUsableSampleCount();

	VkFormat swapchainFormat = m_vk.frameController->GetSwapchain()->GetFormat();

	// Keep presentation render pass for ImGui init compatibility
	m_presentation_render_pass = VWrap::RenderPass::CreatePresentation(m_vk.device, swapchainFormat);
}

void Application::InitImGui() {
	m_gui_renderer = GUIRenderer::Create(m_vk.device);

	VWrap::QueueFamilyIndices indices = m_vk.physicalDevice->FindQueueFamilies();
	ImGui_ImplGlfw_InitForVulkan(m_glfw_window.get()[0], true);

	ImGui_ImplVulkan_InitInfo init_info{};
	init_info.Instance = m_vk.instance->Get();
	init_info.PhysicalDevice = m_vk.physicalDevice->Get();
	init_info.Device = m_vk.device->Get();
	init_info.QueueFamily = indices.graphicsFamily.value();
	init_info.Queue = m_vk.graphicsQueue->Get();
	init_info.PipelineCache = VK_NULL_HANDLE;
	init_info.DescriptorPool = m_gui_renderer->GetDescriptorPool()->Get();
	init_info.Subpass = 0;
	init_info.MinImageCount = m_vk.frameController->GetSwapchain()->Size();
	init_info.ImageCount = m_vk.frameController->GetSwapchain()->Size();
	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init_info.Allocator = VK_NULL_HANDLE;
	init_info.CheckVkResultFn = check_vk_result;
	ImGui_ImplVulkan_Init(&init_info, m_presentation_render_pass->Get());

	// Load font at correct DPI-scaled size (fixes fuzzy text)
	float dpi_scale;
	glfwGetWindowContentScale(m_glfw_window.get()[0], &dpi_scale, nullptr);
	m_gui_renderer->LoadFonts(dpi_scale);
	ImGui_ImplVulkan_DestroyFontsTexture();
	ImGui_ImplVulkan_CreateFontsTexture();

	UIStyle::Apply(dpi_scale);
}

void Application::BuildRenderGraph() {
	m_render_graph.Clear();

	VkFormat format = m_vk.frameController->GetSwapchain()->GetFormat();

	// Scene images (MSAA color, depth, resolve)
	m_scene_color = m_render_graph.CreateImage("scene_color", {
		m_offscreen_extent.width, m_offscreen_extent.height, 1,
		format, m_vk.msaaSamples });
	m_scene_depth = m_render_graph.CreateImage("scene_depth", {
		m_offscreen_extent.width, m_offscreen_extent.height, 1,
		VWrap::FindDepthFormat(m_vk.physicalDevice->Get()),
		m_vk.msaaSamples });
	m_scene_resolve = m_render_graph.CreateImage("scene_resolve", {
		m_offscreen_extent.width, m_offscreen_extent.height, 1,
		format });

	// Register technique passes
	auto ctx = BuildRenderContext();
	m_renderers[m_active_renderer_index]->RegisterPasses(
		m_render_graph, ctx, m_scene_color, m_scene_depth, m_scene_resolve);

	// Import swapchain image (updated per-frame)
	m_swapchain = m_render_graph.ImportImage("swapchain",
		m_vk.frameController->GetImageViews()[0],
		format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		m_vk.frameController->GetSwapchain()->GetExtent());

	// UI pass: renders ImGui to swapchain
	m_render_graph.AddGraphicsPass("UI")
		.SetColorAttachment(m_swapchain, LoadOp::Clear, StoreOp::Store,
			0.059f, 0.059f, 0.059f, 1.0f)
		.Read(m_scene_resolve)
		.SetRecord([this](PassContext& ctx) {
			m_gui_renderer->CmdDraw(ctx.cmd);
		});

	m_render_graph.Compile();

	// Post-compile: techniques write descriptors for graph-allocated images
	m_renderers[m_active_renderer_index]->WriteGraphDescriptors(m_render_graph);

	RegisterSceneTexture();
}

void Application::MainLoop() {
	auto last_time = std::chrono::high_resolution_clock::now();

	while (!glfwWindowShouldClose(m_glfw_window.get()[0])) {
		auto current_time = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_time).count();
		last_time = current_time;

		m_camera_controller->Update(dt);

		if (m_pending_hot_reload) {
			m_pending_hot_reload = false;
			HotReloadShaders();
		}

		if (m_pending_renderer_switch.has_value()) {
			SwitchRenderer(m_pending_renderer_switch.value());
			m_pending_renderer_switch.reset();
		}

		// Click to capture viewport
		if (!m_camera_controller->IsFocused() && m_viewport_panel.WasClicked()) {
			m_camera_controller->SetFocused(true);
		}

		if (m_camera_controller->IsFocused()) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		} else {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
		}

		m_viewport_panel.SetTextureID(m_scene_texture);

		m_gui_renderer->BeginFrame();
		DrawFrame();

		m_metrics_panel.Update(m_last_metrics.fps, m_last_metrics.render_time, dt * 1000.0f);
	}
	vkDeviceWaitIdle(m_vk.device->Get());
}

void Application::Cleanup() {
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(m_glfw_window.get()[0]);
	glfwTerminate();
}

void Application::DrawFrame() {

	// Handle viewport resize from panel
	if (m_viewport_panel.WasResized()) {
		VkExtent2D desired = m_viewport_panel.GetDesiredExtent();
		if (desired.width > 0 && desired.height > 0) {
			vkDeviceWaitIdle(m_vk.device->Get());
			if (m_scene_texture != VK_NULL_HANDLE) {
				ImGui_ImplVulkan_RemoveTexture(m_scene_texture);
				m_scene_texture = VK_NULL_HANDLE;
			}
			m_offscreen_extent = desired;
			m_render_graph.Resize(desired);
			m_renderers[m_active_renderer_index]->WriteGraphDescriptors(m_render_graph);
			m_renderers[m_active_renderer_index]->OnResize(desired, m_render_graph);
			m_camera->SetAspect((float)desired.width / (float)desired.height);
			RegisterSceneTexture();
			m_viewport_panel.SetTextureID(m_scene_texture);
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
	m_render_graph.UpdateImport(m_swapchain, m_vk.frameController->GetImageViews()[image_index]);

	command_buffer->Begin();

	m_gpu_profiler->CmdBegin(command_buffer, frame_index);
	m_render_graph.Execute(command_buffer, frame_index);
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

	vkDeviceWaitIdle(m_vk.device->Get());

	auto ctx = BuildRenderContext();
	renderer->RecreatePipeline(ctx);
	logger->info("Pipeline recreated successfully");
}

void Application::SwitchRenderer(size_t index) {
	if (index >= m_renderers.size() || index == m_active_renderer_index) return;

	auto logger = spdlog::get("App");
	logger->info("Switching renderer to: {}", m_renderers[index]->GetName());

	vkDeviceWaitIdle(m_vk.device->Get());

	if (m_scene_texture != VK_NULL_HANDLE) {
		ImGui_ImplVulkan_RemoveTexture(m_scene_texture);
		m_scene_texture = VK_NULL_HANDLE;
	}

	m_active_renderer_index = index;
	BuildRenderGraph();
	m_viewport_panel.SetTextureID(m_scene_texture);

	logger->info("Switched to: {}", m_renderers[index]->GetName());
}

void Application::CaptureScreenshot() {
	auto resolveImage = m_render_graph.GetImage(m_scene_resolve);
	auto resolveDesc = m_render_graph.GetImageDesc(m_scene_resolve);
	VkFormat format = m_render_graph.GetImageFormat(m_scene_resolve);
	VkExtent2D extent = { resolveDesc.width, resolveDesc.height };

	auto path = ScreenshotCapture::Capture(
		m_vk.device, m_vk.allocator, m_vk.graphicsCommandPool,
		resolveImage, format, extent);
	if (!path.empty()) {
		m_inspector_panel.SetLastScreenshotPath(path);
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

void Application::RegisterSceneTexture() {
	auto resolveView = m_render_graph.GetImageView(m_scene_resolve);
	m_scene_texture = ImGui_ImplVulkan_AddTexture(
		m_scene_sampler->Get(),
		resolveView->Get(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
