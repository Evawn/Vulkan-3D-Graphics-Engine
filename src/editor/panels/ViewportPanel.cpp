#include "ViewportPanel.h"
#include "../UIStyle.h"

#include <algorithm>
#include <cstdio>
#include <cmath>

void ViewportPanel::Draw() {
	const bool active = m_ui && m_ui->camera_focused;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
	ImGui::Begin("Viewport", nullptr, flags);

	m_focused = ImGui::IsWindowFocused();
	m_hovered = ImGui::IsWindowHovered();

	ImVec2 contentSize = ImGui::GetContentRegionAvail();

	if (contentSize.x != m_panel_size.x || contentSize.y != m_panel_size.y) {
		if (contentSize.x > 0 && contentSize.y > 0) {
			m_last_panel_size = m_panel_size;
			m_panel_size = contentSize;
			m_was_resized = true;
		}
	}

	if (m_texture_id != VK_NULL_HANDLE && contentSize.x > 0 && contentSize.y > 0) {
		// Mode-driven blit. The render-target extent (m_render_extent) and the
		// panel extent (contentSize) only diverge in Center / Fit modes — in
		// Native they're equal by construction (Editor::GetEffectiveRenderExtent
		// returns the panel size).
		const ResolutionMode mode = m_ui ? m_ui->resolution.mode : ResolutionMode::Native;
		const ImVec2 cursor = ImGui::GetCursorPos();
		const float renderW = static_cast<float>(m_render_extent.width);
		const float renderH = static_cast<float>(m_render_extent.height);
		const bool haveRenderExtent = renderW > 0.0f && renderH > 0.0f;

		if (mode == ResolutionMode::Center && haveRenderExtent) {
			// Two sub-cases:
			//   A. Render extent <= panel on both axes — sample 1:1, center
			//      with bare panel background (already black) showing as
			//      letterbox / pillarbox margins.
			//   B. Render extent > panel on either axis — down-fit while
			//      preserving aspect so the user sees the *whole* frame,
			//      with margins on the dominant-axis pair. This matches a
			//      preview-tool intuition; the alternative (crop to the
			//      middle of the rendered image) was discussed and
			//      rejected for v1.
			ImVec2 drawSize;
			if (renderW <= contentSize.x && renderH <= contentSize.y) {
				drawSize = ImVec2(renderW, renderH);
			} else {
				const float scale = std::min(contentSize.x / renderW,
				                             contentSize.y / renderH);
				drawSize = ImVec2(renderW * scale, renderH * scale);
			}
			const ImVec2 offset((contentSize.x - drawSize.x) * 0.5f,
			                    (contentSize.y - drawSize.y) * 0.5f);
			ImGui::SetCursorPos(ImVec2(cursor.x + offset.x, cursor.y + offset.y));
			ImGui::Image((ImTextureID)m_texture_id, drawSize);
			if (ImGui::IsItemClicked()) m_clicked = true;
		} else {
			// Native and Fit both stretch the texture to fill the panel.
			// They differ only in what the renderer fed *into* the texture:
			// Native renders at panel size (1:1), Fit renders at target res
			// and accepts the stretch as the user-visible cost of the mode.
			ImGui::Image((ImTextureID)m_texture_id, contentSize);
			if (ImGui::IsItemClicked()) m_clicked = true;
		}
	}

	auto* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();
	ImVec2 ws = ImGui::GetWindowSize();

	// Camera-focused outline
	if (active) {
		ImU32 col = UIStyle::U32(UIStyle::kAccent);
		dl->AddRect(
			ImVec2(wp.x + 0.5f, wp.y + 0.5f),
			ImVec2(wp.x + ws.x - 0.5f, wp.y + ws.y - 0.5f),
			col, 0.0f, 0, 1.0f);
	}

	// === HUD overlays === (click-through; drawn directly with the draw list)
	if (m_ui) {
		const float pad = 8.0f;

		// Top-left: perf readout
		if (m_ui->hud_show_perf) {
			char buf[48];
			snprintf(buf, sizeof(buf), "%4.0f fps  %5.2f ms", m_hud_fps, m_hud_ms);
			ImVec2 tp(wp.x + pad, wp.y + pad);
			// Drop shadow first for readability over bright scenes.
			dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0,0,0,180), buf);
			dl->AddText(tp, UIStyle::U32(UIStyle::Alpha(UIStyle::kText, 0.85f)), buf);
		}

		// Bottom-left: technique label
		if (m_ui->hud_show_technique && !m_hud_technique.empty()) {
			ImVec2 ts = ImGui::CalcTextSize(m_hud_technique.c_str());
			ImVec2 tp(wp.x + pad, wp.y + ws.y - ts.y - pad);
			dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0,0,0,180), m_hud_technique.c_str());
			dl->AddText(tp, UIStyle::U32(UIStyle::Alpha(UIStyle::kAccent, 0.85f)), m_hud_technique.c_str());
		}

		// Top-right: tiny RGB axes gizmo. Three short segments, fixed in screen
		// space — not a real camera-projected gizmo (would require camera
		// transform plumbing into the panel) but reads as orientation.
		if (m_ui->hud_show_axes) {
			float gx = wp.x + ws.x - 36.0f;
			float gy = wp.y + 36.0f;
			float L = 18.0f;
			dl->AddCircleFilled(ImVec2(gx, gy), 2.0f, IM_COL32(255,255,255,160));
			dl->AddLine(ImVec2(gx, gy), ImVec2(gx + L, gy), IM_COL32(220, 80, 80, 220), 2.0f);   // X red
			dl->AddLine(ImVec2(gx, gy), ImVec2(gx, gy - L), IM_COL32( 80,200, 80, 220), 2.0f);   // Y green (up)
			dl->AddLine(ImVec2(gx, gy), ImVec2(gx + L * 0.6f, gy + L * 0.6f), IM_COL32( 80,120,220, 220), 2.0f); // Z blue
			dl->AddText(ImVec2(gx + L + 2, gy - 6), IM_COL32(220, 80, 80, 220), "x");
			dl->AddText(ImVec2(gx - 4, gy - L - 12), IM_COL32(80, 200, 80, 220), "y");
			dl->AddText(ImVec2(gx + L * 0.6f + 2, gy + L * 0.6f - 6), IM_COL32(80, 120, 220, 220), "z");
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
