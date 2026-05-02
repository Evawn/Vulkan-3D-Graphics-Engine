#pragma once

#include "imgui.h"
#include "../UIState.h"
#include <vulkan/vulkan.h>
#include <string>

class ViewportPanel {
private:
	VkDescriptorSet m_texture_id = VK_NULL_HANDLE;
	ImVec2 m_panel_size{0, 0};
	ImVec2 m_last_panel_size{0, 0};
	bool m_focused = false;
	bool m_hovered = false;
	bool m_was_resized = false;
	bool m_clicked = false;
	UIState* m_ui = nullptr;

	// Live offscreen extent the renderer is currently producing into the scene
	// texture. Set per-frame by Editor (after the resize edge fires). Differs
	// from m_panel_size in Center / Fit modes: Center samples 1:1 (or down-fits)
	// from this extent; Fit stretches it to m_panel_size.
	VkExtent2D m_render_extent{0, 0};

	// HUD content — set per-frame from Editor before Draw() runs.
	float m_hud_fps = 0.0f;
	float m_hud_ms  = 0.0f;
	std::string m_hud_technique;

public:
	void SetTextureID(VkDescriptorSet texID) { m_texture_id = texID; }
	void SetUIState(UIState* ui) { m_ui = ui; }
	void SetRenderExtent(VkExtent2D ext) { m_render_extent = ext; }
	void SetHud(float fps, float ms, const std::string& technique) {
		m_hud_fps = fps;
		m_hud_ms = ms;
		m_hud_technique = technique;
	}
	void Draw();

	bool IsFocused() const { return m_focused; }
	bool IsHovered() const { return m_hovered; }
	bool WasClicked();
	bool WasResized();
	// Force the resize edge to fire on the next Application::DrawFrame poll.
	// Used by the status-bar resolution widget so a mode/target change goes
	// through the same rebuild path as a panel drag.
	void MarkResized() { m_was_resized = true; }
	VkExtent2D GetDesiredExtent() const;
};
