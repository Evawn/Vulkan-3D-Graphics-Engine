#include "Application.h"

void Application::Run() {
	Init();
	MainLoop();
	Cleanup();
}

void Application::Init() {
	InitWindow();
	InitVulkan();
	InitImGui();
	VkExtent2D extent = m_frame_controller->GetSwapchain()->GetExtent();
	m_octree_tracer = OctreeTracer::Create(
		m_allocator,
		m_device,
		m_render_pass,
		m_graphics_command_pool,
		extent,
		MAX_FRAMES_IN_FLIGHT);

	m_gpu_profiler = GPUProfiler::Create(m_device, MAX_FRAMES_IN_FLIGHT);

	m_camera = Camera::Create(45, ((float)extent.width / (float)extent.height), 0.1f, 10.0f);

	Input::Init(m_glfw_window.get()[0]);
	Input::AddContext(m_main_context); // :3c
	
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

	// Defining a monitor
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);
	int screen_width = mode->width;
	int screen_height = mode->height;

	// Putting it in the centre
	glfwSetWindowPos(m_glfw_window.get()[0], (screen_width-WIDTH) / 2, (screen_height - HEIGHT) / 2);
}

void Application::glfw_FramebufferResizeCallback(GLFWwindow* window, int width, int height) {
	auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
	app->m_frame_controller->SetResized(true);
}

void Application::glfw_WindowFocusCallback(GLFWwindow* window, int focused) {
	//auto mode = glfwGetInputMode(window, GLFW_CURSOR);
	//if (mode != GLFW_CURSOR_HIDDEN) {
	//	// The window gained input focus, hide the cursor without capturing it.
	//	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
	//}

}

void Application::InitVulkan() {
	m_instance = VWrap::Instance::Create(ENABLE_VALIDATION_LAYERS);
	m_surface = VWrap::Surface::Create(m_instance, m_glfw_window);
	m_physical_device = VWrap::PhysicalDevice::Pick(m_instance, m_surface);
	m_device = VWrap::Device::Create(m_physical_device, ENABLE_VALIDATION_LAYERS);
	m_allocator = VWrap::Allocator::Create(m_instance, m_physical_device, m_device);

	VWrap::QueueFamilyIndices indices = m_physical_device->FindQueueFamilies();

	m_graphics_queue = VWrap::Queue::Create(m_device, indices.graphicsFamily.value());
	m_present_queue = VWrap::Queue::Create(m_device, indices.presentFamily.value());
	m_transfer_queue = VWrap::Queue::Create(m_device, indices.transferFamily.value());
	m_graphics_command_pool = VWrap::CommandPool::Create(m_device, m_graphics_queue);
	m_transfer_command_pool = VWrap::CommandPool::Create(m_device, m_transfer_queue);

	m_frame_controller = VWrap::FrameController::Create(m_device, m_surface, m_graphics_command_pool, m_present_queue, MAX_FRAMES_IN_FLIGHT);
	m_frame_controller->SetResizeCallback([this]() { Resize(); });

	VkSampleCountFlagBits sample_count = m_physical_device->GetMaxUsableSampleCount();

	m_render_pass = VWrap::RenderPass::CreateImGUI(m_device, m_frame_controller->GetSwapchain()->GetFormat(), sample_count);

	CreateColorResources(sample_count);
	CreateDepthResources(sample_count);
	CreateFramebuffers();
}

void Application::InitImGui() {
	m_gui_renderer = GUIRenderer::Create(m_device);

	VWrap::QueueFamilyIndices indices = m_physical_device->FindQueueFamilies();
	ImGui_ImplGlfw_InitForVulkan(m_glfw_window.get()[0], true);

	ImGui_ImplVulkan_InitInfo init_info{};
	init_info.Instance = m_instance->Get();
	init_info.PhysicalDevice = m_physical_device->Get();
	init_info.Device = m_device->Get();
	init_info.QueueFamily = indices.graphicsFamily.value();
	init_info.Queue = m_graphics_queue->Get();
	init_info.PipelineCache = VK_NULL_HANDLE;
	init_info.DescriptorPool = m_gui_renderer->GetDescriptorPool()->Get();
	init_info.Subpass = 1;
	init_info.MinImageCount = m_frame_controller->GetSwapchain()->Size();
	init_info.ImageCount = m_frame_controller->GetSwapchain()->Size();
	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init_info.Allocator = VK_NULL_HANDLE;
	init_info.CheckVkResultFn = check_vk_result;
	ImGui_ImplVulkan_Init(&init_info, m_render_pass->Get());

	float dpi_scale;
	glfwGetWindowContentScale(m_glfw_window.get()[0], &dpi_scale, nullptr);
	m_gui_renderer->SetDpiScale(dpi_scale);
	ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

void Application::MainLoop() {
	auto last_time = std::chrono::high_resolution_clock::now();

	while (!glfwWindowShouldClose(m_glfw_window.get()[0])) {
		auto current_time = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_time).count();
		last_time = current_time;

		auto input_query = Input::Poll();
		ParseInputQuery(input_query);
		
		if(m_app_state.focused) {
			
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
			MoveCamera(dt);
			
		} else {
			
			ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
		}


		m_gui_renderer->BeginFrame();
		DrawFrame();
	}
	vkDeviceWaitIdle(m_device->Get());
}

