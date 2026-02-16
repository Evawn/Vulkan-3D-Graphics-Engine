#include "Application.h"
#include "ShaderCompiler.h"
#include "Log.h"
#include "UIStyle.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <ctime>
#include <filesystem>

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

	// Create offscreen target for scene rendering
	m_offscreen_target = OffscreenTarget::Create(
		m_vk.device, m_vk.allocator, m_scene_render_pass,
		extent, m_vk.msaaSamples,
		m_vk.frameController->GetSwapchain()->GetFormat());

	spdlog::get("App")->debug("Building render context...");
	RenderContext render_ctx{};
	render_ctx.device = m_vk.device;
	render_ctx.allocator = m_vk.allocator;
	render_ctx.graphicsPool = m_vk.graphicsCommandPool;
	render_ctx.renderPass = m_scene_render_pass;
	render_ctx.extent = extent;
	render_ctx.maxFramesInFlight = MAX_FRAMES_IN_FLIGHT;

	spdlog::get("App")->debug("Initializing OctreeTracer...");
	m_renderers.push_back(std::make_unique<OctreeTracer>());
	m_renderers[0]->Init(render_ctx);
	m_active_renderer_index = 0;
	spdlog::get("App")->debug("OctreeTracer initialized");

	spdlog::get("App")->debug("Creating GPU profiler...");
	m_gpu_profiler = GPUProfiler::Create(m_vk.device, MAX_FRAMES_IN_FLIGHT);
	spdlog::get("App")->debug("GPU profiler created");

	m_camera = Camera::Create(45, ((float)extent.width / (float)extent.height), 0.1f, 10.0f);

	InitPanels();

	Input::Init(m_glfw_window.get()[0]);
	Input::AddContext(m_main_context);
}

void Application::InitPanels() {
	// Viewport panel
	m_viewport_panel.SetTextureID(m_offscreen_target->GetImGuiTextureID());
	m_gui_renderer->RegisterPanel("Viewport", [this]() { m_viewport_panel.Draw(); });

	// Metrics panel
	m_metrics_panel.SetRenderers(&m_renderers, &m_active_renderer_index);
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(m_vk.physicalDevice->Get(), &memProps);
	m_metrics_panel.SetAllocator(m_vk.allocator->Get(), memProps.memoryHeapCount);
	m_metrics_panel.SetWireframeCallback([this]() {
		vkDeviceWaitIdle(m_vk.device->Get());
		RenderContext ctx{};
		ctx.device = m_vk.device;
		ctx.allocator = m_vk.allocator;
		ctx.graphicsPool = m_vk.graphicsCommandPool;
		ctx.renderPass = m_scene_render_pass;
		ctx.extent = m_offscreen_target->GetExtent();
		ctx.maxFramesInFlight = MAX_FRAMES_IN_FLIGHT;
		m_renderers[m_active_renderer_index]->RecreatePipeline(ctx);
	});
	m_gui_renderer->RegisterPanel("Metrics", [this]() { m_metrics_panel.Draw(); });

	// Output panel
	m_output_panel.SetSink(Log::GetImGuiSink());
	m_gui_renderer->RegisterPanel("Output", [this]() { m_output_panel.Draw(); });

	// Inspector panel
	m_inspector_panel.SetRenderers(&m_renderers, &m_active_renderer_index);
	m_inspector_panel.SetReloadCallback([this]() { HotReloadShaders(); });
	m_inspector_panel.SetSwitchCallback([this](size_t idx) { SwitchRenderer(idx); });
	m_inspector_panel.SetCamera(m_camera);
	m_inspector_panel.SetAppControls(&m_app_state.sensitivity, &m_app_state.speed);
	m_inspector_panel.SetScreenshotCallback([this]() { CaptureScreenshot(); });
	m_gui_renderer->RegisterPanel("Inspector", [this]() { m_inspector_panel.Draw(); });
}

