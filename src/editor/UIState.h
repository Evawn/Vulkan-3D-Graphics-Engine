#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

// Named layout presets. Develop is the default 4-quadrant editor; Performance
// hides everything except the viewport (and HUD); Profile maximizes
// analytics+render-graph and shrinks the viewport.
enum class LayoutPreset : uint8_t {
	Develop,
	Performance,
	Profile
};

// How the rendered scene is mapped into the viewport panel.
//   Native — render at the panel's pixel extent (no scaling, no margins).
//   Center — render at a fixed target res; place 1:1 in panel, black margins
//            when target < panel; down-fit (preserve aspect) when target > panel.
//   Fit    — render at a fixed target res; stretch to fill panel (aspect ignored).
enum class ResolutionMode : uint8_t {
	Native,
	Center,
	Fit
};

struct ResolutionPolicy {
	ResolutionMode mode   = ResolutionMode::Native;
	VkExtent2D     target = { 1920, 1080 };   // honored only in Center / Fit
	// Custom-resolution scratch — kept separate from `target` so the user can
	// type without overwriting the live target until they press Apply.
	int            customW = 1920;
	int            customH = 1080;
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

	// Render-target sizing policy. Status-bar widget mutates this; Editor
	// converts (mode, target, panel) -> the actual offscreen extent in
	// GetEffectiveRenderExtent().
	ResolutionPolicy resolution{};
};