void Application::ParseInputQuery(InputQuery query)
{
	move_state = { false, false, false, false, false, false, 0.0, 0.0 };
	move_state.dx = query.dx;
	move_state.dy = query.dy;

	for (auto i : query.actions) {
		Action action = static_cast<Action>(i);

		int mode = -1;
		switch (action) {
		case Action::ESCAPE:
			m_app_state.focused = !m_app_state.focused;
			move_state.dx = 0;
			move_state.dy = 0;
			Input::HideCursor(m_app_state.focused);
			Input::CenterCursor(m_app_state.focused);
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

	// ACQUIRE FRAME ------------------------------------------------
	m_frame_controller->AcquireNext();
	uint32_t image_index = m_frame_controller->GetImageIndex();
	uint32_t frame_index = m_frame_controller->GetCurrentFrame();
	auto command_buffer = m_frame_controller->GetCurrentCommandBuffer();

	// BEGIN RECORDING ------------------------------------------------
	std::shared_ptr<VWrap::Framebuffer> framebuffer = m_framebuffers[image_index];
	command_buffer->Begin();

	// BEGIN PROFILING ------------------------------------------------
	m_gpu_profiler->CmdBegin(command_buffer, frame_index);

	// BEGIN RENDER PASS ------------------------------------------------
	command_buffer->CmdBeginRenderPass(m_render_pass, framebuffer);

	// RECORD SCENE COMMANDS ------------------------------------------------
	m_octree_tracer->CmdDraw(command_buffer, frame_index, m_camera);

	// END PROFILING ------------------------------------------------
	m_gpu_profiler->CmdEnd(command_buffer, frame_index);

	// NEXT SUBPASS ------------------------------------------------
	vkCmdNextSubpass(command_buffer->Get(), VK_SUBPASS_CONTENTS_INLINE);

	// RECORD GUI COMMANDS ------------------------------------------------
	GPUProfiler::PerformanceMetrics metrics = m_gpu_profiler->GetMetrics(frame_index);
	m_gui_renderer->CmdDraw(command_buffer, metrics.render_time, metrics.fps, m_app_state.sensitivity, m_app_state.speed);

	// END RENDER PASS - TODO: ABSTRACT ------------------------------------------------
	vkCmdEndRenderPass(command_buffer->Get());

	if (vkEndCommandBuffer(command_buffer->Get()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to end command buffer recording!");
	}

	// RENDER -----------------------------------
	m_frame_controller->Render();
}

void Application::Resize() {
	CreateColorResources(m_render_pass->GetSamples());
	CreateDepthResources(m_render_pass->GetSamples());
	CreateFramebuffers();

	VkExtent2D extent = m_frame_controller->GetSwapchain()->GetExtent();
	m_octree_tracer->Resize(extent);
	m_camera = Camera::Create(45, ((float)extent.width / (float)extent.height), 0.1f, 10.0f);

	float dpi_scale;
	glfwGetWindowContentScale(m_glfw_window.get()[0], &dpi_scale, nullptr);
	m_gui_renderer->SetDpiScale(dpi_scale);
}

void Application::CreateFramebuffers() {
	m_framebuffers.resize(m_frame_controller->GetSwapchain()->Size());
	for (uint32_t i = 0; i < m_frame_controller->GetSwapchain()->Size(); i++) {
		std::vector<std::shared_ptr<VWrap::ImageView>> attachments = {
			m_color_image_view,
			m_depth_image_view,
			m_frame_controller->GetImageViews()[i]
		};

		m_framebuffers[i] = VWrap::Framebuffer::Create2D(m_device, m_render_pass, attachments, m_frame_controller->GetSwapchain()->GetExtent());
	}
}

void Application::CreateDepthResources(VkSampleCountFlagBits samples)
{
	VkFormat depthFormat = VWrap::FindDepthFormat(m_physical_device->Get());

	VWrap::ImageCreateInfo info{};
	info.format = depthFormat;
	info.height = m_frame_controller->GetSwapchain()->GetExtent().height;
	info.width = m_frame_controller->GetSwapchain()->GetExtent().width;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	info.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	info.mip_levels = 1;
	info.samples = samples;
	info.image_type = VK_IMAGE_TYPE_2D;

	auto im = VWrap::Image::Create(m_allocator, info);
	m_depth_image_view = VWrap::ImageView::Create(m_device, im, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Application::CreateColorResources(VkSampleCountFlagBits samples)
{
	VWrap::ImageCreateInfo info{};
	info.format = m_frame_controller->GetSwapchain()->GetFormat();
	info.height = m_frame_controller->GetSwapchain()->GetExtent().height;
	info.width = m_frame_controller->GetSwapchain()->GetExtent().width;
	info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
	info.mip_levels = 1;
	info.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.samples = samples;
	info.image_type = VK_IMAGE_TYPE_2D;

	auto im = VWrap::Image::Create(m_allocator, info);
	m_color_image_view = VWrap::ImageView::Create(m_device, im, VK_IMAGE_ASPECT_COLOR_BIT);
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

	//if(angle - dy < 0.001f) dy = 0.001f - angle;
	//else if(angle - dy > glm::pi<float>() - 0.001f) dy = glm::pi<float>() - 0.001f - angle;
	if (angle - dy < 0.001f || angle - dy > glm::pi<float>() - 0.001f) dy = 0.0f;

	auto x_rot = glm::rotate(glm::mat4(1.0f), (float)dx, up);
	auto y_rot = glm::rotate(glm::mat4(1.0f), (float)dy, glm::cross(forward, up));
	auto final_vec = x_rot * y_rot * glm::vec4(forward, 1.0f);

	m_camera->SetForward(glm::normalize(final_vec));
}