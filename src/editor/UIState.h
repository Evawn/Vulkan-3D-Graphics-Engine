#pragma once

#include <cstdint>

// Named layout presets. Develop is the default 4-quadrant editor; Performance
// hides everything except the viewport (and HUD); Profile maximizes
// analytics+render-graph and shrinks the viewport.
enum class LayoutPreset : uint8_t {
	Develop,
	Performance,
	Profile
};

struct UIState {
	bool viewport_only = false;
	bool os_fullscreen = false;
	float right_panel_px = 0.0f;
	float bottom_panel_px = 0.0f;
	bool camera_focused = false;
	bool layout_dirty = true;

	// Mouse-wheel multiplier — see GUIRenderer::BeginFrame. < 1.0 dampens.
	float scroll_sensitivity = 0.4f;

	// Layout preset; menu/View/Layout cycles through these.
	LayoutPreset layout_preset = LayoutPreset::Develop;

	// Viewport HUD overlays (small, click-through, opt-out).
	bool hud_show_perf = true;
	bool hud_show_axes = true;
	bool hud_show_technique = true;
};