void Application::InitWindow() {
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	float dpi_scale;
	glfwGetMonitorContentScale(glfwGetPrimaryMonitor(), &dpi_scale, nullptr);
	m_glfw_window = std::make_shared<GLFWwindow*>(glfwCreateWindow(WIDTH * dpi_scale, HEIGHT * dpi_scale, "Vulkan", nullptr, nullptr));

	glfwSetWindowUserPointer(m_glfw_window.get()[0], this);
	glfwSetFramebufferSizeCallback(m_glfw_window.get()[0], glfw_FramebufferResizeCallback);
	glfwSetWindowFocusCallback(m_glfw_window.get()[0], glfw_WindowFocusCallback);

	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);
	int screen_width = mode->width;
	int screen_height = mode->height;

	glfwSetWindowPos(m_glfw_window.get()[0], (screen_width-WIDTH) / 2, (screen_height - HEIGHT) / 2);
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
	m_vk.graphicsCommandPool = VWrap::CommandPool::Create(m_vk.device, m_vk.graphicsQueue);
	m_vk.transferCommandPool = VWrap::CommandPool::Create(m_vk.device, m_vk.transferQueue);

	m_vk.frameController = VWrap::FrameController::Create(m_vk.device, m_vk.surface, m_vk.graphicsCommandPool, m_vk.presentQueue, MAX_FRAMES_IN_FLIGHT);
	m_vk.frameController->SetResizeCallback([this]() { Resize(); });

	m_vk.msaaSamples = m_vk.physicalDevice->GetMaxUsableSampleCount();

	VkFormat swapchainFormat = m_vk.frameController->GetSwapchain()->GetFormat();

	m_scene_render_pass = VWrap::RenderPass::CreateOffscreen(m_vk.device, swapchainFormat, m_vk.msaaSamples);
	m_presentation_render_pass = VWrap::RenderPass::CreatePresentation(m_vk.device, swapchainFormat);

	CreatePresentationFramebuffers();
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

void Application::MainLoop() {
	auto last_time = std::chrono::high_resolution_clock::now();

	while (!glfwWindowShouldClose(m_glfw_window.get()[0])) {
		auto current_time = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_time).count();
		last_time = current_time;

		auto input_query = Input::Poll();
		ParseInputQuery(input_query);

		// Click to capture viewport
		if (!m_app_state.focused && m_viewport_panel.WasClicked()) {
			m_app_state.focused = true;
			Input::HideCursor(true);
			Input::CenterCursor(true);
		}

		if (m_app_state.focused) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
			MoveCamera(dt);
		} else {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
		}

		// Update viewport texture (may have changed after resize)
		m_viewport_panel.SetTextureID(m_offscreen_target->GetImGuiTextureID());

		m_gui_renderer->BeginFrame();
		DrawFrame();

		m_metrics_panel.Update(m_last_metrics.fps, m_last_metrics.render_time, dt * 1000.0f);
	}
	vkDeviceWaitIdle(m_vk.device->Get());
}

