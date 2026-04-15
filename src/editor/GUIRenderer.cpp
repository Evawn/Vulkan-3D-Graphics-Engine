#include "GUIRenderer.h"
#include "UIStyle.h"
#include <algorithm>
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
	m_dpi_scale = dpi_scale;
	UIStyle::LoadFonts(dpi_scale);
}

void GUIRenderer::RegisterPanel(const std::string& name, std::function<void()> drawFn) {
	m_panels.push_back({ name, std::move(drawFn) });
}

void GUIRenderer::SetupDefaultLayout(ImGuiID dockspace_id) {
	// Seed UIState absolute sizes from the initial proportional split so the
	// very first resize has something meaningful to restore.
	ImVec2 root_size = ImGui::GetMainViewport()->Size;
	if (m_ui) {
		if (m_ui->right_panel_px <= 0.0f)  m_ui->right_panel_px  = root_size.x * (1.0f - 0.78f);
		if (m_ui->bottom_panel_px <= 0.0f) m_ui->bottom_panel_px = root_size.y * (1.0f - 0.78f);
	}
	float right_px  = m_ui ? m_ui->right_panel_px  : root_size.x * 0.22f;
	float bottom_px = m_ui ? m_ui->bottom_panel_px : root_size.y * 0.22f;
	BuildLayout(dockspace_id, false, right_px, bottom_px);
	if (m_ui) m_ui->layout_dirty = false;
}

void GUIRenderer::BuildLayout(ImGuiID dockspace_id, bool viewport_only, float right_px, float bottom_px) {
	ImGui::DockBuilderRemoveNode(dockspace_id);
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImVec2 root_size = ImGui::GetMainViewport()->Size;
	ImGui::DockBuilderSetNodeSize(dockspace_id, root_size);

	if (viewport_only) {
		// Single-node layout: the viewport fills the entire dockspace, no siblings,
		// no splitters. Other panel windows remain registered but aren't docked here;
		// CmdDraw will skip their Begin() calls.
		if (ImGuiDockNode* vp_node = ImGui::DockBuilderGetNode(dockspace_id)) {
			vp_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
		}
		ImGui::DockBuilderDockWindow("Viewport", dockspace_id);
		m_dock_left = m_dock_right = m_dock_left_top = m_dock_left_bottom = 0;
		ImGui::DockBuilderFinish(dockspace_id);
		return;
	}

	// Compute split ratios from absolute pixel targets so the layout honors UIState.
	float right_ratio  = std::clamp(right_px  / std::max(1.0f, root_size.x), 0.05f, 0.95f);
	float bottom_ratio = std::clamp(bottom_px / std::max(1.0f, root_size.y), 0.05f, 0.95f);

	ImGuiID left, right;
	ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, right_ratio, &right, &left);

	ImGuiID left_top, left_bottom;
	ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, bottom_ratio, &left_bottom, &left_top);

	ImGuiID right_top, right_bottom;
	ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.4f, &right_top, &right_bottom);

	m_dock_left = left;
	m_dock_right = right;
	m_dock_left_top = left_top;
	m_dock_left_bottom = left_bottom;

	if (ImGuiDockNode* vp_node = ImGui::DockBuilderGetNode(left_top)) {
		vp_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
	}

	ImGui::DockBuilderDockWindow("Viewport", left_top);
	ImGui::DockBuilderDockWindow("Output", left_bottom);
	ImGui::DockBuilderDockWindow("Metrics", right_top);
	ImGui::DockBuilderDockWindow("Render Graph", right_top);
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

	// Rebuild the dock graph on mode transitions (viewport-only ↔ normal) or on
	// window resize. Full rebuild avoids "sliver" artifacts from splitters between
	// collapsed sibling nodes: in viewport-only mode the dockspace is a single node.
	const bool viewport_only = m_ui && m_ui->viewport_only;
	if (m_ui && (m_ui->layout_dirty || viewport_only != m_last_viewport_only)) {
		BuildLayout(dockspace_id, viewport_only, m_ui->right_panel_px, m_ui->bottom_panel_px);
		m_ui->layout_dirty = false;
		m_last_viewport_only = viewport_only;
	}

	// Draw panels — skip non-viewport panels while in viewport-only mode.
	for (auto& panel : m_panels) {
		if (viewport_only && panel.name != "Viewport") continue;
		panel.drawFn();
	}

	// Capture user-dragged splitter sizes so subsequent window resizes preserve intent.
	// Skip in viewport-only mode: nodes are collapsed to zero there and we don't want
	// to overwrite the saved widths.
	if (m_ui && !m_ui->layout_dirty && !viewport_only) {
		if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(m_dock_right)) {
			if (n->Size.x > 0) m_ui->right_panel_px = n->Size.x;
		}
		if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(m_dock_left_bottom)) {
			if (n->Size.y > 0) m_ui->bottom_panel_px = n->Size.y;
		}
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
