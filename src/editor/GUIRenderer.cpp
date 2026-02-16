#include "GUIRenderer.h"
#include "config.h"
#include <filesystem>
#include <spdlog/spdlog.h>

std::shared_ptr<GUIRenderer> GUIRenderer::Create(std::shared_ptr<VWrap::Device> device) {
	auto ret = std::make_shared<GUIRenderer>();

	std::vector<VkDescriptorPoolSize> pool_sizes =
	{
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 },
	};
	ret->m_imgui_descriptor_pool = VWrap::DescriptorPool::Create(device, pool_sizes, 10, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	return ret;
}

void GUIRenderer::LoadFonts(float dpi_scale) {
	ImGuiIO& io = ImGui::GetIO();
	m_dpi_scale = dpi_scale;

	io.Fonts->Clear();

	const float base_size = 9.0f;
	float font_size = base_size * dpi_scale;

	std::string font_path = std::string(config::RESOURCE_DIR) + "/fonts/Inter-Regular.ttf";

	if (std::filesystem::exists(font_path)) {
		ImFontConfig font_cfg;
		font_cfg.OversampleH = 3;
		font_cfg.OversampleV = 2;
		font_cfg.PixelSnapH = true;
		io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_size, &font_cfg);
		spdlog::get("App")->debug("Loaded font: {} at {}px", font_path, font_size);
	} else {
		spdlog::get("App")->warn("Font not found: {} - using default", font_path);
		io.Fonts->AddFontDefault();
	}

	io.FontGlobalScale = 1.0f;
}

void GUIRenderer::RegisterPanel(const std::string& name, std::function<void()> drawFn) {
	m_panels.push_back({ name, std::move(drawFn) });
}

void GUIRenderer::SetupDefaultLayout(ImGuiID dockspace_id) {
	ImGui::DockBuilderRemoveNode(dockspace_id);
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

	// Split: left (viewport + output) | right (metrics + inspector)
	ImGuiID left, right;
	ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.78f, &left, &right);

	// Split left: top (viewport) | bottom (output)
	ImGuiID left_top, left_bottom;
	ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.78f, &left_top, &left_bottom);

	// Split right: top (metrics) | bottom (inspector)
	ImGuiID right_top, right_bottom;
	ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.4f, &right_top, &right_bottom);

	ImGui::DockBuilderDockWindow("Viewport", left_top);
	ImGui::DockBuilderDockWindow("Output", left_bottom);
	ImGui::DockBuilderDockWindow("Metrics", right_top);
	ImGui::DockBuilderDockWindow("Inspector", right_bottom);

	ImGui::DockBuilderFinish(dockspace_id);
}

void GUIRenderer::CmdDraw(std::shared_ptr<VWrap::CommandBuffer> command_buffer) {
	// Create fullscreen dockspace
	ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());

	// Setup default layout on first frame
	if (m_first_frame) {
		SetupDefaultLayout(dockspace_id);
		m_first_frame = false;
	}

	// Draw all registered panels
	for (auto& panel : m_panels) {
		panel.drawFn();
	}

	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_buffer->Get());
}

void GUIRenderer::BeginFrame()
{
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}