void Application::ParseInputQuery(InputQuery query)
{
	move_state = { false, false, false, false, false, false, 0.0, 0.0 };
	move_state.dx = query.dx;
	move_state.dy = query.dy;

	for (auto i : query.actions) {
		Action action = static_cast<Action>(i);

		switch (action) {
		case Action::ESCAPE:
			if (m_app_state.focused) {
				m_app_state.focused = false;
				move_state.dx = 0;
				move_state.dy = 0;
				Input::HideCursor(false);
				Input::CenterCursor(false);
			}
			break;
		case Action::MOVE_UP:
			move_state.up = true;
			break;
		case Action::MOVE_DOWN:
			move_state.down = true;
			break;
		case Action::MOVE_LEFT:
			move_state.left = true;
			break;
		case Action::MOVE_RIGHT:
			move_state.right = true;
			break;
		case Action::MOVE_FORWARD:
			move_state.forward = true;
			break;
		case Action::MOVE_BACKWARD:
			move_state.back = true;
			break;
		case Action::RELOAD_SHADERS:
			HotReloadShaders();
			break;
		default:
			return;
		}
	}
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
			m_offscreen_target->Resize(desired);
			m_renderers[m_active_renderer_index]->OnResize(desired);
			m_camera->SetAspect((float)desired.width / (float)desired.height);
			m_viewport_panel.SetTextureID(m_offscreen_target->GetImGuiTextureID());
		}
	}

	// ACQUIRE FRAME ------------------------------------------------
	m_vk.frameController->AcquireNext();
	uint32_t image_index = m_vk.frameController->GetImageIndex();
	uint32_t frame_index = m_vk.frameController->GetCurrentFrame();
	auto command_buffer = m_vk.frameController->GetCurrentCommandBuffer();

	// Read metrics from previous cycle (fence wait in AcquireNext guarantees GPU work done)
	m_last_metrics = m_gpu_profiler->GetMetrics(frame_index);

	command_buffer->Begin();

	// ========== PASS 1: Scene -> Offscreen ==========
	m_gpu_profiler->CmdBegin(command_buffer, frame_index);

	std::vector<VkClearValue> sceneClearValues(3);
	sceneClearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	sceneClearValues[1].depthStencil = { 1.0f, 0 };
	sceneClearValues[2].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

	command_buffer->CmdBeginRenderPass(m_scene_render_pass, m_offscreen_target->GetFramebuffer(), sceneClearValues);

	m_renderers[m_active_renderer_index]->RecordCommands(command_buffer, frame_index, m_camera);

	m_gpu_profiler->CmdEnd(command_buffer, frame_index);

	vkCmdEndRenderPass(command_buffer->Get());

	// ========== PASS 2: ImGui -> Swapchain ==========
	std::vector<VkClearValue> presentClearValues(1);
	presentClearValues[0].color = { { 0.067f, 0.067f, 0.067f, 1.0f } };

	command_buffer->CmdBeginRenderPass(m_presentation_render_pass, m_presentation_framebuffers[image_index], presentClearValues);

	m_gui_renderer->CmdDraw(command_buffer);

	vkCmdEndRenderPass(command_buffer->Get());

	if (vkEndCommandBuffer(command_buffer->Get()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to end command buffer recording!");
	}

	// PRESENT ------------------------------------------------
	m_vk.frameController->Render();
}

void Application::Resize() {
	CreatePresentationFramebuffers();
	// Offscreen target does NOT resize on swapchain resize.
	// It resizes when the viewport panel detects a size change.
}

void Application::CreatePresentationFramebuffers() {
	auto swapchain = m_vk.frameController->GetSwapchain();
	m_presentation_framebuffers.resize(swapchain->Size());

	for (uint32_t i = 0; i < swapchain->Size(); i++) {
		std::vector<std::shared_ptr<VWrap::ImageView>> attachments = {
			m_vk.frameController->GetImageViews()[i]
		};
		m_presentation_framebuffers[i] = VWrap::Framebuffer::Create2D(
			m_vk.device, m_presentation_render_pass, attachments, swapchain->GetExtent());
	}
}

void Application::MoveCamera(float dt) {
	float distance = m_app_state.speed * dt;
	double mouse_sensitivity = (float)(-m_app_state.sensitivity/100.0);

	if (move_state.up && !move_state.down) m_camera->MoveUp(distance);
	if (move_state.down && !move_state.up) m_camera->MoveUp(-distance);
	if (move_state.left && !move_state.right) m_camera->MoveRight(-distance);
	if (move_state.right && !move_state.left) m_camera->MoveRight(distance);
	if (move_state.forward && !move_state.back) m_camera->MoveForward(distance);
	if (move_state.back && !move_state.forward) m_camera->MoveForward(-distance);

	double dx = move_state.dx;
	double dy = move_state.dy;

	dx *= mouse_sensitivity;
	dy *= mouse_sensitivity;

	auto forward = m_camera->GetForward();
	auto up = m_camera->GetUp();

	auto dot = glm::dot(forward, up);
	dot = glm::clamp(dot, -1.0f, 1.0f);
	auto angle = glm::acos(dot);

	if (angle - dy < 0.001f || angle - dy > glm::pi<float>() - 0.001f) dy = 0.0f;

	auto x_rot = glm::rotate(glm::mat4(1.0f), (float)dx, up);
	auto y_rot = glm::rotate(glm::mat4(1.0f), (float)dy, glm::cross(forward, up));
	auto final_vec = x_rot * y_rot * glm::vec4(forward, 1.0f);

	m_camera->SetForward(glm::normalize(final_vec));
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

	RenderContext ctx{};
	ctx.device = m_vk.device;
	ctx.allocator = m_vk.allocator;
	ctx.graphicsPool = m_vk.graphicsCommandPool;
	ctx.renderPass = m_scene_render_pass;
	ctx.extent = m_offscreen_target->GetExtent();
	ctx.maxFramesInFlight = MAX_FRAMES_IN_FLIGHT;

	renderer->RecreatePipeline(ctx);
	logger->info("Pipeline recreated successfully");
}

