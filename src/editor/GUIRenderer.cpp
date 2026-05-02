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
	ImVec2 root_size = ImGui::GetMainViewport()->Size;
	if (m_ui) {
		if (m_ui->right_panel_px <= 0.0f)  m_ui->right_panel_px  = root_size.x * 0.22f;
		if (m_ui->bottom_panel_px <= 0.0f) m_ui->bottom_panel_px = root_size.y * 0.30f;
	}
	float right_px  = m_ui ? m_ui->right_panel_px  : root_size.x * 0.22f;
	float bottom_px = m_ui ? m_ui->bottom_panel_px : root_size.y * 0.30f;
	LayoutPreset preset = m_ui ? m_ui->layout_preset : LayoutPreset::Develop;
	BuildLayout(dockspace_id, preset, false, right_px, bottom_px);
	if (m_ui) m_ui->layout_dirty = false;
}

// Develop layout — the standard IDE shell:
//
//   +-----------------------+----------+
//   |                       | Hierarchy|
//   |                       +----------+
//   |       Viewport        | Inspector|
//   |                       +----------+
//   |                       | Perf | Memory | Render Graph (tabs)
//   +-----------------------+----------+
//   | Console (full-width bottom row)              |
//   +----------------------------------------------+
//
// Console is pinned to a full-width strip along the bottom — the IDE-terminal
// position. Analytics tabs sit at the bottom of the right column so they share
// the side panel with Hierarchy + Inspector instead of competing with Console.
//
// Performance preset = single-node viewport-only.
// Profile preset = analytics + console rows widened, viewport narrowed.
void GUIRenderer::BuildLayout(ImGuiID dockspace_id, LayoutPreset preset, bool viewport_only,
                              float right_px, float bottom_px) {
	ImGui::DockBuilderRemoveNode(dockspace_id);
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImVec2 root_size = ImGui::GetMainViewport()->Size;
	ImGui::DockBuilderSetNodeSize(dockspace_id, root_size);

	if (viewport_only || preset == LayoutPreset::Performance) {
		if (ImGuiDockNode* vp_node = ImGui::DockBuilderGetNode(dockspace_id)) {
			vp_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
		}
		ImGui::DockBuilderDockWindow("Viewport", dockspace_id);
		m_dock_left = m_dock_right = m_dock_left_top = m_dock_left_bottom = 0;
		m_dock_right_top = m_dock_right_bottom = 0;
		ImGui::DockBuilderFinish(dockspace_id);
		return;
	}

	// `bottom_px` is the user-dragged Console height. Default ~22% of the
	// viewport — enough to read 8-10 log lines without dominating.
	float right_ratio   = std::clamp(right_px  / std::max(1.0f, root_size.x), 0.14f, 0.40f);
	float console_ratio = std::clamp(bottom_px / std::max(1.0f, root_size.y), 0.12f, 0.45f);
	if (preset == LayoutPreset::Profile) {
		console_ratio = std::clamp(console_ratio + 0.05f, 0.18f, 0.50f);
		right_ratio   = std::clamp(right_ratio   + 0.06f, 0.18f, 0.45f);
	}

	// Step 1: split off the Console strip at the very bottom (full-width).
	ImGuiID main_area, console_area;
	ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, console_ratio, &console_area, &main_area);

	// Step 2: split main area into left (viewport) and right (side panel column).
	ImGuiID left, right_col;
	ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Right, right_ratio, &right_col, &left);

	// Step 3: split right column into upper (Hierarchy + Inspector) and lower
	// (Analytics tabs). Profile preset gives Analytics more vertical space.
	float analytics_ratio = (preset == LayoutPreset::Profile) ? 0.50f : 0.36f;
	ImGuiID right_upper, right_analytics;
	ImGui::DockBuilderSplitNode(right_col, ImGuiDir_Down, analytics_ratio, &right_analytics, &right_upper);

	// Step 4: split upper into Hierarchy (top ~36%) and Inspector (bottom).
	ImGuiID right_hier, right_insp;
	ImGui::DockBuilderSplitNode(right_upper, ImGuiDir_Up, 0.36f, &right_hier, &right_insp);

	m_dock_left          = left;
	m_dock_right         = right_col;
	m_dock_left_top      = left;
	m_dock_left_bottom   = console_area;   // full-width Console
	m_dock_right_top     = right_hier;
	m_dock_right_bottom  = right_analytics;

	if (ImGuiDockNode* vp_node = ImGui::DockBuilderGetNode(left)) {
		vp_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
	}

	ImGui::DockBuilderDockWindow("Viewport",     left);
	ImGui::DockBuilderDockWindow("Console",      console_area);
	ImGui::DockBuilderDockWindow("Hierarchy",    right_hier);
	ImGui::DockBuilderDockWindow("Inspector",    right_insp);
	ImGui::DockBuilderDockWindow("Performance",  right_analytics);
	ImGui::DockBuilderDockWindow("Memory",       right_analytics);
	ImGui::DockBuilderDockWindow("Render Graph", right_analytics);

	ImGui::DockBuilderFinish(dockspace_id);
}

