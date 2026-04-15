#pragma once

struct UIState {
	bool viewport_only = false;
	bool os_fullscreen = false;
	float right_panel_px = 0.0f;
	float bottom_panel_px = 0.0f;
	bool camera_focused = false;
	bool layout_dirty = true;
};