void Application::SwitchRenderer(size_t index) {
	if (index >= m_renderers.size() || index == m_active_renderer_index) return;

	auto logger = spdlog::get("App");
	logger->info("Switching renderer to: {}", m_renderers[index]->GetName());

	vkDeviceWaitIdle(m_vk.device->Get());

	// Initialize the new renderer if not already done
	RenderContext ctx{};
	ctx.device = m_vk.device;
	ctx.allocator = m_vk.allocator;
	ctx.graphicsPool = m_vk.graphicsCommandPool;
	ctx.renderPass = m_scene_render_pass;
	ctx.extent = m_offscreen_target->GetExtent();
	ctx.maxFramesInFlight = MAX_FRAMES_IN_FLIGHT;

	m_renderers[index]->Init(ctx);
	m_active_renderer_index = index;

	logger->info("Switched to: {}", m_renderers[index]->GetName());
}

void Application::CaptureScreenshot() {
	auto logger = spdlog::get("App");

	vkDeviceWaitIdle(m_vk.device->Get());

	auto resolveImage = m_offscreen_target->GetResolveImage();
	VkFormat format = m_offscreen_target->GetColorFormat();
	VkExtent2D extent = m_offscreen_target->GetExtent();

	VkDeviceSize imageSize = extent.width * extent.height * 4; // RGBA
	auto staging = VWrap::Buffer::CreateStaging(m_vk.allocator, imageSize);

	// Record copy commands
	auto cmd = VWrap::CommandBuffer::Create(m_vk.graphicsCommandPool);
	cmd->BeginSingle();

	// Transition resolve image: SHADER_READ_ONLY -> TRANSFER_SRC
	cmd->CmdTransitionImageLayout(resolveImage, format,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Copy image to staging buffer
	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { extent.width, extent.height, 1 };

	vkCmdCopyImageToBuffer(cmd->Get(), resolveImage->Get(),
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->Get(), 1, &region);

	// Transition resolve image back: TRANSFER_SRC -> SHADER_READ_ONLY
	cmd->CmdTransitionImageLayout(resolveImage, format,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	cmd->EndAndSubmit();

	// Map staging buffer and write PNG
	void* data;
	vmaMapMemory(m_vk.allocator->Get(), staging->GetAllocation(), &data);

	// Generate filename with timestamp
	std::filesystem::create_directories("screenshots");
	std::time_t t = std::time(nullptr);
	char timestr[64];
	std::strftime(timestr, sizeof(timestr), "%Y%m%d_%H%M%S", std::localtime(&t));
	std::string filename = std::string("screenshots/screenshot_") + timestr + ".png";

	// Handle BGRA -> RGBA swizzle if needed
	if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM) {
		uint8_t* pixels = static_cast<uint8_t*>(data);
		for (uint32_t i = 0; i < extent.width * extent.height; i++) {
			std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]); // swap B and R
		}
	}

	int result = stbi_write_png(filename.c_str(), extent.width, extent.height, 4,
		data, extent.width * 4);

	vmaUnmapMemory(m_vk.allocator->Get(), staging->GetAllocation());

	if (result) {
		logger->info("Screenshot saved: {}", filename);
		m_inspector_panel.SetLastScreenshotPath(filename);
	} else {
		logger->error("Failed to save screenshot");
	}
}
