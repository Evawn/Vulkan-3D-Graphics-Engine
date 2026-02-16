#include "GUIRenderer.h"

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

	ImGui::StyleColorsDark();

	return ret;
}

void GUIRenderer::RegisterPanel(const std::string& name, std::function<void()> drawFn) {
	m_panels.push_back({ name, std::move(drawFn) });
}

void GUIRenderer::SetupDefaultLayout(ImGuiID dockspace_id) {
	ImGui::DockBuilderRemoveNode(dockspace_id);
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

	// Split: left (viewport + output) | right (performance + renderer manager)
	ImGuiID left, right;
	ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.7f, &left, &right);

	// Split left: top (viewport) | bottom (output)
	ImGuiID left_top, left_bottom;
	ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.7f, &left_top, &left_bottom);

	// Split right: top (performance) | bottom (renderer manager)
	ImGuiID right_top, right_bottom;
	ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.5f, &right_top, &right_bottom);

	ImGui::DockBuilderDockWindow("Viewport", left_top);
	ImGui::DockBuilderDockWindow("Output", left_bottom);
	ImGui::DockBuilderDockWindow("Performance", right_top);
	ImGui::DockBuilderDockWindow("Renderer", right_bottom);

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
