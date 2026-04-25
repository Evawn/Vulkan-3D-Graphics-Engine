#pragma once

#include <cstddef>

// Application-level events posted by techniques, panels, and other UI hooks.
// Drained by Application::ProcessEvents.
enum class AppEventType {
	HotReloadShaders,
	SwitchRenderer,
	DpiChanged,
	CaptureScreenshot,
	ReloadTechnique,
	RebuildGraph,
	RecreatePipelines,
};

struct AppEvent {
	AppEventType type;
	size_t index = 0;      // SwitchRenderer
	float scale = 1.0f;    // DpiChanged
};