void GUIRenderer::CmdDraw(std::shared_ptr<VWrap::CommandBuffer> command_buffer) {
	// Top menu bar (full-viewport, outside the dockspace). Drawn first so the
	// dockspace below subtracts its height correctly via DockSpaceOverViewport.
	if (m_menu_bar_fn) {
		if (ImGui::BeginMainMenuBar()) {
			m_menu_bar_fn();
			ImGui::EndMainMenuBar();
		}
	}

	ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());

	if (m_first_frame) {
		SetupDefaultLayout(dockspace_id);
		m_first_frame = false;
	}

	const bool viewport_only = m_ui && m_ui->viewport_only;
	const LayoutPreset preset = m_ui ? m_ui->layout_preset : LayoutPreset::Develop;
	if (m_ui && (m_ui->layout_dirty
	             || viewport_only != m_last_viewport_only
	             || preset != m_last_preset)) {
		BuildLayout(dockspace_id, preset, viewport_only, m_ui->right_panel_px, m_ui->bottom_panel_px);
		m_ui->layout_dirty = false;
		m_last_viewport_only = viewport_only;
		m_last_preset = preset;
	}

	for (auto& panel : m_panels) {
		// Skip non-Viewport panels in viewport-only / Performance preset.
		const bool only_viewport = viewport_only || preset == LayoutPreset::Performance;
		if (only_viewport && panel.name != "Viewport") continue;
		panel.drawFn();
	}

	if (m_ui && !m_ui->layout_dirty && !viewport_only && preset != LayoutPreset::Performance) {
		if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(m_dock_right)) {
			if (n->Size.x > 0) m_ui->right_panel_px = n->Size.x;
		}
		if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(m_dock_left_bottom)) {
			if (n->Size.y > 0) m_ui->bottom_panel_px = n->Size.y;
		}
	}

	// Status bar (full-viewport, outside the dockspace).
	if (m_status_bar_fn) {
		const float h = ImGui::GetFrameHeight();
		if (ImGui::BeginViewportSideBar("##statusbar", ImGui::GetMainViewport(),
		                                ImGuiDir_Down, h, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar)) {
			if (ImGui::BeginMenuBar()) {
				m_status_bar_fn();
				ImGui::EndMenuBar();
			}
		}
		ImGui::End();
	}

	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_buffer->Get());
}

void GUIRenderer::BeginFrame()
{
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();

	// Tame mouse-wheel scroll sensitivity. ImGui defaults to 5 lines per notch
	// at body font size; on macOS smooth-scroll input that means dense panels
	// (Console, Inspector) blow past the visible region in a single gesture.
	// Applied *before* NewFrame so ImGui consumes the dampened value.
	{
		ImGuiIO& io = ImGui::GetIO();
		const float scroll_scale = m_ui ? m_ui->scroll_sensitivity : 0.4f;
		io.MouseWheel  *= scroll_scale;
		io.MouseWheelH *= scroll_scale;
	}

	ImGui::NewFrame();
}
