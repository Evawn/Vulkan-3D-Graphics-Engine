#include "ViewportPanel.h"
#include "../UIStyle.h"

void ViewportPanel::Draw() {
	const bool active = m_ui && m_ui->camera_focused;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
	ImGui::Begin("Viewport", nullptr, flags);

	m_focused = ImGui::IsWindowFocused();
	m_hovered = ImGui::IsWindowHovered();

	ImVec2 contentSize = ImGui::GetContentRegionAvail();

	// Detect resize
	if (contentSize.x != m_panel_size.x || contentSize.y != m_panel_size.y) {
		if (contentSize.x > 0 && contentSize.y > 0) {
			m_last_panel_size = m_panel_size;
			m_panel_size = contentSize;
			m_was_resized = true;
		}
	}

	if (m_texture_id != VK_NULL_HANDLE && contentSize.x > 0 && contentSize.y > 0) {
		ImGui::Image((ImTextureID)m_texture_id, contentSize);
		if (ImGui::IsItemClicked()) {
			m_clicked = true;
		}
	}

	// Camera-focused outline: drawn with the window's draw list so docking chrome
	// can't paint over it. Inset by 1px so the stroke sits fully inside the viewport.
	if (active) {
		ImVec2 p_min = ImGui::GetWindowPos();
		ImVec2 sz   = ImGui::GetWindowSize();
		ImVec2 p_max = ImVec2(p_min.x + sz.x, p_min.y + sz.y);
		ImU32 col = ImGui::ColorConvertFloat4ToU32(UIStyle::kAccent);
		ImGui::GetWindowDrawList()->AddRect(
			ImVec2(p_min.x + 0.5f, p_min.y + 0.5f),
			ImVec2(p_max.x - 0.5f, p_max.y - 0.5f),
			col, 0.0f, 0, 1.0f);
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

bool ViewportPanel::WasClicked() {
	bool c = m_clicked;
	m_clicked = false;
	return c;
}

bool ViewportPanel::WasResized() {
	bool r = m_was_resized;
	m_was_resized = false;
	return r;
}

VkExtent2D ViewportPanel::GetDesiredExtent() const {
	return {
		static_cast<uint32_t>(m_panel_size.x),
		static_cast<uint32_t>(m_panel_size.y)
	};
}
