#include "Editor.h"
#include "Window.h"
#include "Log.h"
#include "UIStyle.h"
#include <spdlog/spdlog.h>

static void check_vk_result(VkResult err) {
	if (err == 0) return;
	spdlog::get("App")->error("VkResult = {}", (int)err);
	if (err < 0) abort();
}

void Editor::InitImGui(VWrap::VulkanContext& vk,
					   std::shared_ptr<VWrap::RenderPass> renderPass,
					   Window& window) {
	m_gui = GUIRenderer::Create(vk.device);
	m_scene_sampler = VWrap::Sampler::Create(vk.device);

	VWrap::QueueFamilyIndices indices = vk.physicalDevice->FindQueueFamilies();
	ImGui_ImplGlfw_InitForVulkan(window.GetRaw(), true);

	ImGui_ImplVulkan_InitInfo init_info{};
	init_info.Instance = vk.instance->Get();
	init_info.PhysicalDevice = vk.physicalDevice->Get();
	init_info.Device = vk.device->Get();
	init_info.QueueFamily = indices.graphicsFamily.value();
	init_info.Queue = vk.graphicsQueue->Get();
	init_info.PipelineCache = VK_NULL_HANDLE;
	init_info.DescriptorPool = m_gui->GetDescriptorPool()->Get();
	init_info.Subpass = 0;
	init_info.MinImageCount = vk.frameController->GetSwapchain()->Size();
	init_info.ImageCount = vk.frameController->GetSwapchain()->Size();
	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init_info.Allocator = VK_NULL_HANDLE;
	init_info.CheckVkResultFn = check_vk_result;
	ImGui_ImplVulkan_Init(&init_info, renderPass->Get());

	float dpi_scale = window.GetContentScale();
	m_gui->LoadFonts(dpi_scale);
	ImGui_ImplVulkan_DestroyFontsTexture();
	ImGui_ImplVulkan_CreateFontsTexture();

	UIStyle::Apply();
}

void Editor::InitPanels(std::vector<std::unique_ptr<RenderTechnique>>* renderers,
						size_t* activeRendererIndex,
						std::shared_ptr<Camera> camera,
						std::shared_ptr<CameraController> cameraController,
						VWrap::VulkanContext& vk) {
	// Viewport panel
	m_viewport.SetTextureID(m_scene_texture);
	m_gui->RegisterPanel("Viewport", [this]() { m_viewport.Draw(); });

	// Metrics panel
	m_metrics.SetRenderers(renderers, activeRendererIndex);
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice->Get(), &memProps);
	m_metrics.SetAllocator(vk.allocator->Get(), memProps.memoryHeapCount);
	m_gui->RegisterPanel("Metrics", [this]() { m_metrics.Draw(); });

	// Output panel
	m_output.SetSink(Log::GetImGuiSink());
	m_gui->RegisterPanel("Output", [this]() { m_output.Draw(); });

	// Inspector panel
	m_inspector.SetRenderers(renderers, activeRendererIndex);
	m_inspector.SetCamera(camera);
	m_inspector.SetAppControls(cameraController->SensitivityPtr(), cameraController->SpeedPtr());
	m_gui->RegisterPanel("Inspector", [this]() { m_inspector.Draw(); });

	// Render Graph panel
	m_gui->RegisterPanel("Render Graph", [this]() { m_renderGraphPanel.Draw(); });
}

void Editor::Shutdown() {
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
}

void Editor::BeginFrame() {
	m_gui->BeginFrame();
}

void Editor::CmdDraw(std::shared_ptr<VWrap::CommandBuffer> cmd) {
	m_gui->CmdDraw(cmd);
}

void Editor::RegisterSceneTexture(std::shared_ptr<VWrap::ImageView> resolveView) {
	m_scene_texture = ImGui_ImplVulkan_AddTexture(
		m_scene_sampler->Get(),
		resolveView->Get(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_viewport.SetTextureID(m_scene_texture);
}

void Editor::RemoveSceneTexture() {
	if (m_scene_texture != VK_NULL_HANDLE) {
		ImGui_ImplVulkan_RemoveTexture(m_scene_texture);
		m_scene_texture = VK_NULL_HANDLE;
	}
}

void Editor::UpdateViewportTexture() {
	m_viewport.SetTextureID(m_scene_texture);
}

void Editor::SetReloadCallback(std::function<void()> cb) {
	m_inspector.SetReloadCallback(std::move(cb));
}

void Editor::SetSwitchCallback(std::function<void(size_t)> cb) {
	m_inspector.SetSwitchCallback(std::move(cb));
}

void Editor::SetScreenshotCallback(std::function<void()> cb) {
	m_inspector.SetScreenshotCallback(std::move(cb));
}

void Editor::SetWireframeCallback(std::function<void()> cb) {
	m_metrics.SetWireframeCallback(std::move(cb));
}

bool Editor::ViewportWasResized() const {
	// const_cast needed because WasResized() is non-const (it resets the flag)
	return const_cast<ViewportPanel&>(m_viewport).WasResized();
}

bool Editor::ViewportWasClicked() const {
	return const_cast<ViewportPanel&>(m_viewport).WasClicked();
}

VkExtent2D Editor::GetDesiredViewportExtent() const {
	return m_viewport.GetDesiredExtent();
}

void Editor::OnDpiChanged(float newScale) {
	m_gui->LoadFonts(newScale);
	ImGui_ImplVulkan_DestroyFontsTexture();
	ImGui_ImplVulkan_CreateFontsTexture();
	UIStyle::Apply();
}

void Editor::UpdateMetrics(float fps, float gpuMs, float frameMs) {
	m_metrics.Update(fps, gpuMs, frameMs);
}

void Editor::SetLastScreenshotPath(const std::string& path) {
	m_inspector.SetLastScreenshotPath(path);
}

void Editor::SetGraphSnapshot(const GraphSnapshot* snapshot) {
	m_renderGraphPanel.SetSnapshot(snapshot);
}

void Editor::SetPerformanceMetrics(const GPUProfiler::PerformanceMetrics* metrics) {
	m_renderGraphPanel.SetMetrics(metrics);
}
