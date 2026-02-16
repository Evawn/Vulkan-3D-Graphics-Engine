#include "ViewportPanel.h"

void ViewportPanel::Draw() {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin("Viewport");

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
