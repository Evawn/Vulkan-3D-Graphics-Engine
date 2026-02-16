#pragma once

#include "imgui.h"
#include <vulkan/vulkan.h>

class ViewportPanel {
private:
	VkDescriptorSet m_texture_id = VK_NULL_HANDLE;
	ImVec2 m_panel_size{0, 0};
	ImVec2 m_last_panel_size{0, 0};
	bool m_focused = false;
	bool m_hovered = false;
	bool m_was_resized = false;
	bool m_clicked = false;

public:
	void SetTextureID(VkDescriptorSet texID) { m_texture_id = texID; }
	void Draw();

	bool IsFocused() const { return m_focused; }
	bool IsHovered() const { return m_hovered; }
	bool WasClicked();
	bool WasResized();
	VkExtent2D GetDesiredExtent() const;
};
